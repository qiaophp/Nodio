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
//   · 逐条落地 61 种记录中可独立实现的那一部分
//
// 未实现的记录种类会**明确报错**（种类 + 记录号），绝不静默跳过。
#pragma once

#include "common.h"
#include "aardio_chunk.h"

namespace NodioVM
{
    // 一次执行的结论
    struct AardioRunResult
    {
        bool ok = false;
        Value value;          // RET 返回的值
        wstring err;          // 失败原因
        int pc = -1;          // 失败时的记录号（chunk 内）
        int kind = -1;        // 失败时的记录种类
        int steps = 0;        // 实际执行的记录条数
    };

    // 执行一个官方预编译块的记录流。
    // 目前支持：取值 / 算术 / 位运算 / 比较 / 分支 / 返回 / 成员访问 / 数组构造；
    // 闭包调用、原生函数、导入成员等仍会明确报「尚未实现」。
    AardioRunResult RunAardioChunk(const AardioChunkNode& root);

    // 记录 VM 已能执行的记录种类数（0..61），与 -disasm 的「可执行 N/61」一致
    int AardioVmImplementedKindCount();
}
