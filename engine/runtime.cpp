#include "engine/interp.h"

namespace NodioVM
{
Interp* g_interpCurrent = nullptr;   // M2: active interp for meta hooks
Interp* Interp::g_self = nullptr;    // M2: self for meta callbacks

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
                return Value::Str_(s);
            }
            case Node::Bool: return Value::Bool_(n->num != 0);
            case Node::NullK: return Value();
            case Node::Ident:
            {
                if (n->str == L"..") return Value();  // root marker
                if (Value* v = env->Find(n->str))
                    return *v;
                auto itR = roots.find(n->str);
                if (itR != roots.end())
                    return itR->second;
                return Value();
            }
            case Node::Member:
            {
                Value obj;
                if (n->bRoot)
                    obj = Value();   // root namespace = globals table
                else
                    obj = Eval(n->a, env);
                if (n->bSafe && obj.t == Value::Null)
                    return Value();   // '?.': null short-circuits
                if (n->bMeta)
                {
                    // 'tab@' — the metatable itself
                    if (obj.t == Value::TableT && obj.tab)
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
                Value obj = Eval(n->a, env);
                if (n->bSafe && obj.t == Value::Null)
                    return Value();   // '?[': null short-circuits
                Value idx = Eval(n->b, env);
                // aardio byte-string model: s[i] reads the i-th BYTE
                // (1-based) as a number — the official string library is
                // built on this primitive. Byte-per-wchar strings (from
                // fromto/raw IO, all chars <= 0xFF) index directly; real
                // text indexes its UTF-8 encoding.
                if (obj.t == Value::Str && idx.t == Value::Num)
                {
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
                return GetMember(obj, idx.ToString(), n->bDirect);
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
                Value v = Eval(n->a, env);
                if (n->str == L"-") return Value::Num_(-ToNum(v));
                if (n->str == L"!" || n->str == L"not") return Value::Bool_(!v.Truthy());
                if (n->str == L"~")
                {
                    // aardio style=~0: bitwise NOT over 32-bit width
                    unsigned int u = (unsigned int)(INT_PTR)(long long)ToNum(v);
                    return Value::Num_((double)(int)~u);
                }
                if (n->str == L"#")
                {
                    if (v.t == Value::Str)
                    {
                        // aardio byte-string model: #s = UTF-8 BYTE length.
                        // Storage is wstring, so raw byte strings produced
                        // by fromto/io.readFile("raw") live as one byte
                        // per wchar (all chars <= 0xFF) — for those the
                        // wchar count IS the byte count; real text (any
                        // char > 0xFF) counts its UTF-8 encoding.
                        bool bFake = true;
                        for (wchar_t c : v.s)
                            if ((unsigned int)c > 0xFF) { bFake = false; break; }
                        if (bFake)
                            return Value::Num_((double)v.s.size());
                        int cb = WideCharToMultiByte(CP_UTF8, 0,
                                                     v.s.c_str(),
                                                     (int)v.s.size(),
                                                     nullptr, 0, nullptr,
                                                     nullptr);
                        return Value::Num_((double)cb);
                    }
                    if (v.t == Value::TableT) return Value::Num_((double)v.tab->arr.size());
                }
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
                for (Env* e = env; e; e = e->parent)
                {
                    if (!f.thisVal)
                    {
                        auto itThis = e->vars.find(L"this");
                        if (itThis != e->vars.end() && itThis->second.t != Value::Null)
                            f.thisVal = std::make_shared<Value>(itThis->second);
                    }
                    if (e->nsTable) { f.nsClosure = e->nsTable; break; }
                }
                if (!f.nsClosure)
                {
                    for (Env* e = env; e; e = e->parent)
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
                    if (k.t == Value::Num && k.n == (long long)k.n &&
                        k.n >= 1 && (size_t)(k.n - 1) == tb->arr.size())
                        tb->arr.push_back(v);
                    else
                        tb->map[k.ToString()] = v;
                }
                return Value::Table_(tb);
            }
            case Node::Class: return EvalClass(n, env);
            case Node::ExprStmt: return Eval(n->a, env);
            default: return Value();
            }
        }

// ---- ToNum ----
double Interp::ToNum(const Value& v)
        {
            if (v.t == Value::Num) return v.n;
            if (v.t == Value::Str)
            {
                wchar_t* e = nullptr;
                double d = wcstod(v.s.c_str(), &e);
                return d;
            }
            if (v.t == Value::Bool) return v.b ? 1 : 0;
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
bool Interp::MetaGet(const Value& obj, const wstring& key,
                            Value* out, bool* pErr, wstring* pErrMsg,
                            bool bOwnerCall)
        {
            *pErr = false;
            static thread_local int s_metaDepth = 0;
            if (s_metaDepth > 40)
            {
                fwprintf(stderr, L"[DEBUG MetaGet recursion: key=%s]\n", key.c_str());
                return false;
            }
            struct ScopeDepth { int& d; ScopeDepth(int& x): d(x){ d++; } ~ScopeDepth(){ d--; } } sd(s_metaDepth);
            if (obj.t != Value::TableT || !obj.tab || key == L"_meta")
                return false;
            auto im = obj.tab->map.find(L"_meta");
            if (im == obj.tab->map.end() || im->second.t != Value::TableT ||
                !im->second.tab)
                return false;
            Table* mt = im->second.tab.get();
            // direct method/field entries in the meta table resolve first
            // (RECT._meta = { width = function(){...} } shape)
            auto idm = mt->map.find(key);
            if (idm != mt->map.end() && idm->second.t != Value::Null &&
                key != L"_get" && key != L"_set" && key != L"_call" &&
                key != L"_tostring")
            {
                if (idm->second.t == Value::TableT && idm->second.tab)
                {
                    // nested property table: { _get = fn } / { _set = fn }
                    auto ig2 = idm->second.tab->map.find(L"_get");
                    if (ig2 != idm->second.tab->map.end() &&
                        (ig2->second.t == Value::Func ||
                         ig2->second.native))
                    {
                        vector<Value> noArgs;
                        Value r = CallVmFuncStatic(obj, ig2->second,
                                                   noArgs, pErr, pErrMsg);
                        *out = r;
                        return true;
                    }
                    return false;
                }
                *out = idm->second;
                return true;
            }
            auto ig = mt->map.find(L"_get");
            if (ig == mt->map.end() || ig->second.t == Value::Null)
                return false;
            // re-entrancy guard: _meta walk itself must not recurse
            Value g = ig->second;
            if (g.t == Value::TableT && g.tab)
            {
                auto ie = g.tab->map.find(key);
                if (ie == g.tab->map.end() || ie->second.t == Value::Null)
                    return false;
                Value e = ie->second;
                if (e.t == Value::TableT && e.tab)
                {
                    auto ig2 = e.tab->map.find(L"_get");
                    if (ig2 != e.tab->map.end() &&
                        (ig2->second.t == Value::Func ||
                         ig2->second.native))
                    {
                        // property getter: call with owner=obj, no args
                        vector<Value> noArgs;
                        Value r = CallVmFuncStatic(obj, ig2->second,
                                                   noArgs, pErr, pErrMsg);
                        *out = r;
                        return true;
                    }
                    return false;
                }
                if (e.t == Value::Func || e.native)
                {
                    // method entry: return it; owner binds at call time
                    *out = e;
                    return true;
                }
                *out = e;   // plain value in the _get table
                return true;
            }
            if (g.t == Value::Func || g.native)
            {
                // function form: g(k, ownerCall) with owner=obj
                vector<Value> args{ Value::Str_(key), Value::Bool_(bOwnerCall) };
                Value r = CallVmFuncStatic(obj, g, args, pErr, pErrMsg);
                *out = r;
                return true;
            }
            return false;
        }

// ---- MetaSet ----
bool Interp::MetaSet(const Value& obj, const wstring& key,
                            const Value& val, bool* pErr, wstring* pErrMsg)
        {
            *pErr = false;
            if (obj.t != Value::TableT || !obj.tab || key == L"_meta")
                return false;
            auto im = obj.tab->map.find(L"_meta");
            if (im == obj.tab->map.end() || im->second.t != Value::TableT ||
                !im->second.tab)
                return false;
            Table* mt = im->second.tab.get();
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
                vector<Value> args{ Value::Str_(key), val };
                CallVmFuncStatic(obj, s, args, pErr, pErrMsg);
                return true;
            }
            return false;
        }

        // instance method shim: CallVmFunc with owner forced to obj

// ---- CallOwner ----
Value Interp::CallOwner(const Value& f, const Value& obj,
                        vector<Value>& args)
        {
            if (f.native)
                return f.native(args);
            if (f.t != Value::Func || !f.fn)
            {
                (*err) = L"运行错误: 元表回调不是函数";
                return Value();
            }
            Env local;
            local.parent = &globals;
            local.nsTable = f.nsClosure;
            local.vars[L"owner"] = obj;
            if (f.thisVal)
                local.vars[L"this"] = *f.thisVal;
            else
                local.vars[L"this"] = obj;
            for (size_t i = 0; i < f.params.size(); i++)
            {
                if (i < args.size())
                    local.vars[f.params[i]] = args[i];
                else if (f.pdefaults && i < f.pdefaults->size() &&
                         (*f.pdefaults)[i])
                    local.vars[f.params[i]] =
                        Eval((*f.pdefaults)[i], &local);
                else
                    local.vars[f.params[i]] = Value();
            }
            if (f.params.size() && f.params.back() == L"..." && f.fn)
            {
                if (args.size() > f.params.size() - 1)
                {
                    auto tbV = std::make_shared<Table>();
                    for (size_t k = f.params.size() - 1; k < args.size(); k++)
                        tbV->arr.push_back(args[k]);
                    // aardio scripts read varargs through `arg`, while the
                    // `...` spelling expands them at a call site — bind both
                    // names to the same table. An EMPTY table must read as
                    // null: library code guards recursion with `if(!...)`,
                    // and an empty table is truthy in aardio.
                    Value vDots = tbV->arr.empty() ? Value()
                                                   : Value::Table_(tbV);
                    local.vars[L"..."] = vDots;
                    local.vars[L"arg"] = vDots;
                }
                else
                {
                    local.vars[L"..."] = Value();
                    local.vars[L"arg"] = Value();
                }
            }
            Value old = ret;
            int oldR = nReturn, oldB = nBreak, oldC = nContinue;
            nReturn = nBreak = nContinue = 0;
            Value r = ExecBlock(f.fn, &local);
            nReturn = oldR; nBreak = oldB; nContinue = oldC;
            ret = old;
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

// ---- GetMember ----
Value Interp::GetMember(const Value& obj, const wstring& key,
                        bool bDirect, bool bOwnerCall)
        {
            if (obj.t == Value::TableT || ((obj.t == Value::Func || obj.t == Value::Class) && obj.tab))
            {
                if (obj.tab)
                {
                    auto it = obj.tab->map.find(key);
                    if (it != obj.tab->map.end())
                        return it->second;
                    // 1-based numeric index = array access (aardio arrays)
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
                    if (MetaGet(obj, key, &r, &bErr, &strErrM, bOwnerCall))
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
            // aardio root namespaces visible on a null object (..string / module)
            if (obj.t == Value::Null)
            {
                auto it = roots.find(key);
                if (it != roots.end())
                    return it->second;
                // ::Name also reaches plain globals (::User32 etc.)
                auto ig = globals.vars.find(key);
                if (ig != globals.vars.end())
                    return ig->second;
            }
            return Value();
        }

// ---- EvalCall ----
Value Interp::EvalCall(const PNode& n, Env* env)
        {
            // builtin fast paths: io.print / print
            if (n->a->k == Node::Member && n->a->str == L"print" &&
                n->a->a->k == Node::Ident && n->a->a->str == L"io")
            {
                wstring out;
                for (auto& arg : n->list)
                    out += Eval(arg, env).ToString();
                out += L"\n";
                WriteU8(stdout, out);
                fflush(stdout);
                return Value();
            }
            if (n->a->k == Node::Ident && n->a->str == L"print")
            {
                wstring out;
                for (auto& arg : n->list)
                    out += Eval(arg, env).ToString();
                out += L"\n";
                WriteU8(stdout, out);
                fflush(stdout);
                return Value();
            }
            if (n->a->k == Node::Ident && n->a->str == L"tostring")
            {
                return n->list.empty() ? Value() :
                    Value::Str_(Eval(n->list[0], env).ToString());
            }
            if (n->a->k == Node::Ident && n->a->str == L"tonumber")
            {
                return n->list.empty() ? Value() :
                    Value::Num_(ToNum(Eval(n->list[0], env)));
            }
            if (n->a->k == Node::Ident && n->a->str == L"error")
            {
                // aardio error(): raise a runtime error message
                wstring msg = n->list.empty() ? L"运行错误" :
                    L"运行错误: " + Eval(n->list[0], env).ToString();
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
            if (n->a->k == Node::Member && !n->a->bRoot)
            {
                bMethod = true;
                vThis = Eval(n->a->a, env);
                f = GetMember(vThis, n->a->str, false, /*bOwnerCall=*/true);
            }
            else if (n->a->k == Node::Index)
            {
                bMethod = true;
                vThis = Eval(n->a->a, env);
                Value idx = Eval(n->a->b, env);
                f = GetMember(vThis, idx.ToString(), n->a->bDirect, /*bOwnerCall=*/true);
            }
            else
            {
                f = Eval(n->a, env);
            }
            static thread_local int s_callDepth = 0;
            struct CallDepthGuard {
                int& d;
                CallDepthGuard(int& x) : d(x) { d++; }
                ~CallDepthGuard() { d--; }
            } cdg(s_callDepth);
            if (s_callDepth > 100)
            {
                wstring fnName = f.fname;
                if (fnName.empty() && n->a->k == Node::Member) fnName = n->a->str;
                else if (fnName.empty() && n->a->k == Node::Ident) fnName = n->a->str;
                fwprintf(stderr, L"[DEBUG INFINITE CALL DEPTH %d] line=%d fname=%s\n",
                         s_callDepth, n->line, fnName.c_str());
                if (s_callDepth > 105)
                {
                    (*err) = L"运行错误: 栈溢出 (超过最大调用深度) @ 行 " + std::to_wstring(n->line) + L" 函数: " + fnName;
                    return Value();
                }
            }
            {
                static bool bDbg = GetEnvironmentVariableW(L"CMT", nullptr, 0) != 0;
                if (bDbg && (n->line == 113 || n->line == 7))
                {
                    fwprintf(stderr, L"[DEBUG CALL %d] f.t=%d bMethod=%d vThis.t=%d fn=%s\n",
                             n->line, (int)f.t, bMethod, (int)vThis.t, f.fname.c_str());
                }
            }
            auto evalCallArgs = [this, env](const vector<PNode>& exprs) {
                vector<Value> res;
                for (auto& a : exprs)
                {
                    if (a->k == Node::Ident && a->str == L"...")
                    {
                        Value vDots = Eval(a, env);
                        if (vDots.t == Value::TableT && vDots.tab)
                        {
                            for (auto& item : vDots.tab->arr)
                                res.push_back(item);
                        }
                        else if (vDots.t != Value::Null)
                        {
                            res.push_back(vDots);
                        }
                    }
                    else
                    {
                        res.push_back(Eval(a, env));
                    }
                }
                return res;
            };
            if (f.t == Value::Class)
            {
                // instantiation: copy template members, run ctor with `this`
                vector<Value> args = evalCallArgs(n->list);
                auto inst = std::make_shared<Table>();
                if (f.tab)
                {
                    auto itS = f.tab->map.find(L"_struct");
                    if (itS != f.tab->map.end())
                        inst->map[L"_struct"] = itS->second;
                    auto itM = f.tab->map.find(L"_meta");
                    if (itM != f.tab->map.end())
                        inst->map[L"_meta"] = itM->second;
                }
                Value vInst = Value::Table_(inst);
                Env classNsEnv;
                if (f.tab)
                {
                    classNsEnv.parent = &globals;
                    classNsEnv.nsTable = f.tab;
                }
                Env local;
                local.parent = f.tab ? &classNsEnv : &globals;
                local.nsTable = inst;
                if (bMethod)
                    local.vars[L"owner"] = vThis;   // owner = dot-left
                local.vars[L"this"] = vInst;

                if (f.classBody)
                {
                    Value oldRet = ret;
                    int oldR = nReturn, oldB = nBreak, oldC = nContinue;
                    nReturn = nBreak = nContinue = 0;
                    ExecBlock(f.classBody, &local);
                    nReturn = oldR; nBreak = oldB; nContinue = oldC;
                    ret = oldRet;
                    auto itThis = local.vars.find(L"this");
                    if (itThis != local.vars.end() &&
                        itThis->second.t == Value::TableT)
                        vInst = itThis->second;
                }

                if (f.fn)
                {
                    local.vars[L"this"] = vInst;
                    for (size_t i = 0; i < f.fn->names.size(); i++)
                    {
                        if (i < args.size())
                            local.vars[f.fn->names[i]] = args[i];
                        else if (f.pdefaults &&
                                 i < f.pdefaults->size() &&
                                 (*f.pdefaults)[i])
                            local.vars[f.fn->names[i]] =
                                Eval((*f.pdefaults)[i], &local);
                        else
                            local.vars[f.fn->names[i]] = Value();
                    }
                    if (f.fn->names.size() && f.fn->names.back() == L"...")
                    {
                        if (args.size() > f.fn->names.size() - 1)
                        {
                            auto tbV = std::make_shared<Table>();
                            for (size_t k = f.fn->names.size() - 1; k < args.size(); k++)
                                tbV->arr.push_back(args[k]);
                            // `arg` is aardio's spelling for the varargs
                            // table; `...` expands it at call sites. An
                            // empty table reads as null so `if(!...)`
                            // recursion guards work.
                            Value vDots = tbV->arr.empty() ? Value()
                                                           : Value::Table_(tbV);
                            local.vars[L"..."] = vDots;
                            local.vars[L"arg"] = vDots;
                        }
                        else
                        {
                            local.vars[L"..."] = Value();
                            local.vars[L"arg"] = Value();
                        }
                    }
                    Value oldRet = ret;
                    int oldR = nReturn, oldB = nBreak, oldC = nContinue;
                    nReturn = nBreak = nContinue = 0;
                    ExecBlock(f.fn->a, &local);
                    bool bCtorRet = (nReturn > 0);
                    Value ctorRet = ret;
                    nReturn = oldR; nBreak = oldB; nContinue = oldC;
                    ret = oldRet;
                    if (bCtorRet && ctorRet.t != Value::Null)
                    {
                        if (ctorRet.t == Value::TableT && ctorRet.tab && vInst.tab)
                        {
                            for (auto& kv : vInst.tab->map)
                            {
                                if (ctorRet.tab->map.find(kv.first) == ctorRet.tab->map.end())
                                    ctorRet.tab->map[kv.first] = kv.second;
                            }
                        }
                        return ctorRet;
                    }
                    // aardio: `this = <other>` inside the ctor rebinds the
                    // instance (win.ui fake-window / control wrappers)
                    auto itThis = local.vars.find(L"this");
                    if (itThis != local.vars.end() &&
                        itThis->second.t == Value::TableT)
                    {
                        if (itThis->second.tab != vInst.tab && vInst.tab)
                        {
                            for (auto& kv : vInst.tab->map)
                            {
                                if (itThis->second.tab->map.find(kv.first) == itThis->second.tab->map.end())
                                    itThis->second.tab->map[kv.first] = kv.second;
                            }
                        }
                        vInst = itThis->second;
                    }
                }
                return vInst;
            }
            if (f.native)
            {
                vector<Value> args = evalCallArgs(n->list);
                return f.native(args);
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
            Env local;
            local.parent = &globals;   // aardio: functions see globals
            local.nsTable = f.nsClosure;   // defining namespace stays in scope
            if (bMethod)
            {
                if (f.thisVal)
                    local.vars[L"this"] = *f.thisVal;
                else
                    local.vars[L"this"] = vThis;   // obj.fn() receiver as this
                // aardio owner = the dot-left object of this call
                local.vars[L"owner"] = vThis;
            }
            else
            {
                if (f.thisVal)
                    local.vars[L"this"] = *f.thisVal;
                local.vars[L"owner"] = Value();
            }
            for (size_t i = 0; i < f.params.size(); i++)
            {
                if (i < args.size())
                    local.vars[f.params[i]] = args[i];
                else if (f.pdefaults &&
                         i < f.pdefaults->size() &&
                         (*f.pdefaults)[i])
                    local.vars[f.params[i]] =
                        Eval((*f.pdefaults)[i], &local);
                else
                    local.vars[f.params[i]] = Value();
            }
            // "..." varargs: leftover arguments as a 1-based table. aardio
            // spells the table `arg` in scripts and `...` at call sites, so
            // bind both names.
            if (f.params.size() && f.params.back() == L"..." &&
                f.fn)
            {
                auto tbV = std::make_shared<Table>();
                for (size_t k = f.params.size() - 1; k < args.size(); k++)
                    tbV->arr.push_back(args[k]);
                // An EMPTY varargs table must read as null: library code
                // guards recursion with `if(!...)`, and an empty table is
                // truthy in aardio.
                Value vDots = tbV->arr.empty() ? Value()
                                               : Value::Table_(tbV);
                local.vars[L"..."] = vDots;
                local.vars[L"arg"] = vDots;
            }
            Value old = ret;
            int oldR = nReturn;
            nReturn = 0;
            Value r = ExecBlock(f.fn, &local);
            nReturn = oldR;
            ret = old;
            return r;
        }

// ---- EvalBin ----
Value Interp::EvalBin(const PNode& n, Env* env)
        {
            const wstring& op = n->str;
            if (op == L"?:")   // lazy ternary
                return Eval(n->a, env).Truthy() ? Eval(n->b, env)
                                                : Eval(n->c, env);
            if (op == L"?")    // aardio guard: a ? b — a truthy ? b : false
            {
                Value l = Eval(n->a, env);
                return l.Truthy() ? Eval(n->b, env) : Value::Bool_(0);
            }
            if (op == L"&&")
            {
                Value l = Eval(n->a, env);
                return l.Truthy() ? Eval(n->b, env) : l;
            }
            if (op == L":")   // aardio falsy coalescing: a:b returns b when
                             // a is falsy (null/false/0/""), else a — see
                             // docs/language-reference/operator/logical.md
            {
                Value l = Eval(n->a, env);
                return l.Truthy() ? l : Eval(n->b, env);
            }
            if (op == L"||")
            {
                Value l = Eval(n->a, env);
                return l.Truthy() ? l : Eval(n->b, env);
            }
            Value l = Eval(n->a, env), r = Eval(n->b, env);
            if (op == L"===")
                return ApplyOp(op, l, r);
            if (op == L"==") return Value::Bool_(l.ToString() == r.ToString() &&
                                                l.t == r.t);
            if (op == L"!=") return Value::Bool_(!(l.ToString() == r.ToString() &&
                                                   l.t == r.t));
            if (op == L"!==")
                return Value::Bool_(!(l.ToString() == r.ToString() &&
                                      l.t == r.t) &&
                                    !(l.t == Value::Null &&
                                      r.t == Value::Null));
            if (op == L"..")
            {
                if (l.t == Value::TableT || r.t == Value::TableT)
                    return l;   // aardio table concat quirk; keep simple
                return Value::Str_(l.ToString() + r.ToString());
            }
            if (op == L"++")
                return Value::Str_(ToStringM(l) + ToStringM(r));  // aardio concat
            // aardio: '+' concatenates whenever either operand is a
            // non-numeric string, or a string meets a non-string
            // (docs/operator/concat.md: "引号前后的 + 将自动转换为字符串
            // 连接运算符 ++"). A numeric string plus a number stays
            // arithmetic, matching the documented '1 + "2"' -> 3.
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
                        return Value::Str_(ToStringM(l) + ToStringM(r));
                }
            }
            double a = ToNum(l), b = ToNum(r);
            if (op == L"+") return Value::Num_(a + b);
            if (op == L"-") return Value::Num_(a - b);
            if (op == L"*") return Value::Num_(a * b);
            if (op == L"/") return Value::Num_(b != 0 ? a / b : 0);
            if (op == L"%" || op == L"%=") return Value::Num_(b != 0 ? fmod(a, b) : 0);
            // M2: bitwise (32-bit width, aardio DWORD semantics)
            unsigned int ua = (unsigned int)(INT_PTR)(long long)a;
            unsigned int ub = (unsigned int)(INT_PTR)(long long)b;
            if (op == L"&") return Value::Num_((double)(int)(ua & ub));
            if (op == L"|") return Value::Num_((double)(int)(ua | ub));
            if (op == L"^") return Value::Num_((double)(int)(ua ^ ub));   // aardio: ^ is XOR
            if (op == L"<<") return Value::Num_((double)(int)(ua << (ub & 31)));
            if (op == L">>") return Value::Num_((double)(int)(ua >> (ub & 31)));
            if (op == L">>>")
                return Value::Num_((double)((long long)(ua >> (ub & 31))));
            if (op == L"**") return Value::Num_(pow(a, b));   // aardio: ** is exponent
            if (op == L"<") return Value::Bool_(a < b && l.t == r.t ? true : CmpLess(l, r));
            if (op == L">") return Value::Bool_(CmpLess(r, l));
            if (op == L"<=") return Value::Bool_(!CmpLess(r, l));
            if (op == L">=") return Value::Bool_(!CmpLess(l, r));
            return Value();
        }

// ---- CmpLess ----
bool Interp::CmpLess(const Value& l, const Value& r)
        {
            if (l.t == Value::Str && r.t == Value::Str)
                return l.s < r.s;
            if (l.t == Value::Num && r.t == Value::Num)
                return l.n < r.n;
            return false;
        }

// ---- ApplyOp ----
Value Interp::ApplyOp(const wstring& op, const Value& l, const Value& r)
        {
            if (op == L"===")
                return Value::Bool_(l.t == r.t &&
                                    l.ToString() == r.ToString());
            if (op == L"..")
                return Value::Str_(l.ToString() + r.ToString());
            if (op == L"++")
                return Value::Str_(l.ToString() + r.ToString());   // aardio concat
            double a = ToNum(l), b = ToNum(r);
            if (op == L"+") return Value::Num_(a + b);
            if (op == L"-") return Value::Num_(a - b);
            if (op == L"*") return Value::Num_(a * b);
            if (op == L"/") return Value::Num_(b != 0 ? a / b : 0);
            if (op == L"%" || op == L"%=") return Value::Num_(b != 0 ? fmod(a, b) : 0);
            // aardio: ** is exponent, ^ is bitwise XOR (32-bit)
            if (op == L"**") return Value::Num_(pow(a, b));
            if (op == L"^") return Value::Num_((double)(int)(((int)(INT_PTR)(long long)a) ^ ((int)(INT_PTR)(long long)b)));
            // bitwise: 32-bit width, aardio DWORD semantics (matches EvalBin)
            unsigned int ua = (unsigned int)(INT_PTR)(long long)a;
            unsigned int ub = (unsigned int)(INT_PTR)(long long)b;
            if (op == L"&") return Value::Num_((double)(int)(ua & ub));
            if (op == L"|") return Value::Num_((double)(int)(ua | ub));
            if (op == L"<<") return Value::Num_((double)(int)(ua << (ub & 31)));
            if (op == L">>") return Value::Num_((double)(int)(ua >> (ub & 31)));
            if (op == L">>>")
                return Value::Num_((double)((long long)(ua >> (ub & 31))));
            if (op == L"==") return Value::Bool_(l.ToString() == r.ToString() &&
                                                l.t == r.t);
            if (op == L"!=") return Value::Bool_(!(l.ToString() == r.ToString() &&
                                                   l.t == r.t));
            if (op == L"!==")
                return Value::Bool_(!(l.ToString() == r.ToString() &&
                                      l.t == r.t) &&
                                    !(l.t == Value::Null &&
                                      r.t == Value::Null));
            if (op == L"<") return Value::Bool_(CmpLess(l, r));
            if (op == L">") return Value::Bool_(CmpLess(r, l));
            if (op == L"<=") return Value::Bool_(!CmpLess(r, l));
            if (op == L">=") return Value::Bool_(!CmpLess(l, r));
            return Value();
        }

        // M7: bare-name assign for multi-target unpack (a,b = ...)

// ---- DoAssignSimple ----
void Interp::DoAssignSimple(const wstring& name, const Value& v)
        {
            globals.vars[name] = v;
        }

// ---- DoAssign ----
void Interp::DoAssign(const PNode& target, const Value& v, Env* env,
                      const wstring& op)
        {
            Value val = v;
            if (op != L"=" && op != L":=" && op != L"::=")
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
                // aardio: inside a namespace scope, bare names are
                // namespace members — only locals (var/params) resolve
                // before the namespace table
                Env* eNs = env;
                while (eNs && !eNs->nsTable) eNs = eNs->parent;
                Env* e = env;
                bool bLocal = false;
                while (e)
                {
                    if (e->vars.count(target->str)) { bLocal = true; break; }
                    if (e->nsTable) break;   // namespace boundary
                    e = e->parent;
                }
                if (bLocal && e)
                    *env->Find(target->str) = val;   // local slot
                else if (eNs)
                    eNs->nsTable->map[target->str] = val;
                else
                    globals.vars[target->str] = val;   // aardio: default global
            }
            else if (target->k == Node::Member)
            {
                Value obj = target->bRoot ? Value() : Eval(target->a, env);
                if (target->bMeta)
                {
                    // tab@ = meta — set the metatable
                    if (obj.t == Value::TableT && obj.tab)
                        obj.tab->map[L"_meta"] = val;
                    return;
                }
                // class templates and functions hold members in the same Table shape —
                // ::RECT._meta = ... must land on the class table; type.callable = ... on function table
                if ((obj.t == Value::Class || obj.t == Value::Func) && obj.tab)
                {
                    if (val.t == Value::Null)
                        obj.tab->map.erase(target->str);
                    else
                        obj.tab->map[target->str] = val;
                }
                else if (obj.t == Value::TableT && obj.tab)
                {
                    // M2: existing fields assign directly; missing fields
                    // fall through to the metatable's _set
                    if (obj.tab->map.count(target->str) ||
                        !obj.tab->map.count(L"_meta"))
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
                if ((obj.t == Value::TableT || obj.t == Value::Func || obj.t == Value::Class) && obj.tab)
                {
                    if (idx.t == Value::Num && idx.n == (long long)idx.n &&
                        idx.n >= 1 && (size_t)(idx.n - 1) == obj.tab->arr.size())
                    {
                        if (val.t != Value::Null)
                            obj.tab->arr.push_back(val);
                    }
                    else
                    {
                        if (val.t == Value::Null)
                            obj.tab->map.erase(idx.ToString());
                        else
                            obj.tab->map[idx.ToString()] = val;
                    }
                }
            }
        }

// ---- ExecBlock ----
Value Interp::ExecBlock(const PNode& blk, Env* env)
        {
            Value last;
            for (auto& st : blk->list)
            {
                if (st->line > 0)
                    nErrLine = st->line;
                if (bDebug && st->line > 0 &&
                    (bStep || breakpoints.count(st->line)))
                    DebugStop(st->line, env);
                if (!err->empty())
                    break;
                static bool bDbg = GetEnvironmentVariableW(L"CMT", nullptr, 0) != 0;
                if (bDbg)
                {
                    fwprintf(stderr, L"[RUN] %s:%d k=%d\n", g_curModuleFile.c_str(), st->line, (int)st->k);
                    fflush(stderr);
                }
                last = Exec(st, env);
                if (nBreak || nContinue || nReturn || !err->empty())
                    break;
            }
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
            vClass.classBody = n->a;
            for (Env* e = env; e; e = e->parent)
                if (e->nsTable) { vClass.nsClosure = e->nsTable; break; }
            for (auto& st : n->a->list)
            {
                if (st->k == Node::Assign && st->a &&
                    st->a->k == Node::Ident)
                    tb->map[st->a->str] = Eval(st->b, env);
                else if (st->k == Node::Assign && st->a &&
                         st->a->k == Node::Index && st->a->a &&
                         st->a->a->k == Node::Ident && st->a->a->str == L"this" &&
                         st->a->b && st->a->b->k == Node::Str)
                {
                    tb->map[st->a->b->str] = Eval(st->b, env);
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
                while (eNs && !eNs->nsTable) eNs = eNs->parent;
                if (eNs && !n->bRoot)
                    eNs->nsTable->map[n->names[0]] = vClass;
                else
                {
                    globals.vars[n->names[0]] = vClass;
                    roots[n->names[0]] = vClass;
                }
            }
            else
            {
                Value ns;
                auto itG = globals.vars.find(n->names[0]);
                if (itG != globals.vars.end() &&
                    itG->second.t == Value::TableT)
                    ns = itG->second;
                else
                {
                    auto tbN = std::make_shared<Table>();
                    ns = Value::Table_(tbN);
                    globals.vars[n->names[0]] = ns;
                }
                for (size_t k = 1; k + 1 < n->names.size(); k++)
                {
                    Value nxt = GetMember(ns, n->names[k]);
                    if (nxt.t != Value::TableT)
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
                    // positional exprs, c->list holds targets 2..N
                    size_t nT = n->c->list.size() + 1;
                    Value v;
                    for (size_t k2 = 0; k2 < nT; k2++)
                    {
                        Value item;
                        if (k2 < n->b->list.size())
                            item = Eval(n->b->list[k2], env);
                        else if (v.mrets && k2 - n->b->list.size() <
                                 v.mrets->size())
                            item = (*v.mrets)[k2 - n->b->list.size()];
                        else
                            item = Value();
                        if (k2 == 0)
                        {
                            v = item;
                            DoAssign(n->a, item, env, L"=");
                        }
                        else
                            DoAssign(n->c->list[k2 - 1], item, env, L"=");
                    }
                    return v;
                }
                if (n->names.size() > 1)
                {
                    // a,b,c = e1,e2 — positional like var; when the first
                    // expr yields multi-returns the extras fill the rest
                    Value v = Eval(n->a, env);
                    for (size_t k2 = 0; k2 < n->names.size(); k2++)
                    {
                        Value item;
                        if (k2 == 0)
                            item = v;
                        else if (n->b && k2 - 1 < n->b->list.size())
                            item = Eval(n->b->list[k2 - 1], env);
                        else if (v.mrets && k2 - 1 < v.mrets->size())
                            item = (*v.mrets)[k2 - 1];
                        else
                            item = Value();
                        DoAssignSimple(n->names[k2], item);
                    }
                    return v;
                }
                Value v = Eval(n->b, env);
                DoAssign(n->a, v, env, n->str);
                return v;
            }
            case Node::VarDecl:
            {
                // var a = e1, b = e2 — positional init list
                if (n->b && !n->b->list.empty())
                {
                    Value v0 = n->a ? Eval(n->a, env) : Value();
                    if (!n->names.empty() && !n->names[0].empty())
                        env->vars[n->names[0]] = v0;
                    for (size_t k = 1; k < n->names.size(); k++)
                        if (!n->names[k].empty())
                            env->vars[n->names[k]] =
                                k - 1 < n->b->list.size()
                                    ? Eval(n->b->list[k - 1], env) : Value();
                    return v0;
                }
                Value v = n->a ? Eval(n->a, env) : Value();
                if (n->names.size() == 1)
                {
                    if (!n->names[0].empty()) env->vars[n->names[0]] = v;
                    return v;
                }
                // var a,b = f() — aardio multi-return unpack
                if (!n->names[0].empty()) env->vars[n->names[0]] = v;
                if (v.mrets)
                {
                    for (size_t k = 1; k < n->names.size(); k++)
                        if (!n->names[k].empty())
                            env->vars[n->names[k]] =
                                k - 1 < v.mrets->size()
                                    ? (*v.mrets)[k - 1] : Value();
                }
                else
                    for (size_t k = 1; k < n->names.size(); k++)
                        if (!n->names[k].empty())
                            env->vars[n->names[k]] = Value();
                return v;
            }
            case Node::Import:
                return DoImport(n, env);
            case Node::Namespace:
            {
                Value ns = NsPath(n->names, env, n->bRoot);
                if (!ns.tab)
                    return Value();
                ns.tab->map[L"self"] = ns;
                Env nsEnv;
                nsEnv.parent = env;
                nsEnv.nsTable = ns.tab;
                nsEnv.vars[L"self"] = ns;
                for (auto& kv : ns.tab->scopeVars)
                    nsEnv.vars[kv.first] = kv.second;
                Value r = ExecBlock(n->a, &nsEnv);
                for (auto& kv : nsEnv.vars)
                    ns.tab->scopeVars[kv.first] = kv.second;
                return r;
            }
            case Node::Class:
                return EvalClass(n, env);
            case Node::With:
            {
                // with(expr) body: child scope binds expr's table members
                // as locals; `owner` = the expr value
                Value v = Eval(n->a, env);
                Env local;
                local.parent = env;
                if (v.t == Value::TableT && v.tab)
                {
                    for (auto& pr : v.tab->map)
                        local.vars[pr.first] = pr.second;
                }
                local.vars[L"owner"] = v;
                return ExecBlock(n->b, &local);
            }
            case Node::DoWhile:
            {
                while (true)
                {
                    ExecBlock(n->b, env);
                    if (nReturn || nBreak) break;
                    if (nContinue) nContinue = 0;
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
                            return ExecBlock(n->c->list[i + 1], env);
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
                            return ExecBlock(n->c->list[i + 1], env);
                    }
                    return Value();
                }
                Value vCond = Eval(n->a, env);
                if (nReturn)
                    return vCond;   // inline return in cond: unwind now
                if (vCond.Truthy())
                    return ExecBlock(n->b, env);
                for (size_t i = 0; i + 1 < n->c->list.size(); i += 2)
                {
                    if (Eval(n->c->list[i], env).Truthy())
                        return ExecBlock(n->c->list[i + 1], env);
                }
                if (n->c->list.size() % 2 == 1)
                    return ExecBlock(n->c->list.back(), env);
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
                        Value v = Eval(n->a, env);
                        if (nReturn) return v;
                        if (!v.Truthy())
                            break;
                        Value r = ExecBlock(n->b, env);
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
                        Value v = ExecBlock(n->a, env);
                        if (nReturn) return v;
                        if (!v.Truthy())
                            break;
                        Value r = ExecBlock(n->b, env);
                        if (nBreak) { nBreak--; break; }
                        if (nContinue) { nContinue--; continue; }
                        if (nReturn) return r;
                    }
                    return Value();
                }
                while (Exec(n->a, env).Truthy())
                {
                    Value r = ExecBlock(n->b, env);
                    if (nBreak) { nBreak--; break; }
                    if (nContinue) { nContinue--; continue; }
                    if (nReturn) return r;
                }
                return Value();
            }
            case Node::ForNum:
            {
                Exec(n->a, env);
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
                if (n->a && n->a->k == Node::Assign && n->a->a &&
                    n->a->a->k == Node::Ident)
                {
                    Value* pv = env->Find(n->a->a->str);
                    Value lim = n->b ? Eval(n->b, env) : Value();
                    if (pv && pv->t == Value::Num && lim.t == Value::Num)
                    {
                        nDir = BNum(*pv) > BNum(lim) ? -1 : 1;
                    }
                }
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
                    dStep = n->c->a->num;
                else if (bUnaryStep)
                {
                    dStep = -BNum(Eval(n->c->a->a, env));
                    nDir = -1;
                }
                if (!n->c)
                    dStep = (double)nDir;   // 2-expr form: derived step
                auto bContinue = [&]() -> bool {
                    if (!n->b)
                        return true;        // empty condition slot: forever
                    if (!bLimitForm)
                        return Eval(n->b, env).Truthy();
                    if (n->a && n->a->k == Node::Assign &&
                        n->a->a && n->a->a->k == Node::Ident)
                    {
                        Value* pv = env->Find(n->a->a->str);
                        if (pv && pv->t == Value::Num)
                        {
                            Value lim = Eval(n->b, env);
                            return nDir >= 0 ? pv->n <= BNum(lim)
                                             : pv->n >= BNum(lim);
                        }
                    }
                    return Eval(n->b, env).Truthy();
                };
                while (bContinue())
                {
                    Value r = ExecBlock(n->d, env);
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
                            Value vS = Eval(n->c->a, env);
                            if (vS.t == Value::Num) dStep = vS.n;
                        }
                        if (n->a && n->a->k == Node::Assign && n->a->a &&
                            n->a->a->k == Node::Ident)
                        {
                            Value* pv = env->Find(n->a->a->str);
                            if (pv && pv->t == Value::Num)
                                *pv = Value::Num_(pv->n + dStep);
                        }
                    }
                    else if (n->c)
                    {
                        Exec(n->c, env);
                    }
                    else if (n->a && n->a->k == Node::Assign && n->a->a &&
                             n->a->a->k == Node::Ident)
                    {
                        // 2-expr form: no step slot, derive from direction
                        Value* pv = env->Find(n->a->a->str);
                        if (pv && pv->t == Value::Num)
                            *pv = Value::Num_(pv->n + dStep);
                    }
                }
                return Value();
            }
            case Node::ForIn:
            {
                Value it = Eval(n->a, env);
                if (it.t != Value::TableT || !it.tab)
                    return Value();
                Env loopEnv;
                loopEnv.parent = env;
                auto step = [&](const wstring& k, const Value& v) -> int {
                    loopEnv.vars.clear();
                    loopEnv.vars[n->names[0]] = Value::Str_(k);
                    if (n->names.size() > 1)
                        loopEnv.vars[n->names[1]] = v;
                    Value r = ExecBlock(n->b, &loopEnv);
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
                    keys.push_back(kv.first);
                for (auto& k : keys)
                {
                    auto itv = it.tab->map.find(k);
                    Value v = itv != it.tab->map.end() ? itv->second : Value();
                    int rc = step(k, v);
                    if (rc == 1) return Value();
                    if (rc == 2) return ret;
                }
                return Value();
            }
            case Node::Try:
            {
                ExecBlock(n->a, env);
                if (!err->empty())
                {
                    wstring msg = *err;
                    err->clear();
                    if (!n->b)
                        return Value();   // catch-less try{}: swallow
                    Env cEnv;
                    cEnv.parent = env;
                    if (!n->names.empty())
                        cEnv.vars[n->names[0]] = Value::Str_(msg);
                    ExecBlock(n->b, &cEnv);
                }
                return Value();
            }
            case Node::Return:
                ret = n->a ? Eval(n->a, env) : Value();
                if (!n->list.empty())
                {
                    // aardio multi-return: extras ride on the Value
                    auto extras = std::make_shared<vector<Value>>();
                    for (auto& e : n->list)
                        extras->push_back(Eval(e, env));
                    ret.mrets = extras;
                }
                nReturn++;
                return ret;
            case Node::Break:
                nBreak++;
                return Value();
            case Node::Continue:
                nContinue++;
                return Value();
            case Node::Block:
            {
                Env inner;
                inner.parent = env;
                return ExecBlock(n, &inner);
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
            globals.vars[name] = v;   // bare `string` resolves like aardio
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
                for (Env* e = env; e; e = e->parent)
                {
                    if (e->nsTable) { curNs = e->nsTable; break; }
                }
            }
            if (curNs)
            {
                auto it = curNs->map.find(names[0]);
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
                auto it = roots.find(names[0]);
                if (it == roots.end())
                    it = globals.vars.find(names[0]);
                if (it != roots.end() && (it->second.t == Value::TableT || it->second.t == Value::Class) &&
                    it->second.tab)
                {
                    ns = it->second;
                    roots[names[0]] = ns;
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
            Env local;
            local.parent = &globals;
            local.nsTable = f.nsClosure;
            local.vars[L"owner"] = Value();   // native-side call: no owner
            for (size_t i = 0; i < f.params.size(); i++)
            {
                if (i < args.size())
                    local.vars[f.params[i]] = args[i];
                else if (f.pdefaults &&
                         i < f.pdefaults->size() &&
                         (*f.pdefaults)[i])
                    local.vars[f.params[i]] =
                        Eval((*f.pdefaults)[i], &local);
                else
                    local.vars[f.params[i]] = Value();
            }
            if (f.params.size() && f.params.back() == L"..." && f.fn)
            {
                auto tbV = std::make_shared<Table>();
                for (size_t k = f.params.size() - 1; k < args.size(); k++)
                    tbV->arr.push_back(args[k]);
                // `arg` is aardio's spelling for the varargs table;
                // `...` expands it at call sites.
                local.vars[L"..."] = Value::Table_(tbV);
                local.vars[L"arg"] = Value::Table_(tbV);
            }
            Value oldRet = ret;
            int oldR = nReturn, oldB = nBreak, oldC = nContinue;
            nReturn = 0;
            Value r = ExecBlock(f.fn, &local);
            nReturn = oldR;
            nBreak = oldB;
            nContinue = oldC;
            ret = oldRet;
            return r;
        }
}
