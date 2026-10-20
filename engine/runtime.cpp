#include "engine/interp.h"

namespace NodioVM
{

thread_local Interp* g_interpCurrent = nullptr;   // M2: active interp for meta hooks
thread_local int g_metaDispatchDepth = 0;   // metaProperty dispatch flag
thread_local std::unordered_set<Table*> g_metaDispatchActive;
thread_local Value s_currentOwner;
bool g_nsTrace = false;   // NODIO_NS_TRACE=1 — namespace resolution diagnostics
Interp* Interp::g_self = nullptr;    // M2: self for meta callbacks
Value Interp::GetCurrentOwner() { return s_currentOwner; }
void Interp::SetCurrentOwner(const Value& v) { s_currentOwner = v; }

// ---- S-2: a runtime error owns its file ----
//
// Official aardio keeps 文件 / 错误行号 in a structured error report (the
// field table lives in aardio.exe: 错误 / 文件 / 错误行号 / 字段 / 失败操作
// …) and drops both when the project is published with debugging switched
// off.  Nodio puts the location into the message itself, so console output,
// the LSP and CI logs can attribute a failure to a file — a bare line
// number is ambiguous once a library has been imported (that cost a whole
// debugging round on autos.ui.memories, where "@ 行 85" really belonged to
// string\markdown\_.aardio:85).
//
// Every frame carries the source file that owns the code running in it:
// a module frame carries its own path, a call frame inherits it from the
// DEFINING frame chain (a function of module A called from module B still
// belongs to A).  Walking that chain gives the file the line number
// really comes from.
const wstring* SourceFilePtrOfEnv(Env* env)
{
    for (Env* e = env; e; e = e->parent.get())
    {
        if (!e->srcFile.empty()) return &e->srcFile;
    }
    return nullptr;
}

// S-2: the location marker is always the LAST "@ " of the message:
//   "@ 行 <n>"      — a bare line number (no owning file known)
//   "@ <file>:<n>"  — file and line
bool HasErrorLocation(const wstring& msg)
{
    size_t at = msg.rfind(L" @ ");
    if (at == wstring::npos) return false;
    const wchar_t* p = msg.c_str() + at + 3;
    if (p[0] == L'\u884C')   // 行
    {
        const wchar_t* q = p + 1;
        while (*q == L' ') q++;
        if (*q < L'0' || *q > L'9') return false;
        while (*q >= L'0' && *q <= L'9') q++;
        return true;
    }
    // "<file>:<n>" — the last colon must be followed by digits only
    const wchar_t* colon = nullptr;
    for (const wchar_t* q = p; *q; q++)
        if (*q == L':') colon = q;
    if (!colon || !colon[1]) return false;
    const wchar_t* q = colon + 1;
    while (*q >= L'0' && *q <= L'9') q++;
    return *q == L'\0';
}

void AppendErrorLocation(wstring* msg, const wstring& file, int line)
{
    if (!msg) return;
    if (file.empty())
        (*msg) += L" @ 行 " + std::to_wstring(line);
    else
        (*msg) += L" @ " + file + L":" + std::to_wstring(line);
}

// ---- Eval ----
Value Interp::Eval(const PNode& n, Env* env)
        {
            switch (n->k)
            {
            case Node::Num: return Value::Num_(n->num);
            case Node::Str:
            {
                // $"~\path" that had no file to inline at parse time carries
                // the EMBED marker: resolve it now from the runtime embed
                // map (a published payload's res\), falling back to the
                // path text when the payload does not carry it.
                const wstring& s = n->str;
                if (s.size() > 12 && s.compare(0, 12, L"$NODIOEMBED$") == 0)
                {
                    wstring key = s.substr(12);
                    if (g_interpCurrent)
                    {
                        auto eit = g_interpCurrent->embedded.find(key);
                        if (eit != g_interpCurrent->embedded.end())
                            return Value::Str_(eit->second);
                    }
                    return Value::Str_(key);
                }
                Value v = Value::Str_(s, n->bUtf16);
                v.bRawBytes = n->bRawBytes;
                return v;
            }
            case Node::Bool: return Value::Bool_(n->num != 0);
            case Node::NullK: return Value();
            case Node::Ident:
            {
                if (n->str == L"..") return Value();  // root marker
                if (Value* v = env->Find(n->str))
                    return *v;
                static auto isBuiltinPrimitive = [](const wstring& name) {
                    return name == L"type" || name == L"call" || name == L"loadcode" ||
                           name == L"loadcodex" || name == L"eval" || name == L"error" ||
                           name == L"assert" || name == L"invoke" || name == L"callex" ||
                           name == L"rget";
                };
                if (!isBuiltinPrimitive(n->str))
                {
                    for (Env* e = env; e; e = e->parent.get())
                    {
                        if (e->nsTable && e->nsTable->bExplicitMeta)
                        {
                            auto im = e->nsTable->map.find(L"_meta");
                            if (im != e->nsTable->map.end() && im->second.t == Value::TableT)
                            {
                                Value m = GetMember(Value::Table_(e->nsTable), n->str);
                                if (m.t != Value::Null)
                                    return m;
                            }
                        }
                    }
                }
                auto itR = roots.find(n->str);
                if (itR != roots.end())
                    return itR->second;
                return Value();
            }
            case Node::Member:
            {
                // the receiver lives in a C++ local while the metatable
                // hook runs — keep it reachable for the collector
                GcRootScope rs;
                Value obj;
                if (n->bRoot)
                    obj = Value();   // root namespace = globals table
                else
                    obj = Eval(n->a, env);
                rs.add(obj);
                if (n->bSafe && obj.t == Value::Null)
                    return Value();   // '?.': null short-circuits
                if (n->bMeta)
                {
                    // 'tab@' — the metatable itself
                    if ((obj.t == Value::TableT || obj.t == Value::Class || obj.t == Value::Func || obj.t == Value::Pointer) && obj.tab)
                    {
                        auto im = obj.tab->map.find(L"_meta");
                        if (im != obj.tab->map.end())
                            return im->second;
                    }
                    return Value();
                }
                return GetMember(obj, n->str);
            }
            case Node::Index:
            {
                GcRootScope rs;
                Value obj = Eval(n->a, env);
                rs.add(obj);
                if (n->bSafe && obj.t == Value::Null)
                    return Value();   // '?[': null short-circuits
                Value idx = Eval(n->b, env);
                rs.add(idx);
                // byte-string model: s[i] reads the i-th BYTE
                // (1-based) as a number — string indexing is
                // built on this primitive. Byte-per-wchar strings (from
                // fromto/raw IO, all chars <= 0xFF) index directly; real
                // text indexes its UTF-8 encoding.
                if (obj.t == Value::Str && idx.t == Value::Num)
                {
                    if (obj.bUtf16)
                    {
                        long long i = (long long)idx.n;
                        if (i >= 1 && i <= (long long)obj.s.size())
                            return Value::Num_((double)(unsigned short)obj.s[(size_t)(i - 1)]);
                        return Value();
                    }
                    bool bFake = true;
                    for (wchar_t c : obj.s)
                        if ((unsigned int)c > 0xFF) { bFake = false; break; }
                    std::vector<char> buf;
                    size_t cb;
                    if (bFake)
                    {
                        cb = obj.s.size();
                        buf.resize(cb);
                        for (size_t k = 0; k < cb; k++)
                            buf[k] = (char)(unsigned char)obj.s[k];
                    }
                    else
                    {
                        cb = (size_t)WideCharToMultiByte(CP_UTF8, 0,
                                                         obj.s.c_str(),
                                                         (int)obj.s.size(),
                                                         nullptr, 0,
                                                         nullptr, nullptr);
                        buf.resize(cb);
                        if (cb)
                            WideCharToMultiByte(CP_UTF8, 0, obj.s.c_str(),
                                                (int)obj.s.size(),
                                                buf.data(), (int)cb,
                                                nullptr, nullptr);
                    }
                    long long i = (long long)idx.n;
                    if (i >= 1 && i <= (long long)cb)
                        return Value::Num_(
                            (double)(unsigned char)buf[(size_t)(i - 1)]);
                    return Value();
                }
                return GetMember(obj, idx, n->bDirect, /*ownerCallMode=*/-1);
            }
            case Node::Call: return EvalCall(n, env);
            case Node::Return:
            {
                // inline `return expr` in expression position: unwind via
                // nReturn like the statement form
                Value v = n->a ? Eval(n->a, env) : Value();
                ret = v;
                nReturn++;
                return v;
            }
            case Node::Bin: return EvalBin(n, env);
            case Node::Un:
            {
                GcRootScope rs;
                Value v = Eval(n->a, env);
                rs.add(v);
                if (n->str == L"-") return Value::Num_(-ToNum(v));
                if (n->str == L"!" || n->str == L"not") return Value::Bool_(!v.Truthy());
                if (n->str == L"~")
                {
                    // style=~0: bitwise NOT over 32-bit width
                    unsigned int u = (unsigned int)(INT_PTR)(long long)ToNum(v);
                    return Value::Num_((double)(int)~u);
                }
                if (n->str == L"#")
                {
                    if (v.t == Value::Str)
                    {
                        // byte-string model: #s = UTF-8 BYTE length.
                        // Storage is wstring. Raw binary byte strings (marked with bRawBytes)
                        // hold one byte per wchar — for those the wchar count IS the byte count.
                        // Normal text strings return their UTF-8 byte length.
                        if (v.bRawBytes)
                            return Value::Num_((double)v.s.size());
                        int cb = WideCharToMultiByte(CP_UTF8, 0,
                                                     v.s.c_str(),
                                                     (int)v.s.size(),
                                                     nullptr, 0, nullptr,
                                                     nullptr);
                        return Value::Num_((double)cb);
                    }
                    if (v.t == Value::Pointer)
                    {
                        if (v.ptr && IsBuffer(v.ptr))
                            return Value::Num_((double)GetBufferSize(v.ptr));
                        return Value::Num_(0);
                    }
                    if (v.t == Value::TableT)
                    {
                        if (v.tab)
                        {
                            size_t sz = v.tab->arr.size();
                            while (sz > 0 && v.tab->arr[sz - 1].t == Value::Null)
                                sz--;
                            if (sz > 0)
                                return Value::Num_((double)sz);

                            auto im = v.tab->map.find(L"_meta");
                            if (im != v.tab->map.end() && im->second.t == Value::TableT && im->second.tab)
                            {
                                static const wchar_t* lenNames[] = { L"_len", L"_length", L"len", L"length", nullptr };
                                for (int i = 0; lenNames[i]; i++)
                                {
                                    auto it = im->second.tab->map.find(lenNames[i]);
                                    if (it != im->second.tab->map.end())
                                    {
                                        if (it->second.t == Value::Func || it->second.native)
                                        {
                                            vector<Value> noArgs;
                                            Value r = this->CallOwner(it->second, v, noArgs);
                                            if (r.t == Value::Num)
                                                return r;
                                        }
                                        else if (it->second.t == Value::Num)
                                        {
                                            return it->second;
                                        }
                                    }
                                }
                            }
                            return Value::Num_((double)sz);
                        }
                        return Value::Num_(0);
                    }
                }
                // #null is 0 — code sizes optional tables with
                // it; returning null made the for-loop start value null and the
                // body ran once against a null subscriber.
                if (v.t == Value::Null) return Value::Num_(0);
                return Value();
            }
            case Node::FuncLit:
            {
                Value f;
                f.t = Value::Func;
                f.fn = n->a;
                f.params = n->names;
                f.pdefaults = std::make_shared<vector<PNode>>(
                    n->defaults);
                // lexical upvalues: capture the defining frame chain. The
                // closure's call frame parents into it (CallVmFunc/
                // CallOwner), so enclosing locals like
                // `fib`/`result` stay visible and mutable.
                if (env && env->self)
                    f.envUp = env->self;
                for (Env* e = env; e; e = e->parent.get())
                {
                    if (!f.thisVal)
                    {
                        auto itThis = e->vars.find(L"this");
                        if (itThis != e->vars.end() && itThis->second.t != Value::Null)
                            f.thisVal = std::make_shared<Value>(itThis->second);
                    }
                    if (!f.ownerVal)
                    {
                        auto itOwner = e->vars.find(L"owner");
                        if (itOwner != e->vars.end() && itOwner->second.t != Value::Null)
                            f.ownerVal = std::make_shared<Value>(itOwner->second);
                    }
                    if (!f.nsClosure && e->nsTable && !e->bNsInst)
                    {
                        f.nsClosure = e->nsTable;
                    }
                }
                if (!f.nsClosure)
                {
                    for (Env* e = env; e; e = e->parent.get())
                    {
                        if (e->modTable) { f.nsClosure = e->modTable; break; }
                    }
                }
                return f;
            }
            case Node::TableCtor:
            {
                auto tb = std::make_shared<Table>();
                for (size_t i = 0; i + 1 < n->list.size(); i += 2)
                {
                    if (n->list[i + 1]->k == Node::Ident && n->list[i + 1]->str == L"...")
                    {
                        Value vDots = Eval(n->list[i + 1], env);
                        if (vDots.t == Value::TableT && vDots.tab)
                        {
                            for (auto& elem : vDots.tab->arr)
                                tb->arr.push_back(elem);
                            continue;
                        }
                        else if (vDots.t == Value::Null)
                        {
                            continue;
                        }
                    }
                    Value k = Eval(n->list[i], env);
                    Value v = Eval(n->list[i + 1], env);
                    // an array-slot call expression expands ALL its return
                    // values into consecutive elements (official:
                    // var ret = [ call(func) ] keeps (ok, v) both)
                    if (k.t == Value::Num && k.n == (long long)k.n &&
                        k.n >= 1 && (size_t)(k.n - 1) == tb->arr.size())
                    {
                        if (v.mrets && !v.bDots)
                        {
                            auto mrets = v.mrets;
                            v.mrets = nullptr;
                            tb->arr.push_back(v);
                            for (auto& m : *mrets)
                            {
                                Value em = m;
                                em.mrets = nullptr;
                                tb->arr.push_back(em);
                            }
                        }
                        else if (v.bDots && v.mrets)
                        {
                            for (auto& m : *v.mrets)
                            {
                                Value em = m;
                                em.mrets = nullptr;
                                tb->arr.push_back(em);
                            }
                        }
                        else
                        {
                            v.mrets = nullptr;
                            tb->arr.push_back(v);
                        }
                    }
                    else
                    {
                        v.mrets = nullptr;
                        if (v.t == Value::Null)
                            tb->map.erase(k.ToString());
                        else
                            tb->map[k.ToString()] = v;
                    }
                }
                while (!tb->arr.empty() && tb->arr.back().t == Value::Null)
                    tb->arr.pop_back();
                return Value::Table_(tb);
            }
            case Node::Class: return EvalClass(n, env);
            case Node::ExprStmt: return Eval(n->a, env);
            case Node::VarDecl: return Exec(n, env);
            default: return Value();
            }
        }

// ---- ToNum ----
double Interp::ToNum(const Value& v)
        {
            if (v.t == Value::Num) return v.n;
            if (v.t == Value::Pointer) return (double)(__int64)(INT_PTR)v.ptr;
            if (v.t == Value::Str)
            {
                wchar_t* e = nullptr;
                double d = wcstod(v.s.c_str(), &e);
                return d;
            }
            if (v.t == Value::Bool) return v.b ? 1 : 0;
            if (v.t == Value::TableT && v.tab)
            {
                if (IsSize64(v)) return (double)GetSize64Val(v);
                auto im = v.tab->map.find(L"_meta");
                if (im != v.tab->map.end() && im->second.t == Value::TableT && im->second.tab)
                {
                    auto it = im->second.tab->map.find(L"_tonumber");
                    if (it != im->second.tab->map.end() &&
                        (it->second.t == Value::Func || it->second.native))
                    {
                        vector<Value> noArgs;
                        return ToNum(CallOwner(it->second, v, noArgs));
                    }
                }
                auto iv = v.tab->map.find(L"value");
                if (iv != v.tab->map.end() && iv->second.t == Value::Num)
                    return iv->second.n;
            }
            return 0;
        }

        // ---- round M2: metatable machinery ----
        // a table/class instance may carry "_meta" (declared via '@path;').
        // read miss  -> _meta._get: function -> call(k) with owner=obj;
        //               table -> e=_get.k: {._get=fn} -> call fn() owner=obj;
        //               e function -> return e (owner binds at call time)
        // write miss -> _meta._set (same shapes, fn(v) / e._set(v))
        // table call -> _meta._call(...) owner=obj

// ---- MetaGet ----
bool Interp::MetaGet(const Value& obj, const Value& keyVal,
                     Value* out, bool* pErr, wstring* pErrMsg,
                     int ownerCallMode)
        {
            wstring key = ObjectKeyOf(keyVal);
            *pErr = false;
            static thread_local int s_metaDepth = 0;
            if (s_metaDepth > 40)
                return false;
            struct ScopeDepth { int& d; ScopeDepth(int& x): d(x){ d++; } ~ScopeDepth(){ d--; } } sd(s_metaDepth);
            if ((obj.t != Value::TableT && obj.t != Value::Class && obj.t != Value::Func && obj.t != Value::Pointer) || !obj.tab || key == L"_meta")
                return false;
            if ((obj.t == Value::Class || obj.t == Value::Func) && !obj.tab->bExplicitMeta)
                return false;
            // the meta chain can be nested: util.metaProperty instances
            // carry a class-annotation layer at _meta._meta (the
            // @_metaPrototype prototype) holding the bulk of the members
            for (Value cur = obj;;)
            {
                auto imc = cur.tab->map.find(L"_meta");
                if (imc == cur.tab->map.end() || imc->second.t != Value::TableT ||
                    !imc->second.tab)
                {
                    // class instances resolve @-annotation metas LATE: the
                    // meta may be assigned to the class after instantiation,
                    // so fall back to _class._meta — but ONLY when the class
                    // body actually used an @-annotation (@_meta sets
                    // _metaAnnotated on the instance); a bare `_meta = {...}`
                    // in a class namespace, as in web.rest.client, is NOT an
                    // instance metatable
                    auto icc = cur.tab->map.find(L"_metaAnnotated");
                    if (icc != cur.tab->map.end() &&
                        (icc->second.t == Value::Bool ||
                         icc->second.t == Value::Num))
                    {
                        auto icc2 = cur.tab->map.find(L"_class");
                        if (icc2 != cur.tab->map.end() &&
                            icc2->second.tab)
                        {
                            auto icm = icc2->second.tab->map.find(L"_meta");
                            if (icm != icc2->second.tab->map.end() &&
                                icm->second.t == Value::TableT &&
                                icm->second.tab)
                            {
                                bool hit = MetaGetLayer(obj, icm->second, keyVal,
                                                        out, pErr, pErrMsg,
                                                        ownerCallMode,
                                                        obj.tab && (g_metaDispatchActive.count(obj.tab.get()) > 0));
                                if (hit || *pErr)
                                    return hit;
                            }
                        }
                    }
                    break;
                }
                Value mtc = imc->second;
                bool hit = MetaGetLayer(obj, mtc, keyVal, out, pErr, pErrMsg,
                                        ownerCallMode,
                                        obj.tab && (g_metaDispatchActive.count(obj.tab.get()) > 0));
                if (hit || *pErr)
                    return hit;
                cur = mtc;
            }
            return false;
        }

bool Interp::MetaGet(const Value& obj, const wstring& key,
                     Value* out, bool* pErr, wstring* pErrMsg,
                     bool bOwnerCall)
        {
            return MetaGet(obj, Value::Str_(key), out, pErr, pErrMsg, bOwnerCall ? 1 : 0);
        }

bool Interp::MetaGetLayer(const Value& obj, const Value& mtv,
                          const Value& keyVal, Value* out, bool* pErr,
                          wstring* pErrMsg, int ownerCallMode, bool bInDispatch)
        {
            Table* mt = mtv.tab.get();
            wstring key = ObjectKeyOf(keyVal);
            // direct method/field entries in the meta table resolve first
            // (RECT._meta = { width = function(){...} } shape)
            auto idm = mt->map.find(key);
            if (idm != mt->map.end() && idm->second.t != Value::Null &&
                key != L"_get" && key != L"_set" && key != L"_call" &&
                key != L"_tostring")
            {
                if (idm->second.t == Value::TableT && idm->second.tab &&
                    !bInDispatch)
                {
                    // property-definition table ({ _get = fn, ... }) — a
                    // normal object read invokes the getter; the raw
                    // definition goes back only for the dispatcher itself
                    auto ig2 = idm->second.tab->map.find(L"_get");
                    if (ig2 != idm->second.tab->map.end() &&
                        (ig2->second.t == Value::Func ||
                         ig2->second.native))
                    {
                        Value vOwnerCall = (ownerCallMode == 1) ? Value::Bool_(true) :
                                           (ownerCallMode == 0) ? Value::Bool_(false) : Value();
                        vector<Value> gArgs{ vOwnerCall, idm->second };
                        Value r = CallVmFuncStatic(obj, ig2->second,
                                                   gArgs, pErr, pErrMsg);
                        *out = r;
                        return true;
                    }
                }
                *out = idm->second;
                return true;
            }
            auto ig = mt->map.find(L"_get");
            if (ig == mt->map.end() || ig->second.t == Value::Null)
                return false;
            Value g = ig->second;
            if (g.t == Value::TableT && g.tab)
            {
                auto ie = g.tab->map.find(key);
                if (ie == g.tab->map.end() || ie->second.t == Value::Null)
                    return false;
                Value e = ie->second;
                if (e.t == Value::TableT && e.tab && !bInDispatch)
                {
                    auto ig2 = e.tab->map.find(L"_get");
                    if (ig2 != e.tab->map.end() &&
                        (ig2->second.t == Value::Func ||
                         ig2->second.native))
                    {
                        Value vOwnerCall = (ownerCallMode == 1) ? Value::Bool_(true) :
                                           (ownerCallMode == 0) ? Value::Bool_(false) : Value();
                        vector<Value> gArgs{ vOwnerCall, e };
                        Value r = CallVmFuncStatic(obj, ig2->second,
                                                   gArgs, pErr, pErrMsg);
                        *out = r;
                        return true;
                    }
                }
                // functions = methods; raw values/definitions returned as-is
                *out = e;
                return true;
            }
            if (g.t == Value::Func || g.native)
            {
                static thread_local std::set<std::pair<Table*, wstring>> s_activeProps;
                bool bActiveThis = mtv.tab && (obj.tab == mtv.tab) && (g_metaDispatchActive.count(mtv.tab.get()) > 0);
                bool bActiveProp = obj.tab && (s_activeProps.count({obj.tab.get(), key}) > 0);
                if (bActiveThis || bActiveProp)
                    return false;   // util._get's own this[k]: go raw below
                // function form: g(k, ownerCall) — this is the meta table
                // itself (util.metaProperty's _get reads this[k]); the
                // object reaches the body via `owner`. While it runs,
                // inner this[k] lookups on this same meta table must see RAW definition tables
                g_metaDispatchDepth++;
                if (mtv.tab) g_metaDispatchActive.insert(mtv.tab.get());
                if (obj.tab) s_activeProps.insert({obj.tab.get(), key});
                Value vOwnerCall = (ownerCallMode == 1) ? Value::Bool_(true) :
                                   (ownerCallMode == 0) ? Value::Bool_(false) : Value();
                vector<Value> args{ keyVal, vOwnerCall };
                if (g.native)
                    args.push_back(obj); // native kernel getters receive
                                         // the receiver as args[3]
                Value r = CallThisOwner(mtv, obj, g, args, pErr, pErrMsg);
                if (obj.tab) s_activeProps.erase({obj.tab.get(), key});
                if (mtv.tab) g_metaDispatchActive.erase(mtv.tab.get());
                g_metaDispatchDepth--;
                *out = r;
                return true;
            }
            return false;
        }

bool Interp::MetaGetLayer(const Value& obj, const Value& mtv,
                          const wstring& key, Value* out, bool* pErr,
                          wstring* pErrMsg, bool bOwnerCall, bool bInDispatch)
        {
            return MetaGetLayer(obj, mtv, Value::Str_(key), out, pErr, pErrMsg,
                                bOwnerCall ? 1 : 0, bInDispatch);
        }

// ---- MetaSet ----
bool Interp::MetaSet(const Value& obj, const Value& keyVal,
                     const Value& val, bool* pErr, wstring* pErrMsg,
                     bool bOwnerAttr)
        {
            wstring key = ObjectKeyOf(keyVal);
            *pErr = false;
            if ((obj.t != Value::TableT && obj.t != Value::Class && obj.t != Value::Func && obj.t != Value::Pointer) || !obj.tab || key == L"_meta")
                return false;
            if ((obj.t == Value::Class || obj.t == Value::Func) && !obj.tab->bExplicitMeta)
                return false;
            // the meta chain can be nested: util.metaProperty instances
            // carry a class-annotation layer at _meta._meta (the
            // @_metaPrototype prototype) holding the bulk of the members
            for (Value cur = obj;;)
            {
                auto imc = cur.tab->map.find(L"_meta");
                if (imc == cur.tab->map.end() || imc->second.t != Value::TableT ||
                    !imc->second.tab)
                {
                    auto icc = cur.tab->map.find(L"_metaAnnotated");
                    if (icc != cur.tab->map.end() &&
                        (icc->second.t == Value::Bool ||
                         icc->second.t == Value::Num))
                    {
                        auto icc2 = cur.tab->map.find(L"_class");
                        if (icc2 != cur.tab->map.end() && icc2->second.tab)
                        {
                            auto icm = icc2->second.tab->map.find(L"_meta");
                            if (icm != icc2->second.tab->map.end() &&
                                icm->second.t == Value::TableT &&
                                icm->second.tab)
                            {
                                bool hit = MetaSetLayer(obj, icm->second, keyVal,
                                                        val, pErr, pErrMsg, bOwnerAttr);
                                if (hit || *pErr)
                                    return hit;
                            }
                        }
                    }
                    break;
                }
                Value mtc = imc->second;
                bool hit = MetaSetLayer(obj, mtc, keyVal, val, pErr, pErrMsg, bOwnerAttr);
                if (hit || *pErr)
                    return hit;
                cur = mtc;
            }
            return false;
        }

bool Interp::MetaSet(const Value& obj, const wstring& key,
                     const Value& val, bool* pErr, wstring* pErrMsg)
        {
            return MetaSet(obj, Value::Str_(key), val, pErr, pErrMsg, true);
        }

bool Interp::MetaSetLayer(const Value& obj, const Value& mtv,
                          const Value& keyVal, const Value& val,
                          bool* pErr, wstring* pErrMsg, bool bOwnerAttr)
        {
            Table* mt = mtv.tab.get();
            wstring key = ObjectKeyOf(keyVal);
            auto is = mt->map.find(L"_set");
            if (is == mt->map.end() || is->second.t == Value::Null)
                return false;
            Value s = is->second;
            if (s.t == Value::TableT && s.tab)
            {
                auto ie = s.tab->map.find(key);
                if (ie == s.tab->map.end() || ie->second.t == Value::Null)
                    return false;
                Value e = ie->second;
                if (e.t == Value::TableT && e.tab)
                {
                    auto is2 = e.tab->map.find(L"_set");
                    if (is2 != e.tab->map.end() &&
                        (is2->second.t == Value::Func ||
                         is2->second.native))
                    {
                        vector<Value> args{ val };
                        CallVmFuncStatic(obj, is2->second, args,
                                         pErr, pErrMsg);
                        return true;
                    }
                    return false;
                }
                if (e.t == Value::Func || e.native)
                {
                    vector<Value> args{ val };
                    CallVmFuncStatic(obj, e, args, pErr, pErrMsg);
                    return true;
                }
                return false;
            }
            if (s.t == Value::Func || s.native)
            {
                // function form: s(k,v,ownerAttr) — this is the meta table itself
                // (util.metaProperty's _set reads this[k]); while it runs,
                // inner this[k] lookups on this same meta table must see RAW definition tables
                static thread_local std::set<std::pair<Table*, wstring>> s_activeSetProps;
                bool bActiveThis = mtv.tab && (obj.tab == mtv.tab) && (g_metaDispatchActive.count(mtv.tab.get()) > 0);
                bool bActiveProp = obj.tab && (s_activeSetProps.count({obj.tab.get(), key}) > 0);
                if (bActiveThis || bActiveProp)
                    return false;
                g_metaDispatchDepth++;
                if (mtv.tab) g_metaDispatchActive.insert(mtv.tab.get());
                if (obj.tab) s_activeSetProps.insert({obj.tab.get(), key});
                vector<Value> args{ keyVal, val, Value::Bool_(bOwnerAttr) };
                if (s.native)
                {
                    // For native kernel setters (e.g. ComMeta), pass obj as args[3]
                    // signature: a[0]=key, a[1]=val, a[2]=ownerAttr, a[3]=obj
                    args.push_back(obj);
                }
                CallThisOwner(mtv, obj, s, args, pErr, pErrMsg);
                if (obj.tab) s_activeSetProps.erase({obj.tab.get(), key});
                if (mtv.tab) g_metaDispatchActive.erase(mtv.tab.get());
                g_metaDispatchDepth--;
                return true;
            }
            return false;
        }

bool Interp::MetaSetLayer(const Value& obj, const Value& mtv,
                          const wstring& key, const Value& val,
                          bool* pErr, wstring* pErrMsg)
        {
            return MetaSetLayer(obj, mtv, Value::Str_(key), val, pErr, pErrMsg, true);
        }

        // instance method shim: CallVmFunc with owner forced to obj

// ---- CallOwner ----
Value Interp::CallOwner(const Value& f, const Value& obj,
                        vector<Value>& args)
        {
            return CallThisOwner(f, obj, obj, args);
        }

// owner (dynamic `owner` keyword) and `this` (lexical, overridable) set
// independently — metatable function forms need this=meta table while
// owner stays the receiver object
Value Interp::CallThisOwner(const Value& f, const Value& ths,
                            const Value& owner, vector<Value>& args)
        {
            Value oldOwner = s_currentOwner;
            s_currentOwner = owner;
            if (f.native)
            {
                Value r = f.native(args);
                s_currentOwner = oldOwner;
                return r;
            }
            // a callable table (COM bound member, kernel proxy objects):
            // forward through its _meta._call with the table as owner
            if (f.t == Value::TableT && f.tab)
            {
                auto ic = f.tab->map.find(L"_meta");
                if (ic != f.tab->map.end() && ic->second.t == Value::TableT &&
                    ic->second.tab)
                {
                    auto icl = ic->second.tab->map.find(L"_call");
                    if (icl != ic->second.tab->map.end() &&
                        (icl->second.t == Value::Func || icl->second.native))
                    {
                        if (icl->second.native)
                            args.insert(args.begin(), f);
                        return CallOwner(icl->second, f, args);
                    }
                }
            }
            if (f.t == Value::Class)
            {
                Value r = InstantiateClass(f, args, owner, true);
                s_currentOwner = oldOwner;
                return r;
            }
            if (f.t != Value::Func || !f.fn)
            {
                (*err) = L"运行错误: 元表回调不是函数";
                return Value();
            }
            auto local = std::make_shared<Env>();
            local->self = local;
            // lexical upvalues: the captured defining frame chain replaces
            // the bare globals jump
            local->parent = f.envUp ? f.envUp : globals.self;
            local->nsTable = f.nsClosure;
            local->vars[L"owner"] = owner;
            // the callee frame, the receiver and the call arguments live
            // in C++ locals — the collector needs them as roots
            gcFrames.push_back(local);
            GcRootScope rs;
            rs.add(f);
            rs.add(owner);
            rs.add(ths);
            rs.addAll(args);
            if (f.thisVal)
                local->vars[L"this"] = *f.thisVal;
            else
                local->vars[L"this"] = ths;
            for (size_t i = 0; i < f.params.size(); i++)
            {
                if (i < args.size() && args[i].t != Value::Null)
                    local->vars[f.params[i]] = args[i];
                else if (f.pdefaults && i < f.pdefaults->size() &&
                         (*f.pdefaults)[i])
                    local->vars[f.params[i]] =
                        Eval((*f.pdefaults)[i], local.get());
                else if (i < args.size())
                    local->vars[f.params[i]] = args[i];
                else
                    local->vars[f.params[i]] = Value();
            }
            if (f.params.size() && f.params.back() == L"..." && f.fn)
            {
                if (args.size() > f.params.size() - 1)
                {
                    // keep the raw varargs in a shared vector so the bound
                    // `...` value can also carry mrets — `var a,b = ...`
                    // expands positionally while `var t = ...` still packs
                    auto vargs = std::make_shared<vector<Value>>();
                    auto tbV = std::make_shared<Table>();
                    for (size_t k = f.params.size() - 1; k < args.size(); k++)
                    {
                        vargs->push_back(args[k]);
                        tbV->arr.push_back(args[k]);
                    }
                    // scripts read varargs through `arg`, while the
                    // `...` spelling expands them at a call site — bind both
                    // names to the same table. An EMPTY table must read as
                    // null: library code guards recursion with `if(!...)`,
                    // and an empty table is truthy in the runtime.
                    Value vDots = tbV->arr.empty() ? Value()
                                                   : Value::Table_(tbV);
                    if (vDots.tab) { vDots.mrets = vargs; vDots.bDots = true; }
                    local->vars[L"..."] = vDots;
                    local->vars[L"arg"] = vDots;
                    // an explicit `arg` parameter (gdip.bitmap's ctor) must
                    // win over the vararg alias — restore it
                    for (size_t i = 0; i + 1 < f.params.size(); i++)
                        if (f.params[i] == L"arg" && i < args.size())
                            local->vars[L"arg"] = args[i];
                }
                else
                {
                    local->vars[L"..."] = Value();
                    local->vars[L"arg"] = Value();
                    for (size_t i = 0; i < f.params.size(); i++)
                        if (f.params[i] == L"arg" && i < args.size())
                            local->vars[L"arg"] = args[i];
                }
            }
            Value old = ret;
            int oldR = nReturn, oldB = nBreak, oldC = nContinue;
            nReturn = nBreak = nContinue = 0;
            ret = Value();
            ExecBlock(f.fn, local.get());
            Value r = (nReturn > 0) ? ret : Value();
            nReturn = oldR; nBreak = oldB; nContinue = oldC;
            ret = old;
            s_currentOwner = oldOwner;
            GcFrameCheck(gcFrames, local.get(), "callThisOwner");
            gcFrames.pop_back();
            CleanDeadEnv(local);
            return r;
        }

        // static trampoline used before Interp methods are declared

// ---- CallVmFuncStatic ----
Value Interp::CallVmFuncStatic(const Value& obj, const Value& f,
                                      vector<Value>& args, bool* pErr,
                                      wstring* pErrMsg)
        {
            Interp* pSelf = g_interpCurrent;
            if (!pSelf) { *pErr = true; *pErrMsg = L"无解释器"; return Value(); }
            Value r = pSelf->CallOwner(f, obj, args);
            *pErr = !pSelf->err->empty();
            if (*pErr) *pErrMsg = *pSelf->err;
            return r;
        }

Value Interp::CallThisOwner(const Value& ths, const Value& owner,
                            const Value& f, vector<Value>& args, bool* pErr,
                            wstring* pErrMsg)
        {
            Interp* pSelf = g_interpCurrent;
            if (!pSelf) { *pErr = true; *pErrMsg = L"无解释器"; return Value(); }
            Value r = pSelf->CallThisOwner(f, ths, owner, args);
            *pErr = !pSelf->err->empty();
            if (*pErr) *pErrMsg = *pSelf->err;
            return r;
        }

        // the metatable of a value: its _meta member, or its class
        // template's _meta for class instances

// ---- GetMeta ----
Value Interp::GetMeta(const Value& obj)
        {
            if (obj.t != Value::TableT || !obj.tab) return Value();
            auto im = obj.tab->map.find(L"_meta");
            if (im != obj.tab->map.end())
                return im->second;
            return Value();
        }

        // M2: string conversion honoring a metatable _tostring

// ---- ToStringM ----
wstring Interp::ToStringM(const Value& v)
        {
            if (v.t == Value::TableT && v.tab)
            {
                auto im = v.tab->map.find(L"_meta");
                if (im != v.tab->map.end() && im->second.t == Value::TableT &&
                    im->second.tab)
                {
                    auto it = im->second.tab->map.find(L"_tostring");
                    if (it != im->second.tab->map.end() &&
                        (it->second.t == Value::Func || it->second.native))
                    {
                        vector<Value> noArgs;
                        return CallOwner(it->second, v, noArgs).ToString();
                    }
                }
            }
            return v.ToString();
        }

// ---- ToStringMValue ----
// Same as ToStringM but keeps the result as a Value, so a string returned
// by a _tostring metamethod keeps its byte representation (official aardio
// strings are byte arrays).
Value Interp::ToStringMValue(const Value& v)
        {
            if (v.t == Value::TableT && v.tab)
            {
                auto im = v.tab->map.find(L"_meta");
                if (im != v.tab->map.end() && im->second.t == Value::TableT &&
                    im->second.tab)
                {
                    auto it = im->second.tab->map.find(L"_tostring");
                    if (it != im->second.tab->map.end() &&
                        (it->second.t == Value::Func || it->second.native))
                    {
                        vector<Value> noArgs;
                        return CallOwner(it->second, v, noArgs);
                    }
                }
            }
            return v;
        }

// ---- GetMember ----
Value Interp::GetMember(const Value& obj, const Value& keyVal,
                        bool bDirect, int ownerCallMode)
        {
            wstring key = ObjectKeyOf(keyVal);
            if (obj.t == Value::TableT || ((obj.t == Value::Func || obj.t == Value::Class || obj.t == Value::Pointer) && obj.tab))
            {
                if (obj.tab)
                {
                    // official semantics: a metaProperty attribute wins over
                    // an existing data member of the same name - DSG init
                    // table.assigns ALL attrs (items={}, checked=...) onto
                    // the ctrl, and later reads of e.g. combobox .items must
                    // still hit the live control, not the stale copied table
                    if (!bDirect && obj.tab->bExplicitMeta && (!obj.tab || g_metaDispatchActive.count(obj.tab.get()) == 0))
                    {
                        auto im0 = obj.tab->map.find(L"_meta");
                        if (im0 != obj.tab->map.end() &&
                            im0->second.t == Value::TableT && im0->second.tab)
                        {
                            bool hasMetaGet = false;
                            auto ia = im0->second.tab->map.find(key);
                            if (ia != im0->second.tab->map.end() &&
                                ia->second.t == Value::TableT && ia->second.tab)
                            {
                                auto ig = ia->second.tab->map.find(L"_get");
                                if (ig != ia->second.tab->map.end() &&
                                    (ig->second.t == Value::Func || ig->second.native))
                                    hasMetaGet = true;
                            }
                            if (hasMetaGet)
                            {
                                bool bErr = false;
                                wstring strErrM;
                                Value r;
                                if (MetaGet(obj, keyVal, &r, &bErr,
                                            &strErrM, ownerCallMode))
                                {
                                    if (r.t != Value::Null)
                                        return r;
                                    auto itRaw = obj.tab->map.find(key);
                                    if (itRaw != obj.tab->map.end() && itRaw->second.t != Value::Null)
                                        return itRaw->second;
                                    return r;
                                }
                                if (bErr)
                                {
                                    if (err->empty())
                                        (*err) = L"运行错误: " + strErrM;
                                    return Value();
                                }
                            }
                        }
                    }
                    auto it = obj.tab->map.find(key);
                    if (it != obj.tab->map.end())
                    {
                        if (ownerCallMode)
                        {
                            bool isCallable = false;
                            if (it->second.t == Value::Func || it->second.native || it->second.t == Value::Class)
                                isCallable = true;
                            else if (it->second.t == Value::TableT && it->second.tab)
                            {
                                auto imC = it->second.tab->map.find(L"_meta");
                                if (imC != it->second.tab->map.end() && imC->second.t == Value::TableT && imC->second.tab)
                                {
                                    auto icl = imC->second.tab->map.find(L"_call");
                                    if (icl != imC->second.tab->map.end() && (icl->second.t == Value::Func || icl->second.native))
                                        isCallable = true;
                                }
                            }
                            if (!isCallable && !bDirect)
                            {
                                bool bErr = false;
                                wstring strErrM;
                                Value r;
                                if (MetaGet(obj, keyVal, &r, &bErr, &strErrM, ownerCallMode))
                                {
                                    if (r.t != Value::Null)
                                        return r;
                                }
                            }
                        }
                        return it->second;
                    }
                    // 1-based numeric index = array access
                    if (!key.empty() &&
                        key.find_first_not_of(L"0123456789") == wstring::npos)
                    {
                        long long i = _wtoi64(key.c_str());
                        if (i >= 1 && (size_t)i <= obj.tab->arr.size())
                            return obj.tab->arr[(size_t)i - 1];
                    }
                    // Check if table represents a loaded DLL (_h member)
                    auto ih = obj.tab->map.find(L"_h");
                    if (ih != obj.tab->map.end() && (ih->second.t == Value::Num || ih->second.t == Value::Pointer))
                    {
                        HMODULE hMod = ih->second.t == Value::Pointer ? (HMODULE)ih->second.ptr : (HMODULE)(INT_PTR)(long long)ih->second.n;
                        if (hMod)
                        {
                            bool bCdecl = false;
                            auto ic = obj.tab->map.find(L"_cdecl");
                            if (ic != obj.tab->map.end() && ic->second.b) bCdecl = true;
                            bool bUni = false;
                            auto iu = obj.tab->map.find(L"_unicode");
                            if (iu != obj.tab->map.end() && iu->second.b) bUni = true;
                            Value fn = CreateDirectApiWrapper(hMod, key, bCdecl, bUni);
                            if (fn.t != Value::Null)
                            {
                                obj.tab->map[key] = fn;
                                return fn;
                            }
                        }
                    }
                    // M2: metatable _get fallback — skipped for t[["k"]]
                    if (bDirect)
                        return Value();
                    bool bErr = false;
                    wstring strErrM;
                    Value r;
                    if (MetaGet(obj, keyVal, &r, &bErr, &strErrM, ownerCallMode))
                        return r;
                    if (bErr)
                    {
                        if (err->empty())
                            (*err) = L"运行错误: " + strErrM;
                        return Value();
                    }
                }
                return Value();
            }
            if (obj.t == Value::Str)
            {
                if (!key.empty() && key.find_first_not_of(L"0123456789") == wstring::npos)
                {
                    long long i = _wtoi64(key.c_str());
                    std::string u8;
                    int len = WideCharToMultiByte(CP_UTF8, 0, obj.s.c_str(), (int)obj.s.size(), nullptr, 0, nullptr, nullptr);
                    if (len > 0)
                    {
                        u8.resize(len);
                        WideCharToMultiByte(CP_UTF8, 0, obj.s.c_str(), (int)obj.s.size(), &u8[0], len, nullptr, nullptr);
                    }
                    if (i >= 1 && (size_t)i <= u8.size())
                    {
                        return Value::Num_((unsigned char)u8[(size_t)i - 1]);
                    }
                }
                return Value();
            }
            // root namespaces visible on a null object (..string / module)
            if (obj.t == Value::Null)
            {
                auto it = roots.find(key);
                if (it != roots.end())
                    return it->second;
                // ::Name also reaches plain globals (::User32 etc.)
                auto ig = globals.vars.find(key);
                if (ig != globals.vars.end())
                    return ig->second;

                // bare :: system API (::SetWindowPointer, ::PostMessage, ...)
                // official aardio resolves these across the system DLLs;
                // without them the official win.ui control routing (which
                // installs each control's window procedure with
                // ::SetWindowPointer) never fires a single mouse event.
                {
                    Value api = ResolveSysApi(this, key);
                    if (api.t != Value::Null)
                        return api;
                }

                // On-demand load system / dynamic DLLs when accessed via ::DllName
                if (!key.empty() && key[0] != L'.' && key[0] != L'_')
                {
                    wstring dllName = key;
                    if (dllName.size() < 4 || _wcsicmp(dllName.c_str() + dllName.size() - 4, L".dll") != 0)
                        dllName += L".dll";

                    static const std::unordered_set<wstring> s_sysDlls = {
                        L"kernel32.dll", L"user32.dll", L"gdi32.dll", L"shell32.dll",
                        L"ole32.dll", L"oleaut32.dll", L"advapi32.dll", L"comctl32.dll",
                        L"shlwapi.dll", L"ntdll.dll", L"ws2_32.dll", L"wsock32.dll",
                        L"wininet.dll", L"msimg32.dll", L"crypt32.dll", L"dwmapi.dll",
                        L"uxtheme.dll", L"iphlpapi.dll", L"version.dll", L"psapi.dll",
                        L"winspool.drv", L"imm32.dll", L"comdlg32.dll", L"winmm.dll",
                        L"gdiplus.dll", L"msvfw32.dll", L"avicap32.dll", L"mpr.dll"
                    };

                    wstring lower = dllName;
                    for (auto& c : lower) c = towlower(c);
                    bool isSys = s_sysDlls.count(lower) > 0;

                    HMODULE hMod = isSys ? LoadLibraryW(dllName.c_str()) : GetModuleHandleW(dllName.c_str());
                    if (hMod)
                    {
                        if (isSys) FreeLibrary(hMod);
                        auto ir = roots.find(L"raw");
                        if (ir != roots.end() && ir->second.t == Value::TableT && ir->second.tab)
                        {
                            auto il = ir->second.tab->map.find(L"loadDll");
                            if (il != ir->second.tab->map.end() && il->second.native)
                            {
                                vector<Value> da{ Value::Str_(dllName) };
                                Value dv = il->second.native(da);
                                if (dv.t == Value::TableT && dv.tab)
                                {
                                    roots[key] = dv;
                                    globals.vars[key] = dv;
                                    return dv;
                                }
                            }
                        }
                    }
                }

                static bool bDbgR =
                    GetEnvironmentVariableW(L"CMT", nullptr, 0) != 0;
                if (bDbgR)
                {
                    fwprintf(stderr, L"[ROOTMISS] key=%s roots=%d globals=%d\n",
                             key.c_str(), (int)roots.count(key),
                             (int)globals.vars.count(key));
                    fflush(stderr);
                }
            }
            return Value();
        }

Value Interp::GetMember(const Value& obj, const wstring& key,
                        bool bDirect, bool bOwnerCall)
        {
            return GetMember(obj, Value::Str_(key), bDirect, bOwnerCall ? 1 : 0);
        }

// ---- InstantiateClass ----
Value Interp::InstantiateClass(const Value& f, vector<Value>& args,
                               const Value& vThis, bool bMethod)
        {
            auto inst = std::make_shared<Table>();
            if (f.tab)
            {
                auto itS = f.tab->map.find(L"_struct");
                if (itS != f.tab->map.end())
                    inst->map[L"_struct"] = itS->second;
                inst->map[L"_class"] = f;
            }
            Value vInst = Value::Table_(inst);
            if (!f.classBody && !f.fn && f.tab)
            {
                auto itNC = f.tab->map.find(L"ctor");
                if (itNC != f.tab->map.end() &&
                    itNC->second.t == Value::Func && itNC->second.native)
                {
                    vector<Value> nargs = args;
                    nargs.push_back(vInst);
                    Value rN = itNC->second.native(nargs);
                    if (rN.t == Value::TableT && rN.tab)
                        return rN;
                    if (rN.t == Value::Null)
                        return Value();
                    return rN;
                }
            }
            auto classNsEnv = std::make_shared<Env>();
            classNsEnv->self = classNsEnv;
            auto definingNsEnv = std::make_shared<Env>();
            definingNsEnv->self = definingNsEnv;
            if (f.tab)
            {
                classNsEnv->nsTable = f.tab;
                classNsEnv->vars[L"self"] = f;
            }
            shared_ptr<Env> lexicalParent = f.envUp ? f.envUp : globals.self;
            if (f.nsClosure)
            {
                definingNsEnv->parent = lexicalParent;
                definingNsEnv->nsTable = f.nsClosure;
                classNsEnv->parent = definingNsEnv;
            }
            else
                classNsEnv->parent = lexicalParent;
            auto local = std::make_shared<Env>();
            local->self = local;
            local->parent = f.tab ? classNsEnv
                                  : (f.nsClosure ? definingNsEnv
                                                 : lexicalParent);
            local->nsTable = inst;
            local->bNsInst = true;   // instance scope: never capture as nsClosure
            local->bInstInit = true;
            local->instTable = inst.get();   // the instance under construction
            if (bMethod)
                local->vars[L"owner"] = vThis;   // owner = dot-left
            local->vars[L"this"] = vInst;
            // the instance frame is a GC root while the constructor runs
            gcFrames.push_back(local);
            GcRootScope rsInst;
            rsInst.add(vInst);
            rsInst.add(f);
            rsInst.addAll(args);

            if (f.fn)
            {
                local->vars[L"this"] = vInst;
                for (size_t i = 0; i < f.fn->names.size(); i++)
                {
                    if (i < args.size() && args[i].t != Value::Null)
                        local->vars[f.fn->names[i]] = args[i];
                    else if (f.pdefaults &&
                             i < f.pdefaults->size() &&
                             (*f.pdefaults)[i])
                        local->vars[f.fn->names[i]] =
                            Eval((*f.pdefaults)[i], local.get());
                    else if (i < args.size())
                        local->vars[f.fn->names[i]] = args[i];
                    else
                        local->vars[f.fn->names[i]] = Value();
                }
                if (f.fn->names.size() && f.fn->names.back() == L"...")
                {
                    if (args.size() > f.fn->names.size() - 1)
                    {
                        auto vargs = std::make_shared<vector<Value>>();
                        auto tbV = std::make_shared<Table>();
                        for (size_t k = f.fn->names.size() - 1; k < args.size(); k++)
                        {
                            vargs->push_back(args[k]);
                            tbV->arr.push_back(args[k]);
                        }
                        Value vDots = tbV->arr.empty() ? Value()
                                                       : Value::Table_(tbV);
                        if (vDots.tab) { vDots.mrets = vargs; vDots.bDots = true; }
                        local->vars[L"..."] = vDots;
                        local->vars[L"arg"] = vDots;
                        for (size_t i = 0; i + 1 < f.fn->names.size(); i++)
                            if (f.fn->names[i] == L"arg" && i < args.size())
                                local->vars[L"arg"] = args[i];
                    }
                    else
                    {
                        local->vars[L"..."] = Value();
                        local->vars[L"arg"] = Value();
                        for (size_t i = 0; i < f.fn->names.size(); i++)
                            if (f.fn->names[i] == L"arg" && i < args.size())
                                local->vars[L"arg"] = args[i];
                    }
                }
                Value oldRet = ret;
                int oldR = nReturn, oldB = nBreak, oldC = nContinue;
                nReturn = nBreak = nContinue = 0;
                ret = Value();
                ExecBlock(f.fn->a, local.get());
                bool bCtorRet = (nReturn > 0);
                Value ctorRet = ret;
                nReturn = oldR; nBreak = oldB; nContinue = oldC;
                ret = oldRet;
                local->bInstInit = false;
                local->instTable = nullptr;
                if (bCtorRet)
                {
                    if (ctorRet.t == Value::Null)
                    {
                        GcFrameCheck(gcFrames, local.get(), "classNullCtor");
                        gcFrames.pop_back();
                        CleanDeadEnv(local);
                        return ctorRet;
                    }
                    if (ctorRet.t == Value::TableT && ctorRet.tab && vInst.tab)
                    {
                        for (auto& kv : vInst.tab->map)
                        {
                            if (ctorRet.tab->map.find(kv.first) == ctorRet.tab->map.end())
                                ctorRet.tab->map[kv.first] = kv.second;
                        }
                    }
                    GcFrameCheck(gcFrames, local.get(), "classCtor");
                    gcFrames.pop_back();
                    CleanDeadEnv(local);
                    return ctorRet;
                }
                auto itThis = local->vars.find(L"this");
                if (itThis != local->vars.end() &&
                    itThis->second.t == Value::TableT)
                {
                    if (itThis->second.tab != vInst.tab && vInst.tab)
                    {
                        auto rebindVal = [&](Value& val) {
                            if (val.t == Value::Func && val.thisVal &&
                                val.thisVal->tab == vInst.tab)
                            {
                                *val.thisVal = itThis->second;
                            }
                        };
                        for (auto& kv : vInst.tab->map)
                        {
                            rebindVal(kv.second);
                            if (itThis->second.tab->map.find(kv.first) == itThis->second.tab->map.end())
                                itThis->second.tab->map[kv.first] = kv.second;
                        }
                        for (auto& kv : itThis->second.tab->map)
                        {
                            rebindVal(kv.second);
                        }
                        for (auto& kv : local->vars)
                        {
                            rebindVal(kv.second);
                        }
                        local->nsTable = itThis->second.tab;
                    }
                    vInst = itThis->second;
                }
                if (vInst.tab && f.tab)
                {
                    auto itCls = f.tab->map.find(L"_struct");
                    if (itCls != f.tab->map.end() &&
                        itCls->second.t == Value::Str)
                    {
                        auto itI = vInst.tab->map.find(L"_struct");
                        wstring strCls = itCls->second.s;
                        wstring strInst = (itI != vInst.tab->map.end() &&
                                           itI->second.t == Value::Str)
                                              ? itI->second.s : L"";
                        while (!strInst.empty() && strInst.back() == L';')
                            strInst.pop_back();
                        size_t pos = 0;
                        while (pos < strCls.size())
                        {
                            size_t semi = strCls.find(L';', pos);
                            if (semi == wstring::npos)
                                semi = strCls.size();
                            wstring item = strCls.substr(pos, semi - pos);
                            pos = semi + 1;
                            size_t sp = item.find_first_of(L" \t");
                            if (sp == wstring::npos) continue;
                            wstring name = item.substr(sp + 1);
                            size_t lb = name.find(L'[');
                            if (lb != wstring::npos)
                                name = name.substr(0, lb);
                            while (!name.empty() &&
                                   (name.back() == L' ' || name.back() == L'\t'))
                                name.pop_back();
                            if (name.empty()) continue;
                            if (strInst.find(name) != wstring::npos)
                                continue;
                            if (!strInst.empty())
                                strInst += L";";
                            strInst += item;
                        }
                        if (!strInst.empty()) strInst += L";";
                        vInst.tab->map[L"_struct"] = Value::Str_(strInst);
                    }
                }
            }

            if (f.classBody)
            {
                local->vars[L"this"] = vInst;
                Value oldRet = ret;
                int oldR = nReturn, oldB = nBreak, oldC = nContinue;
                nReturn = nBreak = nContinue = 0;
                local->bClassBody = true;
                ExecBlock(f.classBody, local.get());
                local->bClassBody = false;
                nReturn = oldR; nBreak = oldB; nContinue = oldC;
                ret = oldRet;
                auto itThis = local->vars.find(L"this");
                if (itThis != local->vars.end() &&
                    itThis->second.t == Value::TableT)
                    vInst = itThis->second;
            }
            GcFrameCheck(gcFrames, local.get(), "classBody");
            gcFrames.pop_back();
            CleanDeadEnv(local);
            return vInst;
        }

// ---- EvalCall ----
Value Interp::EvalCall(const PNode& n, Env* env)
        {
            // builtin fast paths: io.print / print
            if (n->a->k == Node::Member && n->a->str == L"print" &&
                n->a->a->k == Node::Ident && n->a->a->str == L"io")
            {
                // strings are byte arrays: write the bytes, not re-encoded
                // text (a raw byte string must survive untouched).
                std::string out;
                for (auto& arg : n->list)
                    out += ValueByteForm(ToStringMValue(Eval(arg, env)));
                out += "\n";
                fwrite(out.data(), 1, out.size(), stdout);
                fflush(stdout);
                return Value();
            }
            if (n->a->k == Node::Ident && n->a->str == L"print")
            {
                Value* vShadow = env ? env->Find(L"print") : nullptr;
                auto itGlob = globals.vars.find(L"print");
                Value* pGlob = (itGlob != globals.vars.end()) ? &itGlob->second : nullptr;
                // Only take the global console print fast path if not shadowed by a local or namespace member
                if (!vShadow || vShadow == pGlob)
                {
                    std::string out;
                    for (auto& arg : n->list)
                        out += ValueByteForm(ToStringMValue(Eval(arg, env)));
                    out += "\n";
                    fwrite(out.data(), 1, out.size(), stdout);
                    fflush(stdout);
                    return Value();
                }
            }
            if (n->a->k == Node::Ident && n->a->str == L"tostring")
            {
                if (n->list.empty()) return Value();
                Value v = Eval(n->list[0], env);
                vector<Value> extraArgs;
                for (size_t ai = 1; ai < n->list.size(); ai++)
                    extraArgs.push_back(Eval(n->list[ai], env));
                if (v.t == Value::TableT && v.tab)
                {
                    auto im = v.tab->map.find(L"_meta");
                    if (im != v.tab->map.end() && im->second.t == Value::TableT && im->second.tab)
                    {
                        auto it = im->second.tab->map.find(L"_tostring");
                        if (it != im->second.tab->map.end() && (it->second.t == Value::Func || it->second.native))
                        {
                            return CallOwner(it->second, v, extraArgs);
                        }
                    }
                }
                // A string is returned unchanged (official strings are byte
                // arrays), so its byte representation survives the call.
                if (v.t == Value::Str) return v;
                return Value::Str_(ToStringM(v));
            }
            if (n->a->k == Node::Ident && n->a->str == L"tonumber" && n->list.size() == 1)
            {
                Value v0 = Eval(n->list[0], env);
                if (v0.t == Value::Num) return v0;
            }
            if (n->a->k == Node::Ident && n->a->str == L"error")
            {
                // error(): raise a runtime error message.  The location is
                // NOT baked into the message here: the report points (the
                // try/catch handler and the top-level report) append it, so
                // a message that already carries a location is never
                // decorated twice.
                Value vErr = n->list.empty() ? Value() : Eval(n->list[0], env);
                wstring msg = n->list.empty() ? L"运行错误" :
                    L"运行错误: " + vErr.ToString();
                (*err) = msg;
                return Value();
            }
            if (n->a->k == Node::Ident && n->a->str == L"sleep")
            {
                if (!n->list.empty())
                    Sleep((DWORD)ToNum(Eval(n->list[0], env)));
                return Value();
            }
            Value f;
            Value vThis;
            bool bMethod = false;
            // the callee and its receiver live in C++ locals across the
            // call — keep them reachable for the collector
            GcRootScope rs;
            if (n->a->k == Node::Member)
            {
                if (n->a->bRoot)
                {
                    f = GetMember(Value(), n->a->str, false, /*bOwnerCall=*/false);
                }
                else
                {
                    bMethod = true;
                    vThis = Eval(n->a->a, env);
                    f = GetMember(vThis, n->a->str, false, /*bOwnerCall=*/true);
                }
            }
            else if (n->a->k == Node::Index)
            {
                bMethod = true;
                vThis = Eval(n->a->a, env);
                Value idx = Eval(n->a->b, env);
                f = GetMember(vThis, idx.ToString(), n->a->bDirect, /*bOwnerCall=*/false);
            }
            else
            {
                f = Eval(n->a, env);
            }
            rs.add(f);
            rs.add(vThis);
            static thread_local int s_callDepth = 0;
            struct CallDepthGuard {
                int& d;
                CallDepthGuard(int& x) : d(x) { d++; }
                ~CallDepthGuard() { d--; }
            } cdg(s_callDepth);
            if (s_callDepth > 500)
            {
                // depth-guard contract: runaway recursion surfaces a
                // catchable 运行错误 (栈溢出) instead of an
                // uncatchable OS EXCEPTION_STACK_OVERFLOW. 500
                // frames fit the host's 16 MB reserve (~14 KB C++ stack
                // per call); the guard must stay well below the
                // physical crash point.
                wstring fnName = f.fname;
                if (fnName.empty() && n->a->k == Node::Member) fnName = n->a->str;
                else if (fnName.empty() && n->a->k == Node::Ident) fnName = n->a->str;
                // S-2: no location here — the report point appends the
                // file and the line, so it is never written twice.
                (*err) = L"运行错误: 栈溢出 (超过最大调用深度) 函数: " + fnName;
                return Value();
            }
            {
                static bool bDbg = GetEnvironmentVariableW(L"CMT", nullptr, 0) != 0;
                if (bDbg && (n->line == 113 || n->line == 7))
                {
                    fwprintf(stderr, L"[DEBUG CALL %d] f.t=%d bMethod=%d vThis.t=%d fn=%s\n",
                             n->line, (int)f.t, bMethod, (int)vThis.t, f.fname.c_str());
                }
            }
            auto evalCallArgs = [this, env, &rs](const vector<PNode>& exprs) {
                vector<Value> res;
                // every argument is rooted as soon as it is evaluated: a
                // later argument may run statements, and the collector
                // must not reclaim an earlier one
                auto keep = [&res, &rs](const Value& v) {
                    res.push_back(v);
                    rs.add(res.back());
                };
                for (size_t ai = 0; ai < exprs.size(); ai++)
                {
                    auto& a = exprs[ai];
                    if (a->k == Node::Ident && a->str == L"...")
                    {
                        Value vDots = Eval(a, env);
                        if (vDots.bDots && vDots.mrets)
                        {
                            for (auto& item : *vDots.mrets)
                            {
                                Value elem = item;
                                elem.mrets = nullptr;
                                keep(elem);
                            }
                        }
                        else if (vDots.t == Value::TableT && vDots.tab)
                        {
                            for (auto& item : vDots.tab->arr)
                            {
                                Value elem = item;
                                elem.mrets = nullptr;
                                keep(elem);
                            }
                        }
                        else if (vDots.t != Value::Null)
                        {
                            Value elem = vDots;
                            elem.mrets = nullptr;
                            keep(elem);
                        }
                    }
                    else
                    {
                        Value v = Eval(a, env);
                        // a multi-return call in the LAST argument
                        // position expands into the parameter list —
                        // `assert2(2, resume(fib))` must receive (2, true,
                        // yielded) so the fiber shim's assert2 sees the
                        // yielded value
                        if (ai + 1 == exprs.size() && v.mrets)
                        {
                            auto mrets = v.mrets;
                            v.mrets = nullptr;
                            keep(v);
                            for (auto& extra : *mrets)
                            {
                                Value ex = extra;
                                ex.mrets = nullptr;
                                keep(ex);
                            }
                        }
                        else
                        {
                            v.mrets = nullptr;
                            keep(v);
                        }
                    }
                }
                return res;
            };
            if (f.t == Value::Class)
            {
                vector<Value> args = evalCallArgs(n->list);
                return InstantiateClass(f, args, vThis, bMethod);
            }
            if (f.native)
            {
                vector<Value> args = evalCallArgs(n->list);
                Value oldOwner = s_currentOwner;
                s_currentOwner = bMethod ? vThis : Value();
                Value r = f.native(args);
                s_currentOwner = oldOwner;
                return r;
            }
            // M2: calling a table -> metatable _call (owner = the table)
            if (f.t == Value::TableT && f.tab)
            {
                auto ic = f.tab->map.find(L"_meta");
                if (ic != f.tab->map.end() && ic->second.t == Value::TableT &&
                    ic->second.tab)
                {
                    auto icl = ic->second.tab->map.find(L"_call");
                    if (icl != ic->second.tab->map.end() &&
                        (icl->second.t == Value::Func ||
                         icl->second.native))
                    {
                        vector<Value> args = evalCallArgs(n->list);
                        if (icl->second.native)
                            args.insert(args.begin(), f); // native _call
                                                          // receives the
                                                          // table as a[0]
                        return CallOwner(icl->second, f, args);
                    }
                }
            }
            // M2: calling a native callback object (the official "cdata"
            // returned by raw.tostdcall) -> metatable _call
            if (f.t == Value::Pointer && f.tab)
            {
                auto ic = f.tab->map.find(L"_meta");
                if (ic != f.tab->map.end() && ic->second.t == Value::TableT &&
                    ic->second.tab)
                {
                    auto icl = ic->second.tab->map.find(L"_call");
                    if (icl != ic->second.tab->map.end() &&
                        (icl->second.t == Value::Func ||
                         icl->second.native))
                    {
                        vector<Value> args = evalCallArgs(n->list);
                        if (icl->second.native)
                            args.insert(args.begin(), f); // native _call
                                                          // receives the
                                                          // cdata as a[0]
                        return CallOwner(icl->second, f, args);
                    }
                }
            }
            if (f.t != Value::Func)
            {
                // name the callee: without it a missing builtin is an
                // untraceable "尝试调用非函数值" buried in library code
                wstring strCallee;
                if (n->a->k == Node::Ident)
                    strCallee = n->a->str;
                else if (n->a->k == Node::Member)
                    strCallee = L"." + n->a->str;
                else if (n->a->k == Node::Index && n->a->b &&
                         n->a->b->k == Node::Str)
                    strCallee = L"[\"" + n->a->b->str + L"\"]";
                (*err) = L"运行错误: 尝试调用非函数值" +
                         (strCallee.empty() ? L""
                                            : (L" (" + strCallee + L")"));
                return Value();
            }
            vector<Value> args = evalCallArgs(n->list);
            auto local = std::make_shared<Env>();
            local->self = local;
            // functions see globals AND the defining frame chain
            // (upvalues). Plain functions capture only globals; nested
            // functions capture their enclosing frame via envUp.
            local->parent = f.envUp ? f.envUp : globals.self;
            local->nsTable = f.nsClosure;   // defining namespace stays in scope
            // the callee frame is a GC root while the body runs
            gcFrames.push_back(local);
            if (bMethod)
            {
                if (f.thisVal)
                    local->vars[L"this"] = *f.thisVal;
                else
                    local->vars[L"this"] = vThis;   // obj.fn() receiver as this
                // owner = the dot-left object of this call
                local->vars[L"owner"] = vThis;
            }
            else
            {
                if (f.thisVal)
                    local->vars[L"this"] = *f.thisVal;
                if (f.ownerVal)
                    local->vars[L"owner"] = *f.ownerVal;
                else
                    local->vars[L"owner"] = Value();
            }
            for (size_t i = 0; i < f.params.size(); i++)
            {
                if (i < args.size() && args[i].t != Value::Null)
                    local->vars[f.params[i]] = args[i];
                else if (f.pdefaults &&
                         i < f.pdefaults->size() &&
                         (*f.pdefaults)[i])
                    local->vars[f.params[i]] =
                        Eval((*f.pdefaults)[i], local.get());
                else if (i < args.size())
                    local->vars[f.params[i]] = args[i];
                else
                    local->vars[f.params[i]] = Value();
            }
            // "..." varargs: leftover arguments as a 1-based table.
            // Spelled `arg` in scripts and `...` at call sites, so
            // bind both names.
            if (f.params.size() && f.params.back() == L"..." &&
                f.fn)
            {
                auto vargs = std::make_shared<vector<Value>>();
                auto tbV = std::make_shared<Table>();
                for (size_t k = f.params.size() - 1; k < args.size(); k++)
                {
                    vargs->push_back(args[k]);
                    tbV->arr.push_back(args[k]);
                }
                // An EMPTY varargs table must read as null: library code
                // guards recursion with `if(!...)`, and an empty table is
                // truthy in the runtime.
                Value vDots = tbV->arr.empty() ? Value()
                                               : Value::Table_(tbV);
                if (vDots.tab) { vDots.mrets = vargs; vDots.bDots = true; }
                local->vars[L"..."] = vDots;
                local->vars[L"arg"] = vDots;
                for (size_t i = 0; i + 1 < f.params.size(); i++)
                    if (f.params[i] == L"arg" && i < args.size())
                        local->vars[L"arg"] = args[i];
            }
            Value old = ret;
            int oldR = nReturn;
            nReturn = 0;
            ret = Value();
            ExecBlock(f.fn, local.get());
            Value r = (nReturn > 0) ? ret : Value();
            nReturn = oldR;
            ret = old;
            GcFrameCheck(gcFrames, local.get(), "evalCall");
            gcFrames.pop_back();
            CleanDeadEnv(local);
            return r;
        }

// ---- EvalBin ----
Value Interp::EvalBin(const PNode& n, Env* env)
        {
            const wstring& op = n->str;
            // operands live in C++ locals across the evaluation of the
            // other side — keep them reachable for the collector
            GcRootScope rs;
            if (op == L"?:")   // lazy ternary
                return Eval(n->a, env).Truthy() ? Eval(n->b, env)
                                                : Eval(n->c, env);
            if (op == L"?")    // guard: a ? b — a truthy ? b : false
            {
                Value l = Eval(n->a, env);
                rs.add(l);
                return l.Truthy() ? Eval(n->b, env) : Value::Bool_(0);
            }
            if (op == L"&&")
            {
                Value l = Eval(n->a, env);
                rs.add(l);
                return l.Truthy() ? Eval(n->b, env) : l;
            }
            if (op == L":")   // falsy coalescing: a:b returns b when
                             // a is falsy (null/false/0/""), else a
            {
                Value l = Eval(n->a, env);
                rs.add(l);
                return l.Truthy() ? l : Eval(n->b, env);
            }
            if (op == L"||")
            {
                Value l = Eval(n->a, env);
                rs.add(l);
                return l.Truthy() ? l : Eval(n->b, env);
            }
            Value l = Eval(n->a, env);
            rs.add(l);
            Value r = Eval(n->b, env);
            rs.add(r);
            if (l.t == Value::Num && r.t == Value::Num)
            {
                if (op == L"+") return Value::Num_(l.n + r.n);
                if (op == L"-") return Value::Num_(l.n - r.n);
                if (op == L"*") return Value::Num_(l.n * r.n);
                if (op == L"/") return Value::Num_(r.n != 0 ? l.n / r.n : 0);
                if (op == L"<") return Value::Bool_(l.n < r.n);
                if (op == L"<=") return Value::Bool_(l.n <= r.n);
                if (op == L">") return Value::Bool_(l.n > r.n);
                if (op == L">=") return Value::Bool_(l.n >= r.n);
                if (op == L"==" || op == L"===") return Value::Bool_(l.n == r.n);
                if (op == L"!=" || op == L"!==") return Value::Bool_(l.n != r.n);
                if (op == L"%" || op == L"%=") return Value::Num_(r.n != 0 ? fmod(l.n, r.n) : 0);
                unsigned int ua = (unsigned int)(INT_PTR)(long long)l.n;
                unsigned int ub = (unsigned int)(INT_PTR)(long long)r.n;
                if (op == L"&") return Value::Num_((double)(int)(ua & ub));
                if (op == L"|") return Value::Num_((double)(int)(ua | ub));
                if (op == L"^") return Value::Num_((double)(int)(ua ^ ub));
                if (op == L"<<") return Value::Num_((double)(int)(ua << (ub & 31)));
                if (op == L">>") return Value::Num_((double)(int)(ua >> (ub & 31)));
                if (op == L">>>") return Value::Num_((double)((long long)(ua >> (ub & 31))));
                if (op == L"**") return Value::Num_(pow(l.n, r.n));
            }
            if (op == L"===")
                return Value::Bool_(ValuesEqual(l, r, true));
            if (op == L"!==")
                return Value::Bool_(!ValuesEqual(l, r, true));
            if (op == L"==")
                return Value::Bool_(ValuesEqual(l, r, false));
            if (op == L"!=")
                return Value::Bool_(!ValuesEqual(l, r, false));
            if (op == L"..")
            {
                if (l.t == Value::TableT || r.t == Value::TableT)
                    return l;   // table concat quirk; keep simple
                return ConcatStrings(l, r);
            }
            if (op == L"++")
                return ConcatStrings(l, r);  // string concat
            // '+' concatenates whenever either operand is a
            // non-numeric string, or a string meets a non-string.
            // A numeric string plus a number stays arithmetic ('1 + "2"' -> 3).
            if (op == L"+")
            {
                bool bLStr = l.t == Value::Str, bRStr = r.t == Value::Str;
                if (bLStr || bRStr)
                {
                    auto numeric = [](const wstring& s) -> bool {
                        if (s.empty()) return false;
                        wchar_t* e = nullptr;
                        wcstod(s.c_str(), &e);
                        return e && !*e;
                    };
                    bool bLN = bLStr && numeric(l.s);
                    bool bRN = bRStr && numeric(r.s);
                    // arithmetic when both sides are numeric (a number, or
                    // a fully numeric string); concat otherwise
                    bool bLNum = !bLStr || bLN;
                    bool bRNum = !bRStr || bRN;
                    if (!(bLNum && bRNum))
                        return ConcatStrings(l, r);
                }
            }
            if (op == L"+" || op == L"-" || op == L"*" || op == L"/" || op == L"%" || op == L"%=" ||
                op == L"<<" || op == L">>" || op == L"<" || op == L"<=" || op == L">" || op == L">=")
            {
                wstring metaOp;
                if (op == L"+") metaOp = L"_add";
                else if (op == L"-") metaOp = L"_sub";
                else if (op == L"*") metaOp = L"_mul";
                else if (op == L"/") metaOp = L"_div";
                else if (op == L"%" || op == L"%=") metaOp = L"_mod";
                else if (op == L"<<") metaOp = L"_lshift";
                else if (op == L">>") metaOp = L"_rshift";
                else if (op == L"<") metaOp = L"_lt";
                else if (op == L"<=") metaOp = L"_le";

                if (!metaOp.empty())
                {
                    if (l.t == Value::TableT && l.tab)
                    {
                        auto im = l.tab->map.find(L"_meta");
                        if (im != l.tab->map.end() && im->second.t == Value::TableT && im->second.tab)
                        {
                            auto ifn = im->second.tab->map.find(metaOp);
                            if (ifn != im->second.tab->map.end() && (ifn->second.t == Value::Func || ifn->second.native))
                            {
                                vector<Value> args{ r };
                                return CallOwner(ifn->second, l, args);
                            }
                        }
                    }
                    if (r.t == Value::TableT && r.tab)
                    {
                        auto im = r.tab->map.find(L"_meta");
                        if (im != r.tab->map.end() && im->second.t == Value::TableT && im->second.tab)
                        {
                            auto ifn = im->second.tab->map.find(metaOp);
                            if (ifn != im->second.tab->map.end() && (ifn->second.t == Value::Func || ifn->second.native))
                            {
                                vector<Value> args{ l };
                                return CallOwner(ifn->second, r, args);
                            }
                        }
                    }
                }
                if (op == L">")
                {
                    if (r.t == Value::TableT && r.tab)
                    {
                        auto im = r.tab->map.find(L"_meta");
                        if (im != r.tab->map.end() && im->second.t == Value::TableT && im->second.tab)
                        {
                            auto ifn = im->second.tab->map.find(L"_lt");
                            if (ifn != im->second.tab->map.end() && (ifn->second.t == Value::Func || ifn->second.native))
                            {
                                vector<Value> args{ l };
                                return CallOwner(ifn->second, r, args);
                            }
                        }
                    }
                    if (l.t == Value::TableT && l.tab)
                    {
                        auto im = l.tab->map.find(L"_meta");
                        if (im != l.tab->map.end() && im->second.t == Value::TableT && im->second.tab)
                        {
                            auto ifn = im->second.tab->map.find(L"_le");
                            if (ifn != im->second.tab->map.end() && (ifn->second.t == Value::Func || ifn->second.native))
                            {
                                vector<Value> args{ r };
                                return Value::Bool_(!CallOwner(ifn->second, l, args).Truthy());
                            }
                        }
                    }
                }
                if (op == L">=")
                {
                    if (l.t == Value::TableT && l.tab)
                    {
                        auto im = l.tab->map.find(L"_meta");
                        if (im != l.tab->map.end() && im->second.t == Value::TableT && im->second.tab)
                        {
                            auto ifn = im->second.tab->map.find(L"_le");
                            if (ifn != im->second.tab->map.end() && (ifn->second.t == Value::Func || ifn->second.native))
                            {
                                vector<Value> args{ r };
                                return CallOwner(ifn->second, l, args);
                            }
                            auto ifnlt = im->second.tab->map.find(L"_lt");
                            if (ifnlt != im->second.tab->map.end() && (ifnlt->second.t == Value::Func || ifnlt->second.native))
                            {
                                vector<Value> args{ r };
                                return Value::Bool_(!CallOwner(ifnlt->second, l, args).Truthy());
                            }
                        }
                    }
                }
            }
            double a = ToNum(l), b = ToNum(r);
            if (op == L"+") return Value::Num_(a + b);
            if (op == L"-") return Value::Num_(a - b);
            if (op == L"*") return Value::Num_(a * b);
            if (op == L"/") return Value::Num_(b != 0 ? a / b : 0);
            if (op == L"%" || op == L"%=") return Value::Num_(b != 0 ? fmod(a, b) : 0);
            // bitwise (32-bit width, DWORD semantics)
            unsigned int ua = (unsigned int)(INT_PTR)(long long)a;
            unsigned int ub = (unsigned int)(INT_PTR)(long long)b;
            if (op == L"&") return Value::Num_((double)(int)(ua & ub));
            if (op == L"|") return Value::Num_((double)(int)(ua | ub));
            if (op == L"^") return Value::Num_((double)(int)(ua ^ ub));   // ^ is XOR
            if (op == L"<<") return Value::Num_((double)(int)(ua << (ub & 31)));
            if (op == L">>") return Value::Num_((double)(int)(ua >> (ub & 31)));
            if (op == L">>>")
                return Value::Num_((double)((long long)(ua >> (ub & 31))));
            if (op == L"**") return Value::Num_(pow(a, b));   // ** is exponent
            if (op == L"<") return Value::Bool_(a < b && l.t == r.t ? true : CmpLess(l, r));
            if (op == L">") return Value::Bool_(CmpLess(r, l));
            if (op == L"<=") return Value::Bool_(!CmpLess(r, l));
            if (op == L">=") return Value::Bool_(!CmpLess(l, r));
            return Value();
        }

// ---- ConcatStrings ----
// Official aardio strings are byte arrays, so the concat operator joins
// BYTES. Nodio keeps text as wide chars and raw file data as one byte per
// wchar, so concatenating a raw byte string with text must not re-encode
// the bytes. When both operands are plain text the result stays wide text
// (the common case); when a raw byte string takes part, the result is a
// raw byte string holding the joined bytes.
Value Interp::ConcatStrings(const Value& l, const Value& r)
        {
            Value lv = ToStringMValue(l);
            Value rv = ToStringMValue(r);
            auto isRaw = [](const Value& v) {
                return v.t == Value::Str && (v.bRawBytes || v.bUtf16);
            };
            if (!isRaw(lv) && !isRaw(rv))
                return Value::Str_(lv.ToString() + rv.ToString());
            std::string bytes = StrByteForm(lv.t == Value::Str ? lv
                                                              : Value::Str_(lv.ToString()));
            bytes += StrByteForm(rv.t == Value::Str ? rv
                                                     : Value::Str_(rv.ToString()));
            wstring out((size_t)bytes.size(), 0);
            for (size_t i = 0; i < bytes.size(); i++)
                out[i] = (wchar_t)(unsigned char)bytes[i];
            Value ret = Value::Str_(out);
            ret.bRawBytes = true;
            return ret;
        }

// ---- CmpLess ----
bool Interp::CmpLess(const Value& l, const Value& r)
        {
            if (l.t == Value::Str && r.t == Value::Str)
            {
                // Official aardio compares strings byte by byte
                // (docs/language-reference/operator/comparison.md: "从第一个
                // 字符开始比较字节码大小"). Nodio stores strings either as
                // wide text or as one-byte-per-wchar raw bytes, so compare the
                // canonical byte form — otherwise a literal and the same bytes
                // read back from a file would not compare equal.
                if (l.bRawBytes == r.bRawBytes && l.bUtf16 == r.bUtf16)
                    return l.s < r.s;
                return StrByteForm(l) < StrByteForm(r);
            }
            if (l.t == Value::Num && r.t == Value::Num)
                return l.n < r.n;
            return false;
        }

// ---- ValuesEqual ----
bool Interp::ValuesEqual(const Value& l, const Value& r, bool strict)
        {
            if (l.t != r.t)
            {
                if (strict) return false;
                if (l.t == Value::Bool || r.t == Value::Bool)
                {
                    return l.Truthy() == r.Truthy();
                }
                if (l.t == Value::Null || r.t == Value::Null)
                    return false;
                if ((l.t == Value::Num && r.t == Value::Str) || (l.t == Value::Str && r.t == Value::Num))
                {
                    const wstring& s = (l.t == Value::Str) ? l.s : r.s;
                    double n = (l.t == Value::Num) ? l.n : r.n;
                    size_t first = s.find_first_not_of(L" \t\r\n");
                    if (first == wstring::npos)
                    {
                        if (n == 0) return true;
                    }
                    wchar_t* endp = nullptr;
                    double v = wcstod(s.c_str(), &endp);
                    if (endp && *endp == 0)
                        return n == v;
                }
                if ((l.t == Value::Pointer && r.t == Value::Num) || (l.t == Value::Num && r.t == Value::Pointer))
                {
                    INT_PTR p = (l.t == Value::Pointer) ? (INT_PTR)l.ptr : (INT_PTR)r.ptr;
                    double n = (l.t == Value::Num) ? l.n : r.n;
                    if ((double)p == n || (double)(UINT_PTR)p == n)
                        return true;
                }
                if (l.t == Value::TableT && l.tab)
                {
                    auto im = l.tab->map.find(L"_meta");
                    if (im != l.tab->map.end() && im->second.t == Value::TableT && im->second.tab)
                    {
                        auto ieq = im->second.tab->map.find(L"_eq");
                        if (ieq != im->second.tab->map.end() && (ieq->second.t == Value::Func || ieq->second.native))
                        {
                            vector<Value> args{ r };
                            return CallOwner(ieq->second, l, args).Truthy();
                        }
                    }
                }
                if (r.t == Value::TableT && r.tab)
                {
                    auto im = r.tab->map.find(L"_meta");
                    if (im != r.tab->map.end() && im->second.t == Value::TableT && im->second.tab)
                    {
                        auto ieq = im->second.tab->map.find(L"_eq");
                        if (ieq != im->second.tab->map.end() && (ieq->second.t == Value::Func || ieq->second.native))
                        {
                            vector<Value> args{ l };
                            return CallOwner(ieq->second, r, args).Truthy();
                        }
                    }
                }
                return false;
            }
            switch (l.t)
            {
            case Value::Null:
                return true;
            case Value::Bool:
                return l.b == r.b;
            case Value::Num:
                return l.n == r.n;
            case Value::Pointer:
                return l.ptr == r.ptr;
            case Value::Str:
                // Official aardio strings are byte arrays, so two strings are
                // equal when their byte content matches. Nodio keeps two
                // internal representations (wide text / one-byte-per-wchar
                // raw bytes), so compare the canonical byte form; otherwise a
                // literal and the same bytes read back from a file would not
                // be equal even though their bytes are identical.
                if (l.bRawBytes == r.bRawBytes && l.bUtf16 == r.bUtf16)
                    return l.s == r.s;
                return StrByteForm(l) == StrByteForm(r);
            case Value::TableT:
            case Value::Class:
                if (l.tab == r.tab) return true;
                if (!strict && l.tab)
                {
                    auto im = l.tab->map.find(L"_meta");
                    if (im != l.tab->map.end() && im->second.t == Value::TableT && im->second.tab)
                    {
                        auto ieq = im->second.tab->map.find(L"_eq");
                        if (ieq != im->second.tab->map.end() && (ieq->second.t == Value::Func || ieq->second.native))
                        {
                            vector<Value> args{ r };
                            return CallOwner(ieq->second, l, args).Truthy();
                        }
                    }
                }
                return false;
            case Value::Func:
                if (l.native && r.native) return l.native.target_type() == r.native.target_type();
                return l.fn == r.fn;
            }
            return false;
        }

// ---- ApplyOp ----
Value Interp::ApplyOp(const wstring& op, const Value& l, const Value& r)
        {
            if (op == L"===")
                return Value::Bool_(ValuesEqual(l, r, true));
            if (op == L"!==")
                return Value::Bool_(!ValuesEqual(l, r, true));
            if (op == L"==")
                return Value::Bool_(ValuesEqual(l, r, false));
            if (op == L"!=")
                return Value::Bool_(!ValuesEqual(l, r, false));
            if (op == L"..")
                return ConcatStrings(l, r);
            if (op == L"++")
                return ConcatStrings(l, r);   // string concat
            if (op == L"+" || op == L"-" || op == L"*" || op == L"/" || op == L"%" || op == L"%=" ||
                op == L"<<" || op == L">>" || op == L"<" || op == L"<=" || op == L">" || op == L">=")
            {
                wstring metaOp;
                if (op == L"+") metaOp = L"_add";
                else if (op == L"-") metaOp = L"_sub";
                else if (op == L"*") metaOp = L"_mul";
                else if (op == L"/") metaOp = L"_div";
                else if (op == L"%" || op == L"%=") metaOp = L"_mod";
                else if (op == L"<<") metaOp = L"_lshift";
                else if (op == L">>") metaOp = L"_rshift";
                else if (op == L"<") metaOp = L"_lt";
                else if (op == L"<=") metaOp = L"_le";

                if (!metaOp.empty())
                {
                    if (l.t == Value::TableT && l.tab)
                    {
                        auto im = l.tab->map.find(L"_meta");
                        if (im != l.tab->map.end() && im->second.t == Value::TableT && im->second.tab)
                        {
                            auto ifn = im->second.tab->map.find(metaOp);
                            if (ifn != im->second.tab->map.end() && (ifn->second.t == Value::Func || ifn->second.native))
                            {
                                vector<Value> args{ r };
                                return CallOwner(ifn->second, l, args);
                            }
                        }
                    }
                    if (r.t == Value::TableT && r.tab)
                    {
                        auto im = r.tab->map.find(L"_meta");
                        if (im != r.tab->map.end() && im->second.t == Value::TableT && im->second.tab)
                        {
                            auto ifn = im->second.tab->map.find(metaOp);
                            if (ifn != im->second.tab->map.end() && (ifn->second.t == Value::Func || ifn->second.native))
                            {
                                vector<Value> args{ l };
                                return CallOwner(ifn->second, r, args);
                            }
                        }
                    }
                }
            }
            double a = ToNum(l), b = ToNum(r);
            if (op == L"+") return Value::Num_(a + b);
            if (op == L"-") return Value::Num_(a - b);
            if (op == L"*") return Value::Num_(a * b);
            if (op == L"/") return Value::Num_(b != 0 ? a / b : 0);
            if (op == L"%" || op == L"%=") return Value::Num_(b != 0 ? fmod(a, b) : 0);
            // ** is exponent, ^ is bitwise XOR (32-bit)
            if (op == L"**") return Value::Num_(pow(a, b));
            if (op == L"^") return Value::Num_((double)(int)(((int)(INT_PTR)(long long)a) ^ ((int)(INT_PTR)(long long)b)));
            // bitwise: 32-bit width, DWORD semantics (matches EvalBin)
            unsigned int ua = (unsigned int)(INT_PTR)(long long)a;
            unsigned int ub = (unsigned int)(INT_PTR)(long long)b;
            if (op == L"&") return Value::Num_((double)(int)(ua & ub));
            if (op == L"|") return Value::Num_((double)(int)(ua | ub));
            if (op == L"<<") return Value::Num_((double)(int)(ua << (ub & 31)));
            if (op == L">>") return Value::Num_((double)(int)(ua >> (ub & 31)));
            if (op == L">>>")
                return Value::Num_((double)((long long)(ua >> (ub & 31))));
            if (op == L"<") return Value::Bool_(CmpLess(l, r));
            if (op == L">") return Value::Bool_(CmpLess(r, l));
            if (op == L"<=") return Value::Bool_(!CmpLess(r, l));
            if (op == L">=") return Value::Bool_(!CmpLess(l, r));
            return Value();
        }

        // M7: bare-name assign for multi-target unpack (a,b = ...)

// ---- DoAssign ----
void Interp::DoAssign(const PNode& target, const Value& v, Env* env,
                      const wstring& op)
        {
            // the assigned value lives in a C++ local while a metatable
            // _set hook may run — keep it reachable for the collector
            GcRootScope rs;
            rs.add(v);
            Value val = v;
            val.mrets = nullptr;
            if (op == L":=" || op == L"::=")
            {
                Value cur;
                if (target->k == Node::Ident)
                {
                    if (Value* pv = env->Find(target->str))
                        cur = *pv;
                }
                else if (target->k == Node::Member || target->k == Node::Index)
                    cur = Eval(target, env);
                if (cur.Truthy())
                    return;
            }
            else if (op != L"=")
            {
                Value cur;
                if (target->k == Node::Ident)
                {
                    if (Value* pv = env->Find(target->str))
                        cur = *pv;
                }
                else if (target->k == Node::Member || target->k == Node::Index)
                    cur = Eval(target, env);
                val = ApplyOp(op.substr(0, op.size() - 1), cur, v);
            }
            if (target->k == Node::Ident)
            {
                // inside a namespace scope, bare names are
                // namespace members — only locals (var/params) resolve
                // before the namespace table. Check if target was declared
                // as a lexical variable in any enclosing env frame (stopping
                // before globals, which is not a local var frame).
                Env* eFound = nullptr;
                for (Env* e = env; e && e->parent; e = e->parent.get())
                {
                    if (e->vars.find(target->str) != e->vars.end())
                    {
                        eFound = e;
                        break;
                    }
                    if (e->bNsBody && (e->nsTable || e->modTable))
                    {
                        // Stop at the enclosing namespace/module boundary.
                        // Outer namespaces should NOT capture bare assignments!
                        break;
                    }
                }
                Env* eNs = env;
                while (eNs && !eNs->nsTable) eNs = eNs->parent.get();
                Env* eMod = env;
                while (eMod && !eMod->modTable) eMod = eMod->parent.get();
                bool bFoundInOrBelowClass = false;
                if (eNs && eNs->bClassBody && eNs->bNsInst && eNs->nsTable)
                {
                    if (env == eNs)
                    {
                        // Directly in class body: member definition (e.g. `bind = { ... }`, `method = function() {}`)
                        // writes to the instance table only, and never overwrites ctor local variables or outer namespaces.
                        eNs->nsTable->map[target->str] = val;
                        return;
                    }
                    for (Env* cur = env; cur; cur = cur->parent.get())
                    {
                        if (cur == eFound) { bFoundInOrBelowClass = true; break; }
                        if (cur == eNs) break;
                    }
                }
                if (eNs && eNs->bClassBody && eNs->bNsInst && eNs->nsTable && !bFoundInOrBelowClass)
                {
                    // In class body: member definition (e.g. `method = function() {}` or `each = function() {}`)
                    // writes to the instance table only, and NEVER overwrites outer namespace/global variables.
                    eNs->nsTable->map[target->str] = val;
                }
                else if (eFound)
                {
                    eFound->vars[target->str] = val;
                    if (eFound->bNsBody && eFound->nsTable)
                    {
                        eFound->nsTable->map[target->str] = val;
                        eFound->nsTable->scopeVars[target->str] = val;
                    }
                    else if (eFound->bNsBody && eFound->modTable)
                    {
                        if (val.t == Value::TableT && val.tab && !env->nsTable)
                        {
                            // If assigning a table to the module name itself before 'namespace ...'
                            // (e.g. debug = global["(debug)"]), copy all members into the module
                            for (auto& kv : val.tab->map)
                                eFound->modTable->map[kv.first] = kv.second;
                        }
                        eFound->modTable->map[target->str] = val;
                        eFound->modTable->scopeVars[target->str] = val;
                        if (target->str.size() >= 3 && target->str.substr(0, 3) == L"___")
                            globals.vars[target->str] = val;
                    }
                    else if (eFound->bNsInst && eFound->nsTable && eFound->bClassBody)
                    {
                        eFound->nsTable->map[target->str] = val;
                    }
                }
                else if (eNs && eNs->nsTable)
                {
                    if (eNs->bNsInst && eNs->bClassBody)
                    {
                        // In class body: member definition (e.g. `method = function() {}`)
                        // writes to the instance table only, NOT as a lexical closure variable.
                        // Otherwise instance methods mistakenly shadow outer namespace members.
                        eNs->nsTable->map[target->str] = val;
                    }
                    else if (eNs->bNsInst)
                    {
                        // Inside ctor: if already an instance property, update instance.
                        // Otherwise, bare assignment targets the class namespace!
                        if (eNs->nsTable->map.find(target->str) != eNs->nsTable->map.end())
                        {
                            eNs->nsTable->map[target->str] = val;
                        }
                        else
                        {
                            Env* eClass = eNs->parent.get();
                            while (eClass && (!eClass->nsTable || eClass->bNsInst)) eClass = eClass->parent.get();
                            if (eClass && eClass->nsTable)
                            {
                                eClass->vars[target->str] = val;
                                eClass->nsTable->map[target->str] = val;
                                eClass->nsTable->scopeVars[target->str] = val;
                            }
                            else
                            {
                                eNs->vars[target->str] = val;
                                eNs->nsTable->map[target->str] = val;
                                eNs->nsTable->scopeVars[target->str] = val;
                            }
                        }
                    }
                    else
                    {
                        eNs->vars[target->str] = val;
                        eNs->nsTable->map[target->str] = val;
                        eNs->nsTable->scopeVars[target->str] = val;
                        if (target->str.size() >= 3 && target->str.substr(0, 3) == L"___")
                            globals.vars[target->str] = val;
                    }
                }
                else if (eMod && eMod->modTable)
                {
                    if (val.t == Value::TableT && val.tab && !env->nsTable)
                    {
                        // If assigning a table to the module name itself before 'namespace ...'
                        // (e.g. debug = global["(debug)"]), copy all members into the module
                        for (auto& kv : val.tab->map)
                            eMod->modTable->map[kv.first] = kv.second;
                    }
                    eMod->vars[target->str] = val;
                    eMod->modTable->map[target->str] = val;
                    eMod->modTable->scopeVars[target->str] = val;
                    if (target->str.size() >= 3 && target->str.substr(0, 3) == L"___")
                        globals.vars[target->str] = val;
                }
                else
                {
                    globals.vars[target->str] = val;   // default global
                    // engine-provided root namespaces are pre-registered in
                    // `roots`, and NsPath/DoImport consult that map first.
                    // Leaving the two copies apart made `namespace process{}`
                    // after `class process{}` extend the STALE engine table,
                    // so the class lost every member the namespace declared.
                    if (roots.count(target->str))
                        roots[target->str] = val;
                }
            }
            else if (target->k == Node::Member)
            {
                Value obj = target->bRoot ? Value() : Eval(target->a, env);
                if (target->bMeta)
                {
                    // tab@ = meta — set the metatable
                    if (target->a->k == Node::Ident)
                    {
                        if (Value* vVar = env->Find(target->a->str))
                        {
                            if (vVar->t == Value::Pointer)
                            {
                                if (!vVar->tab) vVar->tab = std::make_shared<Table>();
                                vVar->tab->map[L"_meta"] = val;
                                vVar->tab->bExplicitMeta = true;
                                return;
                            }
                        }
                    }
                    if (obj.t == Value::Pointer && obj.tab)
                    {
                        obj.tab->map[L"_meta"] = val;
                        obj.tab->bExplicitMeta = true;
                        return;
                    }
                    if ((obj.t == Value::TableT || obj.t == Value::Class || obj.t == Value::Func) && obj.tab)
                    {
                        obj.tab->map[L"_meta"] = val;
                        obj.tab->bExplicitMeta = true;
                    }
                    return;
                }
                // class templates and functions hold members in the same Table shape —
                // ::RECT._meta = ... must land on the class table; type.callable = ... on function table
                if ((obj.t == Value::Class || obj.t == Value::Func) && obj.tab)
                {
                    if (target->str == L"_meta" || !obj.tab->bExplicitMeta || !obj.tab->map.count(L"_meta"))
                    {
                        if (val.t == Value::Null)
                            obj.tab->map.erase(target->str);
                        else
                            obj.tab->map[target->str] = val;
                    }
                    else
                    {
                        bool bErr = false;
                        wstring strErrM;
                        if (!MetaSet(obj, target->str, val, &bErr, &strErrM))
                        {
                            if (val.t == Value::Null)
                                obj.tab->map.erase(target->str);
                            else
                                obj.tab->map[target->str] = val;
                        }
                        else if (bErr && err->empty())
                            (*err) = L"运行错误: " + strErrM;
                    }
                }
                else if (obj.t == Value::TableT && obj.tab)
                {
                    if (target->str == L"_meta")
                    {
                        obj.tab->bExplicitMeta = (val.t != Value::Null);
                    }
                    else if (!obj.tab->bExplicitMeta && obj.tab->map.find(L"_meta") == obj.tab->map.end())
                    {
                        if (val.t == Value::Null)
                            obj.tab->map.erase(target->str);
                        else
                            obj.tab->map[target->str] = val;
                        return;
                    }
                    else
                    {
                        obj.tab->bExplicitMeta = true;
                    }
                    // ctor-body member initialization (`this.x = v` while
                    // the object is being constructed) writes the instance
                    // table directly — the metatable's read-only guards
                    // only apply after construction.
                    // The write must land on the instance being constructed:
                    // after `this = other` (official instance replacement)
                    // the ctor writes to `other`, which is a finished
                    // object and must go through the metatable's _set.
                    bool bInit = false;
                    for (Env* e = env; e; e = e->parent.get())
                    {
                        if (e->bInstInit && e->instTable &&
                            obj.tab.get() == e->instTable)
                        {
                            bInit = true;
                            break;
                        }
                    }
                    bool bHasPropSetter = false;
                    auto im = obj.tab->map.find(L"_meta");
                    if (im != obj.tab->map.end() && im->second.t == Value::TableT && im->second.tab)
                    {
                        auto ik = im->second.tab->map.find(target->str);
                        if (ik != im->second.tab->map.end() && ik->second.t == Value::TableT && ik->second.tab)
                        {
                            if (ik->second.tab->map.count(L"_set"))
                                bHasPropSetter = true;
                        }
                        if (!bHasPropSetter)
                        {
                            auto is = im->second.tab->map.find(L"_set");
                            if (is != im->second.tab->map.end() && is->second.t == Value::TableT && is->second.tab)
                            {
                                if (is->second.tab->map.count(target->str))
                                    bHasPropSetter = true;
                            }
                        }
                    }

                    bool bHasDataMember = (obj.tab->map.count(target->str) > 0);
                    if (bInit || !obj.tab->map.count(L"_meta") || (bHasDataMember && !bHasPropSetter))
                    {
                        if (val.t == Value::Null)
                            obj.tab->map.erase(target->str);
                        else
                            obj.tab->map[target->str] = val;
                    }
                    else
                    {
                        bool bErr = false;
                        wstring strErrM;
                        if (!MetaSet(obj, target->str, val, &bErr,
                                     &strErrM))
                        {
                            if (val.t == Value::Null)
                                obj.tab->map.erase(target->str);
                            else
                                obj.tab->map[target->str] = val;
                        }
                        else if (bErr && err->empty())
                            (*err) = L"运行错误: " + strErrM;
                    }
                }
                else if (target->bRoot)
                {
                    globals.vars[target->str] = val;   // '..' root
                    roots[target->str] = val;
                }
            }
            else if (target->k == Node::Index)
            {
                Value obj = Eval(target->a, env);
                Value idx = Eval(target->b, env);
                wstring idxKey = ObjectKeyOf(idx);
                if ((obj.t == Value::TableT || obj.t == Value::Func || obj.t == Value::Class) && obj.tab)
                {
                    bool bHasMeta = false;
                    if (!target->bDirect)
                    {
                        bHasMeta = (obj.tab->map.count(L"_meta") > 0);
                        if (!bHasMeta)
                        {
                            auto icc = obj.tab->map.find(L"_metaAnnotated");
                            if (icc != obj.tab->map.end() && (icc->second.t == Value::Bool || icc->second.t == Value::Num))
                            {
                                auto icc2 = obj.tab->map.find(L"_class");
                                if (icc2 != obj.tab->map.end() && icc2->second.tab && icc2->second.tab->map.count(L"_meta"))
                                    bHasMeta = true;
                            }
                        }
                    }

                    if (bHasMeta)
                    {
                        bool bErr = false;
                        wstring strErrM;
                        // For index assignment, ownerAttr is false
                        if (MetaSet(obj, idx, val, &bErr, &strErrM, /*bOwnerAttr=*/false))
                        {
                            if (bErr && err->empty())
                                (*err) = L"运行错误: " + strErrM;
                            return;
                        }
                        if (bErr && err->empty())
                        {
                            (*err) = L"运行错误: " + strErrM;
                            return;
                        }
                    }

                    long long numIdx = -1;
                    if (idx.t == Value::Num && idx.n == (long long)idx.n && idx.n >= 1)
                        numIdx = (long long)idx.n;
                    else if (!idxKey.empty() && idxKey.find_first_not_of(L"0123456789") == wstring::npos)
                        numIdx = _wtoi64(idxKey.c_str());

                    if (numIdx >= 1)
                    {
                        if ((size_t)numIdx <= obj.tab->arr.size())
                        {
                            obj.tab->arr[(size_t)(numIdx - 1)] = val;
                            obj.tab->map.erase(idxKey);
                        }
                        else if ((size_t)(numIdx - 1) == obj.tab->arr.size())
                        {
                            if (val.t != Value::Null)
                                obj.tab->arr.push_back(val);
                            obj.tab->map.erase(idxKey);
                        }
                        else
                        {
                            if (val.t == Value::Null)
                                obj.tab->map.erase(idxKey);
                            else
                                obj.tab->map[idxKey] = val;
                        }
                    }
                    else
                    {
                        if (val.t == Value::Null)
                            obj.tab->map.erase(idxKey);
                        else
                        {
                            if (idxKey == L"_struct" && val.t == Value::Str)
                            {
                                auto itCur = obj.tab->map.find(L"_struct");
                                if (itCur != obj.tab->map.end() && itCur->second.t == Value::Str)
                                {
                                    wstring sCur = itCur->second.s;
                                    wstring sVal = val.s;
                                    if (!sCur.empty() && sCur.back() != L';') sCur += L';';
                                    if (!sVal.empty() && sVal.back() != L';') sVal += L';';
                                    if (sCur != sVal && sCur.find(sVal) != wstring::npos)
                                    {
                                        return;
                                    }
                                }
                            }
                            obj.tab->map[idxKey] = val;
                        }
                    }
                }
            }
        }

// ---- ExecBlock ----
// Official aardio: locals declared with var inside a statement block
// (if/while/do bodies, try) are BLOCK-scoped and disappear at block exit
// (docs/language-reference/statements/blocks.md — 块级作用域). ExecBlock
// therefore runs the body in a child frame; on exit the frame is dropped
// and any var declared inside it goes out of scope. Assignments to outer
// locals (bare `x = v` without var) still reach the outer frame through
// DoAssign's eFound search.
Value Interp::ExecBlockScoped(const PNode& blk, Env* env)
{
    if (!blk || blk->list.empty())
        return ExecBlock(blk, env);
    auto child = std::make_shared<Env>();
    child->self = child;
    child->parent = env ? env->self : globals.self;
    // closure captures and namespace-member fallbacks must still see the
    // enclosing scope's namespace/module tables through Find()'s parent walk,
    // so they are intentionally NOT copied into the child frame.
    GcFrameScope fs(*this, child);
    Value res = ExecBlock(blk, child.get());
    CleanDeadEnv(child);
    return res;
}

// ---- GC ----
void Interp::CollectGarbage(Env* env)
        {
            if (bInGC) return;
            long before = g_liveTables + g_liveEnvs;
            GcCollect(*this, env);
            nGCRuns++;
            nLastGCTick = g_gcAllocTick;
            nGCFreed = before - (g_liveTables + g_liveEnvs);
            // Pace the collector by the size of the live set: a sweep
            // costs O(live objects), so it must not run more often than
            // the live set grows. A sweep that frees nothing backs off.
            //
            // The floor matters a lot in a GUI: a repaint allocates
            // hundreds of objects, so a 2000-allocation floor made the
            // collector run 30 times a second - each sweep still pays the
            // finalizer calls, which are far more expensive than the mark
            // phase itself.
            long live = g_liveTables + g_liveEnvs;
            long want = live * 2;
            if (want < 20000) want = 20000;
            if (want > 400000) want = 400000;
            if (nGCFreed <= 0)
            {
                nGCThreshold = want * 2;
                if (nGCThreshold > 800000) nGCThreshold = 800000;
            }
            else
            {
                nGCThreshold = want;
            }
        }

void Interp::MaybeCollectGC(Env* env)
        {
            if (bInGC)
            {
                static int s_nBusy = 0;
                if (EnvFlag(L"NODIO_GC_TRACE") && s_nBusy++ < 5)
                {
                    fwprintf(stderr, L"[GC] skip: bInGC\n");
                    fflush(stderr);
                }
                return;
            }
            // NODIO_GC_THRESHOLD=0 disables automatic collection (the
            // explicit collectgarbage() call still works)
            // -2 = not read yet, -1 = automatic (adaptive threshold),
            //  0 = disabled, >0 = fixed allocation threshold.
            // NOTE: the "not set" default must NOT be 0 — 0 means
            // "disabled" below, and using it as the default silently
            // turned the automatic collector off for every script.
            static long s_fixed = -2;
            if (s_fixed == -2)
            {
                s_fixed = -1;
                wchar_t szThr[32] = L"";
                if (GetEnvironmentVariableW(L"NODIO_GC_THRESHOLD", szThr, 32) > 0)
                    s_fixed = _wtol(szThr);
                if (s_fixed < 0) s_fixed = -1;
            }
            if (s_fixed == 0) return;
            if (s_fixed > 0)
            {
                if (g_gcAllocTick - nLastGCTick < s_fixed) return;
                CollectGarbage(env);
                return;
            }
            if (g_gcAllocTick - nLastGCTick < nGCThreshold)
            {
                static int s_nSkip = 0;
                if (EnvFlag(L"NODIO_GC_TRACE") && s_nSkip++ < 5)
                {
                    fwprintf(stderr, L"[GC] skip: threshold=%ld alloc=%ld last=%ld\n",
                             nGCThreshold, (long)g_gcAllocTick.load(), nLastGCTick);
                    fflush(stderr);
                }
                return;
            }
            CollectGarbage(env);
        }

    // A breakpoint (and a single step) belongs to the script the user
    // asked to run. The official library is imported at runtime and its
    // line numbers collide with the user's script - library line 3 is a
    // namespace statement, and the builtins run hundreds of lines deep -
    // so a bare line number must not stop inside an imported module.
    // g_curModuleFile is empty while the main program (and the functions
    // defined in it) run, and holds the module path while a library
    // module executes.
    static bool InUserScript()
    {
        return g_curModuleFile.empty();
    }

Value Interp::ExecBlock(const PNode& blk, Env* env)
        {
            Value last;
            // The block result lives in a C++ local: the collector cannot
            // see it, so it keeps a slot in the root stack for the whole
            // block and refreshes it after every statement.
            size_t lastRoot = g_gcRoots.size();
            g_gcRoots.push_back(last);
            for (auto& st : blk->list)
            {
                if (st->line > 0)
                    nErrLine = st->line;
                // S-2: remember the file that owns the statement being
                // executed.  The report points (try/catch, top-level report)
                // read it when they append " @ <file>:<line>", and by then
                // the frames may already be gone — so the file is kept as a
                // copy, refreshed whenever the executing frame changes.
                {
                    const wstring* pf = SourceFilePtrOfEnv(env);
                    if (pf)
                    {
                        if (strErrFile != *pf) strErrFile = *pf;
                    }
                    else if (!strErrFile.empty())
                    {
                        strErrFile.clear();
                    }
                }
                if (bDebug && st->line > 0 && InUserScript() &&
                    (bStep || breakpoints.count(st->line)))
                {
                    // "next"/"step" from a DAP session stops exactly once —
                    // clear the one-shot flag before serving the stop so a
                    // subsequent continue really continues
                    bool bOnce = bStep;
                    if (bOnce) bStep = false;
                    DebugStop(st->line, env);
                    (void)bOnce;
                }
                if (!err->empty())
                    break;
                static bool bDbg = GetEnvironmentVariableW(L"CMT", nullptr, 0) != 0;
                if (bDbg)
                {
                    fwprintf(stderr, L"[RUN] %s:%d k=%d\n", g_curModuleFile.c_str(), st->line, (int)st->k);
                    fflush(stderr);
                }
                // statement boundary — the collector's safe point: no
                // half-evaluated expression is live on the C++ stack here
                g_gcEpochStack.push_back(++g_gcEpoch);
                MaybeCollectGC(env);
                last = Exec(st, env);
                g_gcEpochStack.pop_back();
                g_gcRoots[lastRoot] = last;
                if (nBreak || nContinue || nReturn || !err->empty())
                    break;
            }
            g_gcRoots.resize(lastRoot);
            return last;
        }

// ---- EvalClass ----
Value Interp::EvalClass(const PNode& n, Env* env)
        {
            // build the class template: member assigns evaluate now, the
            // ctor body is stored for instantiation (EvalCall Class case)
            auto tb = std::make_shared<Table>();
            Value vClass;
            vClass.t = Value::Class;
            vClass.tab = tb;
            vClass.fn = n->b;
            vClass.pdefaults = std::make_shared<vector<PNode>>(
                n->b ? n->b->defaults : vector<PNode>());
            vClass.classBody = n->a;
            if (env && env->self)
                vClass.envUp = env->self;
            for (Env* e = env; e; e = e->parent.get())
                if (e->nsTable && !e->bNsInst)
                {
                    vClass.nsClosure = e->nsTable;
                    break;
                }
            auto classEnv = std::make_shared<Env>();
            classEnv->self = classEnv;
            classEnv->parent = env ? env->self : globals.self;
            classEnv->nsTable = tb;
            classEnv->bNsBody = true;
            classEnv->vars[L"self"] = vClass;
            GcFrameScope fsClass(*this, classEnv);
            for (auto& st : n->a->list)
            {
                wstring prevErr = err ? *err : L"";
                if (st->k == Node::Assign && st->a &&
                    st->a->k == Node::Ident)
                {
                    Value v = Eval(st->b, classEnv.get());
                    if (err && !err->empty() && prevErr.empty())
                    {
                        // Expression failed during class definition (e.g. references `this` or late-bound namespace methods).
                        // This is an instance field initializer that will be evaluated at instantiation time.
                        *err = L"";
                    }
                    else
                    {
                        tb->map[st->a->str] = v;
                    }
                }
                else if (st->k == Node::Assign && st->a &&
                         st->a->k == Node::Index && st->a->a &&
                         st->a->a->k == Node::Ident && st->a->a->str == L"this" &&
                         st->a->b && st->a->b->k == Node::Str)
                {
                    if (st->a->b->str != L"_meta")
                    {
                        Value v = Eval(st->b, classEnv.get());
                        if (err && !err->empty() && prevErr.empty())
                        {
                            *err = L"";
                        }
                        else
                        {
                            tb->map[st->a->b->str] = v;
                        }
                    }
                }
            }
            // anonymous class (class { ... }) has no name to bind
            if (n->names.empty())
            {
                return vClass;
            }
            // dotted class name (class win.form) binds into the
            // namespace path; single name binds into the ambient
            // namespace ('namespace util; class metaProperty')
            // or the global root when none is active
            else if (n->names.size() == 1)
            {
                Env* eNs = env;
                while (eNs && !eNs->nsTable) eNs = eNs->parent.get();
                if (eNs && !n->bRoot)
                {
                    if (g_nsTrace)
                    {
                        fwprintf(stderr, L"[NS]   class '%s' -> nsTable=%p\n",
                                 n->names[0].c_str(), (void*)eNs->nsTable.get());
                        fflush(stderr);
                    }
                    eNs->nsTable->map[n->names[0]] = vClass;
                }
                else
                {
                    if (g_nsTrace)
                    {
                        fwprintf(stderr, L"[NS]   class '%s' -> GLOBALS (no nsTable in env chain)\n",
                                 n->names[0].c_str());
                        fflush(stderr);
                    }
                    globals.vars[n->names[0]] = vClass;
                    roots[n->names[0]] = vClass;
                }
            }
            else
            {
                Value ns;
                // Dotted class name (class A.B): if not root-prefixed (..A.B),
                // resolve A in ambient namespace or lexical scope first
                if (!n->bRoot)
                {
                    Env* eNs = env;
                    while (eNs && !eNs->nsTable) eNs = eNs->parent.get();
                    if (eNs && eNs->nsTable)
                    {
                        auto it = eNs->nsTable->map.find(n->names[0]);
                        if (it != eNs->nsTable->map.end() &&
                            (it->second.t == Value::TableT || it->second.t == Value::Class))
                            ns = it->second;
                    }
                    if (ns.t == Value::Null)
                    {
                        if (Value* pv = env->Find(n->names[0]))
                        {
                            if (pv->t == Value::TableT || pv->t == Value::Class)
                                ns = *pv;
                        }
                    }
                }
                if (ns.t == Value::Null)
                {
                    auto itG = globals.vars.find(n->names[0]);
                    if (itG != globals.vars.end() &&
                        (itG->second.t == Value::TableT || itG->second.t == Value::Class))
                        ns = itG->second;
                    else
                    {
                        auto tbN = std::make_shared<Table>();
                        ns = Value::Table_(tbN);
                        globals.vars[n->names[0]] = ns;
                    }
                }
                for (size_t k = 1; k + 1 < n->names.size(); k++)
                {
                    Value nxt = GetMember(ns, n->names[k]);
                    if (nxt.t != Value::TableT && nxt.t != Value::Class)
                    {
                        auto tbN = std::make_shared<Table>();
                        nxt = Value::Table_(tbN);
                        if (ns.tab)
                            ns.tab->map[n->names[k]] = nxt;
                    }
                    ns = nxt;
                }
                if (ns.tab)
                    ns.tab->map[n->names.back()] = vClass;
            }
            return vClass;
        }

// ---- Exec ----
Value Interp::Exec(const PNode& n, Env* env)
        {
            switch (n->k)
            {
            case Node::ExprStmt:
                return Eval(n->a, env);
            case Node::Assign:
            {
                if (n->names.empty() && n->c && !n->c->list.empty() &&
                    n->b && !n->b->list.empty())
                {
                    // x.a,x.b = e1,e2 — member-path unpack; b->list holds all
                    // positional exprs, c->list holds targets 2..N.
                    // rhs pre-evaluated before any target binds (see the
                    // names-form comment below)
                    size_t nT = n->c->list.size() + 1;
                    vector<Value> pre;
                    for (auto& eN : n->b->list)
                    {
                        Value ev = Eval(eN, env);
                        pre.push_back(ev);
                        if (ev.mrets && !ev.mrets->empty())
                        {
                            for (auto& mr : *ev.mrets)
                                pre.push_back(mr);
                        }
                    }
                    Value v = pre.empty() ? Value() : pre[0];
                    for (size_t k2 = 0; k2 < nT; k2++)
                    {
                        Value item = (k2 < pre.size()) ? pre[k2] : Value();
                        if (k2 == 0)
                            DoAssign(n->a, item, env, L"=");
                        else
                            DoAssign(n->c->list[k2 - 1], item, env, L"=");
                    }
                    return v;
                }
                if (n->names.size() > 1)
                {
                    // a,b,c = e1,e2 — positional like var; when any expr
                    // yields multi-returns (especially the last or later ones),
                    // the extras fill subsequent targets.
                    // EVERY rhs is evaluated before any target binds —
                    // `a,b = b,a+b` must read the old a.
                    // A packed `...` (bDots) expands across all names.
                    size_t extras = n->b ? n->b->list.size() : 0;
                    vector<Value> rawExprs;
                    rawExprs.reserve(1 + extras);
                    rawExprs.push_back(Eval(n->a, env));
                    for (size_t e = 0; e < extras; e++)
                        rawExprs.push_back(Eval(n->b->list[e], env));

                    vector<Value> flatVals;
                    for (size_t i = 0; i < rawExprs.size(); i++)
                    {
                        const Value& ev = rawExprs[i];
                        if (ev.bDots && ev.mrets)
                        {
                            for (auto& mr : *ev.mrets)
                                flatVals.push_back(mr);
                        }
                        else
                        {
                            flatVals.push_back(ev);
                            if (i == rawExprs.size() - 1 && ev.mrets && !ev.mrets->empty())
                            {
                                for (auto& mr : *ev.mrets)
                                    flatVals.push_back(mr);
                            }
                        }
                    }

                    Value v = flatVals.empty() ? Value() : flatVals[0];
                    for (size_t k2 = 0; k2 < n->names.size(); k2++)
                    {
                        Value item = (k2 < flatVals.size()) ? flatVals[k2] : Value();
                        if (n->names[k2].empty())
                            continue;   // hole slot: consumed a return value
                        PNode idT = std::make_shared<Node>();
                        idT->k = Node::Ident;
                        idT->str = n->names[k2];
                        DoAssign(idT, item, env, L"=");
                    }
                    return v;
                }
                if (n->str == L":=" || n->str == L"::=")
                {
                    Value cur;
                    if (n->a->k == Node::Ident)
                    {
                        if (Value* pv = env->Find(n->a->str))
                            cur = *pv;
                    }
                    else if (n->a->k == Node::Member || n->a->k == Node::Index)
                        cur = Eval(n->a, env);
                    if (cur.Truthy())
                        return cur;
                }
                Value v = Eval(n->b, env);
                DoAssign(n->a, v, env, n->str);
                return v;
            }
            case Node::VarDecl:
            {
                // a var declared directly in a namespace or module
                // body is a namespace member. Mirror the slot into the
                // namespace table's map so member access (ns.name) and
                // code in other modules see it; the env slot keeps the
                // local read/write fast path. Function-call envs have
                // bNsBody=false — their locals stay local.
                auto BindVar = [env](const wstring& name, const Value& v)
                {
                    if (name.empty()) return;
                    Value val = v;
                    val.mrets = nullptr;
                    env->vars[name] = val;
                    if (env->bNsBody)
                    {
                        if (env->nsTable)
                            env->nsTable->map[name] = val;
                        else if (env->modTable)
                            env->modTable->map[name] = val;
                    }
                };
                // var a = e1, b = e2 — positional init list; all initializers
                // evaluated before any name binds. Multi-return values
                // (a call result's mrets, or a packed `...`) flow across
                // the name list: `var w,h,px = arg,...` must give w the
                // first vararg, h the second — the shipped gdip.bitmap
                // ctor depends on this. Only when a slot's value is a
                // plain single value does the next name take the next
                // initializer.
                if (n->b && !n->b->list.empty())
                {
                    vector<Value> flow;
                    auto pushFlow = [&](Value v)
                    {
                        if (v.bDots && v.mrets)
                        {
                            for (auto& m : *v.mrets) flow.push_back(m);
                        }
                        else if (v.mrets)
                        {
                            flow.push_back(v);
                            for (auto& m : *v.mrets) flow.push_back(m);
                        }
                        else
                            flow.push_back(v);
                    };
                    if (n->a)
                        pushFlow(Eval(n->a, env));
                    for (auto& eN : n->b->list)
                        pushFlow(Eval(eN, env));
                    for (size_t k = 0; k < n->names.size(); k++)
                        if (!n->names[k].empty())
                            BindVar(n->names[k],
                                k < flow.size() ? flow[k] : Value());
                    return flow.empty() ? Value() : flow.front();
                }
                Value v = n->a ? Eval(n->a, env) : Value();
                if (n->names.size() == 1)
                {
                    if (v.bDots && v.mrets)
                    {
                        Value firstDot = v.mrets->empty() ? Value() : (*v.mrets)[0];
                        BindVar(n->names[0], firstDot);
                        return firstDot;
                    }
                    BindVar(n->names[0], v);
                    return v;
                }
                // var a,b = f() — multi-return unpack. A packed
                // `...` (bDots) carries ALL varargs in mrets and expands
                // from index 0; a normal call result holds the FIRST
                // return in v with mrets listing the extras.
                size_t dotsOff = v.bDots ? 0 : 1;
                if (v.bDots && v.mrets && !v.mrets->empty())
                    BindVar(n->names[0], (*v.mrets)[0]);
                else if (!n->names[0].empty())
                    BindVar(n->names[0], v);
                if (v.mrets)
                {
                    for (size_t k = 1; k < n->names.size(); k++)
                        if (!n->names[k].empty())
                            BindVar(n->names[k],
                                (k - dotsOff) < v.mrets->size()
                                    ? (*v.mrets)[k - dotsOff] : Value());
                }
                else
                    for (size_t k = 1; k < n->names.size(); k++)
                        if (!n->names[k].empty())
                            BindVar(n->names[k], Value());
                return v;
            }
            case Node::Import:
                return DoImport(n, env);
            case Node::Namespace:
            {
                Value ns = NsPath(n->names, env, n->bRoot);
                if (g_nsTrace)
                {
                    wstring p;
                    for (size_t i = 0; i < n->names.size(); i++)
                        p += (i ? L"." : L"") + n->names[i];
                    fwprintf(stderr, L"[NS] open '%s' root=%d -> tab=%p type=%d\n",
                             p.c_str(), (int)n->bRoot, (void*)ns.tab.get(), (int)ns.t);
                    fflush(stderr);
                }
                if (!ns.tab)
                    return Value();
                ns.tab->map[L"self"] = ns;
                auto nsEnv = std::make_shared<Env>();
                nsEnv->self = nsEnv;
                nsEnv->parent = env ? env->self : globals.self;
                nsEnv->nsTable = ns.tab;
                nsEnv->bNsBody = true;
                nsEnv->vars[L"self"] = ns;
                for (auto& kv : ns.tab->scopeVars)
                    nsEnv->vars[kv.first] = kv.second;
                GcFrameScope fsNs(*this, nsEnv);
                Value r = ExecBlock(n->a, nsEnv.get());
                for (auto& kv : nsEnv->vars)
                    ns.tab->scopeVars[kv.first] = kv.second;
                return r;
            }
            case Node::Class:
                return EvalClass(n, env);
            case Node::With:
            {
                // with(expr) body: bare names inside the body READ AND
                // WRITE the with-target table live (`with t{ k = v }`
                // must land back in the table) — bind the target
                // as this env's namespace instead of snapshotting
                Value v = Eval(n->a, env);
                auto local = std::make_shared<Env>();
                local->self = local;
                local->parent = env ? env->self : globals.self;
                if (v.t == Value::TableT && v.tab)
                    local->nsTable = v.tab;
                local->vars[L"owner"] = v;
                GcFrameScope fs(*this, local);
                return ExecBlock(n->b, local.get());
            }
            case Node::DoWhile:
            {
                while (true)
                {
                    ExecBlockScoped(n->b, env);
                    if (nReturn) break;
                    if (nBreak) { nBreak--; break; }
                    if (nContinue) nContinue--;
                    if (!Eval(n->a, env).Truthy())
                        break;
                }
                return Value();
            }
            case Node::If:
            {
                // select(v){case...} reuses the If node: b holds a Num
                // sentinel node (b->num==1) marking select form, c->list =
                // [caseChain|null, body, ...] pairs — first match wins
                if (n->b && n->b->k == Node::Num && n->b->num == 1)
                {
                    Value vSel = Eval(n->a, env);
                    for (size_t i = 0; i + 1 < n->c->list.size(); i += 2)
                    {
                        PNode chain = n->c->list[i];
                        if (!chain)
                        {
                            // else arm: runs when no case before it matched
                            return ExecBlockScoped(n->c->list[i + 1], env);
                        }
                        bool bHit = false;
                        for (PNode cur = chain; cur;)
                        {
                            Value vCase;
                            if (cur->str == L"range")
                            {
                                // inclusive numeric range >= <=
                                double lo = ToNum(Eval(cur->a, env));
                                double hi = ToNum(Eval(cur->b, env));
                                double v = ToNum(vSel);
                                bHit = (v >= lo && v <= hi);
                            }
                            else
                            {
                                Value vC = Eval(cur->b, env);
                                Value res = ApplyOp(cur->str, vSel, vC);
                                bHit = res.Truthy();
                            }
                            if (bHit)
                                break;
                            cur = cur->c;
                        }
                        if (bHit)
                            return ExecBlockScoped(n->c->list[i + 1], env);
                    }
                    return Value();
                }
                Value vCond = Eval(n->a, env);
                if (nReturn)
                    return vCond;   // inline return in cond: unwind now
                if (vCond.Truthy())
                    return ExecBlockScoped(n->b, env);
                for (size_t i = 0; i + 1 < n->c->list.size(); i += 2)
                {
                    if (Eval(n->c->list[i], env).Truthy())
                        return ExecBlockScoped(n->c->list[i + 1], env);
                }
                if (n->c->list.size() % 2 == 1)
                    return ExecBlockScoped(n->c->list.back(), env);
                return Value();
            }
            case Node::While:
            {
                // while(var name = expr): the VarDecl condition both
                // binds the loop variable and supplies the test value
                if (n->a->k == Node::VarDecl)
                {
                    while (true)
                    {
                        Value v = Exec(n->a, env);
                        if (nReturn) return v;
                        if (!v.Truthy())
                            break;
                        Value r = ExecBlockScoped(n->b, env);
                        if (nBreak) { nBreak--; break; }
                        if (nContinue) { nContinue--; continue; }
                        if (nReturn) return r;
                    }
                    return Value();
                }
                if (n->a->k == Node::Block)
                {
                    while (true)
                    {
                        Value v = ExecBlockScoped(n->a, env);
                        if (nReturn) return v;
                        if (!v.Truthy())
                            break;
                        Value r = ExecBlockScoped(n->b, env);
                        if (nBreak) { nBreak--; break; }
                        if (nContinue) { nContinue--; continue; }
                        if (nReturn) return r;
                    }
                    return Value();
                }
                while (Exec(n->a, env).Truthy())
                {
                    Value r = ExecBlockScoped(n->b, env);
                    if (nBreak) { nBreak--; break; }
                    if (nContinue) { nContinue--; continue; }
                    if (nReturn) return r;
                }
                return Value();
            }
            case Node::ForNum:
            {
                auto loopEnv = std::make_shared<Env>();
                loopEnv->self = loopEnv;
                loopEnv->parent = env ? env->self : globals.self;
                loopEnv->nsTable = env ? env->nsTable : nullptr;
                loopEnv->modTable = env ? env->modTable : nullptr;
                GcFrameScope fsLoop(*this, loopEnv);

                // Loop variable is strictly local to the for loop.
                // It does not mutate outer variables with the same name.
                wstring loopVarName;
                if (n->a && n->a->k == Node::Assign && n->a->a && n->a->a->k == Node::Ident)
                {
                    loopVarName = n->a->a->str;
                    Value initVal = Eval(n->a->b, env);
                    loopEnv->vars[loopVarName] = initVal;
                }
                else if (n->a && n->a->k == Node::VarDecl && !n->a->names.empty())
                {
                    loopVarName = n->a->names[0];
                    Value initVal;
                    if (!n->a->defaults.empty() && n->a->defaults[0])
                        initVal = Eval(n->a->defaults[0], env);
                    loopEnv->vars[loopVarName] = initVal;
                }
                else
                {
                    Exec(n->a, loopEnv.get());
                }

                // The "limit" tag means the middle slot is a plain numeric
                // bound (the 2-expr form 'for i=1;4' or the descending
                // 'for i=#t;1;-1'), not a boolean condition. Distinguish
                // them by the condition's shape: a comparison Bin is a
                // real condition and must be evaluated as written —
                // treating 'a<3' as a limit collapses it to 1/0 and the
                // loop exits an iteration early.
                bool bCmpCond = n->b && n->b->k == Node::Bin &&
                    (n->b->str == L"==" || n->b->str == L"!=" ||
                     n->b->str == L"<"  || n->b->str == L">" ||
                     n->b->str == L"<=" || n->b->str == L">=" ||
                     n->b->str == L"===" || n->b->str == L"!==");
                bool bLimitForm = n->names.size() > 0 &&
                                  n->names[0] == L"limit" && n->b &&
                                  !bCmpCond;
                double dStep = 1;
                int nDir = 1;
                // A literal numeric step ("for i=1;10;2") is folded; anything
                // else (a variable, i++, i+=2) is executed as a statement
                // AFTER the body — but the loop variable must still advance,
                // so a non-literal step is applied as an explicit +=.
                bool bNumStep = n->c && n->c->k == Node::ExprStmt &&
                                n->c->a && n->c->a->k == Node::Num;
                bool bUnaryStep = n->c && n->c->k == Node::ExprStmt &&
                                  n->c->a && n->c->a->k == Node::Un &&
                                  n->c->a->str == L"-";
                if (bNumStep)
                {
                    dStep = n->c->a->num;
                    if (dStep < 0) nDir = -1;
                }
                else if (bUnaryStep)
                {
                    dStep = -BNum(Eval(n->c->a->a, loopEnv.get()));
                    if (dStep < 0) nDir = -1;
                }
                // The loop variable must be looked up BY KEY on every access.
                // `loopEnv->vars` is an unordered_map: a `var` declared inside
                // the loop body inserts a new element, and the resulting
                // rehash invalidates every node pointer. A cached
                // `Value*` would then point at freed memory — the loop
                // counter would read/write garbage and assignments made in
                // the body would silently vanish. Re-resolving the slot
                // keeps the counter correct no matter how the body mutates
                // the environment.
                auto findLoopVar = [&]() -> Value* {
                    if (loopVarName.empty()) return nullptr;
                    auto it = loopEnv->vars.find(loopVarName);
                    return (it == loopEnv->vars.end()) ? nullptr : &it->second;
                };
                bool bConstLimit = bLimitForm && n->b && n->b->k == Node::Num;
                double dConstLimit = bConstLimit ? n->b->num : 0;
                // When step is omitted, default step is ALWAYS 1 (forward).
                // Descending loops require an explicit negative step (e.g. `for(i=10;1;-1)`).
                auto bContinue = [&]() -> bool {
                    if (!n->b)
                        return true;        // empty condition slot: forever
                    if (!bLimitForm)
                        return Eval(n->b, loopEnv.get()).Truthy();
                    Value* pLoopVar = findLoopVar();
                    if (pLoopVar)
                    {
                        if (pLoopVar->t == Value::Num)
                        {
                            double lim = bConstLimit ? dConstLimit : BNum(Eval(n->b, loopEnv.get()));
                            return nDir >= 0 ? pLoopVar->n <= lim : pLoopVar->n >= lim;
                        }
                        return false;
                    }
                    if (n->a && n->a->k == Node::Assign &&
                        n->a->a && n->a->a->k == Node::Ident)
                    {
                        Value* pv = loopEnv->Find(n->a->a->str);
                        if (pv && pv->t == Value::Num)
                        {
                            double lim = bConstLimit ? dConstLimit : BNum(Eval(n->b, loopEnv.get()));
                            return nDir >= 0 ? pv->n <= lim : pv->n >= lim;
                        }
                        return false;
                    }
                    return Eval(n->b, loopEnv.get()).Truthy();
                };
                while (bContinue())
                {
                    Value r = ExecBlock(n->d, loopEnv.get());
                    if (nBreak) { nBreak--; break; }
                    if (nContinue) nContinue--;
                    if (nReturn) return r;
                    // A literal numeric step (or a bare -N) is applied to
                    // the loop variable directly. A bare identifier step
                    // (`for x=x1;x2;step`) is the step VALUE, not a
                    // statement — evaluate it and advance. Everything else
                    // (i++, i+=2) already mutates the variable itself.
                    bool bIdentStep = n->c && n->c->k == Node::ExprStmt &&
                                      n->c->a && n->c->a->k == Node::Ident;
                    if (bNumStep || bUnaryStep || bIdentStep)
                    {
                        if (bIdentStep)
                        {
                            Value vS = Eval(n->c->a, loopEnv.get());
                            if (vS.t == Value::Num) dStep = vS.n;
                        }
                        Value* pLoopVar = findLoopVar();
                        if (pLoopVar)
                        {
                            if (pLoopVar->t == Value::Num)
                                pLoopVar->n += dStep;
                        }
                        else if (n->a && n->a->k == Node::Assign && n->a->a &&
                            n->a->a->k == Node::Ident)
                        {
                            Value* pv = loopEnv->Find(n->a->a->str);
                            if (pv && pv->t == Value::Num)
                                pv->n += dStep;
                        }
                    }
                    else if (n->c)
                    {
                        Exec(n->c, loopEnv.get());
                    }
                    else if (!loopVarName.empty())
                    {
                        Value* pv = loopEnv->Find(loopVarName);
                        if (pv && pv->t == Value::Num)
                            *pv = Value::Num_(pv->n + dStep);
                    }
                    else if (n->a && n->a->k == Node::Assign && n->a->a &&
                             n->a->a->k == Node::Ident)
                    {
                        // 2-expr form: no step slot, derive from direction
                        Value* pv = loopEnv->Find(n->a->a->str);
                        if (pv && pv->t == Value::Num)
                            *pv = Value::Num_(pv->n + dStep);
                    }
                }
                return Value();
            }
            case Node::ForIn:
            {
                // the iterator and the iteration result live in C++ locals
                // across the loop body — keep them reachable
                GcRootScope rs;
                Value it = Eval(n->a, env);
                rs.add(it);
                // iterator function: official for-in calls it with no args
                // until the primary return is null; extra returns bind to
                // the second loop variable (`return k, v`). Iterator
                // closures hand back exactly these multiple returns.
                if (it.t == Value::Func)
                {
                    vector<Value> iterArgs;
                    for (;;)
                    {
                        // native iterators (fiber.generator, io.file
                        // each/lines) must not go through CallVmFunc —
                        // they have no AST body
                        Value r = it.native ? it.native(iterArgs)
                                            : CallVmFunc(it, iterArgs);
                        rs.add(r);
                        if (r.t == Value::Null)
                            return Value();
                        auto iterEnv = std::make_shared<Env>();
                        iterEnv->self = iterEnv;
                        iterEnv->parent = env ? env->self : globals.self;
                        Value r0 = r;
                        r0.mrets = nullptr;
                        iterEnv->vars[n->names[0]] = r0;
                        for (size_t ni = 1; ni < n->names.size(); ni++)
                        {
                            size_t mi = ni - 1;
                            Value rm = (r.mrets && mi < r.mrets->size())
                                    ? (*r.mrets)[mi] : Value();
                            rm.mrets = nullptr;
                            iterEnv->vars[n->names[ni]] = rm;
                        }
                        GcFrameScope fsIter(*this, iterEnv);
                        Value rb = ExecBlock(n->b, iterEnv.get());
                        if (nBreak) { nBreak--; return Value(); }                        if (nContinue) { nContinue--; }
                        if (nReturn) { ret = rb; return ret; }
                        iterArgs = { r0 };
                    }
                }
                if (it.t != Value::TableT || !it.tab)
                    return Value();
                auto step = [&](const wstring& k, const Value& v) -> int {
                    auto iterEnv = std::make_shared<Env>();
                    iterEnv->self = iterEnv;
                    iterEnv->parent = env ? env->self : globals.self;
                    iterEnv->vars[n->names[0]] = Value::Str_(k);
                    if (n->names.size() > 1)
                        iterEnv->vars[n->names[1]] = v;
                    GcFrameScope fsStep(*this, iterEnv);
                    Value r = ExecBlock(n->b, iterEnv.get());
                    if (nBreak) { nBreak--; return 1; }
                    if (nContinue) { nContinue--; return 0; }
                    if (nReturn) { ret = r; return 2; }
                    return 0;
                };
                for (size_t i = 0; i < it.tab->arr.size(); i++)
                {
                    int rc = step(std::to_wstring(i + 1), it.tab->arr[i]);
                    if (rc == 1) return Value();
                    if (rc == 2) return ret;
                }
                vector<wstring> keys;   // snapshot: body may mutate the map
                for (auto& kv : it.tab->map)
                {
                    // The metatable is NOT a member in official aardio —
                    // `t@` reaches it, but it never shows up when the
                    // members are iterated (table.keys / json.stringify
                    // iterate the members, and a visible "_meta" would
                    // leak the metatable into the JSON output).
                    if (kv.first == L"_meta") continue;
                    keys.push_back(kv.first);
                }
                for (auto& k : keys)
                {
                    auto itv = it.tab->map.find(k);
                    Value v = itv != it.tab->map.end() ? itv->second : Value();
                    // a numeric-string key (form._ctrlHwnd[hwndNumber]) is a
                    // NUMBER — iterating it as a string hands
                    // win.getStyle(string) a marshalled pointer and the
                    // WM_SIZE/enumControl chain then tears the control down
                    if (!k.empty() &&
                        k.find_first_not_of(L"0123456789") == wstring::npos)
                    {
                        long long nK = _wtoi64(k.c_str());
                        if (nK > 0)
                        {
                            Value vk = Value::Num_((double)nK);
                            auto iterEnv = std::make_shared<Env>();
                            iterEnv->self = iterEnv;
                            iterEnv->parent = env ? env->self : globals.self;
                            iterEnv->vars[n->names[0]] = vk;
                            if (n->names.size() > 1)
                                iterEnv->vars[n->names[1]] = v;
                            GcFrameScope fsKey(*this, iterEnv);
                            Value r = ExecBlock(n->b, iterEnv.get());
                            if (nBreak) { nBreak--; return Value(); }
                            if (nContinue) { nContinue--; continue; }
                            if (nReturn) { ret = r; return ret; }
                            continue;
                        }
                    }
                    int rc = step(k, v);
                    if (rc == 1) return Value();
                    if (rc == 2) return ret;
                }
                return Value();
            }
            case Node::Try:
            {
                ExecBlockScoped(n->a, env);
                if (!err->empty())
                {
                    wstring msg = *err;
                    err->clear();
                    // S-2: a caught error must carry its location too — the
                    // catch block may print it, and the frames that raised
                    // it are already unwinding.  strErrFile still holds the
                    // file of the statement that failed.
                    if (nErrLine > 0 && !HasErrorLocation(msg))
                        AppendErrorLocation(&msg, strErrFile, nErrLine);
                    if (!n->b)
                        return Value();   // catch-less try{}: swallow
                    auto cEnv = std::make_shared<Env>();
                    cEnv->self = cEnv;
                    cEnv->parent = env ? env->self : globals.self;
                    if (!n->names.empty())
                        cEnv->vars[n->names[0]] = Value::Str_(msg);
                    GcFrameScope fsCatch(*this, cEnv);
                    ExecBlock(n->b, cEnv.get());
                }
                return Value();
            }
            case Node::Return:
            {
                vector<PNode> rawExprs;
                if (n->a) rawExprs.push_back(n->a);
                for (auto& e : n->list) rawExprs.push_back(e);

                vector<Value> flatVals;
                for (size_t i = 0; i < rawExprs.size(); i++)
                {
                    Value ev = Eval(rawExprs[i], env);
                    if (ev.bDots && ev.mrets)
                    {
                        for (auto& mr : *ev.mrets)
                            flatVals.push_back(mr);
                    }
                    else
                    {
                        flatVals.push_back(ev);
                        if (i == rawExprs.size() - 1 && ev.mrets && !ev.mrets->empty())
                        {
                            for (auto& mr : *ev.mrets)
                                flatVals.push_back(mr);
                        }
                    }
                }

                if (flatVals.empty())
                {
                    ret = Value();
                }
                else
                {
                    ret = flatVals[0];
                    ret.bDots = false;
                    if (flatVals.size() > 1)
                    {
                        ret.mrets = std::make_shared<vector<Value>>(
                            flatVals.begin() + 1, flatVals.end());
                    }
                    else
                    {
                        ret.mrets = nullptr;
                    }
                }
                nReturn++;
                return ret;
            }
            case Node::Break:
                nBreak++;
                return Value();
            case Node::Continue:
                nContinue++;
                return Value();
            case Node::Block:
            {
                auto inner = std::make_shared<Env>();
                inner->self = inner;
                inner->parent = env ? env->self : globals.self;
                GcFrameScope fsInner(*this, inner);
                return ExecBlock(n, inner.get());
            }
            default:
                return Eval(n, env);
            }
        }

        // ---------- round 19: import + builtin namespaces ----------

// ---- GetRoot ----
Value Interp::GetRoot(const wstring& name)
        {
            auto it = roots.find(name);
            if (it != roots.end())
                return it->second;
            auto tb = std::make_shared<Table>();
            Value v = Value::Table_(tb);
            roots[name] = v;
            globals.vars[name] = v;   // bare name resolves to root namespace
            return v;
        }

// ---- NsPath ----
Value Interp::NsPath(const vector<wstring>& names, Env* env, bool bRoot)
        {
            // resolve-or-create a dotted namespace table (merges with any
            // existing root or parent ambient namespace, so `namespace guid { ... }` inside `namespace win;`
            // extends win.guid)
            Value ns;
            shared_ptr<Table> curNs;
            if (!bRoot && env)
            {
                for (Env* e = env; e; e = e->parent.get())
                {
                    if (e->nsTable) { curNs = e->nsTable; break; }
                }
            }
            if (curNs)
            {
                auto it = curNs->map.find(names[0]);
                if (g_nsTrace)
                {
                    fwprintf(stderr, L"[NS]   resolve '%s' in curNs=%p -> %s\n",
                             names[0].c_str(), (void*)curNs.get(),
                             (it == curNs->map.end()) ? L"MISS(create)"
                                 : (it->second.t == Value::TableT || it->second.t == Value::Class)
                                   ? L"HIT" : L"HIT(other)");
                    fflush(stderr);
                }
                if (it != curNs->map.end() && (it->second.t == Value::TableT || it->second.t == Value::Class) && it->second.tab)
                {
                    ns = it->second;
                }
                else
                {
                    auto tb = std::make_shared<Table>();
                    ns = Value::Table_(tb);
                    curNs->map[names[0]] = ns;
                }
            }
            else
            {
                // both maps must name the SAME value: a script-level
                // `class X {}` replaces globals.vars[X] while roots[X] may
                // still hold the engine's placeholder table, and a namespace
                // opened afterwards would then extend the stale one
                // NB: the two maps are distinct containers, so their
                // iterators must never be compared to each other's end()
                Value cand;
                auto ir = roots.find(names[0]);
                if (ir != roots.end())
                    cand = ir->second;
                else
                {
                    auto ig = globals.vars.find(names[0]);
                    if (ig != globals.vars.end())
                        cand = ig->second;
                }
                if (!cand.tab && env)
                {
                    for (Env* e = env; e; e = e->parent.get())
                    {
                        if (e->modTable && (names.size() == 1 || names[0] == L"self"))
                        {
                            cand = Value::Table_(e->modTable);
                            break;
                        }
                    }
                }
                if ((cand.t == Value::TableT || cand.t == Value::Class) &&
                    cand.tab)
                {
                    ns = cand;
                    roots[names[0]] = ns;
                    globals.vars[names[0]] = ns;
                }
                else
                {
                    auto tb = std::make_shared<Table>();
                    ns = Value::Table_(tb);
                    roots[names[0]] = ns;
                    globals.vars[names[0]] = ns;
                }
            }
            for (size_t i = 1; i < names.size() && ns.tab; i++)
            {
                auto it2 = ns.tab->map.find(names[i]);
                if (it2 != ns.tab->map.end() && (it2->second.t == Value::TableT || it2->second.t == Value::Class) &&
                    it2->second.tab)
                    ns = it2->second;
                else
                {
                    auto tb = std::make_shared<Table>();
                    Value v = Value::Table_(tb);
                    ns.tab->map[names[i]] = v;
                    ns = v;
                }
            }
            if (ns.tab)
                ns.tab->map[L"self"] = ns;
            return ns;
        }

// ---- CallVmFunc ----
Value Interp::CallVmFunc(const Value& f, vector<Value>& args)
        {
            if (f.native)
                return f.native(args);
            if (f.t != Value::Func || !f.fn)
            {
                (*err) = L"运行错误: 回调不是函数";
                return Value();
            }
            auto local = std::make_shared<Env>();
            local->self = local;
            local->parent = f.envUp ? f.envUp : globals.self;
            local->nsTable = f.nsClosure;
            local->vars[L"owner"] = Value();   // native-side call: no owner
            // the callee frame and the call arguments live in C++ locals
            // — the collector needs them as roots
            gcFrames.push_back(local);
            GcRootScope rs;
            rs.add(f);
            rs.addAll(args);
            for (size_t i = 0; i < f.params.size(); i++)
            {
                if (i < args.size() && args[i].t != Value::Null)
                    local->vars[f.params[i]] = args[i];
                else if (f.pdefaults &&
                         i < f.pdefaults->size() &&
                         (*f.pdefaults)[i])
                    local->vars[f.params[i]] =
                        Eval((*f.pdefaults)[i], local.get());
                else if (i < args.size())
                    local->vars[f.params[i]] = args[i];
                else
                    local->vars[f.params[i]] = Value();
            }
            if (f.params.size() && f.params.back() == L"..." && f.fn)
            {
                auto vargs = std::make_shared<vector<Value>>();
                auto tbV = std::make_shared<Table>();
                for (size_t k = f.params.size() - 1; k < args.size(); k++)
                {
                    vargs->push_back(args[k]);
                    tbV->arr.push_back(args[k]);
                }
                // `arg` is the alias for the varargs table;
                // `...` expands it at call sites.
                Value vDots = Value::Table_(tbV);
                vDots.mrets = vargs;
                vDots.bDots = true;
                local->vars[L"..."] = vDots;
                local->vars[L"arg"] = vDots;
                for (size_t i = 0; i + 1 < f.params.size(); i++)
                    if (f.params[i] == L"arg" && i < args.size())
                        local->vars[L"arg"] = args[i];
            }
            Value oldRet = ret;
            int oldR = nReturn, oldB = nBreak, oldC = nContinue;
            nReturn = 0;
            ret = Value();
            ExecBlock(f.fn, local.get());
            Value r = (nReturn > 0) ? ret : Value();
            nReturn = oldR;
            nBreak = oldB;
            nContinue = oldC;
            ret = oldRet;
            GcFrameCheck(gcFrames, local.get(), "callVmFunc");
            gcFrames.pop_back();
            CleanDeadEnv(local);
            return r;
        }
}
