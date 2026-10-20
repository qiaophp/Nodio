// aardio_vm.h — 官方 "=aardio" 预编译块的**记录 VM**（S-1b 第二阶段）。
//
// 官方运行期并不存在 AST：`=aardio` 预编译块就是「32 位 RISC 记录流 +
// 一个寄存器机解释器」（aardio.exe 的 FUN_004C4BC0，7263 字节，61 个 case）。
// 因此 Nodio 要执行官方预编译块，**不需要**把记录流反推成树，
// 而是照官方做一个记录 VM（见 docs/aardio-bytecode.md §7.5.2 的路线结论）。
//
// 本文件提供该 VM 的最小可执行内核：
//   · 16 字节槽（值 + 官方类型标签 0/1/3/4/5/6/7）
//   · 每 chunk 一份常量表（官方 chunk 帧 +0x08，16 字节/项）
//   · 官方操作数寻址：9 位操作数的 bit8 = 常量表标志；类别 1 的记录用
//     18 位常量下标（见 docs/aardio-bytecode.md §7.5.2）
//   · 官方操作数角色：左操作数在 bits 23..31（B）、右操作数在 bits 14..22（C）
//     （见 docs/aardio-bytecode.md §7.5.4）
//   · **调用帧与闭包**（第十六轮）：被调值 = 槽 A，实参窗口 = 槽 A+1..A+B-1，
//     返回槽 = A+B；闭包捕获条数 = 嵌套对象 flags[0]，上值按引用
//     （见 docs/aardio-bytecode.md §7.5.5）
//   · **命名空间与导入**（第十七轮）：0x15 = 新建空命名空间表，
//     0x16 = 按点号名导入（名字在 B、目标槽在 C），导入后模块命名空间切换；
//     VM 暂存槽 vm+0x40 = **全局命名空间**
//     （见 docs/aardio-bytecode.md §7.5.6）
//   · **字符串拼接与 for 循环**（第十八轮）：0x21 = 槽 B..C 升序拼接，
//     0x2D = 循环推进（越界则落空），0x2E = 槽 A -= 槽 A+2 后无条件跳转
//     （见 docs/aardio-bytecode.md §7.5.7）
//   · 逐条落地 61 种记录中可独立实现的那一部分（当前 57/61）
//
// 未实现的记录种类会**明确报错**（种类 + 记录号），绝不静默跳过。
#pragma once

#include "common.h"
#include "aardio_chunk.h"

namespace NodioVM
{
    // ------------------------------------------------------------------
    // 阶段 F：宿主接线
    //
    // 官方预编译块不是孤立的字节码：它在运行期要访问**全局命名空间**
    // （官方 DAT_007C89B0，解释器里的 vm+0x40），并且会调用全局内建
    // （`import` 等）与官方标准库模块。Nodio 要执行官方块，就得把这三样
    // 交给记录 VM：
    //
    //   · globalNs     全局命名空间（0x07 GET_MEMBER2 / 0x0D SET_MEMBER2
    //                   的对象就是它；所有 `import` 进来的模块也登记在这里）
    //   · moduleNs     模块自己的命名空间（官方 0x15 建的表、父表 = globalNs；
    //                   官方里模块的命名空间是「加载器建好后交给块」的）
    //   · importModule 0x16 IMPORT_MEMBER / 全局 import 内建要用的模块加载器
    // ------------------------------------------------------------------
    struct AardioHost
    {
        // 全局命名空间：为 null 时 VM 自建一张空表（离线反汇编/自测用）
        std::shared_ptr<Table> globalNs;
        // 模块命名空间：为 null 时 VM 自建一张空表（父表 = globalNs）
        std::shared_ptr<Table> moduleNs;
        // 按点号路径加载官方模块（如 L"ide.aifix"）；成功返回命名空间
        std::function<bool(const wstring& name, std::shared_ptr<Table>* out)> importModule;
        // 调用宿主（普通 aardio 源码）里的函数。官方块会调用官方库里的
        // .aardio 源函数（如 string.reduceReplace），记录 VM 必须能回调
        // 宿主解释器执行它们。
        std::function<bool(const Value& fn, std::vector<Value>& args,
            Value* ret, wstring* err)> callHostFunc;
    };

    // 一次执行的结论
    struct AardioRunResult
    {
        bool ok = false;
        Value value;          // RET 返回的值
        wstring err;          // 失败原因
        int pc = -1;          // 失败时的记录号（chunk 内）
        int kind = -1;        // 失败时的记录种类
        int steps = 0;        // 实际执行的记录条数
        std::shared_ptr<Table> ns;   // 模块命名空间（执行后的根帧 ns）
    };

    // ------------------------------------------------------------------
    // 官方块模块的**运行期句柄**
    //
    // 官方块执行完之后并不就此结束：块里定义的函数（闭包）要能被宿主
    // （Nodio 源码、其它官方块）继续调用。为此需要三样东西活得比执行过程久：
    //
    //   · chunk 树      —— 闭包体是指向 chunk 节点的裸指针，树必须一直在
    //   · 运行期状态    —— 闭包池（闭包对象本体）+ 全局/模块命名空间
    //   · 导入回调      —— 块里的 `import` 要继续能加载官方库模块
    // ------------------------------------------------------------------
    struct AardioChunkModule
    {
        std::shared_ptr<AardioChunkNode> root;       // chunk 树（闭包体的有效性靠它）
        std::shared_ptr<Table> globalNs;             // 全局命名空间（官方 DAT_007C89B0）
        std::shared_ptr<Table> moduleNs;             // 模块命名空间
        std::shared_ptr<void> runtime;               // 记录 VM 的闭包池（内部类型）
        std::function<bool(const wstring&, std::shared_ptr<Table>*)> importModule;
        std::function<bool(const Value&, std::vector<Value>&, Value*, wstring*)> callHostFunc;
    };

    // 执行一个官方预编译块的记录流。
    // 目前支持：取值 / 算术 / 位运算 / 比较 / 分支 / 返回 / 成员访问 / 数组构造 /
    // 闭包与调用帧 / 命名空间与导入；字符串拼接、复合赋值、多值调用等
    // 仍会明确报「尚未实现」。
    // host 可为 null（离线执行：全局命名空间为空、模块加载不可用）。
    // mod 非空时，执行状态（闭包池等）会写进 mod，供后续宿主调用块内函数。
    AardioRunResult RunAardioChunk(const AardioChunkNode& root,
        const AardioHost* host = nullptr, AardioChunkModule* mod = nullptr);

    // 调用官方块里定义的函数（闭包）。callee 是 Value::Pointer（闭包对象）。
    bool CallAardioClosure(AardioChunkModule& mod, const Value& callee,
        std::vector<Value>& args, Value* ret, wstring* err);

    // 把官方块里定义的函数包装成宿主可调用的原生函数（Value::Func）。
    Value MakeAardioClosureFn(AardioChunkModule& mod, const Value& callee);

    // 该值是否是本模块闭包池里的函数（Value::Pointer）。
    bool AardioIsClosure(const AardioChunkModule& mod, const Value& v);

    // 记录 VM 已能执行的记录种类数（0..61），与 -disasm 的「可执行 N/61」一致
    int AardioVmImplementedKindCount();
}
