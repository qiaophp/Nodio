// aardio_chunk.cpp — container reader + runtime record decoder for the
// official aardio "=aardio" precompiled chunk.
// See aardio_chunk.h and docs/aardio-bytecode.md for the grammar and the
// reverse-engineering evidence trail.
//
// Grammar (recovered from aardio.exe loader FUN_004e93a0):
//
//   chunk       := name str ; attr1 i32 ; attr2 i32 ; flags u8[5]
//                  ; code codeBlock ; consts constBlock ; objects objectBlock
//                  ; tableA u32Block ; tableB recordBlock ; tableC strBlock
//   str         := u32 len ; (len==0 -> null) ; else (len-1) bytes ; u8 flags
//   codeBlock   := u32 count ; count * u32
//   constBlock  := u32 count ; count * constant
//   constant    := u8 type ; 0 null | 1 bool(u8) | 3 number(8B double)
//                  | 4 string(str)
//   objectBlock := u32 count ; count * chunk
//   u32Block    := u32 count ; count * u32
//   recordBlock := u32 count ; count * (str, i32, i32)
//   strBlock    := u32 count ; count * str
//
// The reader is deliberately strict: every count is validated against the
// bytes actually left in the buffer, and nesting is capped at the official
// limit of 200 syntax levels ("chunk has too many syntax levels").
//
// The record decoder implements the runtime layout of the official
// interpreter FUN_004C4BC0 (image VA 0x004C4BC0).  Official aardio can
// execute its own precompiled chunks but cannot inspect them; Nodio can,
// which is why the decoder lives in the product and not only in the
// research tooling.
#include "aardio_chunk.h"

namespace NodioVM
{
    static const unsigned char kAardioHeader[15] = {
        '=', 'a', 'a', 'r', 'd', 'i', 'o',
        0x10, 0x00, 0x01, 0x04, 0x04, 0x04, 0x08, 0x00
    };

    // 官方加载器允许的最大嵌套层数
    static const int kMaxSyntaxLevels = 200;

    // 官方解释器 FUN_004C4BC0 的 case 表：61 种合法记录种类 0x00..0x3C
    static const wchar_t* const kAardioOpcodeNames[61] = {
        L"LOAD_OUTER",     // 0x00
        L"LOAD_INT",       // 0x01
        L"CLEAR_LOCALS",   // 0x02
        L"LOAD_UPVALUE",   // 0x03
        L"LOAD_LOCAL",     // 0x04
        L"GET_MEMBER",     // 0x05
        L"LOAD_SELF_NS",   // 0x06
        L"GET_MEMBER2",    // 0x07
        L"GET_GLOBAL",     // 0x08
        L"GET_MEMBER3",    // 0x09
        L"GET_MEMBER4",    // 0x0A
        L"INDEX",          // 0x0B
        L"SET_MEMBER",     // 0x0C
        L"SET_MEMBER2",    // 0x0D
        L"STORE_UPVALUE",  // 0x0E
        L"LOAD_IF_SET",    // 0x0F
        L"SET_METATABLE",  // 0x10
        L"SET_MEMBER3",    // 0x11
        L"CONCAT_MEMBER",  // 0x12
        L"RAWSET",         // 0x13
        L"SET_MEMBER4",    // 0x14
        L"NEW_NATIVE",     // 0x15
        L"IMPORT_MEMBER",  // 0x16
        L"METHOD_PREP",    // 0x17
        L"ADD",            // 0x18
        L"SUB",            // 0x19
        L"MUL",            // 0x1A
        L"DIV",            // 0x1B
        L"MOD",            // 0x1C
        L"POW",            // 0x1D
        L"NEG",            // 0x1E
        L"TRUTHY",         // 0x1F
        L"LENGTH",         // 0x20
        L"CONCAT",         // 0x21
        L"JMP",            // 0x22
        L"JZ",             // 0x23
        L"JE",             // 0x24
        L"JG",             // 0x25
        L"JLE",            // 0x26
        L"JTRUE",          // 0x27
        L"JTRUE2",         // 0x28
        L"TAILCALL",       // 0x29
        L"CALL",           // 0x2A
        L"CALL_NATIVE",    // 0x2B
        L"RET",            // 0x2C
        L"ADDEQ",          // 0x2D
        L"SUBEQ",          // 0x2E
        L"CALL_MULTI",     // 0x2F
        L"CALL_VALUE",     // 0x30
        L"CALL_NS",        // 0x31
        L"BUILD_ARRAY",    // 0x32
        L"GC",             // 0x33
        L"NEW_CLOSURE",    // 0x34
        L"BITNOT",         // 0x35
        L"BITAND",         // 0x36
        L"BITOR",          // 0x37
        L"BITXOR",         // 0x38
        L"SHL",            // 0x39
        L"SHR",            // 0x3A
        L"USHR",           // 0x3B
        L"CALL_N",         // 0x3C
    };

    // DAT_007C80A0：每 kind 1 字节（自 aardio.exe 内存导出）
    //   bits 0..1 = 记录类别  0=三操作数 1=单18位 2=单18位有符号
    //   bits 2..3 = C 操作数寻址类别
    //   bits 4..5 = B 操作数（类别 1 时即 18 位操作数）寻址类别
    //   bit  6    = 可作跳转落点
    //   bit  7    = 后面必须紧跟一条 0x22
    //
    // 寻址类别（官方 FUN_004D4400）：
    //   0 = 必须为 0      1 = 不检查
    //   2 = 局部槽下标（< 槽位数 chunk 帧 +0x4B）
    //   3 = bit8 ? 常量表下标（< 常量数 +0x28） : 局部槽下标
    static const unsigned char kAardioDescriptor[64] = {
        0x71, 0x54, 0x60, 0x50, 0x60, 0x71, 0x41, 0x71,
        0x60, 0x6C, 0x6C, 0x6C, 0x31, 0x31, 0x10, 0x20,
        0x30, 0x3C, 0x3C, 0x3C, 0x3C, 0x54, 0x38, 0x6C,
        0x7C, 0x7C, 0x7C, 0x7C, 0x7C, 0x7C, 0x60, 0x60,
        0x60, 0x68, 0x22, 0xBC, 0xBC, 0xBC, 0xBC, 0xE4,
        0xE4, 0x28, 0x54, 0x54, 0x10, 0x62, 0x62, 0x84,
        0x00, 0x84, 0x14, 0x00, 0x51, 0x70, 0x7C, 0x7C,
        0x7C, 0x7C, 0x7C, 0x3B, 0x7C, 0x50, 0x00, 0x00,
    };

    // 跳转类记录：位移取自哪条记录 + 偏置（实测自官方解释器取位表达式）
    //   0 = 本条记录, 1 = 下一条 u32
    struct JumpSpec { int fromNext; unsigned int bias; };

    static bool AardioJumpSpec(int kind, JumpSpec* out)
    {
        switch (kind)
        {
        case 0x22: case 0x2D: case 0x2E:
            out->fromNext = 0; out->bias = 0x1FFFE; return true;
        case 0x23: case 0x24: case 0x25: case 0x26:
            out->fromNext = 1; out->bias = 0x1FFFE; return true;
        case 0x27: case 0x28:
            out->fromNext = 1; out->bias = 0x1FFFD; return true;
        default:
            return false;
        }
    }

    const wchar_t* AardioOpcodeName(int kind)
    {
        if (kind < 0 || kind >= 61) return L"OP??";
        return kAardioOpcodeNames[kind];
    }

    bool AardioKindIsJump(int kind)
    {
        JumpSpec js;
        return AardioJumpSpec(kind, &js);
    }

    // ---- S-6：61 种记录的规格表 ----
    //
    // 名字取自官方解释器 FUN_004C4BC0 的 case 表；**操作数角色**与**语义**
    // 实测自该函数的 61 个 case（反汇编 + 反编译双向核对）。
    //
    // 操作数角色的通用约定（第十五轮按官方反汇编逐 case 复核）：
    //   A  = bits  6..13 → 目标局部槽（< 槽位数，chunk 帧 +0x4B）
    //   C  = bits 14..22 → 操作数②（9 位）
    //   B  = bits 23..31 → 操作数①（9 位）
    //   K  = bits 14..31 合成 → 18 位操作数（描述符类别 1 的记录）
    //
    // **B/C 与「左/右」的对应关系（第十五轮定案）**
    //   在三操作数记录里，**左操作数（第一个操作数）恒在 bits 23..31（B）**，
    //   右操作数（第二个操作数）在 bits 14..22（C）。
    //   实测依据（官方解释器 FUN_004C4BC0 的 case 装参序列）：
    //     · 0x19 SUB  → 结果 = *B − *C            （decomp 345-349）
    //     · 0x1B DIV  → 结果 = *B / *C            （decomp 399-403）
    //     · 0x1D POW  → FUN_007498A0(*B, *C)      （decomp 454-456）
    //     · 0x36/39  → FUN_004C49C0(vm, dest, *B, *C, kind)
    //                   而 FUN_004C49C0 里 local_10 = *param_3(=B)、
    //                   uVar4 = *param_4(=C)，即 lhs=B、rhs=C
    //   字段名 B/C 的归属则由官方校验器 FUN_004D4450 确认：
    //     bits 23..31 对应描述符 bits 4..5（B 类别），
    //     bits 14..22 对应描述符 bits 2..3（C 类别）。
    //
    // **9 位操作数的第 9 位（bit22 / bit31）决定寻址方式**
    // （实测自官方操作数检查器 FUN_004D4400）：
    //   bit8 = 0 → 局部槽下标（局部槽表）
    //   bit8 = 1 → 常量表下标（chunk 帧 +0x08 的 16 字节/项常量表）
    //
    // **18 位操作数**只出现在描述符类别 1 的记录上：
    //   0x00/0x05/0x07/0x0C/0x0D → 常量表下标（成员名等字符串常量）
    //   0x34 NEW_CLOSURE          → 嵌套对象下标（闭包体）
    //
    // get/set 家族的每个变体（第十五轮按官方调用点实参逐个复核）：
    //   0x05 GET_MEMBER   对象 = 模块命名空间(iVar11+0x10, type 5)，键 = 常量表[K]，flag=0
    //   0x07 GET_MEMBER2  对象 = VM 暂存槽(vm+0x40)，键 = 常量表[K]，flag=0
    //   0x09/0x0A GET_M3/4 目标 = A 槽，对象 = B 槽，键 = C 槽，flag=-1
    //   0x0B INDEX          目标 = A 槽，对象 = B 槽，下标 = C 槽
    //   0x0C SET_MEMBER   对象 = 模块命名空间，键 = 常量表[K]，值 = A 槽，flag=1
    //   0x0D SET_MEMBER2  对象 = VM 暂存槽(vm+0x40)，键 = 常量表[K]，值 = A 槽，flag=1
    //   0x11 SET_MEMBER3  对象 = A 槽，键 = B 槽，值 = C 槽，flag=1
    //   0x14 SET_MEMBER4  对象 = A 槽，键 = B 槽，值 = C 槽，flag=0
    //   0x13 RAWSET      对象 = A 槽，键 = B 槽，值 = C 槽（无 flag）
    //   0x12 CONCAT_MEM  表 = A 槽，键 = B 槽，待追加值 = C 槽
    //   0x17 METHOD_PREP  结果 = A 槽，A+1 = B 槽副本，键 = C 槽，flag=1
    //
    // **注意 set 家族与 get 家族的对象/键位置并不对称**：
    //   get（0x09/0x0A/0x0B）→ 对象在 B、键在 C；
    //   set（0x11/0x13/0x14）→ 对象在 A、键在 B、值在 C。
    // 依据是官方调用点的压栈次序（cdecl：最后一次压栈 = 第 1 实参）：
    //   get  → PUSH flag / PUSH dest(A) / PUSH key(C) / PUSH obj(B) / PUSH vm
    //   set  → PUSH flag / PUSH value(C) / PUSH key(B) / PUSH obj(A) / PUSH vm
    // 并由官方样本 aifix.aardio 交叉验证（0x09 的 B 是局部槽、C 是常量名；
    // 0x11 的 A 是局部槽、B 是常量名、C 是值槽）。
    static const std::vector<AardioKindInfo> kKindTable = {
        {0x00, L"LOAD_OUTER",    L"A=目标 K=常量下标(18位)", L"A ← 常量表[K] 拷贝 12 字节（基址取自外层作用域）", AardioImplemented},
        {0x01, L"LOAD_INT",      L"A=目标 B=立即数(9位) C≠0则下一条u32是完整立即数", L"A ← 整数立即数", AardioImplemented},
        {0x02, L"CLEAR_LOCALS",  L"A..B=要清空的槽区间", L"把 A..B 的类型标签清成 null", AardioImplemented},
        {0x03, L"LOAD_UPVALUE",  L"A=目标 B=上值槽", L"A ← 闭包上值槽 B", AardioImplemented},
        {0x04, L"LOAD_LOCAL",    L"A=目标 B=局部槽", L"A ← 局部槽 B（bit8=1 时改指常量表）", AardioImplemented},
        {0x05, L"GET_MEMBER",    L"A=目标 K=常量下标(18位)", L"取模块命名空间(iVar11+0x10)的成员（成员名 = 常量表[K]，须为字符串）", AardioImplemented},
        {0x06, L"LOAD_SELF_NS",  L"A=目标", L"A ← 当前模块命名空间（type 5）", AardioImplemented},
        {0x07, L"GET_MEMBER2",   L"A=目标 K=常量下标(18位)", L"取 VM 暂存槽(vm+0x40)对象的成员（成员名 = 常量表[K]）", AardioImplemented},
        {0x08, L"GET_GLOBAL",    L"A=目标 B=源槽", L"A ← B 槽对象所属的全局/父命名空间表（type 5）", AardioImplemented},
        {0x09, L"GET_MEMBER3",   L"A=目标 B=对象槽 C=键槽(bit8=1→常量名)", L"取表成员（flag=-1）", AardioImplemented},
        {0x0A, L"GET_MEMBER4",   L"A=目标 B=对象槽 C=键槽(bit8=1→常量名)", L"取表成员（flag=-1）", AardioImplemented},
        {0x0B, L"INDEX",         L"A=目标 B=对象槽 C=下标槽", L"按下标取元素（字符串按字符、数组按元素）", AardioImplemented},
        {0x0C, L"SET_MEMBER",    L"A=值槽 K=常量下标(18位)", L"写模块命名空间的成员（成员名 = 常量表[K]）", AardioImplemented},
        {0x0D, L"SET_MEMBER2",   L"A=值槽 K=常量下标(18位)", L"写 VM 暂存槽(vm+0x40)对象的成员（成员名 = 常量表[K]）", AardioImplemented},
        {0x0E, L"STORE_UPVALUE", L"A=源槽 B=上值槽", L"上值槽 B ← 局部槽 A"},
        {0x0F, L"LOAD_IF_SET",   L"A=目标 B=源槽", L"当 B 槽非 null 时才复制到 A"},
        {0x10, L"SET_METATABLE", L"A=对象槽 B=元表槽", L"设置元表（表 +0x08）", AardioImplemented},
        {0x11, L"SET_MEMBER3",   L"A=对象槽 B=键槽(bit8=1→常量名) C=值槽", L"写表成员（flag=1）", AardioImplemented},
        {0x12, L"CONCAT_MEMBER", L"A=表槽 B=键槽 C=待追加值槽", L"表成员串拼接（成员为字符串时以 ; 连接后写回，否则整体覆盖）", AardioImplemented},
        {0x13, L"RAWSET",        L"A=对象槽 B=键槽 C=值槽", L"rawset 表成员（绕过 __set）", AardioImplemented},
        {0x14, L"SET_MEMBER4",   L"A=对象槽 B=键槽(bit8=1→常量名) C=值槽", L"写表成员（flag=0）", AardioImplemented},
        {0x15, L"NEW_NATIVE",    L"A=目标 C:B=内建序号(位域)", L"新建原生函数对象"},
        {0x16, L"IMPORT_MEMBER", L"A=目标 C=名字(bit8=1→常量)", L"按点号名导入命名空间成员"},
        {0x17, L"METHOD_PREP",   L"A=目标 A+1=对象副本 B=对象槽 C=键槽(bit8=1→常量名)", L"取方法：先把 B 槽拷到 A+1，再取 B[C] 到 A", AardioImplemented},
        {0x18, L"ADD",           L"A=目标 B=左 C=右", L"A ← B + C（字符串走拼接）", AardioImplemented},
        {0x19, L"SUB",           L"A=目标 B=左 C=右", L"A ← B - C", AardioImplemented},
        {0x1A, L"MUL",           L"A=目标 B=左 C=右", L"A ← B * C", AardioImplemented},
        {0x1B, L"DIV",           L"A=目标 B=左 C=右", L"A ← B / C", AardioImplemented},
        {0x1C, L"MOD",           L"A=目标 B=左 C=右", L"A ← B % C（浮点取模，整数语义）", AardioImplemented},
        {0x1D, L"POW",           L"A=目标 B=左 C=右", L"A ← B ^ C（幂）", AardioImplemented},
        {0x1E, L"NEG",           L"A=目标 B=源槽", L"A ← -B（一元负号）", AardioImplemented},
        {0x1F, L"TRUTHY",        L"A=目标 B=源槽", L"A ← (B 是否为真)", AardioImplemented},
        {0x20, L"LENGTH",        L"A=目标 B=源槽", L"A ← 长度（字符串长度 / 表元素数）", AardioImplemented},
        {0x21, L"CONCAT",        L"A=目标 B=参数表槽", L"变参串拼接，结果写回 A"},
        {0x22, L"JMP",           L"R18=相对位移", L"无条件跳转：IP ← 本条 + R18", AardioImplemented},
        {0x23, L"JZ",            L"A=期望布尔(0/1) B=左 C=右 R18=位移(下一条u32)", L"r=(B==C)；r==A 时跳转", AardioImplemented},
        {0x24, L"JE",            L"A=期望布尔(0/1) B=左 C=右 R18=位移(下一条u32)", L"r=(B==C)；r==A 时跳转", AardioImplemented},
        {0x25, L"JG",            L"A=期望布尔(0/1) B=左 C=右 R18=位移(下一条u32)", L"r=(C>B)；r==A 时跳转", AardioImplemented},
        {0x26, L"JLE",           L"A=期望布尔(0/1) B=左 C=右 R18=位移(下一条u32)", L"r=(B<=C)；r==A 时跳转", AardioImplemented},
        {0x27, L"JTRUE",         L"A=真值槽 C=立即数(0/1) R18=位移(下一条u32)", L"真值(A) != C 时跳转", AardioImplemented},
        {0x28, L"JTRUE2",        L"A=目标槽 B=真值槽 C=立即数(0/1) R18=位移(下一条u32)", L"真值(B) != C 时跳转，并带上槽 B 的值", AardioImplemented},
        {0x29, L"TAILCALL",      L"A=函数槽", L"尾调用"},
        {0x2A, L"CALL",          L"A=基址槽 B=实参偏移", L"调用（建立新帧），实参从槽 A+B 起"},
        {0x2B, L"CALL_NATIVE",   L"A=基址槽 B=实参偏移", L"调用并搬运实参到新帧"},
        {0x2C, L"RET",           L"A=返回值槽", L"返回", AardioImplemented},
        {0x2D, L"ADDEQ",         L"A=目标槽 R18=位移", L"A ← A + delta（复合赋值）"},
        {0x2E, L"SUBEQ",         L"A=目标槽 R18=位移", L"A ← A - delta（复合赋值）"},
        {0x2F, L"CALL_MULTI",    L"A=基址槽 B=实参偏移", L"多值调用：搬运 4 个槽后调用"},
        {0x30, L"CALL_VALUE",    L"A=基址槽 B=实参偏移", L"调用函数值"},
        {0x31, L"CALL_NS",       L"A=基址槽 B=实参偏移", L"调用（命名空间/函数值）"},
        {0x32, L"BUILD_ARRAY",   L"A=目标表槽 B=源槽数 C=元素个数(C=0则下一条u32是个数)", L"把 A+1..A+B 各槽的元素追加进 A 槽的表", AardioImplemented},
        {0x33, L"GC",            L"—", L"垃圾回收", AardioImplemented},
        {0x34, L"NEW_CLOSURE",   L"A=目标槽 K=嵌套对象下标(18位)", L"建立闭包：收集后续 kind 3/4 记录里的上值"},
        {0x35, L"BITNOT",        L"A=目标 B=源槽", L"A ← ~B", AardioImplemented},
        {0x36, L"BITAND",        L"A=目标 B=左 C=右", L"A ← B & C", AardioImplemented},
        {0x37, L"BITOR",         L"A=目标 B=左 C=右", L"A ← B | C", AardioImplemented},
        {0x38, L"BITXOR",        L"A=目标 B=左 C=右", L"A ← B ^ C", AardioImplemented},
        {0x39, L"SHL",           L"A=目标 B=左 C=右", L"A ← B << C", AardioImplemented},
        {0x3A, L"SHR",           L"A=目标 B=左 C=右", L"A ← B >> C（算术右移）", AardioImplemented},
        {0x3B, L"USHR",          L"A=目标 B=左 C=右", L"A ← B >>> C（逻辑右移）", AardioImplemented},
        {0x3C, L"CALL_N",        L"A=基址槽 B=实参个数-1", L"带显式实参个数的调用"},
    };

    const std::vector<AardioKindInfo>& AardioKindTable()
    {
        return kKindTable;
    }

    const AardioKindInfo* AardioKindInfoOf(int kind)
    {
        if (kind < 0 || kind >= (int)kKindTable.size()) return nullptr;
        return &kKindTable[kind];
    }

    int AardioImplementedKindCount()
    {
        int n = 0;
        for (const auto& ki : kKindTable)
            if (ki.state == AardioImplemented) n++;
        return n;
    }

    wstring AardioUnimplementedSection()
    {
        wstring s;
        int done = AardioImplementedKindCount();
        int total = (int)kKindTable.size();
        s += L"== 未实现记录种类 ==";
        s += L"\n可执行 " + std::to_wstring(done) + L"/" +
            std::to_wstring(total) +
            L"（61 种记录全部可解码；本节列出 Nodio 记录 VM 还不能执行的那部分）";
        for (const auto& ki : kKindTable)
        {
            if (ki.state == AardioImplemented) continue;
            s += L"\n  0x";
            wchar_t hex[8];
            swprintf_s(hex, L"%02X", ki.kind);
            s += hex;
            s += L" ";
            s += ki.name;
            s += L"    ";
            s += ki.operands;
            s += L"    → ";
            s += ki.effect;
        }
        return s;
    }

    AardioRecord DecodeAardioRecord(unsigned int word, int pc, unsigned int nextWord)
    {
        AardioRecord rec;
        rec.word = word;
        rec.kind = (int)(word & 0x3F);
        rec.a = (int)((word >> 6) & 0xFF);
        unsigned int c = (word >> 14) & 0x1FF;
        unsigned int b = (word >> 23) & 0x1FF;
        rec.b = (int)(b & 0xFF);
        rec.bGlobal = (b & 0x100) != 0;
        rec.c = (int)(c & 0xFF);
        rec.cGlobal = (c & 0x100) != 0;

        JumpSpec js;
        if (AardioJumpSpec(rec.kind, &js))
        {
            unsigned int raw = (js.fromNext ? nextWord : word) >> 14;
            rec.rel = (int)raw - (int)js.bias;
            rec.target = pc + rec.rel;
            rec.relFromNext = js.fromNext != 0;
        }
        return rec;
    }

    namespace
    {
        struct ChunkReader
        {
            const unsigned char* p = nullptr;
            size_t size = 0;
            size_t pos = 0;
            bool bad = false;
            wstring err;

            bool Need(size_t n)
            {
                if (bad) return false;
                // NOTE: compare against the remaining bytes instead of
                // `pos + n > size` — the addition overflows on 32-bit builds
                // (size_t is 32 bit there), which would let a bogus length
                // slip through and blow up in std::string.
                if (pos > size || n > size - pos)
                {
                    bad = true;
                    if (err.empty()) err = L"数据提前结束";
                    return false;
                }
                return true;
            }

            unsigned char U8()
            {
                if (!Need(1)) return 0;
                return p[pos++];
            }

            unsigned int U32()
            {
                if (!Need(4)) return 0;
                unsigned int v = (unsigned int)p[pos] |
                    ((unsigned int)p[pos + 1] << 8) |
                    ((unsigned int)p[pos + 2] << 16) |
                    ((unsigned int)p[pos + 3] << 24);
                pos += 4;
                return v;
            }

            void Skip(size_t n)
            {
                if (!Need(n)) return;
                pos += n;
            }

            // str := u32 len ; (len==0 -> null) ; else (len-1) bytes ; u8 flags
            // flags & 0x10 → 宽字符（载荷是 UTF-16LE）
            bool Str(std::string* text, unsigned char* outFlags = nullptr)
            {
                size_t at = pos;
                unsigned int len = U32();
                if (bad) return false;
                if (len == 0)
                {
                    if (text) text->clear();
                    if (outFlags) *outFlags = 0;
                    return true;
                }
                if (len < 2)
                {
                    bad = true;
                    err = L"字符串长度非法 @" + std::to_wstring(at);
                    return false;
                }
                if (!Need((size_t)len - 1)) return false;
                if (text)
                {
                    const char* s = (const char*)p + pos;
                    size_t n = (size_t)len - 1;
                    // 载荷是 text + '\0'，末尾 NUL 不计入文本
                    if (n && s[n - 1] == '\0') n--;
                    text->assign(s, n);
                }
                pos += (size_t)len - 1;
                unsigned char f = U8();
                if (outFlags) *outFlags = f;
                return !bad;
            }
        };

        wstring Utf8ToWide(const std::string& s)
        {
            if (s.empty()) return wstring();
            int w = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
            if (w <= 0) return wstring();
            wstring out;
            out.resize((size_t)w);
            MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], w);
            return out;
        }

        // 宽字符串载荷：UTF-16LE（末尾一个 NUL 字符）
        wstring Utf16ToWide(const std::string& s)
        {
            size_t n = s.size() / 2;
            if (n == 0) return wstring();
            const unsigned char* p = (const unsigned char*)s.data();
            wstring out;
            out.resize(n);
            for (size_t i = 0; i < n; i++)
            {
                out[i] = (wchar_t)(p[i * 2] | (p[i * 2 + 1] << 8));
            }
            // 去掉末尾 NUL
            while (!out.empty() && out[out.size() - 1] == 0) out.resize(out.size() - 1);
            return out;
        }

        // 解码一段 code 数组，把结果累进统计（记录数 / 种类直方图 / 跳转）
        void DecodeCodeArray(const std::vector<unsigned int>& code, AardioChunkStats* st)
        {
            if (!st) return;
            int n = (int)code.size();
            for (int i = 0; i < n; i++)
            {
                unsigned int next = (i + 1 < n) ? code[i + 1] : 0u;
                AardioRecord rec = DecodeAardioRecord(code[i], i, next);
                st->records++;
                if (rec.kind > 0x3C)
                {
                    st->badKinds++;
                    continue;
                }
                st->kindCount[rec.kind]++;
                if (rec.target >= 0)
                {
                    st->jumps++;
                    if (rec.target >= 0 && rec.target < n) st->jumpsResolved++;
                }
            }
        }

        // 递归解析一个 chunk（不含嵌套对象），统计本层的指令字与常量
        bool ParseOne(ChunkReader& r, AardioChunkStats& st, AardioChunkNode* node, int depth)
        {
            if (depth > kMaxSyntaxLevels)
            {
                r.bad = true;
                r.err = L"嵌套层数超过官方上限";
                return false;
            }

            st.objects++;
            if (depth > st.maxDepth) st.maxDepth = depth;

            std::string name;
            if (!r.Str(&name)) return false;
            if (node && node->name.empty()) node->name = Utf8ToWide(name);

            // attr1 / attr2 = 源码首行 / 末行（实测随嵌套顺序单调递增）
            int attr1 = (int)r.U32();
            int attr2 = (int)r.U32();
            unsigned char flags[5] = { 0, 0, 0, 0, 0 };
            for (int i = 0; i < 5; i++) flags[i] = r.U8();
            if (r.bad) return false;
            if (node)
            {
                node->attr1 = attr1;
                node->attr2 = attr2;
                for (int i = 0; i < 5; i++) node->flags[i] = flags[i];
            }

            // code block
            unsigned int nCode = r.U32();
            if (r.bad) return false;
            if ((size_t)nCode > (r.size - r.pos) / 4)
            {
                r.bad = true;
                r.err = L"指令块长度越界";
                return false;
            }
            st.codeWords += (int)nCode;
            {
                // 记录流要照官方运行期布局解码（诊断与反汇编都要用），
                // 所以即使不保留 chunk 树也要把这段 code 读出来。
                std::vector<unsigned int> code((size_t)nCode);
                for (unsigned int i = 0; i < nCode; i++)
                {
                    code[i] = r.U32();
                }
                if (r.bad) return false;
                DecodeCodeArray(code, &st);
                if (node) node->code.swap(code);
            }

            // constant block
            unsigned int nConst = r.U32();
            if (r.bad) return false;
            std::vector<AardioConstValue> consts;
            if (node) consts.reserve((size_t)nConst);
            for (unsigned int i = 0; i < nConst && !r.bad; i++)
            {
                unsigned char t = r.U8();
                st.consts++;
                AardioConstValue cv;
                cv.type = (int)t;
                switch (t)
                {
                case 0:     // null
                    break;
                case 1:     // bool
                    cv.b = r.U8() != 0;
                    break;
                case 3:     // number (IEEE-754 double)
                {
                    st.numbers++;
                    unsigned char raw[8];
                    for (int k = 0; k < 8; k++) raw[k] = r.U8();
                    if (r.bad) return false;
                    double d = 0;
                    memcpy(&d, raw, 8);
                    cv.num = d;
                    break;
                }
                case 4:     // string
                {
                    st.strings++;
                    std::string text;
                    unsigned char sflags = 0;
                    if (!r.Str(&text, &sflags)) return false;
                    cv.str = (sflags & 0x10) ? Utf16ToWide(text) : Utf8ToWide(text);
                    break;
                }
                default:
                    r.bad = true;
                    r.err = L"常量类型非法 (" + std::to_wstring((int)t) + L")";
                    return false;
                }
                if (node) consts.push_back(cv);
            }
            if (r.bad) return false;
            if (node) node->consts.swap(consts);

            // nested objects
            unsigned int nObj = r.U32();
            if (r.bad) return false;
            for (unsigned int i = 0; i < nObj && !r.bad; i++)
            {
                if (node)
                {
                    node->children.resize((size_t)nObj);
                    AardioChunkNode& child = node->children[(size_t)i];
                    if (!ParseOne(r, st, &child, depth + 1)) return false;
                }
                else
                {
                    if (!ParseOne(r, st, nullptr, depth + 1)) return false;
                }
            }
            if (r.bad) return false;

            // table A : u32 array
            unsigned int nA = r.U32();
            if (r.bad) return false;
            if ((size_t)nA > (r.size - r.pos) / 4)
            {
                r.bad = true;
                r.err = L"辅助表 A 长度越界";
                return false;
            }
            r.Skip((size_t)nA * 4);

            // table B : (str, i32, i32) records
            unsigned int nB = r.U32();
            if (r.bad) return false;
            for (unsigned int i = 0; i < nB && !r.bad; i++)
            {
                if (!r.Str(nullptr)) return false;
                r.Skip(4);
                r.Skip(4);
            }
            if (r.bad) return false;

            // table C : string array
            unsigned int nC = r.U32();
            if (r.bad) return false;
            for (unsigned int i = 0; i < nC && !r.bad; i++)
            {
                if (!r.Str(nullptr)) return false;
            }
            return !r.bad;
        }

        wstring Hex8(unsigned int v)
        {
            static const wchar_t* hex = L"0123456789ABCDEF";
            wstring s(8, L'0');
            for (int i = 7; i >= 0; i--)
            {
                s[(size_t)i] = hex[v & 0xF];
                v >>= 4;
            }
            return s;
        }

        wstring Pad(wstring s, size_t n)
        {
            while (s.size() < n) s += L' ';
            return s;
        }

        wstring FormatRecordLine(int pc, const AardioRecord& rec, int codeCount)
        {
            wstring line;
            line += Pad(std::to_wstring(pc), 6);
            line += L"  ";
            line += Hex8(rec.word);
            line += L"  ";
            line += Pad(AardioOpcodeName(rec.kind), 14);
            line += L" ";
            line += L"A=" + Pad(std::to_wstring(rec.a), 4);
            // 描述符类别 1 的记录只有一个 18 位操作数（bits 14..31）：
            // kind 0x00/0x05/0x07/0x0C/0x0D 是常量表下标，0x34 是嵌套对象下标。
            bool wide = (rec.kind <= 0x3C) && ((kAardioDescriptor[rec.kind] & 3) == 1);
            if (wide)
            {
                line += L"K=" + Pad(std::to_wstring((int)(rec.word >> 14)), 8);
                line += L"    ";
            }
            else
            {
                line += L"B=" + Pad(std::to_wstring(rec.b), 4);
                if (rec.bGlobal) line += L"!";
                line += L" ";
                line += L"C=" + Pad(std::to_wstring(rec.c), 4);
                if (rec.cGlobal) line += L"!";
            }
            line += L"  ; ";
            if (rec.target >= 0)
            {
                line += L"rel18=" + std::to_wstring(rec.rel);
                line += rec.relFromNext ? L"(next)" : L"(own)";
                line += L" -> " + std::to_wstring(rec.target);
                if (rec.target < 0 || rec.target >= codeCount)
                    line += L" [越界]";
            }
            else if (rec.bGlobal || rec.cGlobal)
            {
                // bit8 = 1 → 该操作数指向常量表，而不是局部槽表
                line += L"常量下标(bit8=1)";
            }
            return line;
        }
    }

    bool IsAardioChunk(const std::string& data)
    {
        if (data.size() < sizeof(kAardioHeader)) return false;
        return memcmp(data.data(), kAardioHeader, sizeof(kAardioHeader)) == 0;
    }

    wstring DisassembleAardioCode(const std::vector<unsigned int>& code, int maxLines)
    {
        wstring out;
        int n = (int)code.size();
        int limit = (maxLines > 0 && maxLines < n) ? maxLines : n;
        for (int i = 0; i < limit; i++)
        {
            unsigned int next = (i + 1 < n) ? code[i + 1] : 0u;
            AardioRecord rec = DecodeAardioRecord(code[i], i, next);
            out += FormatRecordLine(i, rec, n);
            out += L"\n";
        }
        if (limit < n)
        {
            out += L"  ... 共 " + std::to_wstring(n) + L" 条记录，已省略 " +
                std::to_wstring(n - limit) + L" 条\n";
        }
        return out;
    }

    static void DisassembleTree(const AardioChunkNode& node, int depth, int* budget,
        int maxLines, wstring* out)
    {
        wstring indent((size_t)depth * 2, L' ');
        *out += indent + L"chunk " + node.name + L" (" +
            std::to_wstring((int)node.code.size()) + L" 条记录)\n";
        if (maxLines > 0 && *budget <= 0) return;
        int n = (int)node.code.size();
        int limit = n;
        if (maxLines > 0 && maxLines < limit) limit = maxLines;
        for (int i = 0; i < limit; i++)
        {
            if (maxLines > 0 && *budget <= 0) break;
            unsigned int next = (i + 1 < n) ? node.code[i + 1] : 0u;
            AardioRecord rec = DecodeAardioRecord(node.code[i], i, next);
            *out += indent + L"  " + FormatRecordLine(i, rec, n) + L"\n";
            if (maxLines > 0) (*budget)--;
        }
        for (const AardioChunkNode& child : node.children)
        {
            DisassembleTree(child, depth + 1, budget, maxLines, out);
        }
    }

    wstring DisassembleAardioTree(const AardioChunkNode& root, int maxLines)
    {
        wstring out;
        int budget = maxLines > 0 ? maxLines : 0;
        DisassembleTree(root, 0, &budget, maxLines, &out);
        return out;
    }

    // ------------------------------------------------------------------
    // S-1b：官方校验器 FUN_004D4450 的等价实现
    //
    // 官方 aardio 能执行自己的预编译块，但**不能校验、不能诊断**：校验器
    // 只在加载时内部跑一次，失败就抛 "in precompiled chunk" 然后终止。
    // Nodio 把同一套规则做成产品能力——给出可定位到「对象 + 记录号」的
    // 结构化违规清单，这是「兼容」之上的超越项。
    //
    // 规则全部实测自官方三处代码：
    //   * 描述符表 DAT_007C80A0（每 kind 1 字节，自 aardio.exe 内存导出）
    //   * 操作数检查器 FUN_004D4400（9 位操作数的两种寻址）
    //   * 校验器 FUN_004D4450（逐条记录的结构约束）
    // ------------------------------------------------------------------

    // 官方描述符表见文件上方 kAardioDescriptor。

    namespace
    {
        struct ValidateCtx
        {
            const AardioChunkNode* node = nullptr;
            const std::wstring* path = nullptr;
            AardioValidateResult* res = nullptr;
            std::wstring* report = nullptr;
        };

        void VErr(ValidateCtx& ctx, int pc, const wstring& msg)
        {
            AardioValidateResult* res = ctx.res;
            res->errors++;
            if (res->firstErrorObject < 0)
            {
                res->firstErrorObject = res->objects - 1;
                res->firstErrorPc = pc;
                res->firstError = msg;
            }
            if (ctx.report)
            {
                *ctx.report += L"  ";
                *ctx.report += *ctx.path;
                *ctx.report += L" pc=" + std::to_wstring(pc) + L"  " + msg + L"\n";
            }
        }

        wstring KindName(int kind)
        {
            const wchar_t* n = AardioOpcodeName(kind);
            if (!n || wcscmp(n, L"OP??") == 0)
                return L"0x" + std::to_wstring(kind);
            return n;
        }

        // 9 位操作数：bit8 = 0 → 局部槽；bit8 = 1 → 常量表
        void CheckOperand9(ValidateCtx& ctx, int pc, int kind, const wchar_t* role,
            unsigned int val, int cat)
        {
            if (cat == 0)
            {
                if (val != 0)
                    VErr(ctx, pc, KindName(kind) + L": 操作数" + role + L" 必须为 0，实为 " +
                        std::to_wstring((int)val));
                return;
            }
            if (cat == 1) return;

            int slots = (int)ctx.node->flags[3];
            int nconst = (int)ctx.node->consts.size();
            if (cat == 2)
            {
                if ((int)val >= slots)
                    VErr(ctx, pc, KindName(kind) + L": 操作数" + role + L" 局部槽下标 " +
                        std::to_wstring((int)val) + L" 越界 (槽位数 " + std::to_wstring(slots) + L")");
                return;
            }
            // cat == 3
            if (val & 0x100)
            {
                unsigned int idx = val & 0xFF;
                if ((int)idx >= nconst)
                    VErr(ctx, pc, KindName(kind) + L": 操作数" + role + L" 常量下标 " +
                        std::to_wstring((int)idx) + L" 越界 (常量数 " + std::to_wstring(nconst) + L")");
            }
            else if ((int)val >= slots)
            {
                VErr(ctx, pc, KindName(kind) + L": 操作数" + role + L" 局部槽下标 " +
                    std::to_wstring((int)val) + L" 越界 (槽位数 " + std::to_wstring(slots) + L")");
            }
        }

        // 18 位操作数（描述符类别 1 的记录）
        void CheckOperand18(ValidateCtx& ctx, int pc, int kind, unsigned int val, int cat)
        {
            int slots = (int)ctx.node->flags[3];
            int nconst = (int)ctx.node->consts.size();
            if (cat == 0)
            {
                if (val != 0)
                    VErr(ctx, pc, KindName(kind) + L": 18 位操作数必须为 0，实为 " +
                        std::to_wstring((int)val));
            }
            else if (cat == 2)
            {
                if ((int)val >= slots)
                    VErr(ctx, pc, KindName(kind) + L": 18 位操作数 " + std::to_wstring((int)val) +
                        L" 越界 (槽位数 " + std::to_wstring(slots) + L")");
            }
            else if (cat == 3)
            {
                if ((int)val >= nconst)
                    VErr(ctx, pc, KindName(kind) + L": 常量下标 " + std::to_wstring((int)val) +
                        L" 越界 (常量数 " + std::to_wstring(nconst) + L")");
                else
                {
                    // 官方：取成员名的那几种必须指向字符串常量（type 4）
                    bool needString = (kind == 0x05 || kind == 0x07 ||
                        kind == 0x0C || kind == 0x0D);
                    if (needString && ctx.node->consts[(size_t)val].type != 4)
                        VErr(ctx, pc, KindName(kind) + L": 常量下标 " + std::to_wstring((int)val) +
                            L" 不是字符串常量");
                }
            }
        }

        void ValidateNode(ValidateCtx& ctx, const std::wstring& path)
        {
            const AardioChunkNode& node = *ctx.node;
            const std::vector<unsigned int>& code = node.code;
            int n = (int)code.size();
            int slots = (int)node.flags[3];
            int nconst = (int)node.consts.size();

            ctx.res->objects++;
            ctx.res->records += n;

            for (int pc = 0; pc < n; pc++)
            {
                unsigned int word = code[(size_t)pc];
                int kind = (int)(word & 0x3F);
                if (kind > 0x3C)
                {
                    VErr(ctx, pc, L"非法记录种类 " + std::to_wstring(kind));
                    continue;
                }
                unsigned char desc = kAardioDescriptor[kind];
                int cls = desc & 3;
                int catC = (desc >> 2) & 3;
                int catB = (desc >> 4) & 3;

                unsigned int A = (word >> 6) & 0xFF;
                if ((int)A >= slots)
                {
                    VErr(ctx, pc, KindName(kind) + L": 目标槽 A=" + std::to_wstring((int)A) +
                        L" 越界 (槽位数 " + std::to_wstring(slots) + L")");
                }

                if (cls == 0)
                {
                    CheckOperand9(ctx, pc, kind, L"C", (word >> 14) & 0x1FF, catC);
                    CheckOperand9(ctx, pc, kind, L"B", (word >> 23) & 0x1FF, catB);
                }
                else if (cls == 1)
                {
                    unsigned int op = word >> 14;   // 18 位
                    CheckOperand18(ctx, pc, kind, op, catB);
                    if (kind == 0x34)   // NEW_CLOSURE：操作数是嵌套对象下标
                    {
                        if ((int)op >= (int)node.children.size())
                        {
                            VErr(ctx, pc, L"NEW_CLOSURE: 嵌套对象下标 " +
                                std::to_wstring((int)op) + L" 越界 (对象数 " +
                                std::to_wstring((int)node.children.size()) + L")");
                        }
                        else
                        {
                            // 官方：闭包后面紧跟 N 条 LOAD_UPVALUE / LOAD_LOCAL
                            int up = (int)node.children[(size_t)op].flags[0];
                            for (int i = 1; i <= up; i++)
                            {
                                if (pc + i >= n)
                                {
                                    VErr(ctx, pc, L"NEW_CLOSURE: 上值记录越界");
                                    break;
                                }
                                int nk = (int)(code[(size_t)(pc + i)] & 0x3F);
                                if (nk != 0x03 && nk != 0x04)
                                {
                                    VErr(ctx, pc + i, L"NEW_CLOSURE: 上值记录应为 "
                                        L"LOAD_UPVALUE/LOAD_LOCAL，实为 " + KindName(nk));
                                    break;
                                }
                            }
                        }
                    }
                }
                else  // cls == 2：单 18 位有符号（跳转类）
                {
                    int target = pc + (int)((word >> 14) - 0x1FFFE);
                    if (target < 0 || target >= n)
                    {
                        VErr(ctx, pc, KindName(kind) + L": 跳转落点 " +
                            std::to_wstring(target) + L" 越界 (记录数 " + std::to_wstring(n) + L")");
                    }
                    else
                    {
                        // 官方：落点处连续的 BUILD_ARRAY 必须成对
                        int cnt = 0;
                        int q = target - 1;
                        while (q >= 0)
                        {
                            unsigned int w2 = code[(size_t)q];
                            if ((w2 & 0x3F) != 0x32 || (w2 & 0x7FC000) != 0) break;
                            cnt++;
                            q--;
                        }
                        if ((cnt & 1) != 0)
                            VErr(ctx, pc, KindName(kind) + L": 跳转落点处 BUILD_ARRAY 记录未成对");
                    }
                }

                // bit7：这类记录后面必须紧跟一条 0x22
                if (desc & 0x80)
                {
                    if (pc + 2 > n || (code[(size_t)(pc + 1)] & 0x3F) != 0x22)
                        VErr(ctx, pc, KindName(kind) + L": 后面必须紧跟一条 JMP(0x22)");
                }
                // 条件跳转：位移取自下一条 u32
                if (kind >= 0x23 && kind <= 0x28)
                {
                    if (pc + 1 >= n)
                    {
                        VErr(ctx, pc, KindName(kind) + L": 缺少后继的 JMP(0x22)");
                    }
                    else if ((code[(size_t)(pc + 1)] & 0x3F) != 0x22)
                    {
                        VErr(ctx, pc, KindName(kind) + L": 后继不是 JMP(0x22)");
                    }
                    else
                    {
                        unsigned int nxt = code[(size_t)(pc + 1)];
                        int bias = (kind == 0x27 || kind == 0x28) ? 0x1FFFD : 0x1FFFE;
                        int target = pc + (int)((nxt >> 14) - (unsigned int)bias);
                        if (target < 0 || target >= n)
                            VErr(ctx, pc, KindName(kind) + L": 跳转落点 " +
                                std::to_wstring(target) + L" 越界");
                    }
                }
            }

            for (size_t i = 0; i < node.children.size(); i++)
            {
                ValidateCtx sub = ctx;
                sub.node = &node.children[i];
                std::wstring childPath = path + L"/" + std::to_wstring((int)i);
                sub.path = &childPath;
                ValidateNode(sub, childPath);
            }
        }
    }

    bool ValidateAardioChunkTree(const AardioChunkNode& root,
        AardioValidateResult* out, wstring* report)
    {
        if (!out) return false;
        *out = AardioValidateResult();
        if (report) report->clear();

        ValidateCtx ctx;
        ctx.node = &root;
        ctx.res = out;
        ctx.report = report;
        std::wstring path = L"root";
        ctx.path = &path;
        ValidateNode(ctx, path);
        return out->errors == 0;
    }

    wstring AardioValidateSection(const AardioChunkNode& root,
        const AardioValidateResult& res)
    {
        wstring s;
        s += L"== 结构校验 ==";
        s += L"\n对象 " + std::to_wstring(res.objects) +
            L"  记录 " + std::to_wstring(res.records) +
            L"  违规 " + std::to_wstring(res.errors);
        s += L"\n规则：官方校验器 FUN_004D4450 等价实现"
            L"（描述符表 DAT_007C80A0 + 操作数寻址 FUN_004D4400 + 跳转配对 + 闭包上值）";
        s += res.errors == 0
            ? L"\n结论：通过（官方 aardio 只能执行、给不出这份结构化结论）"
            : L"\n结论：不通过，首个违规 " + std::to_wstring(res.firstErrorObject) +
            L" 号对象 / pc=" + std::to_wstring(res.firstErrorPc) + L"：" + res.firstError;
        return s;
    }

    bool ParseAardioChunkTree(const std::string& data, AardioChunkNode* root,
        AardioChunkStats* out, wstring* err)
    {
        if (!root || !out) return false;
        *root = AardioChunkNode();
        *out = AardioChunkStats();

        if (!IsAardioChunk(data))
        {
            if (err) *err = L"不是官方 =aardio 预编译块";
            return false;
        }

        ChunkReader r;
        r.p = (const unsigned char*)data.data();
        r.size = data.size();
        r.pos = sizeof(kAardioHeader);

        AardioChunkStats st;
        if (!ParseOne(r, st, root, 0))
        {
            if (err) *err = r.err.empty() ? L"结构不完整" : r.err;
            return false;
        }
        st.consumed = r.pos;

        // 记录种类直方图 → 实际用到的种类数
        st.kindsUsed = 0;
        for (int k = 0; k < 61; k++)
        {
            if (st.kindCount[k]) st.kindsUsed++;
        }

        if (!st.name.empty()) root->name = st.name;
        *out = st;
        return true;
    }

    bool ParseAardioChunk(const std::string& data, AardioChunkStats* out, wstring* err)
    {
        if (!out) return false;
        *out = AardioChunkStats();

        if (!IsAardioChunk(data))
        {
            if (err) *err = L"不是官方 =aardio 预编译块";
            return false;
        }

        ChunkReader r;
        r.p = (const unsigned char*)data.data();
        r.size = data.size();
        r.pos = sizeof(kAardioHeader);

        AardioChunkStats st;
        std::string rootName;
        // 根 chunk 的名字单独取一次（诊断用），随后整体解析
        {
            size_t save = r.pos;
            std::string nm;
            if (!r.Str(&nm))
            {
                if (err) *err = r.err;
                return false;
            }
            rootName = nm;
            r.pos = save;
        }

        if (!ParseOne(r, st, nullptr, 0))
        {
            if (err) *err = r.err.empty() ? L"结构不完整" : r.err;
            return false;
        }

        st.consumed = r.pos;
        st.kindsUsed = 0;
        for (int k = 0; k < 61; k++)
        {
            if (st.kindCount[k]) st.kindsUsed++;
        }
        if (!rootName.empty())
        {
            int w = MultiByteToWideChar(CP_UTF8, 0, rootName.c_str(),
                (int)rootName.size(), nullptr, 0);
            if (w > 0)
            {
                st.name.resize((size_t)w);
                MultiByteToWideChar(CP_UTF8, 0, rootName.c_str(),
                    (int)rootName.size(), &st.name[0], w);
            }
        }
        *out = st;
        return true;
    }

    wstring AardioChunkDiagnostic(const AardioChunkStats& st)
    {
        wstring msg = L"格式错误: 官方预编译模块 (=aardio) 暂不可执行 — ";
        msg += L"已解析出 " + std::to_wstring(st.objects) + L" 个对象 / " +
            std::to_wstring(st.codeWords) + L" 条指令 / " +
            std::to_wstring(st.consts) + L" 个常量";
        if (st.maxDepth > 0)
        {
            msg += L"（最大嵌套 " + std::to_wstring(st.maxDepth) + L" 层）";
        }
        // 记录流结论：官方 aardio 只会报「格式错误」，这里给出逐条解码后的
        // 可用信息（这是官方产品做不到的）。
        if (st.records > 0)
        {
            msg += L"；记录流已按官方运行期布局解码：";
            msg += std::to_wstring(st.records) + L" 条记录，";
            msg += L"用到 " + std::to_wstring(st.kindsUsed) + L"/61 种记录种类，";
            msg += L"非法种类 " + std::to_wstring(st.badKinds) + L" 条";
            if (st.jumps > 0)
            {
                msg += L"，" + std::to_wstring(st.jumps) + L" 个跳转目标中 " +
                    std::to_wstring(st.jumpsResolved) + L" 个落在指令边界上";
            }
        }
        msg += L"，但 Nodio 尚未实现该字节码的解释器";
        return msg;
    }
}
