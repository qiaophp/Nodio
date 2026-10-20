// aardio_chunk.h — reader for the official aardio "=aardio" precompiled
// chunk container (aardio.exe calls this format "precompiled chunk").
//
// The container grammar was recovered from aardio.exe (loader FUN_004e93a0
// and friends) and is byte exact: re-serializing a parsed chunk reproduces
// the input file bit for bit (see docs/aardio-bytecode.md).
//
// Besides the container walk this module also implements the **runtime
// record layout** of the official interpreter FUN_004C4BC0 (aardio.exe image
// VA 0x004C4BC0, the only function in the binary that does
// `switch (word & 0x3F)` over the 61 legal record kinds 0x00..0x3C).
//
// That is deliberately *more* than the official product can do: official
// aardio can execute its own precompiled chunks but cannot disassemble or
// diagnose them.  Nodio can do both — see DisassembleAardioCode() and
// AardioChunkDiagnostic().
#pragma once

#include "common.h"

namespace NodioVM
{
    struct AardioChunkStats
    {
        wstring name;         // 顶层 chunk 名（官方样本里通常为空）
        int objects = 0;      // chunk 总数（含根）
        int codeWords = 0;    // 指令字总数
        int consts = 0;       // 常量总数
        int strings = 0;      // 字符串常量数
        int numbers = 0;      // 数字常量数
        int maxDepth = 0;     // 最大嵌套层数
        size_t consumed = 0;  // 已消费字节

        // ---- 运行期记录流（官方解释器 FUN_004C4BC0 的位布局）----
        int records = 0;        // 解码出的记录数（== codeWords）
        int badKinds = 0;       // 非法记录种类（>= 0x3D，官方校验器直接拒绝）
        int kindsUsed = 0;      // 实际出现过的记录种类数
        int jumps = 0;          // 跳转类记录数
        int jumpsResolved = 0;  // 落点落在合法指令边界上的跳转数
        int kindCount[64] = {}; // 记录种类直方图
    };

    // 官方 15 字节头："=aardio" 10 00 01 04 04 04 08 00
    bool IsAardioChunk(const std::string& data);

    // 解析官方 "=aardio" 预编译块容器。成功返回 true 并填写 out。
    bool ParseAardioChunk(const std::string& data, AardioChunkStats* out, wstring* err);

    // 给用户看的诊断文案：说明「结构已读懂，但字节码解释器尚未实现」，
    // 并附上按官方运行期布局解码出来的记录流结论。
    wstring AardioChunkDiagnostic(const AardioChunkStats& st);

    // ------------------------------------------------------------------
    // 运行期记录（32 位 RISC 记录流）
    //
    //   bits  0.. 5 : kind     (6 bit)   0x00..0x3C
    //   bits  6..13 : A        (8 bit)   目标局部槽
    //   bits 14..22 : C        (9 bit)   操作数①
    //   bits 23..31 : B        (9 bit)   操作数②
    //
    // **9 位操作数（C / B）的寻址由第 9 位（bit22 / bit31）决定**
    // （实测自官方校验器 FUN_004D4450 调用的操作数检查器 FUN_004D4400）：
    //
    //   bit8 = 0 → **局部槽下标**，必须 < 槽位数（chunk 帧 +0x4B）
    //   bit8 = 1 → **常量表下标**，必须 < 常量数（chunk 帧 +0x28）
    //
    // **18 位操作数（bits 14..31 合成）** 出现在描述符类别为 1 的记录上
    // （kind 0x00 / 0x05 / 0x07 / 0x0C / 0x0D / 0x34）：
    //
    //   0x00/0x05/0x07/0x0C/0x0D → 常量表下标（成员名等字符串常量）
    //   0x34 NEW_CLOSURE          → 嵌套对象下标（闭包体）
    //
    // 跳转类记录把 bits 14..31 当成一个 18 位有符号相对位移，但位移取自哪条
    // 记录、偏置多少，逐 kind 不同（实测自官方解释器的取位表达式）：
    //
    //   0x22 / 0x2D / 0x2E   本条记录    0x1FFFE
    //   0x23..0x26           下一条 u32  0x1FFFE
    //   0x27 / 0x28          下一条 u32  0x1FFFD
    // ------------------------------------------------------------------
    struct AardioRecord
    {
        unsigned int word = 0;
        int kind = 0;
        int a = 0;             // 目标局部槽
        int b = 0;             // 操作数②（9 位；bit8 = 1 时是常量下标）
        int c = 0;             // 操作数①（9 位；bit8 = 1 时是常量下标）
        bool bGlobal = false;  // B 段 bit8：1 = 常量表下标，0 = 局部槽下标
        bool cGlobal = false;  // C 段 bit8：同上
        int rel = 0;           // 18 位相对位移
        int target = -1;       // 跳转落点（-1 表示不是跳转记录）
        bool relFromNext = false;
    };

    // 记录种类名（61 种，取自官方解释器的 case 表）；越界返回 L"OP??"
    const wchar_t* AardioOpcodeName(int kind);

    // 该 kind 是否使用 18 位相对位移
    bool AardioKindIsJump(int kind);

    // 解码一条记录。pc 为该记录在 code 数组中的下标，nextWord 为 pc+1 处的字
    // （跳转类记录的条件跳转从下一条取位移）。
    AardioRecord DecodeAardioRecord(unsigned int word, int pc, unsigned int nextWord);

    // 反汇编一段 code 数组（官方运行期布局）。maxLines <= 0 表示不限。
    wstring DisassembleAardioCode(const std::vector<unsigned int>& code, int maxLines);

    // 整棵 chunk 树的反汇编（带层级与 chunk 名）。
    //
    // 常量表（chunk 帧 +0x08）每项 16 字节：值 8 字节 @+0、类型标签 @+8。
    // 官方类型标签：0 null / 1 bool / 3 number / 4 string / 5 命名空间 /
    // 6 函数 / 7 表。
    struct AardioConstValue
    {
        int type = 0;          // 0 null / 1 bool / 3 number / 4 string
        bool b = false;
        double num = 0;
        wstring str;
    };

    struct AardioChunkNode
    {
        wstring name;
        std::vector<unsigned int> code;
        std::vector<AardioChunkNode> children;

        // 运行期帧字段（第十三轮补全）——
        //   attr1 / attr2：源码首行 / 末行（实测：随嵌套顺序单调递增、不重叠）
        //   flags[0]：upvalue 数量上限（chunk 帧 +0x48）
        //   flags[1]：保留（+0x49）
        //   flags[2]：kind 0x3C 的许可位（+0x4A）
        //   flags[3]：**局部槽数**（+0x4B；校验器要求 目标槽 A < 它）
        //   flags[4]：保留（+0x4C）
        int attr1 = 0;
        int attr2 = 0;
        unsigned char flags[5] = { 0, 0, 0, 0, 0 };
        std::vector<AardioConstValue> consts;   // 常量池（含载荷，供记录 VM 使用）
    };

    // 解析并保留整棵 chunk 树（用于反汇编）。
    bool ParseAardioChunkTree(const std::string& data, AardioChunkNode* root,
        AardioChunkStats* out, wstring* err);

    // 反汇编整棵树。
    wstring DisassembleAardioTree(const AardioChunkNode& root, int maxLines);

    // ------------------------------------------------------------------
    // S-1b：官方校验器 FUN_004D4450 的等价实现
    //
    // 官方 aardio 能执行自己的预编译块，但**不能校验、不能诊断**——校验器
    // 只在内部跑一次，失败就报 "in precompiled chunk" 然后终止。
    // Nodio 把同一套规则做成产品能力：给出结构化、可定位到对象与记录号的
    // 违规清单（官方做不到，这是「兼容」之上的超越项）。
    //
    // 规则全部实测自官方三处代码：
    //   * 描述符表 DAT_007C80A0（每 kind 1 字节）
    //   * 操作数检查器 FUN_004D4400（9 位操作数的两种寻址）
    //   * 校验器 FUN_004D4450（逐条记录的结构约束）
    // ------------------------------------------------------------------
    struct AardioValidateResult
    {
        int objects = 0;        // 检查过的 chunk 对象数
        int records = 0;        // 检查过的记录数
        int errors = 0;         // 违规数
        int firstErrorObject = -1;
        int firstErrorPc = -1;
        wstring firstError;
    };

    // 校验整棵 chunk 树；report 非空时写入人类可读的违规清单。
    bool ValidateAardioChunkTree(const AardioChunkNode& root,
        AardioValidateResult* out, wstring* report);

    // 「结构校验」一节的文本（-disasm 输出的一节）。
    wstring AardioValidateSection(const AardioChunkNode& root,
        const AardioValidateResult& res);

    // ------------------------------------------------------------------
    // S-6：61 种记录的「实现状态」与缺口清单
    //
    // 官方 aardio 能执行自己的预编译块，Nodio 目前只能**解释**它（反汇编 +
    // 结构化诊断）。这张表把「还差什么」量化到记录种类：名字、操作数角色、
    // 运行期语义都实测自官方解释器 FUN_004C4BC0 的 61 个 case，state 标记
    // Nodio 侧是否已经能把该记录翻译成 PNode 并执行。
    //
    // 操作数角色的通用约定（实测自解释器自身的取位表达式）：
    //   A  = bits  6..13（8 bit） → 目标槽（局部槽文件里的下标）
    //   C  = bits 14..22（9 bit） → 操作数①；bit22 = 1 时表示「外层作用域」，
    //        此时只有 bits 14..21（8 bit）是下标，基址换成外层槽数组
    //   B  = bits 23..31（9 bit） → 操作数②；bit31 = 1 同样是外层作用域
    //   R18= bits 14..31 合成     → 跳转类的 18 位相对位移
    // ------------------------------------------------------------------
    enum AardioImplState
    {
        AardioNotImplemented = 0,   // 还不能翻译成 PNode / 不能执行
        AardioImplemented = 1       // 已实现
    };

    struct AardioKindInfo
    {
        int kind = 0;
        const wchar_t* name = L"";       // 官方 case 名
        const wchar_t* operands = L"";   // 操作数角色
        const wchar_t* effect = L"";     // 运行期语义
        AardioImplState state = AardioNotImplemented;
    };

    // 61 种记录的规格表（顺序 = kind 0x00..0x3C）
    const std::vector<AardioKindInfo>& AardioKindTable();
    // 取一种记录的规格；越界返回 nullptr
    const AardioKindInfo* AardioKindInfoOf(int kind);
    // 已实现的记录种类数（当前 S-1b 尚未落地 → 0）
    int AardioImplementedKindCount();
    // 「未实现记录种类」一节的文本（-disasm 输出的一节）
    wstring AardioUnimplementedSection();
}
