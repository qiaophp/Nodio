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

    static Value WrapRawThunk(IUnknown* punk);

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
        case VT_UNKNOWN:
            // .NET CCWs often hand back IUnknown; promote to IDispatch
            if (in.punkVal)
            {
                IDispatch* pDisp = nullptr;
                HRESULT hrU = in.punkVal->QueryInterface(
                    IID_IDispatch, (void**)&pDisp);
                if (SUCCEEDED(hrU) && pDisp)
                    return WrapDispatch(pDisp);
                if (hrU == E_NOINTERFACE)
                {
                    // CLR CCW quirk: the object carries the IDispatch
                    // methods in its vtable but refuses the QI — try the
                    // coclass/class-interface route first (AssemblyName:
                    // {F12FDE6A-9394-3C32-8E4D-F3D470947284}), whose QI
                    // may yield a dispatchable face
                    IID iidClass;
                    if (SUCCEEDED(IIDFromString(
                            L"{F12FDE6A-9394-3C32-8E4D-F3D470947284}",
                            &iidClass)))
                    {
                        IUnknown* pClass = nullptr;
                        if (SUCCEEDED(in.punkVal->QueryInterface(
                                iidClass, (void**)&pClass)) && pClass)
                        {
                            IDispatch* pD2 = nullptr;
                            HRESULT hrD2 = pClass->QueryInterface(
                                IID_IDispatch, (void**)&pD2);
                            if (SUCCEEDED(hrD2) && pD2)
                            {
                                pClass->Release();
                                return WrapDispatch(pD2);
                            }
                            pClass->Release();
                        }
                    }
                    // last resort: raw-vtable dispatch wrapper
                    in.punkVal->AddRef();
                    return WrapRawThunk(in.punkVal);
                }
                return Value::Pointer_((void*)in.punkVal);
            }
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

    // ---------- raw IDispatch-over-vtable ----------
    // Some CLR CCW hand-backs (e.g. System._AssemblyName from
    // _Assembly::GetName) implement the four IDispatch methods in their
    // vtable but refuse QI(IID_IDispatch) with E_NOINTERFACE. For those
    // we call GetIDsOfNames (slot 5) / Invoke (slot 6) directly through
    // the vtable.
    static Value RawDispatchGet(void* punk, const wstring& name,
                                vector<Value>& args, WORD wFlags,
                                wstring* pErr)
    {
        void** vtbl = *reinterpret_cast<void***>(punk);
        // slot 5: GetIDsOfNames(riid, rgszNames*, cNames, lcid, rgsid*)
        typedef __int64 (__stdcall *GetIdsFn)(void*, const IID*, LPOLESTR*,
                                              UINT, ULONG, DISPID*);
        GetIdsFn fnIds = nullptr;
        memcpy(&fnIds, &vtbl[5], sizeof(fnIds));
        DISPID dispid = -1;
        LPOLESTR nm = const_cast<LPOLESTR>(name.c_str());
        __int64 hr = fnIds(punk, &IID_NULL, &nm, 1, LOCALE_USER_DEFAULT,
                           &dispid);
        if (getenv("NODIO_TRACE"))
            fwprintf(stderr, L"[raw] GetIDsOfNames(%s) hr=0x%08llX id=%d\n",
                     name.c_str(), (unsigned long long)hr, (int)dispid);
        if (hr < 0 || dispid < 0)
        {
            // half-implemented CCW dispatch (E_NOTIMPL): route through
            // ITypeInfo (slot 4: GetTypeInfo) and ITypeInfo::Invoke,
            // the standard late-binding fallback for CLR CCWs
            typedef __int64 (__stdcall *GetTIFn)(void*, UINT, LCID,
                                                 ITypeInfo**, HRESULT*);
            GetTIFn fnTI = nullptr;
            memcpy(&fnTI, &vtbl[4], sizeof(fnTI));
            ITypeInfo* pTI = nullptr;
            HRESULT hrTI = 0;
            if (SUCCEEDED(fnTI(punk, 0, LOCALE_USER_DEFAULT, &pTI,
                               &hrTI)) &&
                pTI)
            {
                // FUNCDESC walk: find memid by name
                TYPEATTR* pAttr = nullptr;
                if (SUCCEEDED(pTI->GetTypeAttr(&pAttr)))
                {
                    for (short f = 0; f < pAttr->cFuncs; f++)
                    {
                        FUNCDESC* pFD = nullptr;
                        if (FAILED(pTI->GetFuncDesc(f, &pFD)))
                            continue;
                        BSTR names[1] = {nullptr};
                        UINT pcNamesU = 0;
                        if (SUCCEEDED(pTI->GetNames(
                                pFD->memid, names, 1, &pcNamesU)) &&
                            pcNamesU > 0)
                        {
                            bool bMatch = name ==
                                          wstring(names[0]);
                            SysFreeString(names[0]);
                            if (bMatch)
                            {
                                dispid = pFD->memid;
                                pTI->ReleaseFuncDesc(pFD);
                                break;
                            }
                        }
                        pTI->ReleaseFuncDesc(pFD);
                    }
                    pTI->ReleaseTypeAttr(pAttr);
                }
                if (dispid < 0)
                {
                    pTI->Release();
                    if (pErr && pErr->empty())
                        *pErr = L"dotNet: 成员不存在 " + name;
                    return Value();
                }
                // invoke via ITypeInfo::Invoke
                DISPPARAMS dp2{};
                vector<VARIANT> vars2;
                if (!args.empty())
                {
                    vars2.resize(args.size());
                    dp2.cArgs = (int)args.size();
                    dp2.rgvarg = vars2.data();
                    for (size_t i = 0; i < args.size(); i++)
                        VariantFromValue(args[args.size() - 1 - i],
                                         vars2[i], pErr);
                    if (pErr && !pErr->empty())
                        return Value();
                }
                EXCEPINFO ex2{};
                UINT argErr2 = 0;
                VARIANT ret2;
                VariantInit(&ret2);
                UINT argcArgs = (UINT)dp2.cArgs;
                HRESULT hrInv = pTI->Invoke(punk, dispid, wFlags, &dp2,
                                            &ret2, &ex2, &argcArgs);
                for (auto& v : vars2)
                    VariantClear(&v);
                pTI->Release();
                if (FAILED(hrInv))
                {
                    if (pErr)
                    {
                        wchar_t buf[16];
                        swprintf(buf, 16, L"%08lX", (unsigned long)hrInv);
                        *pErr = wstring(L"dotNet: TypeInvoke 失败 hr=0x") +
                                buf;
                    }
                    VariantClear(&ret2);
                    return Value();
                }
                Value r = VariantToValue(ret2, pErr);
                VariantClear(&ret2);
                return r;
            }
            if (pErr && pErr->empty())
                *pErr = L"dotNet: 成员不存在 " + name;
            return Value();
        }
        // slot 6: Invoke(dispid, riid, lcid, wFlags, dp, ret, ex, argErr)
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
        typedef __int64 (__stdcall *InvokeFn)(void*, DISPID, const IID*,
                                              ULONG, WORD, DISPPARAMS*,
                                              VARIANT*, EXCEPINFO*, UINT*);
        InvokeFn fnInv = nullptr;
        memcpy(&fnInv, &vtbl[6], sizeof(fnInv));
        hr = fnInv(punk, dispid, &IID_NULL, LOCALE_USER_DEFAULT, wFlags,
                   &dp, &ret, &ex, &argErr);
        if (getenv("NODIO_TRACE"))
            fwprintf(stderr, L"[raw] Invoke(%s) hr=0x%08llX vt=%d\n",
                     name.c_str(), (unsigned long long)hr, (int)ret.vt);
        for (auto& v : vars)
            VariantClear(&v);
        if (FAILED(hr))
        {
            if (pErr)
            {
                if (hr == DISP_E_EXCEPTION && ex.bstrDescription)
                    *pErr = L"dotNet: " + BStrToW(ex.bstrDescription);
                else
                {
                    wchar_t buf[16];
                    swprintf(buf, 16, L"%08lX", (unsigned long)hr);
                    *pErr = wstring(L"dotNet: Invoke 失败 hr=0x") + buf;
                }
            }
            VariantClear(&ret);
            return Value();
        }
        if (getenv("NODIO_TRACE"))
            fwprintf(stderr, L"[inv] hr=0x%08lX vt=%d\n",
                     (unsigned long)hr, (int)ret.vt);
        Value r = VariantToValue(ret, pErr);
        VariantClear(&ret);
        return r;
    }

    static Table* RawThunkMeta()
    {
        static std::shared_ptr<Table> s_meta;
        if (!s_meta)
        {
            s_meta = std::make_shared<Table>();
            s_meta->map[L"_get"] = NF([](vector<Value>& a) -> Value {
                if (a.size() < 3 || a[2].t != Value::TableT || !a[2].tab)
                    return Value();
                auto io = a[2].tab->map.find(L"_object");
                if (io == a[2].tab->map.end() ||
                    io->second.t != Value::Pointer)
                    return Value();
                wstring key = BStr(a[0]);
                if (getenv("NODIO_TRACE"))
                    fwprintf(stderr, L"[rawget] key=%s\n", key.c_str());
                vector<Value> noArgs;
                Interp* it = g_interpCurrent;
                return RawDispatchGet(io->second.ptr, key, noArgs,
                                      DISPATCH_METHOD |
                                          DISPATCH_PROPERTYGET,
                                      it ? it->err : nullptr);
            });
            s_meta->map[L"_call"] = NF([](vector<Value>& a) -> Value {
                if (a.empty() || a[0].t != Value::TableT || !a[0].tab)
                    return Value();
                auto io = a[0].tab->map.find(L"_object");
                if (io == a[0].tab->map.end() ||
                    io->second.t != Value::Pointer)
                    return Value();
                // a[1] = member name (string), a[2..] = args
                if (a.size() < 2)
                    return Value();
                wstring key = BStr(a[1]);
                vector<Value> args(a.begin() + 2, a.end());
                Interp* it = g_interpCurrent;
                return RawDispatchGet(io->second.ptr, key, args,
                                      DISPATCH_METHOD |
                                          DISPATCH_PROPERTYGET,
                                      it ? it->err : nullptr);
            });
        }
        return s_meta.get();
    }

    // wrap an IUnknown that carries IDispatch methods in its vtable but
    // refuses QI(IID_IDispatch) — CLR CCW quirk (e.g. _AssemblyName)
    static Value WrapRawThunk(IUnknown* punk)
    {
        auto w = std::make_shared<Table>();
        w->map[L"_isComObject"] = Value::Bool_(true);
        w->map[L"_isNetObject"] = Value::Bool_(true);
        w->map[L"_object"] = Value::Pointer_((void*)punk);
        w->map[L"_meta"] = Value::Table_(
            std::shared_ptr<Table>(RawThunkMeta(), [](Table*) {}));
        return Value::Table_(w);
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
        if (getenv("NODIO_TRACE"))
            fwprintf(stderr, L"[idisp] hr=0x%08lX vt=%d\n",
                     (unsigned long)hr, (int)ret.vt);
        for (auto& v : vars)
            VariantClear(&v);
        if (FAILED(hr))
        {
            if (getenv("NODIO_TRACE"))
                fwprintf(stderr, L"[inv] FAILED hr=0x%08lX vt=%d\n",
                         (unsigned long)hr, (int)ret.vt);
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
                // a probe that yields a .NET object wrapper (vt=13, the
                // _isNetObject thunk from VariantToValue) means the
                // member returned a .NET object (e.g. _Assembly.GetName
                // -> AssemblyName CCW); it must stay callable so the
                // official `asm.GetName().Name` chain works
                bool bNetThunk = v.t == Value::TableT && v.tab &&
                                 v.tab->map.count(L"_isNetObject") > 0;
                if (!err.empty() || v.t == Value::Null || bNetThunk)
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

    // ---------- _AppDomain vtable dispatch ----------
    // System._AppDomain (the legacy CreateDomainEx result) exposes no
    // IDispatch, so appDomain.loadFile/load call Load_3/Load_4 etc.
    // directly through the interface vtable.
    //
    // vtable layout (mscorlib System._AppDomain, dual — CONFIRMED from
    // mscorlib.tlb FUNCDESC.oVft, not reflection order):
    //   0-2  IUnknown (QI/AddRef/Release)
    //   3-6  IDispatch (GetTypeInfoCount/GetTypeInfo/GetIDsOfNames/Invoke)
    //   7..  interface methods in tlb declaration order; Load overloads:
    //        43=Load(AssemblyName) 44=Load_2(String) 45=Load_3(Byte[])
    //        46=Load_4(Byte[],Byte[])
    //   53=get_FriendlyName 54=get_BaseDirectory

    static __int64 TryCallSlot3(void* itf, void* target)
    {
        typedef __int64 (__stdcall *Fn3)(void*, void*);
        Fn3 fn3 = nullptr;
        memcpy(&fn3, &target, sizeof(fn3));
        __int64 hr3 = 0;
        __try {
            hr3 = fn3(itf, nullptr);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            hr3 = -1;
        }
        return hr3;
    }

    // SEH-guarded BSTR readback: the CCW stub may return S_OK with a
    // garbage/empty BSTR (half-built CCW) — guard length + copy
    static bool TryReadBStr(BSTR out, wchar_t* buf, unsigned int* pLen)
    {
        __try {
            unsigned int len = (out && SysStringLen(out) < 0x3FFF)
                                   ? SysStringLen(out) : 0;
            if (len)
                memcpy(buf, out, len * sizeof(wchar_t));
            *pLen = len;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            *pLen = 0;
            return false;
        }
    }

    static void TryFreeBStr(BSTR out)
    {
        __try {
            if (out)
                SysFreeString(out);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    // SEH-guarded 2-param (this, BSTR* retval) vtable call; the CLR CCW
    // stub blocks are heap JIT thunks — a wrong slot or half-built CCW
    // faults inside the call, which must surface as E_FAIL not a crash
    static __int64 TryGetStrSlot(void* itf, void* target, BSTR* pOut)
    {
        typedef __int64 (__stdcall *GetStrFn)(void*, BSTR*);
        GetStrFn fn = nullptr;
        memcpy(&fn, &target, sizeof(fn));
        __try {
            return fn(itf, pOut);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return -1;
        }
    }

    // SEH-guarded Load(SAFEARRAY* [,SAFEARRAY*], _Assembly**) call
    static __int64 TryLoadSlot(void* itf, void* target, bool bTwoArrays,
                               void* psa1, void* psa2, void** pOut)
    {
        typedef __int64 (__stdcall *LoadFn)(void*, void*, void*, void*);
        LoadFn fn = nullptr;
        memcpy(&fn, &target, sizeof(fn));
        __try {
            return bTwoArrays
                ? fn(itf, psa1, psa2, pOut)
                : fn(itf, psa1, pOut, nullptr);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return -1;
        }
    }

    // SEH-guarded Load(BSTR, _Assembly**) call
    static __int64 TryLoadStrSlot(void* itf, void* target, void* bstrPath,
                                  void** pOut)
    {
        typedef __int64 (__stdcall *LoadStrFn)(void*, void*, void*);
        LoadStrFn fn = nullptr;
        memcpy(&fn, &target, sizeof(fn));
        __try {
            return fn(itf, bstrPath, pOut);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return -1;
        }
    }

    static Table* AppDomainMeta()
    {
        static std::shared_ptr<Table> s_meta;
        if (!s_meta)
        {
            s_meta = std::make_shared<Table>();

            // _get(k, ownerCall, owner) — expose known members; unknown
            // members stay null so official optional checks keep working
            s_meta->map[L"_get"] = NF([](vector<Value>& a) -> Value {
                if (a.size() < 3 || a[2].t != Value::TableT || !a[2].tab)
                    return Value();
                auto io = a[2].tab->map.find(L"_object");
                if (io == a[2].tab->map.end() ||
                    io->second.t != Value::Pointer)
                    return Value();
                auto* itf = io->second.ptr;
                wstring key = BStr(a[0]);

                if (key == L"FriendlyName" || key == L"BaseDirectory")
                    // CCW stub result for these getters is unreliable
                    // (S_OK with garbage BSTR) — do not surface them
                    return Value();
                if (key == L"Load_3" || key == L"Load_4" ||
                    key == L"Load_2")
                {
                    // bound member: calling it runs the vtable slot
                    // (Load_2 = Load(String) = slot 48 — diagnostic path)
                    auto w = std::make_shared<Table>();
                    w->map[L"_boundSlot"] = Value::Num_(
                        (double)((key == L"Load_3") ? 45
                                 : (key == L"Load_2") ? 44 : 46));
                    w->map[L"_boundAppDomain"] =
                        Value::Pointer_(itf);
                    w->map[L"_meta"] = Value::Table_(s_meta);
                    return Value::Table_(w);
                }
                return Value();
            });

            // _call(owner, ...) on a Load_3/Load_4 bound member
            s_meta->map[L"_call"] = NF([](vector<Value>& a) -> Value {
                if (a.empty() || a[0].t != Value::TableT || !a[0].tab)
                    return Value();
                auto ib = a[0].tab->map.find(L"_boundSlot");
                auto ip = a[0].tab->map.find(L"_boundAppDomain");
                if (ib == a[0].tab->map.end() ||
                    ip == a[0].tab->map.end() ||
                    ip->second.t != Value::Pointer)
                    return Value();
                int slot = (int)(INT_PTR)BNum(ib->second);
                auto* itf = ip->second.ptr;

                // marshal the byte[] argument(s): aardio byte string
                // (one byte per wchar) -> flat SAFEARRAY of UI1
                auto MakeByteArray = [](const Value& v) -> SAFEARRAY* {
                    if (v.t != Value::Str || v.s.empty())
                        return nullptr;
                    ULONG n = (ULONG)v.s.size();
                    SAFEARRAYBOUND bd{ n, 0 };
                    SAFEARRAY* psa = SafeArrayCreate(VT_UI1, 1, &bd);
                    if (!psa)
                        return nullptr;
                    void* pData = nullptr;
                    if (SUCCEEDED(SafeArrayAccessData(psa, &pData)))
                    {
                        for (ULONG i = 0; i < n; i++)
                            ((unsigned char*)pData)[i] =
                                (unsigned char)v.s[i];
                        SafeArrayUnaccessData(psa);
                    }
                    return psa;
                };

                // Load_2(String) = slot 44: HRESULT Load(BSTR name,
                //                    _Assembly** retval)
                // Load_3(Byte[]) = slot 45; Load_4(Byte[],Byte[]) = 46
                void* arg1 = nullptr;
                SAFEARRAY* psa1 = nullptr;
                if (slot == 44)
                {
                    // BSTR marshaling: aardio str is UTF-8-ish byte str;
                    // an assembly name is ASCII-safe here
                    int wlen = (int)a[1].s.size();
                    arg1 = SysAllocStringLen(nullptr, (UINT)wlen);
                    for (int i = 0; i < wlen; i++)
                        ((OLECHAR*)arg1)[i] = (OLECHAR)a[1].s[i];
                }
                else
                    psa1 = a.size() > 1 ? MakeByteArray(a[1]) : nullptr;
                SAFEARRAY* psa2 = (slot == 46 && a.size() > 2)
                    ? MakeByteArray(a[2]) : nullptr;
                void* pAsm = nullptr;
                void** vtbl = *reinterpret_cast<void***>(itf);
                __int64 hr;
                if (slot == 44)
                {
                    hr = TryLoadStrSlot(itf, vtbl[slot], arg1, &pAsm);
                    if (getenv("NODIO_TRACE") && arg1)
                        fwprintf(stderr, L"[load2] bstr=%s len=%d\n",
                                 (BSTR)arg1,
                                 (int)SysStringLen((BSTR)arg1));
                }
                else
                    hr = TryLoadSlot(itf, vtbl[slot], slot == 46,
                                     psa1, psa2, &pAsm);
                if (arg1)
                    SysFreeString((BSTR)arg1);
                if (psa1)
                    SafeArrayDestroy(psa1);
                if (psa2)
                    SafeArrayDestroy(psa2);
                if (hr < 0 || !pAsm)
                {
                    Interp* it = g_interpCurrent;
                    if (it)
                    {
                        wchar_t buf[16];
                        swprintf(buf, 16, L"%08lX", (unsigned long)hr);
                        *it->err = wstring(L"dotNet: Load 失败 hr=0x") +
                                   buf;
                    }
                    return Value();
                }
                // the Assembly object is a .NET CCW: IDispatch works
                auto* pUnkAsm = reinterpret_cast<IUnknown*>(pAsm);
                IDispatch* pd = nullptr;
                if (SUCCEEDED(pUnkAsm->QueryInterface(
                        IID_IDispatch, (void**)&pd)))
                {
                    pUnkAsm->Release();
                    return WrapDispatch(pd);
                }
                return Value::Pointer_(pAsm);
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

    // com.CreateEmbed(embedObject, formOrCtrl, progId, pik)
    // official kernel API used by win.ui.ctrl metaProperty's createEmbed:
    // create the COM control, store it in the container table as
    // _object, and provide a minimal _host so the embed chain can run
    com->map[L"CreateEmbed"] = NF([](vector<Value>& a) -> Value {
        if (a.size() < 3 || a[0].t != Value::TableT || !a[0].tab)
        {
            Interp* it = g_interpCurrent;
            if (it && it->err->empty())
                *it->err = L"com.CreateEmbed: 参数不足";
            return Value();
        }
        Table* embed = a[0].tab.get();
        wstring progId =
            (a.size() > 2 && a[2].t == Value::Str) ? BStr(a[2]) : L"";
        // winform.createEmbed(,this) — omitted ProgID defaults to the
        // web browser control (official metaProperty behaviour)
        if (progId.empty())
            progId = L"Shell.Explorer";
        CLSID clsid;
        if (progId.size() > 1 && progId[0] == L'{')
            IIDFromString(progId.c_str(), &clsid);
        else if (FAILED(CLSIDFromProgID(progId.c_str(), &clsid)))
        {
            Interp* it = g_interpCurrent;
            if (it)
                *it->err = L"com.CreateEmbed: 未找到 ProgID " + progId;
            return Value();
        }
        IUnknown* punk = nullptr;
        IDispatch* pd = nullptr;
        if (FAILED(CoCreateInstance(clsid, nullptr,
                                    CLSCTX_INPROC_SERVER | CLSCTX_LOCAL_SERVER,
                                    IID_IUnknown, (void**)&punk)) ||
            !punk)
        {
            Interp* it = g_interpCurrent;
            if (it)
                *it->err = L"com.CreateEmbed: 创建失败 " + progId;
            return Value();
        }
        if (SUCCEEDED(punk->QueryInterface(IID_IDispatch, (void**)&pd)) &&
            pd)
        {
            embed->map[L"_object"] = WrapDispatch(pd);
            punk->Release();
        }
        else
        {
            embed->map[L"_object"] = Value::Pointer_((void*)punk);
        }
        // minimal OLE host: official embedObject._host.close() /
        // tranacc() get real implementations with full CAxHost support;
        // these stubs keep the embed chain runnable
        auto host = std::make_shared<Table>();
        host->map[L"close"] = NF([](vector<Value>&) -> Value {
            return Value();
        });
        host->map[L"tranacc"] = NF([](vector<Value>&) -> Value {
            return Value();
        });
        embed->map[L"_host"] = Value::Table_(host);
        return a[0];
    });

    // com.IsNetObject(v) — true for wrapped .NET CCWs (used by
    // dotNet.appDomain's object routing)
    com->map[L"IsNetObject"] = NF([](vector<Value>& a) -> Value {
        if (a.empty() || a[0].t != Value::TableT || !a[0].tab)
            return Value::Bool_(false);
        return Value::Bool_(a[0].tab->map.count(L"_isNetObject") > 0);
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
        if (getenv("NODIO_TRACE"))
        {
            IUnknown* pADX = nullptr;
            IID iidADX;
            IIDFromString(L"{05F696DC-2B29-3663-AD8B-C4389CF2A713}",
                          &iidADX);
            HRESULT hrX = punk->QueryInterface(iidADX, (void**)&pADX);
            fwprintf(stderr, L"[ccw] QI(_AppDomain) hr=0x%08lX p=%p\n",
                     (unsigned long)hrX, (void*)pADX);
            if (SUCCEEDED(hrX))
            {
                void** vtbl = *reinterpret_cast<void***>(pADX);
                int nCode = 0, nData = 0, iLastCode = -1;
                MEMORY_BASIC_INFORMATION mbi{};
                for (int i = 0; i < 70; i++)
                {
                    void* p = vtbl[i];
                    bool bCode = false;
                    if (p && VirtualQuery(p, &mbi, sizeof(mbi)) ==
                                sizeof(mbi) &&
                        mbi.Type == MEM_IMAGE)
                    {
                        bCode = true;
                        nCode++;
                        iLastCode = i;
                    }
                    else
                        nData++;
                }
                fwprintf(stderr, L"[ccw] slots code=%d data=%d lastCode=%d\n",
                         nCode, nData, iLastCode);
                for (int i = 0; i < 12; i++)
                    fwprintf(stderr, L"[ccw] slot %d = %p\n", i, vtbl[i]);
                // call the slot-3 target: on CLR 4 the slot value itself
                // IS the stub entry (verified: block begins with a normal
                // prologue when the method is JITted)
                __int64 hr3 = TryCallSlot3(pADX, vtbl[3]);
                fwprintf(stderr, L"[ccw] slot3() hr=%lld\n", hr3);
                pADX->Release();
            }
        }
        // System._AppDomain (legacy CreateDomainEx result) exposes no
        // IDispatch; its CCW slots 3+ point to CLR heap stub blocks whose
        // first bytes ARE callable thunk code (verified: slot3() returns
        // E_FAIL instead of crashing) — wrap with a vtable dispatcher so
        // appDomain.loadFile/load can call Load_3/Load_4
        IUnknown* pAD = nullptr;
        IID iidAD;
        if (SUCCEEDED(IIDFromString(
                L"{05F696DC-2B29-3663-AD8B-C4389CF2A713}", &iidAD)) &&
            SUCCEEDED(punk->QueryInterface(iidAD, (void**)&pAD)) && pAD)
        {
            auto w = std::make_shared<Table>();
            w->map[L"_isComObject"] = Value::Bool_(true);
            w->map[L"_isNetObject"] = Value::Bool_(true);
            w->map[L"_object"] = Value::Pointer_((void*)pAD);
            w->map[L"_meta"] = Value::Table_(
                std::shared_ptr<Table>(AppDomainMeta(), [](Table*) {}));
            return Value::Table_(w);
        }
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
