// aardio_vm.cpp — 官方 "=aardio" 预编译块的记录 VM 内核。
//
// 语义来源（全部实测自 aardio.exe，见 docs/aardio-bytecode.md §7）：
//   · 记录字位布局            §7.2
//   · 16 字节槽与类型标签      §7.3
//   · 调用帧与调用约定         §7.4
//   · 61 种记录语义            §7.5
//   · 操作数寻址（描述符表）   §7.5.2
//   · 操作数角色（A/B/C）      §7.5.4
//   · 调用帧与闭包            §7.5.5
//
// 本文件实现「不依赖原生函数 / 导入成员」的一组记录：
// 取值（LOAD_* / CLEAR_LOCALS）、算术（0x18-0x1D）、一元负号、真值、长度、
// 位运算（0x35-0x3B）、跳转与条件跳转（0x22-0x28）、返回（0x2C）、GC（0x33），
// 第十五轮新增的**成员访问族**与**数组构造**
// （0x05/0x06/0x07/0x08/0x09/0x0A/0x0B/0x0C/0x0D/0x10/0x11/0x12/0x13/0x14/0x17、
//  0x32），以及第十六轮的**调用帧与闭包**：
//   0x0E STORE_UPVALUE / 0x0F LOAD_IF_SET / 0x29 TAILCALL / 0x2A CALL /
//   0x2B CALL_NATIVE / 0x34 NEW_CLOSURE，以及第十七轮的**命名空间与导入**：
//   0x15 NEW_NATIVE / 0x16 IMPORT_MEMBER，
//   第十八轮的**拼接与循环**：0x21 CONCAT / 0x2D ADDEQ / 0x2E SUBEQ，
//   第二十轮的**动态调用族**：0x2F CALL_MULTI / 0x30 CALL_VALUE /
//   0x31 CALL_NS / 0x3C CALL_N（至此 61/61 全部落地）
// 其余种类一律明确报错（种类 + 记录号），不静默跳过。
#include "aardio_vm.h"
#include "engine/interp.h"   // AardioMod —— 官方取模语义

namespace NodioVM
{
    namespace
    {
        struct RtCell;      // 上值盒（官方 FUN_004D5210 的 cell，按引用）
        struct RtClosure;   // 闭包对象（官方 type 6）

        // 官方 16 字节槽：值 8 字节 @+0 + 类型标签 4 字节 @+8
        struct RtSlot
        {
            int type = 0;       // 0 null / 1 bool / 3 number / 4 string / 5 table / 6 func / 7 class
            Value val;
            // type 6（闭包）：闭包体 + 上值数组
            std::shared_ptr<RtClosure> closure;
            // 上值别名：真实值存放在 box->value（官方 cell 指向定义帧的局部槽）
            std::shared_ptr<RtCell> box;
        };

        // 上值盒：官方 FUN_004D5210 —— 指向定义帧的局部槽，**按引用**
        struct RtCell
        {
            RtSlot value;
        };

        // 闭包对象（官方 FUN_004D50A0，type 6）
        //   +0x07 上值个数 / +0x10 属主命名空间 / +0x14 闭包体 chunk / +0x18 上值数组
        struct RtClosure
        {
            const AardioChunkNode* chunk = nullptr;              // 闭包体（嵌套对象）
            std::vector<std::shared_ptr<RtCell>> upvalues;        // 上值数组
            std::shared_ptr<Table> ownerNs;                       // 属主命名空间
        };

        // 官方块模块的运行期状态（闭包池）—— 活得比一次执行更久，
        // 使块里定义的函数在执行结束后仍然可以被宿主调用。
        struct RtRuntime
        {
            std::vector<std::shared_ptr<RtClosure>> arena;
        };

        struct RtFrame
        {
            std::vector<RtSlot> slots;
            const AardioChunkNode* chunk = nullptr;
            // 本作用域的上值数组（官方 iVar11+0x18）
            std::vector<std::shared_ptr<RtCell>> upvalues;
            RtFrame* parent = nullptr;
            size_t ip = 0;
            // 模块命名空间（官方 iVar11+0x10）：0x05/0x0C 的对象，0x06 的取值
            std::shared_ptr<Table> ns;
            // 返回值写回**调用方**帧的槽（官方 CALL 的 A 决定：返回槽 = A）
            int retSlot = -1;
            // 调用方期望的返回值个数（官方 0x2A 装参块 0x004C5A44：`PUSH C-1`；
            // 官方 0x2F 块 0x004C5BA2：`PUSH C`）
            int retCount = 1;
            // 被调方返回后，调用方还要做的事（官方 0x2F 的尾处理）：
            //   把调用方帧的 postCopySrc 槽拷到 postCopyDst 槽，并把调用方 ip 设为 postJump。
            // 官方 0x2F：调用返回后若槽 A+3 非空 → 槽 A-1 := 槽 A+3，并按下一条 0x22 跳转。
            int postCopySrc = -1;
            int postCopyDst = -1;
            long long postJump = -1;
            // 官方 0x2F：调用结果为**空**时**跳过**后继的那条 0x22（官方 0x004C5C41：
            // `ADD dword ptr [ESP+0x38],0x4` —— ip 落到 0x22 之后），
            // 即「循环结束」走这条；结果非空才按 postJump 跳回循环头。
            int postSkipIp = -1;
            // 0x31 表迭代的游标：槽号 → 下一个成员的位置
            // （官方用 VM 级游标 FUN_004BFFD0，Nodio 用帧内游标，语义等价）
            std::map<int, int> iterCur;
        };

        // 官方类型标签（实测自解释器 FUN_004C4BC0 的槽类型判定）
        //   0 null / 1 bool / 3 number / 4 string / 5 table / 6 func / 7 class
        int TagOfValue(const Value& v)
        {
            switch (v.t)
            {
            case Value::Null:   return 0;
            case Value::Bool:   return 1;
            case Value::Num:    return 3;
            case Value::Str:    return 4;
            case Value::Func:   return 6;
            case Value::TableT: return 5;
            case Value::Class:  return 7;
            default:            return 0;
            }
        }

        RtSlot SlotOf(const Value& v)
        {
            RtSlot s;
            s.type = TagOfValue(v);
            s.val = v;
            return s;
        }

        // 上值别名：读穿透 box，写也穿透 box（官方 cell 按引用）
        RtSlot SlotGet(const RtSlot& s)
        {
            return s.box ? s.box->value : s;
        }

        void SlotPut(RtSlot& s, const RtSlot& v)
        {
            if (s.box) s.box->value = v;
            else s = v;
        }

        // 官方记录 VM 的真值判定（case 0x1F / 0x27 / 0x28）：
        //   只有 null、false、数值 0 算「假」；空串、空表都算「真」。
        bool TruthyOf(const Value& v)
        {
            switch (v.t)
            {
            case Value::Null:  return false;
            case Value::Bool:  return v.b;
            case Value::Num:   return v.n != 0;
            default:           return true;
            }
        }

        double NumOf(const Value& v)
        {
            switch (v.t)
            {
            case Value::Bool: return v.b ? 1 : 0;
            case Value::Num:  return v.n;
            case Value::Str:  return wcstod(v.s.c_str(), nullptr);
            default:          return 0;
            }
        }

        long long IntOf(const Value& v)
        {
            if (v.t == Value::Bool) return v.b ? 1 : 0;
            if (v.t == Value::Num) return (long long)v.n;
            if (v.t == Value::Str) return _wtoi64(v.s.c_str());
            return 0;
        }

        // 数字 → 文本（记录 VM 内部用；官方走各自的 __tostring 链）
        wstring NumText(double v)
        {
            wchar_t buf[64];
            swprintf_s(buf, L"%.15g", v);
            return buf;
        }

        // 官方 FUN_004C4410（变参串拼接）把数值按 "%.14g" 转成文本
        wstring NumText14(double v)
        {
            wchar_t buf[64];
            swprintf_s(buf, L"%.14g", v);
            return buf;
        }

        // 槽 → 数值：官方要求「数值或可转数值的字符串」，否则报错
        bool AsNumber(const RtSlot& s, double* out)
        {
            switch (s.val.t)
            {
            case Value::Num:  *out = s.val.n; return true;
            case Value::Bool: *out = s.val.b ? 1 : 0; return true;
            case Value::Str:
            {
                wchar_t* end = nullptr;
                double d = wcstod(s.val.s.c_str(), &end);
                if (end == s.val.s.c_str()) return false;
                *out = d;
                return true;
            }
            default:
                return false;
            }
        }

        int CompareOf(const Value& a, const Value& b)
        {
            if (a.t == Value::Str || b.t == Value::Str)
            {
                wstring x = (a.t == Value::Str) ? a.s : NumText(NumOf(a));
                wstring y = (b.t == Value::Str) ? b.s : NumText(NumOf(b));
                return x.compare(y) < 0 ? -1 : (x == y ? 0 : 1);
            }
            double x = NumOf(a), b2 = NumOf(b);
            return x < b2 ? -1 : (x == b2 ? 0 : 1);
        }

        // ---- 官方表（table）语义 ----
        //
        // 官方表的两个部分：哈希部分（字符串键）+ 数组部分（整数键，1 基）。
        // 查表走 FUN_004C0B40：整数键 → FUN_004C0A60（数组部分），
        // 字符串键 → FUN_004C0B00（哈希部分）；命中但值为 null 时
        // 继续回落到父表（表 +0x08）。
        struct RtKey
        {
            bool isIndex = false;   // true → 数组部分下标（1 基）
            long long index = 0;
            wstring name;           // false → 哈希部分的键
        };

        RtKey KeyOfValue(const Value& v)
        {
            RtKey k;
            if (v.t == Value::Num)
            {
                long long i = (long long)v.n;
                if ((double)i == v.n) { k.isIndex = true; k.index = i; return k; }
                k.name = NumText(v.n);     // 非整数按文本键处理
                return k;
            }
            if (v.t == Value::Bool) { k.isIndex = true; k.index = v.b ? 1 : 0; return k; }
            if (v.t == Value::Str) { k.name = v.s; return k; }
            k.name = v.ToString();
            return k;
        }

        // 沿父表链查找；命中且非 null 才返回
        bool RtTableFind(const std::shared_ptr<Table>& t, const RtKey& k, Value* out)
        {
            for (std::shared_ptr<Table> cur = t; cur; cur = cur->parent)
            {
                if (k.isIndex)
                {
                    if (k.index >= 1 && (size_t)k.index <= cur->arr.size())
                    {
                        *out = cur->arr[(size_t)(k.index - 1)];
                        return true;
                    }
                }
                else
                {
                    auto it = cur->map.find(k.name);
                    if (it != cur->map.end())
                    {
                        if (it->second.t != Value::Null) { *out = it->second; return true; }
                        // 命中但为 null → 官方继续找父表
                    }
                }
            }
            return false;
        }

        // 只在本表里找（用于 CONCAT_MEMBER 的「就地改写」）
        Value* RtTableFindLocal(Table* t, const RtKey& k)
        {
            if (!t) return nullptr;
            if (k.isIndex)
            {
                if (k.index >= 1 && (size_t)k.index <= t->arr.size())
                    return &t->arr[(size_t)(k.index - 1)];
                return nullptr;
            }
            auto it = t->map.find(k.name);
            if (it == t->map.end()) return nullptr;
            return &it->second;
        }

        void RtTableSet(const std::shared_ptr<Table>& t, const RtKey& k, const Value& v)
        {
            if (!t) return;
            if (k.isIndex)
            {
                if (k.index < 1) return;
                if (t->arr.size() < (size_t)k.index)
                    t->arr.resize((size_t)k.index);
                t->arr[(size_t)(k.index - 1)] = v;
                return;
            }
            t->map[k.name] = v;
        }

        // 取成员（官方 FUN_004C30A0 / 0x0B 内联路径）
        //   表   → 按键查（含父表回退）
        //   字符串 + 数字键 → 1 基字符码，结果是**数字**
        //   其它 → 取不到
        bool RtGetMember(const Value& obj, const Value& key, Value* out)
        {
            if (obj.t == Value::TableT && obj.tab)
                return RtTableFind(obj.tab, KeyOfValue(key), out);
            if (obj.t == Value::Str && key.t == Value::Num)
            {
                long long i = (long long)key.n;
                if (i >= 1 && (size_t)i <= obj.s.size())
                    *out = Value::Num_((double)(unsigned short)obj.s[(size_t)(i - 1)]);
                else
                    *out = Value();
                return true;
            }
            return false;
        }

        const int kMaxSteps = 20000000;   // 防止死循环（官方有 200 层调用限制）
    }

    // 已在记录 VM 里落地的记录种类（与 kKindTable 的 state 保持一致）
    static bool VmKindImplemented(int kind)
    {
        switch (kind)
        {
        case 0x00: case 0x01: case 0x02: case 0x03: case 0x04:
        case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0A:
        case 0x0B: case 0x0C: case 0x0D: case 0x0E: case 0x0F:
        case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15:
        case 0x16: case 0x17:
        case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D:
        case 0x1E: case 0x1F: case 0x20: case 0x21:
        case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27: case 0x28:
        case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2E:
        case 0x2F: case 0x30: case 0x31:
        case 0x32: case 0x33: case 0x34:
        case 0x35: case 0x36: case 0x37: case 0x38: case 0x39: case 0x3A: case 0x3B:
        case 0x3C:
            return true;
        default:
            return false;
        }
    }

    int AardioVmImplementedKindCount()
    {
        int n = 0;
        for (int k = 0; k < 61; k++)
            if (VmKindImplemented(k)) n++;
        return n;
    }

    // 执行内核。entryCallee 非空时：以「调用这个闭包」为入口，
    // 而不是执行根 chunk（宿主调用块内函数时走这条路）。
    static AardioRunResult RunAardioChunkInner(const AardioChunkNode& root,
        const AardioHost* host, AardioChunkModule* mod,
        const Value* entryCallee, const std::vector<Value>* entryArgs)
    {
        AardioRunResult res;

        // ---- 帧栈（官方是「就地压栈」，Nodio 用每帧独立槽数组，语义等价）----
        std::vector<RtFrame> frames;
        // 闭包保活区：闭包以 Value::Pointer 存进表 / 槽，指针由这里兜底。
        // 官方块模块的闭包池跨执行存活（宿主后续还要调块里的函数）。
        std::shared_ptr<RtRuntime> rt;
        if (mod && mod->runtime)
            rt = std::static_pointer_cast<RtRuntime>(mod->runtime);
        else
            rt = std::make_shared<RtRuntime>();
        if (mod) mod->runtime = rt;
        std::vector<std::shared_ptr<RtClosure>>& arena = rt->arena;
        // VM 暂存槽（官方 vm+0x40）：0x07/0x0D 的对象
        RtSlot scratch;

        // 全局命名空间（官方 DAT_007C89B0 的运行期表）：
        //   · VM 暂存槽 vm+0x40 指向它 —— 0x07 GET_MEMBER2 / 0x0D SET_MEMBER2
        //     的对象就是它（实测：官方样本根 chunk 的第一条 GET_MEMBER2
        //     从 vm+0x40 取到的是全局函数）；
        //   · 模块命名空间与 0x15 建出的命名空间，父表都指向它
        //     （官方 FUN_004C0810 建表时写死 puVar3[4] = &DAT_007C89B0）。
        // 阶段 F：宿主可以把 Nodio 的全局命名空间交给记录 VM（官方块要靠它
        // 找 `import` 与已导入的标准库模块）。
        std::shared_ptr<Table> globalNs;
        if (host && host->globalNs) globalNs = host->globalNs;
        else globalNs = std::make_shared<Table>();

        // 记录 VM 的全局/模块命名空间必须对 GC 可达：
        // Nodio 的循环回收器会**清空不可达表的内容**，而这两张表只被
        // C++ 局部变量持有（官方块运行期间会调用 import，import 会执行
        // 官方库源码，源码执行到语句边界就会触发一次回收）。
        GcRootScope gcKeep;
        gcKeep.add(Value::Table_(globalNs));
        if (host && host->moduleNs) gcKeep.add(Value::Table_(host->moduleNs));

        scratch = SlotOf(Value::Table_(globalNs));

        auto Cur = [&]() -> RtFrame& { return frames.back(); };

        auto Fail = [&](int pc, int kind, const wstring& msg) -> AardioRunResult
        {
            AardioRunResult r;
            r.ok = false;
            r.err = msg;
            r.pc = pc;
            r.kind = kind;
            r.steps = res.steps;
            return r;
        };

        // 值 → 槽：闭包以 Value::Pointer 存放，这里还原成 type 6
        auto SlotOfValue = [&](const Value& v) -> RtSlot
        {
            if (v.t == Value::Pointer)
            {
                for (size_t i = 0; i < arena.size(); i++)
                {
                    if (arena[i].get() == v.ptr)
                    {
                        RtSlot s;
                        s.type = 6;
                        s.val = v;
                        s.closure = arena[i];
                        return s;
                    }
                }
            }
            return SlotOf(v);
        };

        // Value::Pointer（闭包）→ 闭包对象；不是闭包则返回空
        auto ClosureOfValue = [&](const Value& v) -> std::shared_ptr<RtClosure>
        {
            if (v.t != Value::Pointer) return nullptr;
            for (size_t i = 0; i < arena.size(); i++)
                if (arena[i].get() == v.ptr) return arena[i];
            return nullptr;
        };

        // ---- 调用辅助（0x29 / 0x2A / 0x2B / 0x2F / 0x30 / 0x31 / 0x3C 共用）----
        //
        // 官方调用约定（实测自 FUN_004D3040、0x2A 装参块 0x004C5A44、
        // 官方校验器 FUN_004D4450 与官方样本 ide/aifix.aardio 的调用点）：
        //
        //   · **调用区 = 槽 A+1 .. A+B-1**（共 B-1 个槽，B=1 时无实参）
        //   · **调用区的第 1 个槽（A+1）是 owner**：
        //       - 0x17 METHOD_PREP 把被调对象（接收者）复制到 A+1；
        //       - 普通调用则由编译器显式放入（如 LOAD_SELF_NS 放模块命名空间）
        //     owner 不属于实参：它对应 aardio 的 `owner`（隐含的首参数）。
        //   · **实参 = 槽 A+2 .. A+B-1**，实参个数 = **B-2**
        //   · 被调帧：**槽 0 = owner，槽 1.. = 实参**，
        //     声明的形参从槽 1 开始（官方 chunk flags[1] = 形参数 + 1，
        //     官方样本 ide/aifix 的函数体 flags[1]=4 → owner + 3 个形参）。
        //   · 返回值写回调用方槽 A 起（个数 = C-1，C=0 为变参返回）。
        //
        // 返回：0 = 已同步完成（结果已写回 retSlot）；1 = 已压入被调帧；
        //       -1 = 出错（原因在 callErr）。
        wstring callErr;
        // 把返回值写回调用方帧：第 1 个写 retSlot，额外的写 retSlot+1..，
        // 不足的补 null（官方 FUN_004D3330 弹帧时的补零行为）。
        auto WriteReturns = [&](RtFrame& caller, int retSlot, int retCount,
                                const Value& out)
        {
            if (retSlot < 0) return;
            int n = caller.slots.size();
            int k = 0;
            if (retCount >= 1 && retSlot < n)
                SlotPut(caller.slots[(size_t)retSlot], SlotOfValue(out));
            k = 1;
            if (out.mrets && retCount > 1)
            {
                for (const Value& v : *out.mrets)
                {
                    if (k >= retCount) break;
                    if (retSlot + k < n) SlotPut(caller.slots[(size_t)(retSlot + k)], SlotOfValue(v));
                    k++;
                }
            }
            for (; k < retCount; k++)
            {
                if (retSlot + k < n) SlotPut(caller.slots[(size_t)(retSlot + k)], RtSlot());
            }
        };
        auto DoCall = [&](const RtSlot& fn, const RtSlot& owner,
                          const std::vector<RtSlot>& args,
                          int retSlot, int postCopySrc, int postCopyDst,
                          long long postJump, int retCount = 1) -> int
        {
            if (fn.closure)
            {
                RtFrame nf;
                nf.chunk = fn.closure->chunk;
                if (!nf.chunk) { callErr = L"调用的闭包没有函数体"; return -1; }
                int need = (int)args.size() + 2;      // 帧槽 0 = owner，槽 1.. = 实参
                int nSlots = (int)nf.chunk->flags[3];
                if (nSlots < need) nSlots = need;
                nf.slots.assign((size_t)nSlots, RtSlot());
                nf.slots[0] = owner;                 // owner（隐含首参数）
                for (size_t k = 0; k < args.size(); k++)
                {
                    // 实参按值拷贝（官方：整段复制）；box 穿透一次拷贝到位
                    const RtSlot& rs = args[k];
                    nf.slots[k + 1] = rs.box ? rs.box->value : rs;
                }
                nf.upvalues = fn.closure->upvalues;
                nf.ns = fn.closure->ownerNs ? fn.closure->ownerNs : frames.back().ns;
                nf.parent = nullptr;                  // 帧栈会重排，不留悬挂父指针
                nf.retSlot = retSlot;
                nf.retCount = retCount;
                nf.postCopySrc = postCopySrc;
                nf.postCopyDst = postCopyDst;
                nf.postJump = postJump;
                nf.ip = 0;
                frames.push_back(nf);
                return 1;
            }
            if (fn.val.t == Value::Func && fn.val.native)
            {
                std::vector<Value> a;
                a.reserve(args.size());
                for (size_t k = 0; k < args.size(); k++)
                {
                    // one Value copy per arg (SlotGet's by-value RtSlot
                    // copy was doubling string traffic on every call)
                    const RtSlot& rs = args[k];
                    a.push_back(rs.box ? rs.box->value.val : rs.val);
                }
                Value out = fn.val.native(fn.val.nativeCtx.get(), a);
                WriteReturns(frames.back(), retSlot, retCount, out);
                return 0;
            }
            if (fn.val.t == Value::Func && host && host->callHostFunc)
            {
                // Nodio 源码函数（官方库里的 .aardio 源函数）：回调宿主解释器
                std::vector<Value> a;
                a.reserve(args.size());
                for (size_t k = 0; k < args.size(); k++)
                {
                    // one Value copy per arg (SlotGet's by-value RtSlot
                    // copy was doubling string traffic on every call)
                    const RtSlot& rs = args[k];
                    a.push_back(rs.box ? rs.box->value.val : rs.val);
                }
                Value out;
                wstring herr;
                if (!host->callHostFunc(fn.val, a, &out, &herr))
                {
                    callErr = herr.empty() ? wstring(L"调用宿主函数失败") : herr;
                    return -1;
                }
                WriteReturns(frames.back(), retSlot, retCount, out);
                return 0;
            }
            callErr = L"调用的对象不是函数";
            return -1;
        };

        // ---- 建立入口帧 ----
        if (entryCallee && entryArgs)
        {
            // 入口是「调用一个闭包」：被调帧槽 0 = owner，槽 1.. = 实参
            std::shared_ptr<RtClosure> cl = ClosureOfValue(*entryCallee);
            if (!cl || !cl->chunk)
                return Fail(0, -1, L"调用的官方块函数已失效（闭包池不在）");
            RtFrame f;
            f.chunk = cl->chunk;
            int need = (int)entryArgs->size() + 2;
            int nSlots = (int)cl->chunk->flags[3];
            if (nSlots < need) nSlots = need;
            f.slots.assign((size_t)nSlots, RtSlot());
            f.slots[0] = SlotOf(Value::Table_(cl->ownerNs ? cl->ownerNs : globalNs));
            for (size_t k = 0; k < entryArgs->size(); k++)
                f.slots[k + 1] = SlotOfValue((*entryArgs)[k]);
            f.upvalues = cl->upvalues;
            f.ns = cl->ownerNs ? cl->ownerNs : globalNs;
            f.parent = nullptr;
            f.retSlot = 0;
            f.ip = 0;
            frames.push_back(f);
        }
        else
        {
            RtFrame f;
            f.chunk = &root;
            int nSlots = (int)root.flags[3];
            if (nSlots < 0) nSlots = 0;
            f.slots.assign((size_t)nSlots, RtSlot());
            // 模块命名空间：官方加载器先把模块的命名空间建好交给块
            // （块的 0x06 LOAD_SELF_NS / 0x05 GET_MEMBER 用的就是它）。
            if (host && host->moduleNs) f.ns = host->moduleNs;
            else f.ns = std::make_shared<Table>();
            f.ns->parent = globalNs;
            // 官方加载器把「模块自己的命名空间」放在根帧的 0 号槽：
            // 官方样本 ide/aifix.aardio 的收尾两条 IMPORT_MEMBER 读的正是它，
            // 结尾的 RET(A=0) 把它作为模块的值返回。
            if (host && host->moduleNs && !f.slots.empty())
                f.slots[0] = SlotOf(Value::Table_(f.ns));
            frames.push_back(f);
        }

        while (!frames.empty())
        {
            RtFrame& frame = Cur();
            const AardioChunkNode* chunk = frame.chunk;
            const std::vector<unsigned int>& code = chunk->code;
            const size_t n = code.size();
            const std::vector<AardioConstValue>& consts = chunk->consts;

            if (frame.ip >= n)
            {
                // 记录流跑完但没有 RET —— 视为返回 null
                if (frames.size() <= 1)
                {
                    res.ok = true;
                    res.value = Value();
                    res.ns = frames[0].ns;
                    return res;
                }
                int ret = frame.retSlot;
                frames.pop_back();
                if (ret >= 0 && ret < (int)frames.back().slots.size())
                    frames.back().slots[(size_t)ret] = RtSlot();
                continue;
            }

            if (res.steps >= kMaxSteps)
                return Fail((int)frame.ip, -1, L"执行步数超过上限（疑似死循环）");

            size_t pc = frame.ip;
            unsigned int word = code[pc];
            int kind = (int)(word & 0x3F);
            int A = (int)((word >> 6) & 0xFF);
            unsigned int C = (word >> 14) & 0x1FF;
            unsigned int B = (word >> 23) & 0x1FF;
            unsigned int K = word >> 14;              // 18 位操作数
            frame.ip = pc + 1;
            res.steps++;

            if (!VmKindImplemented(kind))
            {
                wchar_t hex[16];
                swprintf_s(hex, L"%02X", kind);
                return Fail((int)pc, kind,
                    L"记录种类 0x" + wstring(hex) + L" 尚未在记录 VM 中实现");
            }

            // 局部槽存取（穿透上值别名）
            auto LocalSlot = [&](unsigned int v) -> RtSlot
            {
                int idx = (int)(v & 0xFF);
                if (idx < 0 || idx >= (int)frame.slots.size()) return RtSlot();
                return SlotGet(frame.slots[(size_t)idx]);
            };

            auto ConstSlot = [&](int idx) -> RtSlot
            {
                RtSlot s;
                if (idx < 0 || idx >= (int)consts.size()) return s;
                const AardioConstValue& c = consts[(size_t)idx];
                s.type = c.type;
                switch (c.type)
                {
                case 1: s.val = Value::Bool_(c.b); break;
                case 3: s.val = Value::Num_(c.num); break;
                case 4: s.val = Value::Str_(c.str); break;
                default: s.val = Value(); break;
                }
                return s;
            };

            // 9 位操作数：bit8=1 → 常量表；否则 → 当前帧局部槽表
            auto Operand9 = [&](unsigned int v) -> RtSlot
            {
                if (v & 0x100) return ConstSlot((int)(v & 0xFF));
                int idx = (int)v;
                if (idx < 0 || idx >= (int)frame.slots.size()) return RtSlot();
                return SlotGet(frame.slots[(size_t)idx]);
            };

            auto SetSlot = [&](int idx, const RtSlot& s)
            {
                if (idx < 0 || idx >= (int)frame.slots.size()) return;
                SlotPut(frame.slots[(size_t)idx], s);
            };

            switch (kind)
            {
            case 0x00:   // LOAD_OUTER：18 位常量下标
            {
                SetSlot(A, ConstSlot((int)K));
                break;
            }
            case 0x01:   // LOAD_INT：B 是 9 位立即数；C≠0 时下一条 u32 是完整立即数
            {
                double v = (double)B;
                if (C != 0)
                {
                    if (pc + 1 >= n)
                        return Fail((int)pc, kind, L"LOAD_INT 缺少后继的完整立即数");
                    int imm = (int)code[pc + 1];
                    v = (double)imm;
                    frame.ip = pc + 2;
                }
                SetSlot(A, SlotOf(Value::Num_(v)));
                break;
            }
            case 0x02:   // CLEAR_LOCALS：B..A 的类型标签清成 null
            {
                int hi = (int)B, lo = A;
                if (hi < lo) std::swap(hi, lo);
                for (int i = lo; i <= hi && i < (int)frame.slots.size(); i++)
                    SlotPut(frame.slots[(size_t)i], RtSlot());
                break;
            }
            case 0x03:   // LOAD_UPVALUE：B 是上值槽
            {
                int idx = (int)(B & 0xFF);
                RtSlot s;
                if (idx >= 0 && idx < (int)frame.upvalues.size())
                {
                    const std::shared_ptr<RtCell>& cell = frame.upvalues[(size_t)idx];
                    if (cell) s = SlotGet(cell->value);
                }
                SetSlot(A, s);
                break;
            }
            case 0x04:   // LOAD_LOCAL：B 是局部槽
            {
                SetSlot(A, LocalSlot(B));
                break;
            }
            case 0x0E:   // STORE_UPVALUE：A 是源槽，B 是上值槽（按引用写入）
            {
                int idx = (int)(B & 0xFF);
                RtSlot s = LocalSlot(A);
                if (idx >= 0 && idx < (int)frame.upvalues.size())
                {
                    const std::shared_ptr<RtCell>& cell = frame.upvalues[(size_t)idx];
                    if (cell) cell->value = s;
                }
                break;
            }
            case 0x0F:   // LOAD_IF_SET：目标槽为空（null）时写入 A 槽的值
            {
                int idx = (int)(B & 0x1FF);
                if (idx >= 0 && idx < (int)frame.slots.size())
                {
                    if (frame.slots[(size_t)idx].type == 0)
                        SlotPut(frame.slots[(size_t)idx], LocalSlot(A));
                }
                break;
            }
            case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D:
            {   // ADD / SUB / MUL / DIV / MOD / POW
                // 官方：左操作数在 bits 23..31（B），右操作数在 bits 14..22（C）
                RtSlot l = Operand9(B), r = Operand9(C);
                Value out;
                if (kind == 0x18 && (l.val.t == Value::Str || r.val.t == Value::Str))
                {
                    // 官方 `+` 对字符串是拼接
                    wstring a = (l.val.t == Value::Str) ? l.val.s : NumText(NumOf(l.val));
                    wstring b = (r.val.t == Value::Str) ? r.val.s : NumText(NumOf(r.val));
                    out = Value::Str_(a + b);
                }
                else
                {
                    double x = NumOf(l.val), y = NumOf(r.val), z = 0;
                    switch (kind)
                    {
                    case 0x18: z = x + y; break;
                    case 0x19: z = x - y; break;
                    case 0x1A: z = x * y; break;
                    case 0x1B: z = (y != 0) ? x / y : 0; break;
                    case 0x1C: z = AardioMod(x, y); break;
                    case 0x1D: z = std::pow(x, y); break;
                    }
                    out = Value::Num_(z);
                }
                SetSlot(A, SlotOf(out));
                break;
            }
            case 0x1E:   // NEG
            {
                RtSlot s = Operand9(B);
                SetSlot(A, SlotOf(Value::Num_(-NumOf(s.val))));
                break;
            }
            case 0x1F:   // TRUTHY
            {
                RtSlot s = Operand9(B);
                SetSlot(A, SlotOf(Value::Bool_(TruthyOf(s.val))));
                break;
            }
            case 0x20:   // LENGTH
            {
                RtSlot s = Operand9(B);
                double len = 0;
                if (s.val.t == Value::Str) len = (double)s.val.s.size();
                else if (s.val.t == Value::TableT && s.val.tab)
                    len = (double)(s.val.tab->map.size() + s.val.tab->arr.size());
                SetSlot(A, SlotOf(Value::Num_(len)));
                break;
            }
            case 0x21:   // CONCAT：把槽 B..C 的值拼成一个字符串
            {
                // 官方 case 0x21（0x004C592F）：
                //   count = C - B + 1；FUN_004C4410(vm, count, C) 把槽 B..C
                //   的字符串**按升序**拼起来写回槽 B；随后解释器再把槽 B
                //   拷进槽 A。
                int b = (int)(B & 0x1FF);
                int c = (int)(C & 0x1FF);
                if (c < b)
                    return Fail((int)pc, kind, L"CONCAT 的操作数区间非法（C < B）");
                wstring out;
                for (int i = b; i <= c; i++)
                {
                    RtSlot s = LocalSlot((unsigned int)i);
                    switch (s.val.t)
                    {
                    case Value::Str:  out += s.val.s; break;
                    case Value::Num:  out += NumText14(s.val.n); break;
                    case Value::Bool: out += (s.val.b ? L"true" : L"false"); break;
                    case Value::Null: break;                       // 空串
                    default:
                        return Fail((int)pc, kind,
                            L"CONCAT 的参数既不是字符串也不是数值（槽 " +
                            std::to_wstring(i) + L"）");
                }
                }
                SetSlot(b, SlotOf(Value::Str_(out)));   // 官方：结果先写回槽 B
                SetSlot(A, SlotOf(Value::Str_(out)));   // 再拷进槽 A
                break;
            }
            case 0x22:   // JMP：位移取自本条
            {
                int rel = (int)(K - 0x1FFFE);
                long long t = (long long)pc + rel;
                if (t < 0 || t >= (long long)n)
                    return Fail((int)pc, kind, L"跳转落点越界");
                frame.ip = (size_t)t;
                break;
            }
            case 0x23: case 0x24: case 0x25: case 0x26: case 0x27: case 0x28:
            {   // 条件跳转：位移取自下一条 u32（官方要求下一条必须是 0x22）
                if (pc + 1 >= n)
                    return Fail((int)pc, kind, L"条件跳转缺少后继的 JMP(0x22)");
                unsigned int nxt = code[pc + 1];
                if ((nxt & 0x3F) != 0x22)
                    return Fail((int)pc, kind, L"条件跳转的后继不是 JMP(0x22)");
                unsigned int bias = (kind == 0x27 || kind == 0x28) ? 0x1FFFD : 0x1FFFE;
                int rel = (int)((nxt >> 14) - bias);
                long long t = (long long)pc + rel;
                bool jump = false;
                switch (kind)
                {
                case 0x23:   // JZ：r = (B == C)，当 r == A 时跳转
                case 0x24:   // JE：同上（官方一个是 helper、一个是内联快路径）
                {
                    RtSlot l = Operand9(B), r = Operand9(C);
                    int res = (CompareOf(l.val, r.val) == 0) ? 1 : 0;
                    jump = (res == A);
                    break;
                }
                case 0x25:   // JG：r = (C > B)，当 r == A 时跳转
                {
                    RtSlot l = Operand9(B), r = Operand9(C);
                    int res = (CompareOf(r.val, l.val) > 0) ? 1 : 0;
                    jump = (res == A);
                    break;
                }
                case 0x26:   // JLE：r = (B <= C)，当 r == A 时跳转
                {
                    RtSlot l = Operand9(B), r = Operand9(C);
                    int res = (CompareOf(l.val, r.val) <= 0) ? 1 : 0;
                    jump = (res == A);
                    break;
                }
                case 0x27:   // JTRUE：真值槽 A；官方判定「假值标志 != C」时跳转
                {           //   （官方 case 0x27：uVar6 = 1 表示值为假，再与 C 比较）
                    RtSlot l = (A >= 0 && A < (int)frame.slots.size())
                        ? SlotGet(frame.slots[(size_t)A]) : RtSlot();
                    int falsy = TruthyOf(l.val) ? 0 : 1;
                    jump = (falsy != (int)(C & 0x1FF));
                    break;
                }
                case 0x28:   // JTRUE2：真值槽 B；跳转时把该值带进槽 A
                {
                    RtSlot l = Operand9(B);
                    int falsy = TruthyOf(l.val) ? 0 : 1;
                    jump = (falsy != (int)(C & 0x1FF));
                    if (jump) SetSlot(A, l);
                    break;
                }
                }
                if (jump)
                {
                    if (t < 0 || t >= (long long)n)
                        return Fail((int)pc, kind, L"跳转落点越界");
                    frame.ip = (size_t)t;
                }
                else
                {
                    frame.ip = pc + 2;   // 跳过那条 0x22
                }
                break;
            }
            case 0x29: case 0x2A: case 0x2B:
            {   // TAILCALL / CALL / CALL_NATIVE
                //
                // 官方调用约定（四路实测：官方装参块 0x004C5A44 + 官方校验器
                // FUN_004D4450 + 官方建帧函数 FUN_004D3040 + 官方样本
                // ide/aifix.aardio 的调用点）：
                //
                //   · **被调值 = 槽 A**（官方 0x2A 块 `MOV EAX,EDI`，EDI = &slot[A]）
                //   · **调用区 = 槽 A+1 .. A+B-1**（B-1 个槽，B = bits23..31）
                //     官方校验器的槽位检查「nslots > A+B-1」正是这个窗口的末槽。
                //   · **A+1 是 owner**（隐含首参数，对应 aardio 的 `owner`）：
                //     0x17 METHOD_PREP 把接收者复制到 A+1（官方 0x004C5033 块：
                //     `mov [edi+0x10],edx` —— slot[A+1] = slot[B]）；
                //     普通调用由编译器显式放入（如官方样本 `import` 之前的
                //     LOAD_SELF_NS，把模块命名空间放进槽 2 当 owner）。
                //   · **实参 = 槽 A+2 .. A+B-1**，实参个数 = **B-2**
                //     官方建帧函数 FUN_004D3040：被调帧基址 = &slot[A+1]，
                //     即**被调帧槽 0 = owner、槽 1.. = 实参**；
                //     官方 chunk 的 flags[1] = 形参数 + 1（官方样本 ide/aifix
                //     的函数体 flags[1]=4 → owner + 3 个形参）。
                //     B=0 → 变参（实参个数由被调方决定）。
                //   · **返回值写回槽 A .. A+C-2**，返回值个数 = **C-1**（C = bits14..22）
                //     官方 0x2A 装参块 0x004C5A44：`SHR ESI,0xe; AND ESI,0x1ff; DEC ESI;
                //     PUSH ESI` —— 压栈的正是 **C-1**，即建帧时登记的返回值个数；
                //     官方 FUN_004D3330（弹帧）按这个个数把被调帧里的返回值
                //     拷到调用方槽 A 起（不足的补 null）；
                //     官方校验器的「nslots > A+C-2」正是返回窗口的末槽。
                //     C=0 → 变参返回（官方压的是 0x1FF，按被调方实际返回值个数）。
                //   · 0x29：官方尾调用把当前帧换成被调帧；结果仍写回原调用方的返回槽。
                int calleeSlot = A;
                int argCount = -1;         // -1 = 变参
                int retSlot = A;           // 返回值写回调用方槽 A
                int retCount = 1;          // 调用方期望的返回值个数
                if (kind == 0x29)
                {
                    if (B != 0) calleeSlot = A + (int)B;
                    retSlot = frame.retSlot;   // 尾调用：结果仍写回原调用方的返回槽
                    retCount = frame.retCount;
                }
                else
                {
                    if (B >= 2) argCount = (int)(B - 2);
                    // 官方压栈的是 C-1；C=0/1 时按「至少 1 个返回值」处理
                    retCount = (C >= 2) ? (int)(C - 1) : 1;
                }

                RtSlot fn = LocalSlot((unsigned int)calleeSlot);
                RtSlot owner = LocalSlot((unsigned int)(calleeSlot + 1));
                std::vector<RtSlot> args;
                if (argCount >= 0)
                {
                    for (int k = 0; k < argCount; k++)
                        args.push_back(LocalSlot((unsigned int)(calleeSlot + 2 + k)));
                }
                else
                {
                    // 变参（B=0）：官方推 -1，被调方按自己的形参个数取。
                    // 闭包按自己的 flags[1]（owner + 形参数）取；原生函数没有形参表，
                    // 用「实参区最后一个非空槽」截断。
                    int want = -1;
                    if (fn.closure && fn.closure->chunk)
                        want = (int)fn.closure->chunk->flags[1];
                    int last = calleeSlot + 1;
                    if (want > 1)
                    {
                        last = calleeSlot + want;   // owner + 形参
                    }
                    else
                    {
                        for (int i = (int)frame.slots.size() - 1; i > calleeSlot + 1; i--)
                        {
                            if (LocalSlot((unsigned int)i).type != 0) { last = i; break; }
                        }
                    }
                    for (int i = calleeSlot + 2; i <= last; i++)
                        args.push_back(LocalSlot((unsigned int)i));
                }

                int rc = DoCall(fn, owner, args, retSlot, -1, -1, -1, retCount);
                if (rc < 0)
                    return Fail((int)pc, kind, L"调用的对象不是函数（槽 " +
                        std::to_wstring(calleeSlot) + L"）：" + callErr);
                if (rc == 1) continue;
                break;
            }
            case 0x2F: case 0x31:
            {   // CALL_MULTI / CALL_NS
                //
                // 官方 0x2F（0x004C5BA2 块）：
                //   · 把 (A, A+1, A+2) 三个槽上移 3 位 → (A+3, A+4, A+5)
                //     （官方只搬两个实参，故实参个数 = C-1 ≤ 2）
                //   · 以 A+3（= 原 A）为被调值调用（官方 `PUSH EAX`，EAX = &slot[A+3]），
                //     owner = 槽 A+4、实参 = 槽 A+5 起
                //   · 官方 `PUSH ESI`（ESI = C）：登记的返回值个数 = **C**
                //   · 返回值写回**被调槽 A+3**（官方 `MOV [EBX+8],EDX` 只是把栈顶
                //     抬到 &slot[A+6]，返回值落点仍是被调槽 —— 与 0x2A 写回被调槽 A 同理）
                //   · 调用后若**返回值**（槽 A+3）非空 → 槽 A+2 := 槽 A+3，
                //     并按**下一条 0x22** 的位移跳转（官方 LAB_004C5C2D）
                //
                // 官方 0x31（0x004C5B40 块）：
                //   · 槽 A 是**表** → 取表里第一个非空成员（官方 FUN_004C00C0：
                //     先扫数组部分再扫哈希部分），键写进槽 A+3 再拷到槽 A+1，
                //     成员值写进槽 A+4；找到则按下一条 0x22 的位移跳转。
                //   · 槽 A 是**函数** → 走 0x2F 的调用路径。
                //   · 其它类型 → 跳过 2 条记录（官方 puVar15 = local_148 + 2）。
                if (kind == 0x31)
                {
                    RtSlot obj = LocalSlot(A);
                    if (obj.type == 5)
                    {
                        // 官方 0x31（0x004C5B40 块 + FUN_004C00C0）：
                        //   · 槽 A 是**表** → 取下一个成员（先数组部分、后哈希部分，
                        //     官方 FUN_004C00C0 的顺序），键写进槽 A+3（再拷到槽 A+2），
                        //     成员值写进槽 A+4；找到则跳到循环体，
                        //     没找到则顺序执行（循环结束）。
                        //   · 官方把游标放在 VM 级（FUN_004BFFD0），
                        //     Nodio 用帧内游标（frame.iterCur[槽号]）。
                        Value key, val;
                        bool found = false;
                        if (obj.val.t == Value::TableT && obj.val.tab)
                        {
                            // 成员序列：数组部分（1 基下标）在前，哈希部分在后
                            std::vector<std::pair<Value, Value>> members;
                            for (size_t i = 0; i < obj.val.tab->arr.size(); i++)
                            {
                                if (obj.val.tab->arr[i].t != Value::Null)
                                    members.push_back({ Value::Num_((double)(i + 1)),
                                        obj.val.tab->arr[i] });
                            }
                            for (auto it = obj.val.tab->map.begin();
                                it != obj.val.tab->map.end(); ++it)
                            {
                                if (it->second.t != Value::Null)
                                    members.push_back({ Value::Str_(it->first), it->second });
                            }
                            int pos = 0;
                            auto itCur = frame.iterCur.find(A);
                            if (itCur != frame.iterCur.end()) pos = itCur->second;
                            if (pos < 0) pos = 0;
                            if (pos < (int)members.size())
                            {
                                key = members[(size_t)pos].first;
                                val = members[(size_t)pos].second;
                                found = true;
                                frame.iterCur[A] = pos + 1;
                            }
                            else
                            {
                                frame.iterCur.erase(A);
                            }
                        }
                        if (!found)
                        {
                            // 官方 0x004C5C41：没找到成员 → **跳过下一条记录**
                            // （后继是循环回边的 0x22 JMP，跳过它即退出循环）
                            frame.ip = pc + 2;
                            break;
                        }
                        SetSlot(A + 3, SlotOfValue(key));
                        SetSlot(A + 4, SlotOfValue(val));
                        SetSlot(A + 2, SlotOfValue(key));   // 官方：槽 A+2 := 槽 A+3
                        SetSlot(A + 1, SlotOfValue(key));
                        if (pc + 1 >= n)
                            return Fail((int)pc, kind, L"CALL_NS 缺少后继的 JMP(0x22)");
                        unsigned int nxtNs = code[pc + 1];
                        if ((nxtNs & 0x3F) != 0x22)
                            return Fail((int)pc, kind, L"CALL_NS 的后继不是 JMP(0x22)");
                        int relNs = (int)((nxtNs >> 14) - 0x1FFFE);
                        // 官方 0x004C5C2D：落点 = (本条 + 1) + 位移
                        long long tNs = (long long)(pc + 1) + relNs;
                        if (tNs < 0 || tNs >= (long long)n)
                            return Fail((int)pc, kind, L"CALL_NS 跳转落点越界");
                        frame.ip = (size_t)tNs;
                        break;
                    }
                    if (obj.type != 6) { frame.ip = pc + 2; break; }  // 跳过 2 条记录
                }

                // 官方 0x2F（0x004C5BA2 块）：
                //   · 把 (A, A+1, A+2) 三个槽上移 3 位 → (A+3, A+4, A+5)
                //   · 以 **A+3**（= 原 A）为被调值调用（官方 `PUSH EAX`，
                //     EAX = &slot[A+3]），owner = 槽 A+4、实参 = 槽 A+5 起
                //   · 官方 `PUSH ESI`（ESI = C）—— 登记的返回值个数 = **C**
                //   · 返回值写回**被调槽 A+3**（与 0x2A 写回被调槽 A 同理）
                //   · 调用后若槽 A+3 非空 → 槽 A+2 := 槽 A+3，
                //     并按**下一条 0x22** 的位移跳转（官方 LAB_004c5c2d）
                int argc = (C >= 1) ? (int)(C - 1) : 0;
                if (argc > 2) argc = 2;
                RtSlot fn2 = LocalSlot(A);
                RtSlot owner2 = LocalSlot((unsigned int)(A + 1));
                std::vector<RtSlot> args2;
                for (int k = 0; k < argc; k++)
                    args2.push_back(LocalSlot((unsigned int)(A + 2 + k)));
                SetSlot(A + 3, LocalSlot(A));
                SetSlot(A + 4, LocalSlot((unsigned int)(A + 1)));
                SetSlot(A + 5, LocalSlot((unsigned int)(A + 2)));

                long long jump = -1;
                if (pc + 1 >= n)
                    return Fail((int)pc, kind, L"CALL_MULTI 缺少后继的 JMP(0x22)");
                unsigned int nxt = code[pc + 1];
                if ((nxt & 0x3F) != 0x22)
                    return Fail((int)pc, kind, L"CALL_MULTI 的后继不是 JMP(0x22)");
                int rel = (int)((nxt >> 14) - 0x1FFFE);
                // 官方 0x004C5C2D：落点 = (本条 + 1) + 位移
                long long t = (long long)(pc + 1) + rel;
                if (t < 0 || t >= (long long)n)
                    return Fail((int)pc, kind, L"CALL_MULTI 跳转落点越界");
                jump = t;
                int retCount2 = (C >= 1) ? (int)C : 1;
                int rc2 = DoCall(fn2, owner2, args2, A + 3, A + 3, A + 2, jump, retCount2);
                if (rc2 < 0)
                    return Fail((int)pc, kind, L"调用的对象不是函数（槽 " +
                        std::to_wstring(A) + L"）：" + callErr);
                if (rc2 == 1)
                {
                    // 闭包路径：尾处理在返回时执行 —— 结果为空就跳过那条 0x22
                    frames.back().postSkipIp = (int)(pc + 2);
                    continue;
                }
                // 同步路径（原生函数）：官方尾处理立即生效
                //   官方 0x004C5C12：`CMP dword ptr [EAX+8],0`（槽 A+3 的类型标签）
                //   —— 判的是**返回值**（返回写在被调槽 A+3）：
                //     · 非空 → 槽 A+2 := 槽 A+3，并跳到后继 0x22 的落点（循环头）
                //     · 为空 → **跳过**后继的那条 0x22（官方 0x004C5C41），循环结束
                if (A + 3 < (int)frame.slots.size() &&
                    frame.slots[(size_t)(A + 3)].type != 0)
                {
                    if (A + 2 < (int)frame.slots.size())
                        SlotPut(frame.slots[(size_t)(A + 2)],
                            SlotGet(frame.slots[(size_t)(A + 3)]));
                    frame.ip = (size_t)jump;
                }
                else
                {
                    frame.ip = pc + 2;   // 跳过那条 0x22
                }
                break;
            }
            case 0x30:   // CALL_VALUE：官方 0x004C661C 块
            {
                // 官方 0x004C661C：
                //   `cmp dword [edi+0x18],6`（槽 A+2 的类型标签是 6）→
                //   `lea eax,[edi+0x20]`（返回槽 = &slot[A+2]）→
                //   `add edi,0x10`（被调帧基址 = &slot[A+2]，即 **owner = 槽 A+2**）→
                //   `push 0`（形参数 = 0，无实参）→ FUN_004D33C0(vm, &slot[A+1], 0)
                //   被调值 = 槽 A+1（官方从帧指针 &slot[A+1] 取）。
                RtSlot fn = LocalSlot((unsigned int)(A + 1));
                if (fn.type != 6) break;
                RtSlot owner = LocalSlot((unsigned int)(A + 2));
                std::vector<RtSlot> noargs;
                int rc = DoCall(fn, owner, noargs, A + 2, -1, -1, -1);
                if (rc < 0)
                    return Fail((int)pc, kind, L"调用的对象不是函数（槽 " +
                        std::to_wstring(A + 1) + L"）：" + callErr);
                if (rc == 1) continue;
                break;
            }
            case 0x3C:   // CALL_N：官方「显式帧」调用
            {
                // 官方 case 0x3c 不是「立即调用」，而是**准备被调帧**：
                //   把当前帧顶端的槽复制到被调帧的槽区（N = B-1），
                //   返回槽设成被调帧的第 0 槽。
                // 官方校验器 FUN_004D4450 要求槽 A..A+B-2 在帧内。
                // Nodio 用独立帧栈，没有「就地搭临时帧」这一步，
                // 这里按等价语义实现：以实参个数 B-3 调用槽 A 的被调值
                // （调用区 = 槽 A+1..A+B-1：首槽是 owner，其余是实参），
                // 返回值写回槽 A+B-1（官方被调帧的第 0 槽 = 调用方看到的返回槽）。
                int argc = (B >= 3) ? (int)(B - 3) : 0;
                RtSlot fn = LocalSlot(A);
                RtSlot owner = LocalSlot((unsigned int)(A + 1));
                std::vector<RtSlot> args;
                for (int k = 0; k < argc; k++)
                    args.push_back(LocalSlot((unsigned int)(A + 2 + k)));
                int retSlot = (B >= 1) ? (A + (int)B - 1) : -1;
                int rc = DoCall(fn, owner, args, retSlot, -1, -1, -1);
                if (rc < 0)
                    return Fail((int)pc, kind, L"调用的对象不是函数（槽 " +
                        std::to_wstring(A) + L"）：" + callErr);
                if (rc == 1) continue;
                break;
            }
            case 0x2C:   // RET：返回值 = 帧槽 A（B>1 时官方返回 B-1 个值）
            {
                // 官方 RET（case 0x2c + 官方校验器 FUN_004D4450 case 0x2c）：
                //   · 返回值从帧槽 **A** 起，共 **B-1** 个（B≥2 时；
                //     官方校验：`nslots > A+B-2` 即末槽 A+B-2）
                //   · B=0 / B=1 → 只返回帧槽 A 这一个值
                int count = (B >= 2) ? (int)(B - 1) : 1;
                std::vector<RtSlot> vals;
                vals.reserve((size_t)count);
                for (int k = 0; k < count; k++)
                {
                    int idx = A + k;
                    if (idx >= 0 && idx < (int)frame.slots.size())
                        vals.push_back(SlotGet(frame.slots[(size_t)idx]));
                    else
                        vals.push_back(RtSlot());
                }
                if (frames.size() <= 1)
                {
                    res.ok = true;
                    res.value = vals.empty() ? Value() : vals[0].val;
                    res.ns = frames[0].ns;
                    return res;
                }
                int ret = frame.retSlot;
                int retc = frame.retCount;
                int pcs = frame.postCopySrc, pcd = frame.postCopyDst;
                long long pj = frame.postJump;
                int pskip = frame.postSkipIp;
                frames.pop_back();
                // 返回值写回调用方槽 ret 起（个数 = 调用方登记的 retCount，
                // 官方 FUN_004D3330：不足的补 null）
                if (ret >= 0 && retc > 0)
                {
                    int n = (int)frames.back().slots.size();
                    for (int k = 0; k < retc; k++)
                    {
                        if (ret + k >= n) break;
                        if (k < (int)vals.size())
                            SlotPut(frames.back().slots[(size_t)(ret + k)], vals[(size_t)k]);
                        else
                            SlotPut(frames.back().slots[(size_t)(ret + k)], RtSlot());
                    }
                }
                // 官方 0x2F 的尾处理：返回值（被调槽）非空才拷槽、再跳转
                //   （官方 0x004C5C12：`CMP dword ptr [EAX+8],0`）
                //   结果为空 → **跳过**后继的那条 0x22（官方 0x004C5C41）
                if (pj >= 0 && pcs >= 0 && pcd >= 0 &&
                    pcs < (int)frames.back().slots.size() &&
                    pcd < (int)frames.back().slots.size() &&
                    frames.back().slots[(size_t)pcs].type != 0)
                {
                    SlotPut(frames.back().slots[(size_t)pcd],
                        SlotGet(frames.back().slots[(size_t)pcs]));
                    frames.back().ip = (size_t)pj;
                }
                else if (pskip >= 0)
                {
                    frames.back().ip = (size_t)pskip;
                }
                continue;
            }
            case 0x2D:   // ADDEQ：for 循环推进（计数器 += 步长，越界则落空）
            {
                // 官方 case 0x2d：
                //   cur  = (槽[A+3] 是数值) ? 槽[A+3] : 槽[A]
                //   newv = cur + 槽[A+2]（步长）
                //   lim  = 槽[A+1]
                //   步长 <= 0 时 newv <  lim → 跳过赋值（顺序执行，循环结束）
                //   步长 >  0 时 newv >  lim → 跳过赋值
                //   否则：槽[A] = 槽[A+3] = newv，并跳到 pc + (K - 0x1FFFE)
                RtSlot sA = LocalSlot(A), sLim = LocalSlot(A + 1);
                RtSlot sStep = LocalSlot(A + 2), sCnt = LocalSlot(A + 3);
                double cur = 0, step = 0, lim = 0;
                if (!AsNumber(sA, &cur) || !AsNumber(sLim, &lim) ||
                    !AsNumber(sStep, &step))
                    return Fail((int)pc, kind, L"ADDEQ 的操作数不是数值");
                if (sCnt.type == 3) cur = NumOf(sCnt.val);
                double newv = cur + step;
                bool skip = (step <= 0) ? (newv < lim) : (newv > lim);
                if (!skip)
                {
                    SetSlot(A, SlotOf(Value::Num_(newv)));
                    SetSlot(A + 3, SlotOf(Value::Num_(newv)));
                    int rel = (int)(K - 0x1FFFE);
                    long long t = (long long)pc + rel;
                    if (t < 0 || t >= (long long)n)
                        return Fail((int)pc, kind, L"ADDEQ 跳转落点越界");
                    frame.ip = (size_t)t;
                }
                break;
            }
            case 0x2E:   // SUBEQ：槽[A] -= 槽[A+2]，随后**无条件**跳转
            {
                // 官方 case 0x2e：槽[A]、槽[A+1]、槽[A+2] 都必须是数值
                // （字符串会尝试转数值），然后
                //   槽[A] = 槽[A] - 槽[A+2]；槽[A+3] 的类型标签清成 null；
                //   ip = pc + (K - 0x1FFFE)（**总是**跳）。
                // 官方样本里它是 for 循环的「序言」：把计数器先减去步长，
                // 让随后的 ADDEQ 正好加回步长、得到循环的第一个值。
                RtSlot sA = LocalSlot(A), sLim = LocalSlot(A + 1);
                RtSlot sStep = LocalSlot(A + 2);
                double v = 0, step = 0, dummy = 0;
                if (!AsNumber(sA, &v) || !AsNumber(sLim, &dummy) ||
                    !AsNumber(sStep, &step))
                    return Fail((int)pc, kind, L"SUBEQ 的操作数不是数值");
                SetSlot(A, SlotOf(Value::Num_(v - step)));
                SetSlot(A + 3, RtSlot());        // 官方：槽[A+3] 清成 null
                int rel = (int)(K - 0x1FFFE);
                long long t = (long long)pc + rel;
                if (t < 0 || t >= (long long)n)
                    return Fail((int)pc, kind, L"SUBEQ 跳转落点越界");
                frame.ip = (size_t)t;
                break;
            }
            case 0x33:   // GC：记录 VM 不做回收
                break;
            case 0x35: case 0x36: case 0x37: case 0x38: case 0x39: case 0x3A: case 0x3B:
            {   // 位运算（官方 FUN_004C49C0：lhs = bits23..31，rhs = bits14..22）
                RtSlot l = Operand9(B), r = Operand9(C);
                long long x = IntOf(l.val), y = IntOf(r.val), z = 0;
                switch (kind)
                {
                case 0x35: z = ~x; break;
                case 0x36: z = x & y; break;
                case 0x37: z = x | y; break;
                case 0x38: z = x ^ y; break;
                case 0x39: z = x << y; break;
                case 0x3A: z = x >> y; break;
                case 0x3B: z = (long long)((unsigned long long)x >> y); break;
                }
                SetSlot(A, SlotOf(Value::Num_((double)z)));
                break;
            }
            case 0x05:   // GET_MEMBER：对象 = 模块命名空间，键 = 常量表[K]
            {
                RtSlot key = ConstSlot((int)K);
                Value out;
                bool ok = frame.ns && RtTableFind(frame.ns, KeyOfValue(key.val), &out);
                SetSlot(A, ok ? SlotOfValue(out) : RtSlot());
                break;
            }
            case 0x06:   // LOAD_SELF_NS：A ← 当前模块命名空间
            {
                SetSlot(A, SlotOf(Value::Table_(frame.ns)));
                break;
            }
            case 0x07:   // GET_MEMBER2：对象 = VM 暂存槽(vm+0x40)，键 = 常量表[K]
            {
                RtSlot key = ConstSlot((int)K);
                Value out;
                bool ok = RtGetMember(scratch.val, key.val, &out);
                SetSlot(A, ok ? SlotOfValue(out) : RtSlot());
                break;
            }
            case 0x08:   // GET_GLOBAL：A ← B 槽对象所属的父命名空间表
            {
                RtSlot s = LocalSlot(B);
                std::shared_ptr<Table> p;
                if (s.val.t == Value::TableT && s.val.tab) p = s.val.tab->parent;
                SetSlot(A, p ? SlotOf(Value::Table_(p)) : RtSlot());
                break;
            }
            case 0x09: case 0x0A:   // GET_MEMBER3/4：目标 = A 槽，对象 = B 槽，键 = C 槽
            case 0x0B:              // INDEX：目标 = A 槽，对象 = B 槽，下标 = C 槽
            {
                RtSlot obj = LocalSlot(B);
                RtSlot key = Operand9(C);
                Value out;
                bool ok = RtGetMember(obj.val, key.val, &out);
                SetSlot(A, ok ? SlotOfValue(out) : RtSlot());
                break;
            }
            case 0x0C:   // SET_MEMBER：对象 = 模块命名空间，键 = 常量表[K]，值 = A 槽
            {
                RtSlot key = ConstSlot((int)K);
                RtSlot val = LocalSlot(A);
                if (frame.ns) RtTableSet(frame.ns, KeyOfValue(key.val), val.val);
                break;
            }
            case 0x0D:   // SET_MEMBER2：对象 = VM 暂存槽(vm+0x40)，键 = 常量表[K]，值 = A 槽
            {
                RtSlot key = ConstSlot((int)K);
                RtSlot val = LocalSlot(A);
                if (scratch.val.t == Value::TableT && scratch.val.tab)
                    RtTableSet(scratch.val.tab, KeyOfValue(key.val), val.val);
                break;
            }
            case 0x10:   // SET_METATABLE：对象 = A 槽，元表 = B 槽
            {
                RtSlot obj = LocalSlot(A);
                RtSlot meta = Operand9(B);
                if (obj.val.t == Value::TableT && obj.val.tab)
                {
                    obj.val.tab->parent =
                        (meta.val.t == Value::TableT) ? meta.val.tab : nullptr;
                }
                break;
            }
            case 0x11: case 0x13: case 0x14:
            {   // SET_MEMBER3 / RAWSET / SET_MEMBER4：对象 = A 槽，键 = B 槽，值 = C 槽
                RtSlot obj = LocalSlot(A);
                RtSlot key = Operand9(B);
                RtSlot val = Operand9(C);
                if (obj.val.t == Value::TableT && obj.val.tab)
                    RtTableSet(obj.val.tab, KeyOfValue(key.val), val.val);
                break;
            }
            case 0x12:   // CONCAT_MEMBER：表 = A 槽，键 = B 槽，待追加值 = C 槽
            {
                RtSlot tbl = LocalSlot(A);
                RtSlot key = Operand9(B);
                RtSlot val = Operand9(C);
                if (tbl.val.t == Value::TableT && tbl.val.tab)
                {
                    RtKey k = KeyOfValue(key.val);
                    Value* member = RtTableFindLocal(tbl.val.tab.get(), k);
                    if (!member)
                    {
                        RtTableSet(tbl.val.tab, k, Value());
                        member = RtTableFindLocal(tbl.val.tab.get(), k);
                    }
                    if (member)
                    {
                        if (member->t == Value::Str)
                            *member = Value::Str_(member->s + L";" + val.val.ToString());
                        else
                            *member = val.val;
                    }
                }
                break;
            }
            case 0x15:   // NEW_NATIVE：新建命名空间表（父表 = 全局命名空间）
            {
                // 官方 FUN_004C0810：分配一张 type 5 的**空表**，父表写死全局内建表
                // （puVar3[4] = &DAT_007C89B0），表内容由随后的 SET_MEMBER3 填。
                // B / C 是两个打包的小整数位域（((x & 7) + 8) << (x >> 3 - 1)），
                // 官方只把它落到表标志位上，确切含义尚未查明（见 docs §7.8）。
                std::shared_ptr<Table> ns = std::make_shared<Table>();
                ns->parent = globalNs;
                SetSlot(A, SlotOf(Value::Table_(ns)));
                break;
            }
            case 0x16:   // IMPORT_MEMBER：名字 = B 操作数（bit8=1 → 常量），目标 = 槽 C
            {
                // 官方 FUN_004C3B80：
                //   · 目标槽若已是表 → 以它为查找起点；否则目标槽 ← 原模块命名空间；
                //   · 名字是字符串时按 '.' 逐段查表；名字以 '.' 开头则从
                //     VM 暂存槽（= 全局命名空间）开始查；
                //   · 查不到的段**就地建一个空命名空间**并登记在当前表里；
                //   · 最后模块命名空间 ← 解析出来的命名空间。
                RtSlot name = Operand9(B);
                int dest = (int)(C & 0x1FF);
                std::shared_ptr<Table> ctx;
                RtSlot destSlot = (dest >= 0 && dest < (int)frame.slots.size())
                    ? SlotGet(frame.slots[(size_t)dest]) : RtSlot();
                if (destSlot.val.t == Value::TableT && destSlot.val.tab)
                {
                    // 官方 FUN_004C3B80：目标槽已是表 → 直接以它为新的模块命名空间，
                    // **名字根本不看**（所以名字是 null 也不会报错）。
                    ctx = destSlot.val.tab;
                }
                else
                {
                    if (!frame.ns)
                        return Fail((int)pc, kind, L"IMPORT_MEMBER：当前没有模块命名空间");
                    ctx = frame.ns;
                    SetSlot(dest, SlotOf(Value::Table_(ctx)));
                    if (name.val.t == Value::Str)
                    {
                        const wstring& path = name.val.s;
                        size_t pos = 0;
                        if (!path.empty() && path[0] == L'.')
                        {
                            ctx = globalNs;            // 绝对路径：从全局命名空间开始
                            pos = 1;
                        }
                        while (pos <= path.size())
                        {
                            size_t dot = path.find(L'.', pos);
                            size_t end = (dot == wstring::npos) ? path.size() : dot;
                            wstring seg = path.substr(pos, end - pos);
                            if (!seg.empty())
                            {
                                Value found;
                                if (ctx && RtTableFind(ctx, KeyOfValue(Value::Str_(seg)), &found))
                                {
                                    if (found.t == Value::TableT && found.tab)
                                    {
                                        ctx = found.tab;
                                    }
                                    else
                                    {
                                        // 闭包 → 取它的属主命名空间（官方 FUN_004C3B80 的 type 6 分支）
                                        std::shared_ptr<RtClosure> cl = ClosureOfValue(found);
                                        if (cl && cl->ownerNs) ctx = cl->ownerNs;
                                        else
                                            return Fail((int)pc, kind,
                                                L"IMPORT_MEMBER：导入的成员不是命名空间：" + seg);
                                    }
                                }
                                else
                                {
                                    std::shared_ptr<Table> sub = std::make_shared<Table>();
                                    sub->parent = globalNs;
                                    if (ctx)
                                        RtTableSet(ctx, KeyOfValue(Value::Str_(seg)), Value::Table_(sub));
                                    ctx = sub;
                                }
                            }
                            if (dot == wstring::npos) break;
                            pos = dot + 1;
                        }
                    }
                    else if (name.val.t == Value::TableT && name.val.tab)
                    {
                        ctx = name.val.tab;           // 名字本身就是一张表 → 直接用它
                    }
                    else
                    {
                        return Fail((int)pc, kind, L"IMPORT_MEMBER：名字既不是字符串也不是表");
                    }
                }
                frame.ns = ctx;
                break;
            }
            case 0x17:   // METHOD_PREP：结果 = A 槽，A+1 = B 槽副本，键 = C 槽
            {
                RtSlot obj = LocalSlot(B);
                RtSlot key = Operand9(C);
                SetSlot(A + 1, obj);
                Value out;
                bool ok = RtGetMember(obj.val, key.val, &out);
                SetSlot(A, ok ? SlotOfValue(out) : RtSlot());
                break;
            }
            case 0x32:   // BUILD_ARRAY：目标表 = A 槽，元素取自 A+1..A+B
            {
                if ((C & 0x1FF) == 0)
                {
                    // 官方：C == 0 时元素个数写在紧随其后的 u32 里
                    if (pc + 1 >= n)
                        return Fail((int)pc, kind, L"BUILD_ARRAY 缺少后继的元素个数");
                    frame.ip = pc + 2;
                }
                int count = (int)(B & 0xFF);
                if (count == 0)
                    return Fail((int)pc, kind,
                        L"BUILD_ARRAY 的基址依赖调用帧返回槽，记录 VM 尚未实现调用帧");
                RtSlot dest = LocalSlot(A);
                if (dest.val.t == Value::TableT && dest.val.tab)
                {
                    for (int i = 1; i <= count; i++)
                    {
                        int idx = A + i;
                        if (idx < 0 || idx >= (int)frame.slots.size()) break;
                        dest.val.tab->arr.push_back(SlotGet(frame.slots[(size_t)idx]).val);
                    }
                }
                break;
            }
            case 0x34:   // NEW_CLOSURE：K = 嵌套对象下标，捕获条数 = 嵌套对象 flags[0]
            {
                int k = (int)K;
                const std::vector<AardioChunkNode>& kids = chunk->children;
                if (k < 0 || k >= (int)kids.size())
                    return Fail((int)pc, kind, L"NEW_CLOSURE 的嵌套对象下标越界：" +
                        std::to_wstring(k));
                const AardioChunkNode* body = &kids[(size_t)k];
                int nu = (int)body->flags[0];      // 官方运行时 +0x48 = 上值个数

                std::vector<std::shared_ptr<RtCell>> ups;
                ups.reserve((size_t)nu);
                for (int i = 0; i < nu; i++)
                {
                    size_t cpc = pc + 1 + (size_t)i;
                    if (cpc >= n)
                        return Fail((int)pc, kind, L"NEW_CLOSURE 的捕获记录越界");
                    unsigned int cw = code[cpc];
                    int ck = (int)(cw & 0x3F);
                    int cb = (int)((cw >> 23) & 0x1FF);
                    if (ck == 0x03)
                    {
                        // 捕获当前作用域的上值（共享同一个 cell）
                        std::shared_ptr<RtCell> cell;
                        if (cb < (int)frame.upvalues.size())
                            cell = frame.upvalues[(size_t)cb];
                        if (!cell) cell = std::make_shared<RtCell>();
                        ups.push_back(cell);
                    }
                    else
                    {
                        // 捕获局部槽：把该槽**装箱**，之后写局部即写上值（按引用）
                        int idx = cb & 0xFF;
                        if (idx >= 0 && idx < (int)frame.slots.size())
                        {
                            RtSlot& s = frame.slots[(size_t)idx];
                            if (!s.box)
                            {
                                std::shared_ptr<RtCell> cell = std::make_shared<RtCell>();
                                cell->value = SlotGet(s);
                                s.box = cell;
                            }
                            ups.push_back(s.box);
                        }
                        else
                        {
                            ups.push_back(std::make_shared<RtCell>());
                        }
                    }
                }
                if (nu > 0)
                    frame.ip = pc + 1 + (size_t)nu;   // 捕获记录被消费

                std::shared_ptr<RtClosure> cl = std::make_shared<RtClosure>();
                cl->chunk = body;
                cl->upvalues = ups;
                cl->ownerNs = frame.ns;
                arena.push_back(cl);

                RtSlot s;
                s.type = 6;
                s.val = Value::Pointer_(cl.get());
                s.closure = cl;
                SetSlot(A, s);
                break;
            }
            default:
                break;
            }
        }

        // 记录流跑完但没有 RET
        res.ok = true;
        res.value = Value();
        if (!frames.empty()) res.ns = frames[0].ns;
        return res;
    }

    // ---- 公共入口 ----

    AardioRunResult RunAardioChunk(const AardioChunkNode& root,
        const AardioHost* host, AardioChunkModule* mod)
    {
        return RunAardioChunkInner(root, host, mod, nullptr, nullptr);
    }

    // 在模块的闭包池里找闭包对象
    static std::shared_ptr<RtClosure> ClosureInModule(const AardioChunkModule& mod,
        const Value& v)
    {
        if (!mod.runtime) return nullptr;
        if (v.t != Value::Pointer) return nullptr;
        auto rt = std::static_pointer_cast<RtRuntime>(mod.runtime);
        for (size_t i = 0; i < rt->arena.size(); i++)
            if (rt->arena[i].get() == v.ptr) return rt->arena[i];
        return nullptr;
    }

    bool AardioIsClosure(const AardioChunkModule& mod, const Value& v)
    {
        return ClosureInModule(mod, v) != nullptr;
    }

    bool CallAardioClosure(AardioChunkModule& mod, const Value& callee,
        std::vector<Value>& args, Value* ret, wstring* err)
    {
        if (!mod.root)
        {
            if (err) *err = L"官方块模块已释放";
            return false;
        }
        if (!ClosureInModule(mod, callee))
        {
            if (err) *err = L"调用的对象不是官方块里的函数";
            return false;
        }
        AardioHost host;
        host.globalNs = mod.globalNs;
        host.moduleNs = mod.moduleNs;
        host.importModule = mod.importModule;
        host.callHostFunc = mod.callHostFunc;
        std::vector<Value> a(args);
        AardioRunResult r = RunAardioChunkInner(*mod.root, &host, &mod, &callee, &a);
        if (!r.ok)
        {
            if (err)
            {
                wchar_t hex[8];
                swprintf_s(hex, L"%02X", r.kind < 0 ? 0 : r.kind);
                *err = r.err + L"（记录号 " + std::to_wstring(r.pc) + L" / 种类 0x" + hex + L"）";
            }
            return false;
        }
        if (ret) *ret = r.value;
        return true;
    }

    Value MakeAardioClosureFn(AardioChunkModule& mod, const Value& callee)
    {
        // 拷贝一份模块句柄：chunk 树 / 闭包池 / 命名空间都是共享的，
        // 副本活得和这个原生函数一样久。
        AardioChunkModule m = mod;
        return Value::Native_([m, callee](std::vector<Value>& a) mutable -> Value {
            Value ret;
            wstring err;
            if (!CallAardioClosure(m, callee, a, &ret, &err))
            {
                // 原生函数没有错误通道（官方内建也是返回 null），
                // 这里把原因按 UTF-8 字节写到 stderr，便于定位。
                std::string msg = Utf8Bytes(L"[aardio 预编译块] " + err + L"\n");
                fwrite(msg.data(), 1, msg.size(), stderr);
                fflush(stderr);
                return Value();
            }
            return ret;
        });
    }
}
