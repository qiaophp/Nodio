// com.cpp — kernel COM host (IDispatch dynamic invocation)
// global.com for the official library: com.CreateObject / GetObject /
// IsObject / Release ... Wrappers are aardio tables carrying
// {_object = IDispatch*, _meta = comMeta} where comMeta._get/_set
// dispatch through IDispatch::Invoke.
#include "pch.h"
#include "interp.h"

namespace NodioVM
{
namespace
{
    static wstring BStrToW(BSTR b)
    {
        return b ? wstring(b, SysStringLen(b)) : wstring();
    }

    static Table* ComMeta();

    static Value WrapDispatch(IDispatch* pd)
    {
        pd->AddRef();
        auto w = std::make_shared<Table>();
        w->map[L"_object"] = Value::Pointer_((void*)pd);
        w->map[L"_isComObject"] = Value::Bool_(true);
        w->map[L"_meta"] = Value::Table_(
            std::shared_ptr<Table>(ComMeta(), [](Table*) {}));
        return Value::Table_(w);
    }

    // ---------- VARIANT -> Value ----------
    static Value VariantToValue(const VARIANT& in, wstring* pErr)
    {
        switch (in.vt)
        {
        case VT_EMPTY:
        case VT_NULL:
            return Value();
        case VT_BOOL:
            return Value::Bool_(in.boolVal != VARIANT_FALSE);
        case VT_I1:
        case VT_I2:
        case VT_I4:
        case VT_INT:
            return Value::Num_((double)V_I4(&in));
        case VT_UI1:
        case VT_UI2:
        case VT_UI4:
        case VT_UINT:
            return Value::Num_((double)V_UI4(&in));
        case VT_R4:
            return Value::Num_((double)in.fltVal);
        case VT_R8:
            return Value::Num_(in.dblVal);
        case VT_BSTR:
            return Value::Str_(BStrToW(in.bstrVal));
        case VT_DISPATCH:
            if (in.pdispVal)
                return WrapDispatch(in.pdispVal);
            return Value();
        case VT_ERROR:
        case VT_HRESULT:
            return Value::Num_((double)(long)in.lVal);
        default:
        {
            VARIANT tmp;
            VariantInit(&tmp);
            if (SUCCEEDED(VariantChangeType(&tmp, &in, 0, VT_BSTR)))
            {
                Value r = Value::Str_(BStrToW(tmp.bstrVal));
                VariantClear(&tmp);
                return r;
            }
            if (pErr && pErr->empty())
                *pErr = L"com: 不支持的返回类型 vt=" +
                        std::to_wstring(in.vt);
            return Value();
        }
        }
    }

    // ---------- Value -> VARIANT ----------
    static void VariantFromValue(const Value& v, VARIANT& out, wstring* pErr)
    {
        VariantInit(&out);
        switch (v.t)
        {
        case Value::Null:
            out.vt = VT_EMPTY;
            break;
        case Value::Bool:
            out.vt = VT_BOOL;
            out.boolVal = v.b ? VARIANT_TRUE : VARIANT_FALSE;
            break;
        case Value::Num:
            out.vt = VT_R8;
            out.dblVal = v.n;
            break;
        case Value::Str:
            out.vt = VT_BSTR;
            out.bstrVal = SysAllocStringLen(v.s.c_str(), (UINT)v.s.size());
            break;
        case Value::TableT:
            if (v.tab)
            {
                auto io = v.tab->map.find(L"_object");
                if (io != v.tab->map.end() &&
                    io->second.t == Value::Pointer)
                {
                    out.vt = VT_DISPATCH;
                    out.pdispVal =
                        reinterpret_cast<IDispatch*>(io->second.ptr);
                    out.pdispVal->AddRef();
                    break;
                }
                auto ib = v.tab->map.find(L"_boundDispatch");
                if (ib != v.tab->map.end() &&
                    ib->second.t == Value::Pointer)
                {
                    out.vt = VT_DISPATCH;
                    out.pdispVal =
                        reinterpret_cast<IDispatch*>(ib->second.ptr);
                    out.pdispVal->AddRef();
                    break;
                }
            }
            if (pErr && pErr->empty())
                *pErr = L"com: 不支持的参数类型(table)";
            break;
        default:
            if (pErr && pErr->empty())
                *pErr = L"com: 不支持的参数类型";
            break;
        }
    }

    // Invoke a DISPID on pd
    static Value InvokeDisp(IDispatch* pd, DISPID dispid, WORD wFlags,
                            vector<Value>& args, wstring* pErr)
    {
        DISPPARAMS dp{};
        vector<VARIANT> vars;
        if (!args.empty())
        {
            vars.resize(args.size());
            dp.cArgs = (int)args.size();
            dp.rgvarg = vars.data();
            for (size_t i = 0; i < args.size(); i++)
                VariantFromValue(args[args.size() - 1 - i], vars[i], pErr);
            if (pErr && !pErr->empty())
                return Value();
        }
        EXCEPINFO ex{};
        UINT argErr = 0;
        VARIANT ret;
        VariantInit(&ret);
        HRESULT hr = pd->Invoke(dispid, IID_NULL, LOCALE_USER_DEFAULT,
                                wFlags, &dp, &ret, &ex, &argErr);
        for (auto& v : vars)
            VariantClear(&v);
        if (FAILED(hr))
        {
            if (pErr)
            {
                if (hr == DISP_E_EXCEPTION && ex.bstrDescription)
                    *pErr = L"com: " + BStrToW(ex.bstrDescription);
                else
                {
                    wchar_t buf[16];
                    swprintf(buf, 16, L"%08lX", (unsigned long)hr);
                    *pErr = wstring(L"com: Invoke 失败 hr=0x") + buf;
                }
            }
            VariantClear(&ret);
            return Value();
        }
        Value r = VariantToValue(ret, pErr);
        VariantClear(&ret);
        return r;
    }

    // bound member: comObj.member(args) — a member read returns this
    // wrapper; calling it invokes the DISPID with the call args
    static Value WrapBoundMember(IDispatch* pd, DISPID dispid,
                                 const wstring& name)
    {
        auto w = std::make_shared<Table>();
        w->map[L"_boundDispatch"] = Value::Pointer_((void*)pd);
        w->map[L"_boundDispId"] = Value::Num_((double)dispid);
        w->map[L"_boundName"] = Value::Str_(name);
        w->map[L"_meta"] = Value::Table_(
            std::shared_ptr<Table>(ComMeta(), [](Table*) {}));
        return Value::Table_(w);
    }

    static Table* ComMeta()
    {
        static std::shared_ptr<Table> s_meta;
        if (!s_meta)
        {
            s_meta = std::make_shared<Table>();

            // _get(k, ownerCall, owner) — dynamic member read; returns a
            // bound-member wrapper (callable) for methods, the property
            // value for pure gets
            s_meta->map[L"_get"] = NF([](vector<Value>& a) -> Value {
                if (a.size() < 3 || a[2].t != Value::TableT || !a[2].tab)
                    return Value();
                auto io = a[2].tab->map.find(L"_object");
                if (io == a[2].tab->map.end() ||
                    io->second.t != Value::Pointer)
                    return Value();
                auto* pd = reinterpret_cast<IDispatch*>(io->second.ptr);
                wstring key = BStr(a[0]);
                DISPID dispid;
                OLECHAR* name = const_cast<OLECHAR*>(key.c_str());
                if (FAILED(pd->GetIDsOfNames(IID_NULL, &name, 1,
                                             LOCALE_USER_DEFAULT, &dispid)))
                    return Value();
                wstring err;
                vector<Value> noArgs;
                Value v = InvokeDisp(pd, dispid, DISPATCH_PROPERTYGET | DISPATCH_METHOD,
                                     noArgs, &err);
                // a zero-arg probe that yields EMPTY is a Sub-style
                // method (e.g. IWshShortcut.Save) — keep it callable
                if (!err.empty() || v.t == Value::Null)
                    return WrapBoundMember(pd, dispid, key);
                return v;
            });

            // _set(k, v, owner) — property put
            s_meta->map[L"_set"] = NF([](vector<Value>& a) -> Value {
                if (a.size() < 3 || a[2].t != Value::TableT || !a[2].tab)
                    return Value();
                auto io = a[2].tab->map.find(L"_object");
                if (io == a[2].tab->map.end() ||
                    io->second.t != Value::Pointer)
                    return Value();
                auto* pd = reinterpret_cast<IDispatch*>(io->second.ptr);
                wstring key = BStr(a[0]);
                DISPID dispid;
                OLECHAR* name = const_cast<OLECHAR*>(key.c_str());
                if (FAILED(pd->GetIDsOfNames(IID_NULL, &name, 1,
                                             LOCALE_USER_DEFAULT, &dispid)))
                {
                    Interp* it = g_interpCurrent;
                    if (it)
                        *it->err = L"com: 成员不存在 " + key;
                    return Value();
                }
                vector<Value> putArgs{ a[1] };
                wstring err;
                InvokeDisp(pd, dispid, DISPATCH_PROPERTYPUT, putArgs, &err);
                if (!err.empty())
                {
                    Interp* it = g_interpCurrent;
                    if (it)
                        *it->err = err;
                }
                return Value();
            });

            // _call(owner, ...) on a bound member
            s_meta->map[L"_call"] = NF([](vector<Value>& a) -> Value {
                if (a.empty() || a[0].t != Value::TableT || !a[0].tab)
                    return Value();
                auto ib = a[0].tab->map.find(L"_boundDispatch");
                auto ii = a[0].tab->map.find(L"_boundDispId");
                if (ib == a[0].tab->map.end() ||
                    ib->second.t != Value::Pointer ||
                    ii == a[0].tab->map.end())
                    return Value();
                auto* pd = reinterpret_cast<IDispatch*>(ib->second.ptr);
                DISPID dispid = (DISPID)(INT_PTR)BNum(ii->second);
                vector<Value> args(a.begin() + 1, a.end());
                wstring err;
                Value r = InvokeDisp(pd, dispid, DISPATCH_METHOD | DISPATCH_PROPERTYGET,
                                     args, &err);
                if (!err.empty())
                {
                    Interp* it = g_interpCurrent;
                    if (it)
                        *it->err = err;
                }
                return r;
            });
        }
        return s_meta.get();
    }

    struct ComRuntime
    {
        ComRuntime()
        {
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        }
        ~ComRuntime() { CoUninitialize(); }
    };
} // namespace

// ---- RegisterComKernel ----
// builds global.com with the kernel members the official library needs
void Interp::RegisterComKernel(Env& globalsEnv)
{
    static ComRuntime s_com;
    auto com = std::make_shared<Table>();

    // com.CreateObject(progId) -> COM wrapper table
    com->map[L"CreateObject"] = NF([](vector<Value>& a) -> Value {
        if (a.empty())
        {
            Interp* it = g_interpCurrent;
            if (it)
                *it->err = L"com.CreateObject: 缺少 ProgID 参数";
            return Value();
        }
        CLSID clsid;
        wstring prog = BStr(a[0]);
        HRESULT hr = CLSIDFromProgID(prog.c_str(), &clsid);
        if (FAILED(hr))
        {
            Interp* it = g_interpCurrent;
            if (it)
                *it->err = L"com.CreateObject: 无效 ProgID " + prog;
            return Value();
        }
        IDispatch* pd = nullptr;
        hr = CoCreateInstance(clsid, nullptr, CLSCTX_ALL, IID_IDispatch,
                              (void**)&pd);
        if (FAILED(hr) || !pd)
        {
            Interp* it = g_interpCurrent;
            if (it)
            {
                wchar_t buf[16];
                swprintf(buf, 16, L"%08lX", (unsigned long)hr);
                *it->err = wstring(L"com.CreateObject: 创建失败 hr=0x") + buf;
            }
            return Value();
        }
        return WrapDispatch(pd);
    });

    // com.TryCreateObject: null on failure (no error)
    com->map[L"TryCreateObject"] = NF([](vector<Value>& a) -> Value {
        if (a.empty())
            return Value();
        CLSID clsid;
        wstring prog = BStr(a[0]);
        if (FAILED(CLSIDFromProgID(prog.c_str(), &clsid)))
            return Value();
        IDispatch* pd = nullptr;
        if (FAILED(CoCreateInstance(clsid, nullptr, CLSCTX_ALL,
                                    IID_IDispatch, (void**)&pd)) ||
            !pd)
            return Value();
        return WrapDispatch(pd);
    });

    // com.GetObject(path/moniker) — file moniker binding
    com->map[L"GetObject"] = NF([](vector<Value>& a) -> Value {
        if (a.empty())
            return Value();
        wstring path = BStr(a[0]);
        IBindCtx* bc = nullptr;
        IMoniker* mk = nullptr;
        if (FAILED(CreateBindCtx(0, &bc)))
            return Value();
        ULONG eaten = 0;
        HRESULT hr = MkParseDisplayName(bc, path.c_str(), &eaten, &mk);
        if (SUCCEEDED(hr) && mk)
        {
            IDispatch* pd = nullptr;
            if (SUCCEEDED(mk->BindToObject(bc, nullptr, IID_IDispatch,
                                           (void**)&pd)) &&
                pd)
            {
                mk->Release();
                bc->Release();
                return WrapDispatch(pd);
            }
        }
        if (mk)
            mk->Release();
        bc->Release();
        Interp* it = g_interpCurrent;
        if (it)
            *it->err = L"com.GetObject: 无法绑定 " + path;
        return Value();
    });

    // com.IsObject(v)
    com->map[L"IsObject"] = NF([](vector<Value>& a) -> Value {
        if (a.empty() || a[0].t != Value::TableT || !a[0].tab)
            return Value::Bool_(false);
        return Value::Bool_(
            a[0].tab->map.count(L"_object") ||
            a[0].tab->map.count(L"_boundDispatch"));
    });

    // com.Release(v)
    com->map[L"Release"] = NF([](vector<Value>& a) -> Value {
        if (!a.empty() && a[0].t == Value::TableT && a[0].tab)
        {
            auto io = a[0].tab->map.find(L"_object");
            if (io != a[0].tab->map.end() &&
                io->second.t == Value::Pointer)
            {
                auto* pd = reinterpret_cast<IDispatch*>(io->second.ptr);
                pd->Release();
                return Value::Num_(1);
            }
        }
        return Value::Num_(0);
    });

    // com.GetIUnknown(obj, iidStr) -> raw IUnknown* QI'd to the given
    // interface (official com.kernel API used by com.interface / dotNet.clr)
    com->map[L"GetIUnknown"] = NF([](vector<Value>& a) -> Value {
        if (a.size() < 2)
            return Value();
        IUnknown* punk = nullptr;
        if (a[0].t == Value::TableT && a[0].tab)
        {
            auto io = a[0].tab->map.find(L"_object");
            if (io != a[0].tab->map.end() && io->second.t == Value::Pointer)
                punk = reinterpret_cast<IUnknown*>(io->second.ptr);
            else
            {
                auto ip = a[0].tab->map.find(L"_pointer");
                if (ip != a[0].tab->map.end() &&
                    (ip->second.t == Value::Pointer))
                    punk = reinterpret_cast<IUnknown*>(ip->second.ptr);
                else if (a[0].t == Value::Pointer)
                    punk = reinterpret_cast<IUnknown*>(a[0].ptr);
            }
        }
        else if (a[0].t == Value::Pointer)
            punk = reinterpret_cast<IUnknown*>(a[0].ptr);
        if (!punk)
            return Value();
        IID iid;
        // the IID may be a string ("{...}") or a win.guid struct table
        // (com.interface's checkInterface converts it before calling)
        if (a[1].t == Value::Str)
        {
            wstring sIid = BStr(a[1]);
            if (FAILED(IIDFromString(sIid.c_str(), &iid)))
                return Value();
        }
        else if (a[1].t == Value::TableT && a[1].tab)
        {
            int total = 0;
            void* pG = StructSerialize(a[1], &total);
            if (!pG || total < (int)sizeof(IID))
            {
                if (pG)
                    HeapFree(GetProcessHeap(), 0, pG);
                return Value();
            }
            memcpy(&iid, pG, sizeof(IID));
            HeapFree(GetProcessHeap(), 0, pG);
        }
        else
            return Value();
        void* pv = nullptr;
        if (FAILED(punk->QueryInterface(iid, &pv)) || !pv)
            return Value();
        return Value::Pointer_(pv);
    });

    // com.QueryObject(punk) -> COM wrapper: QI to IDispatch and wrap
    // (official kernel API used by dotNet.appDomain on raw IUnknown*)
    com->map[L"QueryObject"] = NF([](vector<Value>& a) -> Value {
        if (a.empty())
            return Value();
        IUnknown* punk = nullptr;
        if (a[0].t == Value::Pointer)
            punk = reinterpret_cast<IUnknown*>(a[0].ptr);
        else if (a[0].t == Value::Num)
            punk = reinterpret_cast<IUnknown*>(
                (INT_PTR)(long long)a[0].n);
        else if (a[0].t == Value::TableT && a[0].tab)
        {
            auto io = a[0].tab->map.find(L"_object");
            if (io != a[0].tab->map.end() && io->second.t == Value::Pointer)
                punk = reinterpret_cast<IUnknown*>(io->second.ptr);
        }
        if (!punk)
            return Value();
        IDispatch* pd = nullptr;
        HRESULT hrQI = punk->QueryInterface(IID_IDispatch, (void**)&pd);
        if (SUCCEEDED(hrQI) && pd)
            return WrapDispatch(pd);
        // not an automation object: keep the raw pointer reachable
        return a[0];
    });

    globalsEnv.vars[L"com"] = Value::Table_(com);
    // plain `import com` must resolve to this kernel namespace (official
    // builtin\com.aardio merges into it) instead of creating an empty
    // fallback module that would shadow it
    roots[L"com"] = Value::Table_(com);
}
} // namespace NodioVM
