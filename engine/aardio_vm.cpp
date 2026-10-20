// aardio_vm.cpp — 官方 "=aardio" 预编译块的记录 VM 内核。
//
// 语义来源（全部实测自 aardio.exe，见 docs/aardio-bytecode.md §7）：
//   · 记录字位布局            §7.2
//   · 16 字节槽与类型标签      §7.3
//   · 调用帧与调用约定         §7.4
//   · 61 种记录语义            §7.5
//   · 操作数寻址（描述符表）   §7.5.2
//   · 操作数角色（A/B/C）      §7.5.4
//
// 本文件实现「不依赖调用帧 / 闭包 / 原生函数」的一组记录：
// 取值（LOAD_* / CLEAR_LOCALS）、算术（0x18-0x1D）、一元负号、真值、长度、
// 位运算（0x35-0x3B）、跳转与条件跳转（0x22-0x28）、返回（0x2C）、GC（0x33），
// 以及第十五轮新增的**成员访问族**与**数组构造**：
//   0x05/0x06/0x07/0x08/0x09/0x0A/0x0B/0x0C/0x0D/0x10/0x11/0x12/0x13/0x14/0x17
//   0x32 BUILD_ARRAY
// 其余种类一律明确报错（种类 + 记录号），不静默跳过。
#include "aardio_vm.h"

namespace NodioVM
{
    namespace
    {
        // 官方 16 字节槽：值 8 字节 @+0 + 类型标签 4 字节 @+8
        struct RtSlot
        {
            int type = 0;       // 0 null / 1 bool / 3 number / 4 string / 6 func / 7 table
            Value val;
        };

        struct RtFrame
        {
            std::vector<RtSlot> slots;
            const AardioChunkNode* chunk = nullptr;
            std::vector<RtSlot> upvalues;
            RtFrame* parent = nullptr;
            size_t ip = 0;
            // 模块命名空间（官方 iVar11+0x10）：0x05/0x0C 的对象，0x06 的取值
            std::shared_ptr<Table> ns;
            // VM 暂存槽（官方 vm+0x40）：0x07/0x0D 的对象
            RtSlot scratch;
        };

        // 官方类型标签（实测自解释器 FUN_004C4BC0 的槽类型判定）
        //   0 null / 1 bool / 3 number / 4 string / 5 table / 6 func / 7 array
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

        bool TruthyOf(const Value& v)
        {
            switch (v.t)
            {
            case Value::Null:  return false;
            case Value::Bool:  return v.b;
            case Value::Num:   return v.n != 0;
            case Value::Str:   return !v.s.empty();
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

        int CompareOf(const Value& a, const Value& b)
        {
            if (a.t == Value::Str || b.t == Value::Str)
            {
                wstring x = (a.t == Value::Str) ? a.s : NumText(NumOf(a));
                wstring y = (b.t == Value::Str) ? b.s : NumText(NumOf(b));
                return x.compare(y) < 0 ? -1 : (x == y ? 0 : 1);
            }
            double x = NumOf(a), y = NumOf(b);
            return x < y ? -1 : (x == y ? 0 : 1);
        }

        // 官方类型标签 → 值的类型
        Value ValueOfSlot(const RtSlot& s)
        {
            return s.val;
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

        // 记录 VM 的错误出口
        struct VmFail
        {
            wstring msg;
            int pc = -1;
            int kind = -1;
        };

        const int kMaxSteps = 20000000;   // 防止死循环（官方有 200 层调用限制）
    }

    // 已在记录 VM 里落地的记录种类（与 kKindTable 的 state 保持一致）
    static bool VmKindImplemented(int kind)
    {
        switch (kind)
        {
        case 0x00: case 0x01: case 0x02: case 0x03: case 0x04:
        case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0A:
        case 0x0B: case 0x0C: case 0x0D: case 0x10: case 0x11: case 0x12:
        case 0x13: case 0x14: case 0x17:
        case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D:
        case 0x1E: case 0x1F: case 0x20:
        case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27: case 0x28:
        case 0x2C: case 0x32: case 0x33:
        case 0x35: case 0x36: case 0x37: case 0x38: case 0x39: case 0x3A: case 0x3B:
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

    AardioRunResult RunAardioChunk(const AardioChunkNode& root)
    {
        AardioRunResult res;
        RtFrame frame;
        frame.chunk = &root;

        // 局部槽表：官方 chunk 帧 +0x4B（槽位数）
        int nSlots = (int)root.flags[3];
        if (nSlots < 0) nSlots = 0;
        frame.slots.assign((size_t)nSlots, RtSlot());

        // 模块命名空间（官方 iVar11+0x10）与 VM 暂存槽（官方 vm+0x40）
        frame.ns = std::make_shared<Table>();
        frame.scratch = SlotOf(Value::Table_(frame.ns));

        const std::vector<unsigned int>& code = root.code;
        const size_t n = code.size();

        // 常量表（chunk 帧 +0x08）—— 9 位操作数 bit8=1 与 18 位操作数都指向它
        const std::vector<AardioConstValue>& consts = root.consts;

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
            return frame.slots[(size_t)idx];
        };

        // 只按局部槽下标取（官方在「对象槽」位置一律直取局部槽，不看 bit8）
        auto LocalSlot = [&](unsigned int v) -> RtSlot
        {
            int idx = (int)(v & 0xFF);
            if (idx < 0 || idx >= (int)frame.slots.size()) return RtSlot();
            return frame.slots[(size_t)idx];
        };

        auto SetSlot = [&](int idx, const RtSlot& s)
        {
            if (idx < 0 || idx >= (int)frame.slots.size()) return;
            frame.slots[(size_t)idx] = s;
        };

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

        while (frame.ip < n)
        {
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
                {
                    frame.slots[(size_t)i].type = 0;
                    frame.slots[(size_t)i].val = Value();
                }
                break;
            }
            case 0x03:   // LOAD_UPVALUE：B 是上值槽
            {
                int idx = (int)(B & 0xFF);
                RtSlot s;
                if (idx >= 0 && idx < (int)frame.upvalues.size())
                    s = frame.upvalues[(size_t)idx];
                SetSlot(A, s);
                break;
            }
            case 0x04:   // LOAD_LOCAL：B 是局部槽
            {
                int idx = (int)(B & 0xFF);
                RtSlot s;
                if (idx >= 0 && idx < (int)frame.slots.size())
                    s = frame.slots[(size_t)idx];
                SetSlot(A, s);
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
                    case 0x1C: z = (y != 0) ? std::fmod(x, y) : 0; break;
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
                case 0x27:   // JTRUE：真值槽 A 的真值 != C（立即数 0/1）则跳转
                {
                    RtSlot l = (A >= 0 && A < (int)frame.slots.size())
                        ? frame.slots[(size_t)A] : RtSlot();
                    int t1 = TruthyOf(l.val) ? 1 : 0;
                    jump = (t1 != (int)(C & 0x1FF));
                    break;
                }
                case 0x28:   // JTRUE2：真值槽 B 的真值 != C 则跳转，并带上 B 的值
                {
                    RtSlot l = Operand9(B);
                    int t1 = TruthyOf(l.val) ? 1 : 0;
                    jump = (t1 != (int)(C & 0x1FF));
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
            case 0x2C:   // RET：A 是返回值槽
            {
                RtSlot s;
                if (A >= 0 && A < (int)frame.slots.size())
                    s = frame.slots[(size_t)A];
                res.ok = true;
                res.value = ValueOfSlot(s);
                return res;
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
                SetSlot(A, ok ? SlotOf(out) : RtSlot());
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
                bool ok = RtGetMember(frame.scratch.val, key.val, &out);
                SetSlot(A, ok ? SlotOf(out) : RtSlot());
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
                SetSlot(A, ok ? SlotOf(out) : RtSlot());
                break;
            }
            case 0x0C:   // SET_MEMBER：对象 = 模块命名空间，键 = 常量表[K]，值 = A 槽
            {
                RtSlot key = ConstSlot((int)K);
                RtSlot val = (A >= 0 && A < (int)frame.slots.size())
                    ? frame.slots[(size_t)A] : RtSlot();
                if (frame.ns) RtTableSet(frame.ns, KeyOfValue(key.val), val.val);
                break;
            }
            case 0x0D:   // SET_MEMBER2：对象 = VM 暂存槽(vm+0x40)，键 = 常量表[K]，值 = A 槽
            {
                RtSlot key = ConstSlot((int)K);
                RtSlot val = (A >= 0 && A < (int)frame.slots.size())
                    ? frame.slots[(size_t)A] : RtSlot();
                if (frame.scratch.val.t == Value::TableT && frame.scratch.val.tab)
                    RtTableSet(frame.scratch.val.tab, KeyOfValue(key.val), val.val);
                break;
            }
            case 0x10:   // SET_METATABLE：对象 = A 槽，元表 = B 槽
            {
                RtSlot obj = (A >= 0 && A < (int)frame.slots.size())
                    ? frame.slots[(size_t)A] : RtSlot();
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
                RtSlot obj = (A >= 0 && A < (int)frame.slots.size())
                    ? frame.slots[(size_t)A] : RtSlot();
                RtSlot key = Operand9(B);
                RtSlot val = Operand9(C);
                if (obj.val.t == Value::TableT && obj.val.tab)
                    RtTableSet(obj.val.tab, KeyOfValue(key.val), val.val);
                break;
            }
            case 0x12:   // CONCAT_MEMBER：表 = A 槽，键 = B 槽，待追加值 = C 槽
            {
                RtSlot tbl = (A >= 0 && A < (int)frame.slots.size())
                    ? frame.slots[(size_t)A] : RtSlot();
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
            case 0x17:   // METHOD_PREP：结果 = A 槽，A+1 = B 槽副本，键 = C 槽
            {
                RtSlot obj = LocalSlot(B);
                RtSlot key = Operand9(C);
                SetSlot(A + 1, obj);
                Value out;
                bool ok = RtGetMember(obj.val, key.val, &out);
                SetSlot(A, ok ? SlotOf(out) : RtSlot());
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
                RtSlot dest = (A >= 0 && A < (int)frame.slots.size())
                    ? frame.slots[(size_t)A] : RtSlot();
                if (dest.val.t == Value::TableT && dest.val.tab)
                {
                    for (int i = 1; i <= count; i++)
                    {
                        int idx = A + i;
                        if (idx < 0 || idx >= (int)frame.slots.size()) break;
                        dest.val.tab->arr.push_back(frame.slots[(size_t)idx].val);
                    }
                }
                break;
            }
            default:
                break;
            }
        }

        // 记录流跑完但没有 RET
        res.ok = true;
        res.value = Value();
        return res;
    }
}
