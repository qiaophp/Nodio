// com.cpp — kernel COM host (IDispatch dynamic invocation)
// global.com: com.CreateObject / GetObject /
// IsObject / Release ... Wrappers are tables carrying
// {_object = IDispatch*, _meta = comMeta} where comMeta._get/_set
// dispatch through IDispatch::Invoke.
#include "pch.h"
#include <ole2.h>
#include <ocidl.h>
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
        static const GUID IID_IDispatchableObject =
            { 0x7C856F49, 0x0310, 0x40F6, { 0xA1, 0xF2, 0xB7, 0xBB, 0xB4, 0xC4, 0x8F, 0x30 } };
        static const IID IID_ClrObject =
            { 0x65074f7f, 0x63c0, 0x304e, { 0xaf, 0x0a, 0xd5, 0x17, 0x41, 0xcb, 0x4a, 0x8d } };
        IUnknown* pDispObj = nullptr;
        IUnknown* pClr = nullptr;
        if (SUCCEEDED(pd->QueryInterface(IID_IDispatchableObject, (void**)&pDispObj)) && pDispObj)
        {
            pDispObj->Release();
            w->map[L"_isNetObject"] = Value::Num_(2);
        }
        else if (SUCCEEDED(pd->QueryInterface(IID_ClrObject, (void**)&pClr)) && pClr)
        {
            pClr->Release();
            w->map[L"_isNetObject"] = Value::Num_(1);
        }
        w->map[L"_meta"] = Value::Table_(
            std::shared_ptr<Table>(ComMeta(), [](Table*) {}));
        return Value::Table_(w);
    }

    // ---------- Forward Declarations ----------
    static void VariantFromValue(const Value& v, VARIANT& out, wstring* pErr);

    typedef HRESULT (WINAPI *PFN_AtlAxWinInit)();
    typedef HRESULT (WINAPI *PFN_AtlAxAttachControl)(IUnknown* pControl, HWND hWnd, IUnknown** ppUnkContainer);

    static PFN_AtlAxWinInit s_pfnAtlAxWinInit = nullptr;
    static PFN_AtlAxAttachControl s_pfnAtlAxAttachControl = nullptr;
    static bool s_bAtlLoaded = false;

    static void EnsureAtl()
    {
        if (s_bAtlLoaded) return;
        s_bAtlLoaded = true;
        HMODULE hAtl = LoadLibraryW(L"atl.dll");
        if (hAtl)
        {
            s_pfnAtlAxWinInit = (PFN_AtlAxWinInit)GetProcAddress(hAtl, "AtlAxWinInit");
            s_pfnAtlAxAttachControl = (PFN_AtlAxAttachControl)GetProcAddress(hAtl, "AtlAxAttachControl");
            if (s_pfnAtlAxWinInit)
                s_pfnAtlAxWinInit();
        }
    }

    static SAFEARRAY* CreateSafeArrayFromVector(const vector<Value>& arr, VARTYPE forcedVt)
    {
        // Check if arr is a 2D array: every element is a table with non-empty arr
        bool allSubArrays = !arr.empty();
        ULONG maxCols = 0;
        for (const auto& item : arr)
        {
            if (item.t != Value::TableT || !item.tab || item.tab->arr.empty())
            {
                allSubArrays = false;
                break;
            }
            if (item.tab->arr.size() > maxCols)
                maxCols = (ULONG)item.tab->arr.size();
        }

        if (allSubArrays && maxCols > 0)
        {
            ULONG rows = (ULONG)arr.size();
            SAFEARRAYBOUND rgsabound[2];
            rgsabound[0].lLbound = 0;
            rgsabound[0].cElements = rows;    // CLR maps rgsabound[0] to GetLength(0)
            rgsabound[1].lLbound = 0;
            rgsabound[1].cElements = maxCols; // CLR maps rgsabound[1] to GetLength(1)

            SAFEARRAY* psa = SafeArrayCreate(VT_VARIANT, 2, rgsabound);
            if (!psa) return nullptr;

            for (LONG r = 0; r < (LONG)rows; ++r)
            {
                const auto& rowVec = arr[r].tab->arr;
                for (LONG c = 0; c < (LONG)maxCols; ++c)
                {
                    LONG indices[2] = { r, c };
                    VARIANT var;
                    VariantInit(&var);
                    if (c < (LONG)rowVec.size())
                    {
                        VariantFromValue(rowVec[c], var, nullptr);
                    }
                    SafeArrayPutElement(psa, indices, &var);
                    VariantClear(&var);
                }
            }
            return psa;
        }

        VARTYPE vt = forcedVt;
        if (vt == VT_EMPTY || vt == 0 || vt == 0xFFFF)
        {
            bool allNum = !arr.empty();
            bool allStr = !arr.empty();
            for (const auto& item : arr)
            {
                if (item.t != Value::Num) allNum = false;
                if (item.t != Value::Str && !(item.t == Value::TableT && item.tab && item.tab->arr.empty() && item.tab->map.empty())) allStr = false;
            }
            if (allNum) vt = VT_R8;
            else if (allStr) vt = VT_BSTR;
            else vt = VT_VARIANT;
        }

        SAFEARRAYBOUND rgsabound[1];
        rgsabound[0].lLbound = 0;
        rgsabound[0].cElements = (ULONG)arr.size();
        SAFEARRAY* psa = SafeArrayCreate(vt, 1, rgsabound);
        if (!psa) return nullptr;

        for (LONG i = 0; i < (LONG)arr.size(); ++i)
        {
            const Value& item = arr[i];
            if (vt == VT_R8)
            {
                double d = BNum(item);
                SafeArrayPutElement(psa, &i, &d);
            }
            else if (vt == VT_BSTR)
            {
                wstring ws = BStr(item);
                BSTR bs = SysAllocStringLen(ws.c_str(), (UINT)ws.size());
                SafeArrayPutElement(psa, &i, bs);
                SysFreeString(bs);
            }
            else if (vt == VT_VARIANT)
            {
                VARIANT var;
                VariantInit(&var);
                VariantFromValue(item, var, nullptr);
                SafeArrayPutElement(psa, &i, &var);
                VariantClear(&var);
            }
            else
            {
                VARIANT var;
                VariantInit(&var);
                VariantFromValue(item, var, nullptr);
                VARIANT conv;
                VariantInit(&conv);
                if (SUCCEEDED(VariantChangeType(&conv, &var, 0, vt)))
                {
                    if (vt == VT_I4 || vt == VT_UI4 || vt == VT_INT || vt == VT_UINT)
                        SafeArrayPutElement(psa, &i, &conv.lVal);
                    else if (vt == VT_I2 || vt == VT_UI2)
                        SafeArrayPutElement(psa, &i, &conv.iVal);
                    else if (vt == VT_I1 || vt == VT_UI1)
                        SafeArrayPutElement(psa, &i, &conv.bVal);
                    else if (vt == VT_R4)
                        SafeArrayPutElement(psa, &i, &conv.fltVal);
                    else if (vt == VT_R8)
                        SafeArrayPutElement(psa, &i, &conv.dblVal);
                    else if (vt == VT_BSTR)
                        SafeArrayPutElement(psa, &i, conv.bstrVal);
                    else if (vt == VT_DISPATCH)
                        SafeArrayPutElement(psa, &i, conv.pdispVal);
                    else if (vt == VT_UNKNOWN)
                        SafeArrayPutElement(psa, &i, conv.punkVal);
                    VariantClear(&conv);
                }
                VariantClear(&var);
            }
        }
        return psa;
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
        case VT_DATE:
        {
            SYSTEMTIME st;
            if (VariantTimeToSystemTime(in.date, &st))
            {
                Value vtCtor = g_interpCurrent ? g_interpCurrent->globals.vars[L"time"] : Value();
                if (vtCtor.t == Value::TableT && vtCtor.tab)
                {
                    auto icl = vtCtor.tab->map.find(L"_call");
                    if (icl != vtCtor.tab->map.end() && icl->second.native)
                    {
                        auto inst = std::make_shared<Table>();
                        inst->map[L"year"] = Value::Num_(st.wYear);
                        inst->map[L"month"] = Value::Num_(st.wMonth);
                        inst->map[L"day"] = Value::Num_(st.wDay);
                        inst->map[L"hour"] = Value::Num_(st.wHour);
                        inst->map[L"minute"] = Value::Num_(st.wMinute);
                        inst->map[L"second"] = Value::Num_(st.wSecond);
                        vector<Value> args = { Value::Table_(inst) };
                        return icl->second.native(args);
                    }
                }
            }
            return Value::Num_(in.date);
        }
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
                static const IID IID_ClrObject =
                    { 0x65074f7f, 0x63c0, 0x304e, { 0xaf, 0x0a, 0xd5, 0x17, 0x41, 0xcb, 0x4a, 0x8d } };
                IUnknown* pClr = nullptr;
                if (SUCCEEDED(in.punkVal->QueryInterface(IID_ClrObject, (void**)&pClr)) && pClr)
                {
                    // _Object interface inherits from IDispatch and provides IDispatch vtable
                    IDispatch* pD = reinterpret_cast<IDispatch*>(pClr);
                    Value vd = WrapDispatch(pD);
                    vd.tab->map[L"_isNetObject"] = Value::Num_(1);
                    return vd;
                }
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
            if (in.vt & VT_ARRAY)
            {
                SAFEARRAY* psa = in.parray;
                if (!psa) return Value::Pointer_(nullptr);
                VARTYPE vtElem = (in.vt & ~VT_ARRAY);
                if (vtElem == VT_UI1 || vtElem == VT_I1)
                {
                    LONG lBound = 0, uBound = -1;
                    SafeArrayGetLBound(psa, 1, &lBound);
                    SafeArrayGetUBound(psa, 1, &uBound);
                    LONG count = (uBound >= lBound) ? (uBound - lBound + 1) : 0;
                    if (count <= 0) return Value::Pointer_(nullptr);
                    void* pData = nullptr;
                    if (SUCCEEDED(SafeArrayAccessData(psa, &pData)) && pData)
                    {
                        void* pBuf = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size_t)count);
                        if (pBuf)
                        {
                            memcpy(pBuf, pData, (size_t)count);
                            RegisterBuffer(pBuf, (size_t)count);
                        }
                        SafeArrayUnaccessData(psa);
                        return Value::Pointer_(pBuf);
                    }
                }
                else
                {
                    LONG lBound = 0, uBound = -1;
                    SafeArrayGetLBound(psa, 1, &lBound);
                    SafeArrayGetUBound(psa, 1, &uBound);
                    auto tb = std::make_shared<Table>();
                    for (LONG idx = lBound; idx <= uBound; ++idx)
                    {
                        if (vtElem == VT_R8)
                        {
                            double d = 0;
                            SafeArrayGetElement(psa, &idx, &d);
                            tb->arr.push_back(Value::Num_(d));
                        }
                        else if (vtElem == VT_R4)
                        {
                            float f = 0;
                            SafeArrayGetElement(psa, &idx, &f);
                            tb->arr.push_back(Value::Num_((double)f));
                        }
                        else if (vtElem == VT_I4 || vtElem == VT_INT)
                        {
                            LONG l = 0;
                            SafeArrayGetElement(psa, &idx, &l);
                            tb->arr.push_back(Value::Num_((double)l));
                        }
                        else if (vtElem == VT_UI4 || vtElem == VT_UINT)
                        {
                            ULONG ul = 0;
                            SafeArrayGetElement(psa, &idx, &ul);
                            tb->arr.push_back(Value::Num_((double)ul));
                        }
                        else if (vtElem == VT_I2)
                        {
                            SHORT s = 0;
                            SafeArrayGetElement(psa, &idx, &s);
                            tb->arr.push_back(Value::Num_((double)s));
                        }
                        else if (vtElem == VT_UI2)
                        {
                            USHORT us = 0;
                            SafeArrayGetElement(psa, &idx, &us);
                            tb->arr.push_back(Value::Num_((double)us));
                        }
                        else if (vtElem == VT_BSTR)
                        {
                            BSTR bs = nullptr;
                            SafeArrayGetElement(psa, &idx, &bs);
                            if (bs)
                            {
                                tb->arr.push_back(Value::Str_(BStrToW(bs)));
                                SysFreeString(bs);
                            }
                            else
                            {
                                tb->arr.push_back(Value::Str_(L""));
                            }
                        }
                        else if (vtElem == VT_DISPATCH)
                        {
                            IDispatch* pDisp = nullptr;
                            SafeArrayGetElement(psa, &idx, &pDisp);
                            if (pDisp)
                                tb->arr.push_back(WrapDispatch(pDisp));
                            else
                                tb->arr.push_back(Value());
                        }
                        else if (vtElem == VT_UNKNOWN)
                        {
                            IUnknown* pUnk = nullptr;
                            SafeArrayGetElement(psa, &idx, &pUnk);
                            if (pUnk)
                            {
                                IDispatch* pDisp = nullptr;
                                if (SUCCEEDED(pUnk->QueryInterface(IID_IDispatch, (void**)&pDisp)) && pDisp)
                                {
                                    tb->arr.push_back(WrapDispatch(pDisp));
                                    pUnk->Release();
                                }
                                else
                                {
                                    tb->arr.push_back(WrapRawThunk(pUnk));
                                }
                            }
                            else
                            {
                                tb->arr.push_back(Value());
                            }
                        }
                        else if (vtElem == VT_VARIANT)
                        {
                            VARIANT var;
                            VariantInit(&var);
                            SafeArrayGetElement(psa, &idx, &var);
                            tb->arr.push_back(VariantToValue(var, nullptr));
                            VariantClear(&var);
                        }
                        else
                        {
                            tb->arr.push_back(Value());
                        }
                    }
                    return Value::Table_(tb);
                }
            }
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
    class NodioCallbackDispatch : public IDispatch
    {
        LONG m_ref{ 1 };
        Value m_fn;
    public:
        NodioCallbackDispatch(const Value& fn) : m_fn(fn) {}

        STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
        {
            if (!ppv) return E_POINTER;
            if (riid == IID_IUnknown || riid == IID_IDispatch)
            {
                *ppv = static_cast<IDispatch*>(this);
                AddRef();
                return S_OK;
            }
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        STDMETHODIMP_(ULONG) AddRef() override
        {
            return InterlockedIncrement(&m_ref);
        }
        STDMETHODIMP_(ULONG) Release() override
        {
            ULONG r = InterlockedDecrement(&m_ref);
            if (r == 0) delete this;
            return r;
        }

        STDMETHODIMP GetTypeInfoCount(UINT* pctinfo) override
        {
            if (pctinfo) *pctinfo = 0;
            return S_OK;
        }
        STDMETHODIMP GetTypeInfo(UINT, LCID, ITypeInfo** ppti) override
        {
            if (ppti) *ppti = nullptr;
            return E_NOTIMPL;
        }
        STDMETHODIMP GetIDsOfNames(REFIID, LPOLESTR*, UINT, LCID, DISPID* prgdispids) override
        {
            if (prgdispids) *prgdispids = 0;
            return S_OK;
        }
        STDMETHODIMP Invoke(DISPID, REFIID, LCID, WORD,
                            DISPPARAMS* pDispParams, VARIANT* pVarResult,
                            EXCEPINFO*, UINT*) override
        {
            std::vector<Value> args;
            if (pDispParams && pDispParams->cArgs > 0)
            {
                args.resize(pDispParams->cArgs);
                for (UINT i = 0; i < pDispParams->cArgs; i++)
                {
                    VARIANT& v = pDispParams->rgvarg[pDispParams->cArgs - 1 - i];
                    wstring err;
                    args[i] = VariantToValue(v, &err);
                }
            }
            Interp* it = g_interpCurrent;
            if (!it) return E_FAIL;
            Value ret = it->CallOwner(m_fn, Value(), args);
            if (pVarResult)
            {
                wstring err;
                VariantFromValue(ret, *pVarResult, &err);
            }
            return S_OK;
        }
    };

    class NodioEnumVariant : public IEnumVARIANT
    {
        LONG m_ref{ 1 };
        std::vector<Value> m_items;
        size_t m_pos{ 0 };
    public:
        NodioEnumVariant(const std::vector<Value>& items, size_t pos = 0)
            : m_items(items), m_pos(pos) {}

        STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
        {
            if (!ppv) return E_POINTER;
            if (riid == IID_IUnknown || riid == IID_IEnumVARIANT)
            {
                *ppv = static_cast<IEnumVARIANT*>(this);
                AddRef();
                return S_OK;
            }
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_ref); }
        STDMETHODIMP_(ULONG) Release() override
        {
            ULONG r = InterlockedDecrement(&m_ref);
            if (r == 0) delete this;
            return r;
        }
        STDMETHODIMP Next(ULONG celt, VARIANT* rgVar, ULONG* pCeltFetched) override
        {
            if (!rgVar) return E_POINTER;
            if (celt > 1 && !pCeltFetched) return E_INVALIDARG;
            ULONG fetched = 0;
            while (fetched < celt && m_pos < m_items.size())
            {
                VariantInit(&rgVar[fetched]);
                VariantFromValue(m_items[m_pos++], rgVar[fetched], nullptr);
                fetched++;
            }
            if (pCeltFetched) *pCeltFetched = fetched;
            return (fetched == celt) ? S_OK : S_FALSE;
        }
        STDMETHODIMP Skip(ULONG celt) override
        {
            m_pos += celt;
            if (m_pos > m_items.size()) m_pos = m_items.size();
            return S_OK;
        }
        STDMETHODIMP Reset() override
        {
            m_pos = 0;
            return S_OK;
        }
        STDMETHODIMP Clone(IEnumVARIANT** ppEnum) override
        {
            if (!ppEnum) return E_POINTER;
            *ppEnum = new NodioEnumVariant(m_items, m_pos);
            return S_OK;
        }
    };

    class NodioTableDispatch : public IDispatch
    {
        LONG m_ref{ 1 };
        Value m_tab;
        std::unordered_map<DISPID, wstring> m_idToName;
        std::unordered_map<wstring, DISPID> m_nameToId;
        DISPID m_nextDispId{ 1 };

    public:
        NodioTableDispatch(const Value& tab) : m_tab(tab) {}

        STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
        {
            if (!ppv) return E_POINTER;
            if (riid == IID_IUnknown || riid == IID_IDispatch)
            {
                *ppv = static_cast<IDispatch*>(this);
                AddRef();
                return S_OK;
            }
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        STDMETHODIMP_(ULONG) AddRef() override
        {
            return InterlockedIncrement(&m_ref);
        }
        STDMETHODIMP_(ULONG) Release() override
        {
            ULONG r = InterlockedDecrement(&m_ref);
            if (r == 0) delete this;
            return r;
        }

        STDMETHODIMP GetTypeInfoCount(UINT* pctinfo) override
        {
            if (pctinfo) *pctinfo = 0;
            return S_OK;
        }
        STDMETHODIMP GetTypeInfo(UINT, LCID, ITypeInfo** ppti) override
        {
            if (ppti) *ppti = nullptr;
            return E_NOTIMPL;
        }
        STDMETHODIMP GetIDsOfNames(REFIID, LPOLESTR* rgszNames, UINT cNames, LCID, DISPID* prgdispids) override
        {
            if (!rgszNames || !prgdispids || cNames == 0) return E_INVALIDARG;
            for (UINT i = 0; i < cNames; i++)
            {
                wstring name = rgszNames[i];
                if (name.empty() || name == L"[DispId=0]" || name == L"_value")
                {
                    prgdispids[i] = DISPID_VALUE;
                    continue;
                }
                if (name == L"_NewEnum" || name == L"[DispId=-4]")
                {
                    prgdispids[i] = DISPID_NEWENUM;
                    continue;
                }
                if (name.size() > 8 && name.rfind(L"[DispId=", 0) == 0 && name.back() == L']')
                {
                    wstring idStr = name.substr(8, name.size() - 9);
                    prgdispids[i] = (DISPID)_wtoi(idStr.c_str());
                    continue;
                }
                auto it = m_nameToId.find(name);
                if (it != m_nameToId.end())
                {
                    prgdispids[i] = it->second;
                }
                else
                {
                    DISPID newId = m_nextDispId++;
                    m_nameToId[name] = newId;
                    m_idToName[newId] = name;
                    prgdispids[i] = newId;
                }
            }
            return S_OK;
        }
        STDMETHODIMP Invoke(DISPID dispIdMember, REFIID, LCID, WORD wFlags,
                            DISPPARAMS* pDispParams, VARIANT* pVarResult,
                            EXCEPINFO*, UINT*) override
        {
            Interp* it = g_interpCurrent;
            if (!it) return E_FAIL;

            if (dispIdMember == DISPID_NEWENUM)
            {
                if (pVarResult)
                {
                    std::vector<Value> items;
                    if (m_tab.t == Value::TableT && m_tab.tab)
                    {
                        for (const auto& kv : m_tab.tab->map)
                        {
                            if (!kv.first.empty() && kv.first[0] == L'_') continue;
                            items.push_back(Value::Str_(kv.first));
                        }
                    }
                    auto* pEnum = new NodioEnumVariant(items);
                    VariantInit(pVarResult);
                    pVarResult->vt = VT_UNKNOWN;
                    pVarResult->punkVal = pEnum;
                }
                return S_OK;
            }

            if (dispIdMember == DISPID_VALUE)
            {
                if (wFlags & (DISPATCH_METHOD | DISPATCH_PROPERTYGET))
                {
                    std::vector<Value> args;
                    if (pDispParams && pDispParams->cArgs > 0)
                    {
                        args.resize(pDispParams->cArgs);
                        for (UINT i = 0; i < pDispParams->cArgs; i++)
                        {
                            VARIANT& v = pDispParams->rgvarg[pDispParams->cArgs - 1 - i];
                            wstring err;
                            args[i] = VariantToValue(v, &err);
                        }
                    }
                    Value ret = it->CallOwner(m_tab, m_tab, args);
                    if (pVarResult)
                    {
                        wstring err;
                        VariantFromValue(ret, *pVarResult, &err);
                    }
                    return S_OK;
                }
            }

            auto itName = m_idToName.find(dispIdMember);
            if (itName == m_idToName.end()) return DISP_E_MEMBERNOTFOUND;
            wstring memberName = itName->second;

            // Handle InvokeMember method call: target.InvokeMember("method", args...)
            if (memberName == L"InvokeMember" && (wFlags & (DISPATCH_METHOD | DISPATCH_PROPERTYGET)))
            {
                if (pDispParams && pDispParams->cArgs >= 1)
                {
                    // In DISPPARAMS, args are in reverse order
                    VARIANT& vMethod = pDispParams->rgvarg[pDispParams->cArgs - 1];
                    wstring invokedMethod;
                    if (vMethod.vt == VT_BSTR && vMethod.bstrVal)
                        invokedMethod = vMethod.bstrVal;
                    else
                    {
                        wstring errV;
                        Value vm = VariantToValue(vMethod, &errV);
                        invokedMethod = vm.ToString();
                    }

                    std::vector<Value> subArgs;
                    for (int i = (int)pDispParams->cArgs - 2; i >= 0; i--)
                    {
                        wstring errV;
                        subArgs.push_back(VariantToValue(pDispParams->rgvarg[i], &errV));
                    }

                    Value fn = it->GetMember(m_tab, Value::Str_(invokedMethod), false, 1);
                    if (fn.t == Value::Func || fn.native)
                    {
                        Value ret = it->CallOwner(fn, m_tab, subArgs);
                        if (pVarResult)
                        {
                            wstring err;
                            VariantFromValue(ret, *pVarResult, &err);
                        }
                        return S_OK;
                    }
                    else
                    {
                        // Maybe property get
                        if (pVarResult)
                        {
                            wstring err;
                            VariantFromValue(fn, *pVarResult, &err);
                        }
                        return S_OK;
                    }
                }
            }

            // Normal method call or property get
            if (wFlags & (DISPATCH_METHOD | DISPATCH_PROPERTYGET))
            {
                Value memberVal = it->GetMember(m_tab, Value::Str_(memberName), false, (wFlags & DISPATCH_METHOD) ? 1 : 0);
                if ((wFlags & DISPATCH_METHOD) && (memberVal.t == Value::Func || memberVal.native))
                {
                    std::vector<Value> args;
                    if (pDispParams && pDispParams->cArgs > 0)
                    {
                        args.resize(pDispParams->cArgs);
                        for (UINT i = 0; i < pDispParams->cArgs; i++)
                        {
                            VARIANT& v = pDispParams->rgvarg[pDispParams->cArgs - 1 - i];
                            wstring err;
                            args[i] = VariantToValue(v, &err);
                        }
                    }
                    Value ret = it->CallOwner(memberVal, m_tab, args);
                    if (pVarResult)
                    {
                        wstring err;
                        VariantFromValue(ret, *pVarResult, &err);
                    }
                    return S_OK;
                }
                else
                {
                    if (pVarResult)
                    {
                        wstring err;
                        VariantFromValue(memberVal, *pVarResult, &err);
                    }
                    return S_OK;
                }
            }
            else if (wFlags & (DISPATCH_PROPERTYPUT | DISPATCH_PROPERTYPUTREF))
            {
                if (pDispParams && pDispParams->cArgs > 0)
                {
                    wstring err;
                    Value val = VariantToValue(pDispParams->rgvarg[0], &err);
                    bool bErr = false;
                    wstring strErr;
                    it->MetaSet(m_tab, memberName, val, &bErr, &strErr);
                    return S_OK;
                }
            }

            return DISP_E_MEMBERNOTFOUND;
        }
    };

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
        case Value::Func:
        case Value::Class:
        {
            auto* pCb = new NodioCallbackDispatch(v);
            out.vt = VT_DISPATCH;
            out.pdispVal = pCb;
            break;
        }
        case Value::Num:
            if (v.n == (long long)v.n && v.n >= -2147483648.0 && v.n <= 2147483647.0)
            {
                out.vt = VT_I4;
                out.lVal = (LONG)(long long)v.n;
            }
            else
            {
                out.vt = VT_R8;
                out.dblVal = v.n;
            }
            break;
        case Value::Str:
            out.vt = VT_BSTR;
            out.bstrVal = SysAllocStringLen(v.s.c_str(), (UINT)v.s.size());
            break;
        case Value::TableT:
            if (v.tab)
            {
                // Follow chain of _object / _boundDispatch / _dotNetObject
                Table* curTab = v.tab.get();
                bool bFoundDisp = false;
                for (int depth = 0; depth < 5 && curTab; ++depth)
                {
                    auto io = curTab->map.find(L"_object");
                    if (io != curTab->map.end() && io->second.t == Value::Pointer && io->second.ptr)
                    {
                        out.vt = VT_DISPATCH;
                        out.pdispVal = reinterpret_cast<IDispatch*>(io->second.ptr);
                        out.pdispVal->AddRef();
                        bFoundDisp = true;
                        break;
                    }
                    auto ib = curTab->map.find(L"_boundDispatch");
                    if (ib != curTab->map.end() && ib->second.t == Value::Pointer && ib->second.ptr)
                    {
                        out.vt = VT_DISPATCH;
                        out.pdispVal = reinterpret_cast<IDispatch*>(ib->second.ptr);
                        out.pdispVal->AddRef();
                        bFoundDisp = true;
                        break;
                    }
                    auto idno = curTab->map.find(L"_dotNetObject");
                    if (idno != curTab->map.end())
                    {
                        if (idno->second.t == Value::Pointer && idno->second.ptr)
                        {
                            out.vt = VT_DISPATCH;
                            out.pdispVal = reinterpret_cast<IDispatch*>(idno->second.ptr);
                            out.pdispVal->AddRef();
                            bFoundDisp = true;
                            break;
                        }
                        if (idno->second.t == Value::TableT && idno->second.tab)
                        {
                            curTab = idno->second.tab.get();
                            continue;
                        }
                    }
                    break;
                }
                if (bFoundDisp) break;

                // Date/time struct -> VT_DATE
                auto is = v.tab->map.find(L"_struct");
                bool bIsTime = false;
                if (is != v.tab->map.end() && is->second.t == Value::Str &&
                    is->second.s.find(L"WORD year;") != wstring::npos)
                {
                    bIsTime = true;
                }
                else if (v.tab->map.find(L"year") != v.tab->map.end() &&
                         v.tab->map.find(L"month") != v.tab->map.end() &&
                         v.tab->map.find(L"day") != v.tab->map.end())
                {
                    bIsTime = true;
                }
                if (bIsTime)
                {
                    SYSTEMTIME st{};
                    st.wYear = (WORD)BNum(v.tab->map[L"year"]);
                    st.wMonth = (WORD)BNum(v.tab->map[L"month"]);
                    st.wDay = (WORD)BNum(v.tab->map[L"day"]);
                    st.wDayOfWeek = (WORD)BNum(v.tab->map[L"dayofweek"]);
                    st.wHour = (WORD)BNum(v.tab->map[L"hour"]);
                    st.wMinute = (WORD)BNum(v.tab->map[L"minute"]);
                    st.wSecond = (WORD)BNum(v.tab->map[L"second"]);
                    st.wMilliseconds = (WORD)BNum(v.tab->map[L"milliseconds"]);
                    double dVal = 0;
                    if (SystemTimeToVariantTime(&st, &dVal))
                    {
                        out.vt = VT_DATE;
                        out.date = dVal;
                        break;
                    }
                }

                auto ivt = v.tab->map.find(L"vt");
                auto ival = v.tab->map.find(L"value");
                if (ivt != v.tab->map.end() && ival != v.tab->map.end() && ivt->second.t == Value::Num)
                {
                    VARTYPE reqVt = (VARTYPE)ivt->second.n;
                    if (ival->second.t == Value::TableT && ival->second.tab && !ival->second.tab->arr.empty())
                    {
                        SAFEARRAY* psa = CreateSafeArrayFromVector(ival->second.tab->arr, reqVt & ~VT_ARRAY);
                        if (psa)
                        {
                            out.vt = VT_ARRAY | (reqVt & ~VT_ARRAY ? (reqVt & ~VT_ARRAY) : VT_VARIANT);
                            out.parray = psa;
                            break;
                        }
                    }
                    else
                    {
                        VARIANT raw;
                        VariantInit(&raw);
                        VariantFromValue(ival->second, raw, pErr);
                        if (reqVt == VT_EMPTY || reqVt == raw.vt)
                        {
                            out = raw;
                            break;
                        }
                        if (SUCCEEDED(VariantChangeType(&out, &raw, 0, reqVt)))
                        {
                            VariantClear(&raw);
                            break;
                        }
                        VariantClear(&raw);
                    }
                }
                if (!v.tab->arr.empty())
                {
                    SAFEARRAY* psa = CreateSafeArrayFromVector(v.tab->arr, VT_EMPTY);
                    if (psa)
                    {
                        VARTYPE vtElem = VT_VARIANT;
                        SafeArrayGetVartype(psa, &vtElem);
                        out.vt = VT_ARRAY | vtElem;
                        out.parray = psa;
                        break;
                    }
                }

                // General table passed as COM parameter -> wrap as IDispatch
                auto* pTableDisp = new NodioTableDispatch(v);
                out.vt = VT_DISPATCH;
                out.pdispVal = pTableDisp;
                break;
            }
            if (pErr && pErr->empty())
                *pErr = L"com: 不支持的参数类型(table)";
            break;
        case Value::Pointer:
        {
            if (!v.ptr)
            {
                out.vt = VT_BYREF | VT_UI1;
                out.byref = nullptr;
                break;
            }
            size_t bufSize = GetBufferSize(v.ptr);
            if (bufSize > 0)
            {
                SAFEARRAY* psa = SafeArrayCreateVector(VT_UI1, 0, (ULONG)bufSize);
                if (psa)
                {
                    void* pData = nullptr;
                    if (SUCCEEDED(SafeArrayAccessData(psa, &pData)) && pData)
                    {
                        memcpy(pData, v.ptr, bufSize);
                        SafeArrayUnaccessData(psa);
                    }
                    out.vt = VT_ARRAY | VT_UI1;
                    out.parray = psa;
                    break;
                }
            }
            out.vt = VT_INT;
            out.intVal = (INT)(INT_PTR)v.ptr;
            break;
        }
        default:
            if (pErr && pErr->empty())
                *pErr = L"com: 不支持的参数类型 t=" + std::to_wstring((int)v.t);
            break;
        }
    }

    // ---------- raw IDispatch-over-vtable ----------
    // Some CLR CCW hand-backs (e.g. System._AssemblyName from
    // _Assembly::GetName) implement the four IDispatch methods in their
    // vtable but refuse QI(IID_IDispatch) with E_NOINTERFACE. For those
    // we call GetIDsOfNames (slot 5) / Invoke (slot 6) directly through
    // the vtable.
    static __int64 SafeCallGetIds(void* fn, void* punk, LPOLESTR nm, DISPID* pDispid)
    {
        typedef __int64 (__stdcall *GetIdsFn)(void*, const IID*, LPOLESTR*,
                                              UINT, ULONG, DISPID*);
        __try
        {
            return ((GetIdsFn)fn)(punk, &IID_NULL, &nm, 1, LOCALE_USER_DEFAULT, pDispid);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return E_FAIL;
        }
    }

    static HRESULT SafeCallGetTI(void* fn, void* punk, ITypeInfo** ppTI)
    {
        typedef HRESULT (__stdcall *GetTIFn)(void*, UINT, LCID, ITypeInfo**);
        __try
        {
            return fn ? ((GetTIFn)fn)(punk, 0, LOCALE_USER_DEFAULT, ppTI) : E_FAIL;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return E_FAIL;
        }
    }

    static Value RawDispatchGet(void* punk, const wstring& name,
                                vector<Value>& args, WORD wFlags,
                                wstring* pErr)
    {
        void** vtbl = *reinterpret_cast<void***>(punk);
        // slot 5: GetIDsOfNames(riid, rgszNames*, cNames, lcid, rgsid*)
        void* fnIds = nullptr;
        memcpy(&fnIds, &vtbl[5], sizeof(fnIds));
        DISPID dispid = -1;
        LPOLESTR nm = const_cast<LPOLESTR>(name.c_str());
        __int64 hr = SafeCallGetIds(fnIds, punk, nm, &dispid);
        if (getenv("NODIO_TRACE"))
        {
            fwprintf(stderr, L"[raw] GetIDsOfNames(%s) hr=0x%08llX id=%d\n",
                     name.c_str(), (unsigned long long)hr, (int)dispid);
            for (int si = 0; si < 25; si++)
                fwprintf(stderr, L"  punk vtbl[%d] = %p\n", si, vtbl[si]);
        }
        if (hr < 0 || dispid < 0)
        {
            // half-implemented CCW dispatch (E_NOTIMPL): route through
            // ITypeInfo (slot 4: GetTypeInfo) and ITypeInfo::Invoke,
            // the standard late-binding fallback for CLR CCWs
            void* fnTI = nullptr;
            memcpy(&fnTI, &vtbl[4], sizeof(fnTI));
            ITypeInfo* pTI = nullptr;
            HRESULT hrCallTI = SafeCallGetTI(fnTI, punk, &pTI);
            if (getenv("NODIO_TRACE"))
                fwprintf(stderr, L"[raw] fnTI hr=0x%08X pTI=%p\n", (unsigned int)hrCallTI, (void*)pTI);
            if (SUCCEEDED(hrCallTI) && pTI)
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
                pTI->Release();
                if (dispid < 0)
                {
                    // Fallback to _Object below
                }
                else
                {
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
            }

            // CLR CCW fallback: check if object implements _Object {65074f7f-63c0-304e-af0a-d51741cb4a8d}
            static const IID IID_ClrObject = { 0x65074f7f, 0x63c0, 0x304e, { 0xaf, 0x0a, 0xd5, 0x17, 0x41, 0xcb, 0x4a, 0x8d } };
            IUnknown* pClrObj = nullptr;
            auto* pUnk = reinterpret_cast<IUnknown*>(punk);
            if (SUCCEEDED(pUnk->QueryInterface(IID_ClrObject, (void**)&pClrObj)) && pClrObj)
            {
                void** objVtbl = *reinterpret_cast<void***>(pClrObj);
                // slot 7: ToString(BSTR* pRet)
                typedef HRESULT (__stdcall *ToStringFn)(void*, BSTR*);
                ToStringFn fnTS = nullptr;
                memcpy(&fnTS, &objVtbl[7], sizeof(fnTS));
                BSTR bstr = nullptr;
                HRESULT hrTS = fnTS ? fnTS(pClrObj, &bstr) : E_FAIL;
                wstring strVal = (SUCCEEDED(hrTS) && bstr) ? wstring(bstr) : L"";
                if (bstr)
                    SysFreeString(bstr);
                pClrObj->Release();

                if (!strVal.empty())
                {
                    if (name == L"ToString")
                    {
                        return NF([strVal](vector<Value>&) {
                            return Value::Str_(strVal);
                        });
                    }
                    if (name == L"FullName")
                        return Value::Str_(strVal);
                    if (name == L"Name")
                    {
                        size_t comma = strVal.find(L',');
                        wstring simpleName = (comma != wstring::npos) ? strVal.substr(0, comma) : strVal;
                        return Value::Str_(simpleName);
                    }
                    if (name == L"Version")
                    {
                        size_t pos = strVal.find(L"Version=");
                        if (pos != wstring::npos)
                        {
                            size_t start = pos + 8;
                            size_t end = strVal.find(L',', start);
                            wstring verStr = (end != wstring::npos) ? strVal.substr(start, end - start) : strVal.substr(start);
                            auto verTab = std::make_shared<Table>();
                            verTab->map[L"ToString"] = NF([verStr](vector<Value>&) {
                                return Value::Str_(verStr);
                            });
                            verTab->map[L"_tostring"] = NF([verStr](vector<Value>&) {
                                return Value::Str_(verStr);
                            });
                            return Value::Table_(verTab);
                        }
                    }
                }
            }

            if (pErr && pErr->empty())
                *pErr = L"dotNet: 成员不存在 " + name;
            return Value();
        }
        // slot 6: Invoke(dispid, riid, lcid, wFlags, dp, ret, ex, argErr)
        DISPPARAMS dp{};
        DISPID dispidNamed = DISPID_PROPERTYPUT;
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
        if (wFlags & (DISPATCH_PROPERTYPUT | DISPATCH_PROPERTYPUTREF))
        {
            dp.cNamedArgs = 1;
            dp.rgdispidNamedArgs = &dispidNamed;
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
                bool bOwnerCall = (a.size() >= 2 && a[1].t == Value::Bool && a[1].b);
                if (bOwnerCall)
                {
                    auto w = std::make_shared<Table>();
                    w->map[L"_boundRawThunk"] = io->second;
                    w->map[L"_boundName"] = Value::Str_(key);
                    w->map[L"_meta"] = Value::Table_(s_meta);
                    return Value::Table_(w);
                }
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
                Interp* it = g_interpCurrent;
                auto ib = a[0].tab->map.find(L"_boundRawThunk");
                if (ib != a[0].tab->map.end() && ib->second.t == Value::Pointer)
                {
                    auto in = a[0].tab->map.find(L"_boundName");
                    wstring key = (in != a[0].tab->map.end()) ? in->second.ToString() : L"";
                    vector<Value> args(a.begin() + 1, a.end());
                    return RawDispatchGet(ib->second.ptr, key, args,
                                          DISPATCH_METHOD |
                                              DISPATCH_PROPERTYGET,
                                          it ? it->err : nullptr);
                }
                auto io = a[0].tab->map.find(L"_object");
                if (io == a[0].tab->map.end() ||
                    io->second.t != Value::Pointer)
                    return Value();
                // a[1] = member name (string), a[2..] = args
                if (a.size() < 2)
                    return Value();
                wstring key = BStr(a[1]);
                vector<Value> args(a.begin() + 2, a.end());
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
        w->map[L"_rawThunk"] = Value::Bool_(true);
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
        DISPID dispidNamed = DISPID_PROPERTYPUT;
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
        if (getenv("NODIO_TRACE"))
        {
            fwprintf(stderr, L"[InvokeDisp] dispid=%ld cArgs=%d wFlags=0x%04X\n", (long)dispid, (int)dp.cArgs, (int)wFlags);
            for (size_t i = 0; i < vars.size(); i++)
                fwprintf(stderr, L"  arg[%d] vt=%d\n", (int)i, (int)vars[i].vt);
        }
        if (wFlags & (DISPATCH_PROPERTYPUT | DISPATCH_PROPERTYPUTREF))
        {
            dp.cNamedArgs = 1;
            dp.rgdispidNamedArgs = &dispidNamed;
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
                wstring key = (a.size() > 0) ? BStr(a[0]) : L"";
                if (a.size() < 3 || a[2].t != Value::TableT || !a[2].tab)
                    return Value();
                Value targetObj = a[2];
                IDispatch* pd = nullptr;
                auto io = targetObj.tab->map.find(L"_object");
                if (io != targetObj.tab->map.end() && io->second.t == Value::Pointer)
                {
                    pd = reinterpret_cast<IDispatch*>(io->second.ptr);
                }
                else
                {
                    auto ib = targetObj.tab->map.find(L"_boundDispatch");
                    auto ii = targetObj.tab->map.find(L"_boundDispId");
                    if (ib != targetObj.tab->map.end() && ib->second.t == Value::Pointer && ii != targetObj.tab->map.end())
                    {
                        auto* parentDisp = reinterpret_cast<IDispatch*>(ib->second.ptr);
                        DISPID parentDispid = (DISPID)(INT_PTR)BNum(ii->second);
                        wstring evalErr;
                        vector<Value> noArgs;
                        Value inner = InvokeDisp(parentDisp, parentDispid, DISPATCH_PROPERTYGET | DISPATCH_METHOD, noArgs, &evalErr);
                        if (inner.t == Value::TableT && inner.tab)
                        {
                            auto ioInner = inner.tab->map.find(L"_object");
                            if (ioInner != inner.tab->map.end() && ioInner->second.t == Value::Pointer)
                            {
                                pd = reinterpret_cast<IDispatch*>(ioInner->second.ptr);
                                targetObj.tab->map[L"_object"] = ioInner->second;
                            }
                        }
                    }
                }
                if (!pd)
                    return Value();
                DISPID dispid;
                OLECHAR* name = const_cast<OLECHAR*>(key.c_str());
                if (FAILED(pd->GetIDsOfNames(IID_NULL, &name, 1,
                                             LOCALE_USER_DEFAULT, &dispid)))
                {
                    // COM accessor dispatch: obj.getFoo / obj.setFoo gets the bound method
                    // e.g. "getCompile" -> "Compile", "getReference" -> "Reference", "getInvokeObjectMember" -> "InvokeObjectMember"
                    if (key.size() > 3 && (key.rfind(L"get", 0) == 0 || key.rfind(L"set", 0) == 0) && iswupper(key[3]))
                    {
                        wstring methodKey = key.substr(3);
                        OLECHAR* mname = const_cast<OLECHAR*>(methodKey.c_str());
                        if (SUCCEEDED(pd->GetIDsOfNames(IID_NULL, &mname, 1,
                                                        LOCALE_USER_DEFAULT, &dispid)))
                        {
                            return WrapBoundMember(pd, dispid, methodKey);
                        }
                    }
                    return Value();
                }
                bool bOwnerCall = (a.size() >= 2 && a[1].t == Value::Bool && a[1].b);
                if (bOwnerCall)
                    return WrapBoundMember(pd, dispid, key);

                wstring err;
                vector<Value> noArgs;
                Value v = InvokeDisp(pd, dispid, DISPATCH_PROPERTYGET | DISPATCH_METHOD,
                                     noArgs, &err);
                // a zero-arg probe that yields EMPTY is a Sub-style
                // method (e.g. IWshShortcut.Save) — keep it callable
                // a probe that yields a raw CCW thunk (_rawThunk) must stay
                // callable so late-binding calls on placeholder CCWs still work
                bool bNetThunk = v.t == Value::TableT && v.tab &&
                                 v.tab->map.count(L"_rawThunk") > 0;
                if (!err.empty() || v.t == Value::Null || bNetThunk)
                    return WrapBoundMember(pd, dispid, key);
                return v;
            });

            // _set(k, v, ownerAttr, owner) — property put
            s_meta->map[L"_set"] = NF([](vector<Value>& a) -> Value {
                Value targetObj;
                if (a.size() >= 4 && a[3].t == Value::TableT && a[3].tab)
                    targetObj = a[3];
                else if (a.size() >= 3 && a[2].t == Value::TableT && a[2].tab)
                    targetObj = a[2];
                else
                    return Value();
                IDispatch* pd = nullptr;
                auto io = targetObj.tab->map.find(L"_object");
                if (io != targetObj.tab->map.end() && io->second.t == Value::Pointer)
                {
                    pd = reinterpret_cast<IDispatch*>(io->second.ptr);
                }
                else
                {
                    auto ib = targetObj.tab->map.find(L"_boundDispatch");
                    auto ii = targetObj.tab->map.find(L"_boundDispId");
                    if (ib != targetObj.tab->map.end() && ib->second.t == Value::Pointer && ii != targetObj.tab->map.end())
                    {
                        auto* parentDisp = reinterpret_cast<IDispatch*>(ib->second.ptr);
                        DISPID parentDispid = (DISPID)(INT_PTR)BNum(ii->second);
                        wstring evalErr;
                        vector<Value> noArgs;
                        Value inner = InvokeDisp(parentDisp, parentDispid, DISPATCH_PROPERTYGET | DISPATCH_METHOD, noArgs, &evalErr);
                        if (inner.t == Value::TableT && inner.tab)
                        {
                            auto ioInner = inner.tab->map.find(L"_object");
                            if (ioInner != inner.tab->map.end() && ioInner->second.t == Value::Pointer)
                            {
                                pd = reinterpret_cast<IDispatch*>(ioInner->second.ptr);
                                targetObj.tab->map[L"_object"] = ioInner->second;
                            }
                        }
                    }
                }
                if (!pd)
                    return Value();
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
                auto iname = a[0].tab->map.find(L"_boundName");
                wstring bname = iname != a[0].tab->map.end() ? iname->second.ToString() : L"";
                if (getenv("NODIO_TRACE"))
                    fwprintf(stderr, L"[ComMeta._call] boundName=%s dispid=%ld cArgs=%d\n", bname.c_str(), (long)dispid, (int)(a.size() - 1));
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
        typedef __int64 (__stdcall *Load3Fn)(void*, void*, void*);
        LoadFn fn = nullptr;
        memcpy(&fn, &target, sizeof(fn));
        Load3Fn fn3 = nullptr;
        memcpy(&fn3, &target, sizeof(fn3));
        __try {
            return bTwoArrays
                ? fn(itf, psa1, psa2, pOut)
                : fn3(itf, psa1, pOut);
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

    // SEH-guarded GetAssemblies(SAFEARRAY** pRet) call
    static __int64 TryGetAssembliesSlot(void* itf, void* target, SAFEARRAY** pOut)
    {
        typedef __int64 (__stdcall *GetAsmsFn)(void*, SAFEARRAY**);
        GetAsmsFn fn = nullptr;
        memcpy(&fn, &target, sizeof(fn));
        __try {
            return fn(itf, pOut);
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

                if (key == L"ToString")
                {
                    return NF([](vector<Value>&) -> Value {
                        return Value::Str_(L"System.AppDomain");
                    });
                }
                if (key == L"Id")
                {
                    return Value::Num_(1.0);
                }
                if (key == L"FriendlyName")
                {
                    return Value::Str_(L"Nodio.CLR.AppDomain");
                }
                if (key == L"BaseDirectory")
                {
                    wchar_t szExe[MAX_PATH] = L"";
                    GetModuleFileNameW(nullptr, szExe, MAX_PATH);
                    wchar_t* pSep = wcsrchr(szExe, L'\\');
                    if (pSep) *pSep = 0;
                    return Value::Str_(szExe);
                }
                if (key == L"GetAssemblies")
                {
                    auto w = std::make_shared<Table>();
                    w->map[L"_boundSlot"] = Value::Num_(57.0);
                    w->map[L"_boundAppDomain"] = Value::Pointer_(itf);
                    w->map[L"_meta"] = Value::Table_(s_meta);
                    return Value::Table_(w);
                }
                if (key == L"Load_3" || key == L"Load_4" ||
                    key == L"Load_2")
                {
                    // bound member: calling it runs the vtable slot
                    // (Load_2 = Load(String) = slot 44)
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
                void** vtbl = *reinterpret_cast<void***>(itf);

                if (slot == 57)
                {
                    // GetAssemblies() = slot 57: HRESULT GetAssemblies(SAFEARRAY** pRet)
                    SAFEARRAY* psa = nullptr;
                    __int64 hr = TryGetAssembliesSlot(itf, vtbl[slot], &psa);
                    if (hr < 0 || !psa)
                    {
                        Interp* it = g_interpCurrent;
                        if (it)
                        {
                            wchar_t buf[16];
                            swprintf(buf, 16, L"%08lX", (unsigned long)hr);
                            *it->err = wstring(L"dotNet: GetAssemblies 失败 hr=0x") + buf;
                        }
                        return Value();
                    }

                    auto resTable = std::make_shared<Table>();
                    long lBound = 0, uBound = -1;
                    SafeArrayGetLBound(psa, 1, &lBound);
                    SafeArrayGetUBound(psa, 1, &uBound);

                    IUnknown** pElems = nullptr;
                    if (SUCCEEDED(SafeArrayAccessData(psa, (void**)&pElems)))
                    {
                        for (long i = lBound; i <= uBound; i++)
                        {
                            IUnknown* pUnk = pElems[i - lBound];
                            if (pUnk)
                            {
                                IDispatch* pDisp = nullptr;
                                if (SUCCEEDED(pUnk->QueryInterface(IID_IDispatch, (void**)&pDisp)))
                                {
                                    resTable->arr.push_back(WrapDispatch(pDisp));
                                }
                                else
                                {
                                    pUnk->AddRef();
                                    resTable->arr.push_back(Value::Pointer_(pUnk));
                                }
                            }
                            else
                            {
                                resTable->arr.push_back(Value());
                            }
                        }
                        SafeArrayUnaccessData(psa);
                    }
                    SafeArrayDestroy(psa);
                    return Value::Table_(resTable);
                }

                // marshal the byte[] argument(s): byte string
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
                    // BSTR marshaling: str is UTF-8-ish byte str;
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
    // _object, and provide a full OLE _host via AtlAxAttachControl
    com->map[L"CreateEmbed"] = NF([](vector<Value>& a) -> Value {
        if (a.empty() || a[0].t != Value::TableT || !a[0].tab)
        {
            Interp* it = g_interpCurrent;
            if (it && it->err->empty())
                *it->err = L"com.CreateEmbed: 参数不足";
            return Value();
        }
        Table* embed = a[0].tab.get();

        IUnknown* punk = nullptr;
        if (a.size() > 2 && a[2].t == Value::TableT && a[2].tab)
        {
            auto itObj = a[2].tab->map.find(L"_object");
            if (itObj != a[2].tab->map.end())
            {
                if (itObj->second.t == Value::Pointer && itObj->second.ptr)
                {
                    punk = (IUnknown*)itObj->second.ptr;
                    punk->AddRef();
                }
                else if (itObj->second.t == Value::TableT && itObj->second.tab)
                {
                    auto itSub = itObj->second.tab->map.find(L"_object");
                    if (itSub != itObj->second.tab->map.end() && itSub->second.t == Value::Pointer && itSub->second.ptr)
                    {
                        punk = (IUnknown*)itSub->second.ptr;
                        punk->AddRef();
                    }
                }
            }
        }

        if (!punk)
        {
            wstring progId = (a.size() > 2 && a[2].t == Value::Str) ? BStr(a[2]) : L"";
            if (progId.empty())
                progId = L"Shell.Explorer";
            CLSID clsid{};
            if (progId.size() > 1 && progId[0] == L'{')
                IIDFromString(progId.c_str(), &clsid);
            else if (FAILED(CLSIDFromProgID(progId.c_str(), &clsid)))
            {
                Interp* it = g_interpCurrent;
                if (it)
                    *it->err = L"com.CreateEmbed: 未找到 ProgID " + progId;
                return Value();
            }
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
        }

        HWND hwndHost = nullptr;
        if (a.size() > 1 && a[1].t == Value::TableT && a[1].tab)
        {
            auto itH = a[1].tab->map.find(L"hwnd");
            if (itH != a[1].tab->map.end())
            {
                if (itH->second.t == Value::Pointer)
                    hwndHost = (HWND)itH->second.ptr;
                else if (itH->second.t == Value::Num)
                    hwndHost = (HWND)(INT_PTR)itH->second.n;
            }
        }

        IUnknown* pContainer = nullptr;
        if (hwndHost && IsWindow(hwndHost))
        {
            EnsureAtl();
            if (s_pfnAtlAxAttachControl)
            {
                s_pfnAtlAxAttachControl(punk, hwndHost, &pContainer);
            }
        }

        IDispatch* pd = nullptr;
        if (SUCCEEDED(punk->QueryInterface(IID_IDispatch, (void**)&pd)) && pd)
        {
            embed->map[L"_object"] = WrapDispatch(pd);
            pd->Release();
        }
        else
        {
            embed->map[L"_object"] = Value::Pointer_((void*)punk);
        }

        if (a.size() > 1)
        {
            embed->map[L"_form"] = a[1];
        }

        auto host = std::make_shared<Table>();
        host->map[L"close"] = NF([pContainer, punk](vector<Value>&) -> Value {
            if (pContainer) pContainer->Release();
            if (punk) punk->Release();
            return Value();
        });
        host->map[L"tranacc"] = NF([punk](vector<Value>& aMsg) -> Value {
            if (!punk || aMsg.empty()) return Value::Bool_(false);
            IOleInPlaceActiveObject* pActive = nullptr;
            if (SUCCEEDED(punk->QueryInterface(IID_IOleInPlaceActiveObject, (void**)&pActive)) && pActive)
            {
                MSG msg{};
                if (aMsg[0].t == Value::TableT && aMsg[0].tab)
                {
                    msg.hwnd = (HWND)(INT_PTR)BNum(aMsg[0].tab->map[L"hwnd"]);
                    msg.message = (UINT)BNum(aMsg[0].tab->map[L"message"]);
                    msg.wParam = (WPARAM)(INT_PTR)BNum(aMsg[0].tab->map[L"wParam"]);
                    msg.lParam = (LPARAM)(INT_PTR)BNum(aMsg[0].tab->map[L"lParam"]);
                }
                HRESULT hr = pActive->TranslateAcceleratorW(&msg);
                pActive->Release();
                return Value::Bool_(hr == S_OK);
            }
            return Value::Bool_(false);
        });
        host->map[L"adjust"] = NF([hwndHost](vector<Value>&) -> Value {
            if (hwndHost && IsWindow(hwndHost))
            {
                RECT rc;
                GetClientRect(hwndHost, &rc);
                HWND hChild = GetWindow(hwndHost, GW_CHILD);
                if (hChild)
                {
                    SetWindowPos(hChild, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                                 SWP_NOZORDER | SWP_NOACTIVATE);
                }
            }
            return Value();
        });
        host->map[L"_adjust"] = host->map[L"adjust"];
        host->map[L"doObjectVerb"] = NF([punk, hwndHost](vector<Value>& aV) -> Value {
            LONG verb = aV.empty() ? OLEIVERB_SHOW : (LONG)BNum(aV[0]);
            IOleObject* pOle = nullptr;
            if (punk && SUCCEEDED(punk->QueryInterface(IID_IOleObject, (void**)&pOle)) && pOle)
            {
                RECT rc{};
                if (hwndHost && IsWindow(hwndHost)) GetClientRect(hwndHost, &rc);
                pOle->DoVerb(verb, nullptr, nullptr, 0, hwndHost, &rc);
                pOle->Release();
            }
            return Value();
        });
        embed->map[L"_host"] = Value::Table_(host);
        return a[0];
    });

    // com.IsNetObject(v) — 1 for .NET CCW, 2 for DispatchableObject, 0 otherwise
    com->map[L"IsNetObject"] = NF([](vector<Value>& a) -> Value {
        if (a.empty() || a[0].t != Value::TableT || !a[0].tab)
            return Value::Num_(0);
        Table* t = a[0].tab.get();
        if (t->map.count(L"_dotNetObject"))
        {
            auto id = t->map.find(L"_dotNetObject");
            if (id != t->map.end() && id->second.t == Value::TableT && id->second.tab)
                t = id->second.tab.get();
        }
        auto in = t->map.find(L"_isNetObject");
        if (in != t->map.end() && in->second.t == Value::Num)
            return in->second;
        auto io = t->map.find(L"_object");
        if (io != t->map.end() && io->second.t == Value::Pointer && io->second.ptr)
        {
            IUnknown* punk = reinterpret_cast<IUnknown*>(io->second.ptr);
            static const GUID IID_IDispatchableObject =
                { 0x7C856F49, 0x0310, 0x40F6, { 0xA1, 0xF2, 0xB7, 0xBB, 0xB4, 0xC4, 0x8F, 0x30 } };
            IUnknown* pDispObj = nullptr;
            if (SUCCEEDED(punk->QueryInterface(IID_IDispatchableObject, (void**)&pDispObj)) && pDispObj)
            {
                pDispObj->Release();
                return Value::Num_(2);
            }
            static const IID IID_ClrObject =
                { 0x65074f7f, 0x63c0, 0x304e, { 0xaf, 0x0a, 0xd5, 0x17, 0x41, 0xcb, 0x4a, 0x8d } };
            IUnknown* pClr = nullptr;
            if (SUCCEEDED(punk->QueryInterface(IID_ClrObject, (void**)&pClr)) && pClr)
            {
                pClr->Release();
                return Value::Num_(1);
            }
        }
        if (in != t->map.end() && in->second.Truthy())
            return Value::Num_(1);
        return Value::Num_(0);
    });

    // com.Release(v)
    com->map[L"Release"] = NF([](vector<Value>& a) -> Value {
        if (!a.empty())
        {
            if (a[0].t == Value::Pointer && a[0].ptr)
            {
                auto* punk = reinterpret_cast<IUnknown*>(a[0].ptr);
                punk->Release();
                return Value::Num_(1);
            }
            if (a[0].t == Value::TableT && a[0].tab)
            {
                auto io = a[0].tab->map.find(L"_object");
                if (io != a[0].tab->map.end() &&
                    io->second.t == Value::Pointer && io->second.ptr)
                {
                    auto* punk = reinterpret_cast<IUnknown*>(io->second.ptr);
                    punk->Release();
                    return Value::Num_(1);
                }
            }
        }
        return Value::Num_(0);
    });

    // com.GetIPicture(punkOrObj) -> IPicture COM wrapper table
    com->map[L"GetIPicture"] = NF([](vector<Value>& a) -> Value {
        if (a.empty()) return Value();
        IUnknown* punk = nullptr;
        if (a[0].t == Value::Pointer)
            punk = (IUnknown*)a[0].ptr;
        else if (a[0].t == Value::Num)
            punk = (IUnknown*)(INT_PTR)(long long)a[0].n;
        else if (a[0].t == Value::TableT && a[0].tab)
        {
            auto io = a[0].tab->map.find(L"_object");
            if (io != a[0].tab->map.end() && io->second.t == Value::Pointer)
                punk = (IUnknown*)io->second.ptr;
        }
        if (!punk) return Value();

        IPicture* pPic = nullptr;
        punk->QueryInterface(IID_IPicture, (void**)&pPic);
        IDispatch* pd = nullptr;
        punk->QueryInterface(IID_IDispatch, (void**)&pd);

        std::shared_ptr<Table> w;
        if (pd)
        {
            Value vObj = WrapDispatch(pd);
            w = vObj.tab;
            pd->Release();
        }
        else
        {
            w = std::make_shared<Table>();
            punk->AddRef();
            w->map[L"_object"] = Value::Pointer_(punk);
            w->map[L"_isComObject"] = Value::Bool_(true);
            w->map[L"_meta"] = Value::Table_(
                std::shared_ptr<Table>(ComMeta(), [](Table*) {}));
        }

        w->map[L"_type"] = Value::Str_(L"com.IPicture");
        w->map[L"_topointer"] = NF([punk](vector<Value>&) {
            return Value::Pointer_(punk);
        });

        // CopyHandle(cx=0, cy=0) -> (hNewHandle, imgType)
        w->map[L"CopyHandle"] = NF([punk](vector<Value>& args) -> Value {
            IPicture* pI = nullptr;
            if (FAILED(punk->QueryInterface(IID_IPicture, (void**)&pI)) || !pI)
                return Value();

            OLE_HANDLE hSrc = 0;
            pI->get_Handle(&hSrc);
            SHORT picType = 0;
            pI->get_Type(&picType);

            int cx = (args.size() > 0 && args[0].t == Value::Num) ? (int)args[0].n : 0;
            int cy = (args.size() > 1 && args[1].t == Value::Num) ? (int)args[1].n : 0;

            HANDLE hCopy = nullptr;
            int imgType = 0;
            if (picType == PICTYPE_BITMAP)
            {
                imgType = 0; // _IMAGE_BITMAP
                hCopy = CopyImage((HANDLE)(INT_PTR)hSrc, IMAGE_BITMAP, cx, cy, LR_COPYRETURNORG);
            }
            else if (picType == PICTYPE_ICON)
            {
                imgType = 1; // _IMAGE_ICON
                hCopy = (HANDLE)CopyIcon((HICON)(INT_PTR)hSrc);
            }
            else
            {
                imgType = 0;
                hCopy = CopyImage((HANDLE)(INT_PTR)hSrc, IMAGE_BITMAP, cx, cy, LR_COPYRETURNORG);
            }
            pI->Release();

            Value first = Value::Pointer_(hCopy);
            auto outs = std::make_shared<vector<Value>>();
            outs->push_back(Value::Num_(imgType));
            first.mrets = outs;
            return first;
        });

        if (pPic) pPic->Release();
        return Value::Table_(w);
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

    // com.ImplInterface(implObject) -> IDispatch COM wrapper table
    com->map[L"ImplInterface"] = NF([](vector<Value>& a) -> Value {
        if (a.empty()) return Value();
        const Value& v = a[0];
        if (v.t == Value::TableT && v.tab)
        {
            if (v.tab->map.count(L"_object") || v.tab->map.count(L"_boundDispatch"))
                return v;
        }
        IDispatch* pDisp = nullptr;
        if (v.t == Value::Func || v.native)
            pDisp = new NodioCallbackDispatch(v);
        else
            pDisp = new NodioTableDispatch(v);
        return WrapDispatch(pDisp);
    });

    // com.SetPreferredInvokeKind(comObject, preferredInvokeKind)
    com->map[L"SetPreferredInvokeKind"] = NF([](vector<Value>& a) -> Value {
        if (a.size() > 1 && a[0].t == Value::TableT && a[0].tab)
        {
            a[0].tab->map[L"_preferredInvokeKind"] = a[1];
        }
        return Value::Bool_(true);
    });

    // com.Variant(val, vt, byRef)
    com->map[L"Variant"] = NF([](vector<Value>& a) -> Value {
        auto tb = std::make_shared<Table>();
        tb->map[L"_isComVariant"] = Value::Bool_(true);
        VARTYPE vt = VT_EMPTY;
        if (a.empty())
        {
            tb->map[L"vt"] = Value::Num_(VT_EMPTY);
            tb->map[L"value"] = Value();
        }
        else
        {
            Value val = a[0];
            if (a.size() > 1 && a[1].t == Value::Num)
                vt = (VARTYPE)a[1].n;
            else if (val.t == Value::Null)
                vt = VT_NULL;
            else if (val.t == Value::Bool)
                vt = VT_BOOL;
            else if (val.t == Value::Num)
                vt = (val.n == (long long)val.n) ? VT_I4 : VT_R8;
            else if (val.t == Value::Str)
                vt = VT_BSTR;
            else if (val.t == Value::TableT)
                vt = VT_ARRAY | VT_VARIANT;
            else
                vt = VT_EMPTY;

            tb->map[L"vt"] = Value::Num_((double)vt);
            tb->map[L"value"] = val;
            if (a.size() > 2 && a[2].Truthy())
                tb->map[L"byRef"] = Value::Bool_(true);
        }
        return Value::Table_(tb);
    });

    // com.each(obj)
    com->map[L"each"] = NF([](vector<Value>& a) -> Value {
        if (a.empty())
            return Value();
        Value target = a[0];
        // If target is a table with array elements, iterate its array
        if (target.t == Value::TableT && target.tab && !target.tab->arr.empty())
        {
            auto pIdx = std::make_shared<int>(0);
            auto arrCopy = target.tab->arr;
            return NF([pIdx, arrCopy](vector<Value>&) -> Value {
                if (*pIdx < (int)arrCopy.size())
                {
                    Value retVal = arrCopy[*pIdx];
                    (*pIdx)++;
                    Value res = Value::Num_((double)*pIdx);
                    res.mrets = std::make_shared<vector<Value>>();
                    res.mrets->push_back(retVal);
                    return res;
                }
                return Value();
            });
        }
        // If target has IEnumVARIANT or is a COM dispatch with _NewEnum (DISPID_NEWENUM = -4)
        IDispatch* pDisp = nullptr;
        if (target.t == Value::TableT && target.tab)
        {
            auto io = target.tab->map.find(L"_object");
            if (io != target.tab->map.end() && io->second.t == Value::Pointer)
                pDisp = reinterpret_cast<IDispatch*>(io->second.ptr);
            else
            {
                auto ib = target.tab->map.find(L"_boundDispatch");
                if (ib != target.tab->map.end() && ib->second.t == Value::Pointer)
                    pDisp = reinterpret_cast<IDispatch*>(ib->second.ptr);
            }
        }
        if (pDisp)
        {
            DISPPARAMS dpNoArgs = { nullptr, nullptr, 0, 0 };
            VARIANT varEnum;
            VariantInit(&varEnum);
            HRESULT hr = pDisp->Invoke(DISPID_NEWENUM, IID_NULL, LOCALE_USER_DEFAULT,
                                       DISPATCH_METHOD | DISPATCH_PROPERTYGET,
                                       &dpNoArgs, &varEnum, nullptr, nullptr);
            IEnumVARIANT* pEnum = nullptr;
            if (SUCCEEDED(hr))
            {
                if (varEnum.vt == VT_UNKNOWN && varEnum.punkVal)
                    varEnum.punkVal->QueryInterface(IID_IEnumVARIANT, (void**)&pEnum);
                else if (varEnum.vt == VT_DISPATCH && varEnum.pdispVal)
                    varEnum.pdispVal->QueryInterface(IID_IEnumVARIANT, (void**)&pEnum);
                VariantClear(&varEnum);
            }
            if (pEnum)
            {
                auto pIdx = std::make_shared<int>(0);
                auto spEnum = std::shared_ptr<IEnumVARIANT>(pEnum, [](IEnumVARIANT* pe) { if (pe) pe->Release(); });
                return NF([spEnum, pIdx](vector<Value>&) -> Value {
                    VARIANT varItem;
                    VariantInit(&varItem);
                    ULONG fetched = 0;
                    HRESULT hrNext = spEnum->Next(1, &varItem, &fetched);
                    if (hrNext == S_OK && fetched == 1)
                    {
                        (*pIdx)++;
                        Value val = VariantToValue(varItem, nullptr);
                        VariantClear(&varItem);
                        Value res = Value::Num_((double)*pIdx);
                        res.mrets = std::make_shared<vector<Value>>();
                        res.mrets->push_back(val);
                        return res;
                    }
                    VariantClear(&varItem);
                    return Value();
                });
            }
        }
        // Fallback: table key-value or index iterator
        if (target.t == Value::TableT && target.tab)
        {
            auto pIdx = std::make_shared<int>(0);
            auto arrCopy = target.tab->arr;
            return NF([pIdx, arrCopy](vector<Value>&) -> Value {
                if (*pIdx < (int)arrCopy.size())
                {
                    Value retVal = arrCopy[*pIdx];
                    (*pIdx)++;
                    Value res = Value::Num_((double)*pIdx);
                    res.mrets = std::make_shared<vector<Value>>();
                    res.mrets->push_back(retVal);
                    return res;
                }
                return Value();
            });
        }
        return Value();
    });

    auto fnGetEnumerator = NF([](vector<Value>& a) -> Value {
        if (a.empty()) return Value();
        Value target = a[0];
        IDispatch* pDisp = nullptr;
        if (target.t == Value::Pointer)
        {
            pDisp = reinterpret_cast<IDispatch*>(target.ptr);
        }
        else if (target.t == Value::TableT && target.tab)
        {
            auto io = target.tab->map.find(L"_object");
            if (io != target.tab->map.end() && io->second.t == Value::Pointer)
                pDisp = reinterpret_cast<IDispatch*>(io->second.ptr);
            else
            {
                auto ib = target.tab->map.find(L"_boundDispatch");
                if (ib != target.tab->map.end() && ib->second.t == Value::Pointer)
                    pDisp = reinterpret_cast<IDispatch*>(ib->second.ptr);
            }
        }
        IEnumVARIANT* pEnum = nullptr;
        if (pDisp)
        {
            DISPPARAMS dpNoArgs = { nullptr, nullptr, 0, 0 };
            VARIANT varEnum;
            VariantInit(&varEnum);
            HRESULT hr = pDisp->Invoke(DISPID_NEWENUM, IID_NULL, LOCALE_USER_DEFAULT,
                                       DISPATCH_METHOD | DISPATCH_PROPERTYGET,
                                       &dpNoArgs, &varEnum, nullptr, nullptr);
            if (SUCCEEDED(hr))
            {
                if (varEnum.vt == VT_UNKNOWN && varEnum.punkVal)
                    varEnum.punkVal->QueryInterface(IID_IEnumVARIANT, (void**)&pEnum);
                else if (varEnum.vt == VT_DISPATCH && varEnum.pdispVal)
                    varEnum.pdispVal->QueryInterface(IID_IEnumVARIANT, (void**)&pEnum);
                VariantClear(&varEnum);
            }
            if (!pEnum)
            {
                pDisp->QueryInterface(IID_IEnumVARIANT, (void**)&pEnum);
            }
        }
        if (!pEnum) return Value();

        auto spEnum = std::shared_ptr<IEnumVARIANT>(pEnum, [](IEnumVARIANT* pe) { if (pe) pe->Release(); });
        auto tbEnum = std::make_shared<Table>();
        tbEnum->map[L"_isEnumerator"] = Value::Bool_(true);
        tbEnum->map[L"_object"] = Value::Pointer_(pEnum);

        tbEnum->map[L"Next"] = NF([spEnum](vector<Value>&) -> Value {
            VARIANT varItem;
            VariantInit(&varItem);
            ULONG fetched = 0;
            HRESULT hrNext = spEnum->Next(1, &varItem, &fetched);
            if (hrNext == S_OK && fetched == 1)
            {
                Value val = VariantToValue(varItem, nullptr);
                VariantClear(&varItem);
                return val;
            }
            VariantClear(&varItem);
            return Value();
        });

        tbEnum->map[L"Reset"] = NF([spEnum](vector<Value>&) -> Value {
            spEnum->Reset();
            return Value::Bool_(true);
        });

        tbEnum->map[L"Skip"] = NF([spEnum](vector<Value>& aArgs) -> Value {
            ULONG c = (aArgs.empty() || aArgs[0].t != Value::Num) ? 1 : (ULONG)aArgs[0].n;
            spEnum->Skip(c);
            return Value::Bool_(true);
        });

        return Value::Table_(tbEnum);
    });

    auto fnIsEnumerator = NF([](vector<Value>& a) -> Value {
        if (a.empty()) return Value::Bool_(false);
        Value v = a[0];
        if (v.t == Value::TableT && v.tab && v.tab->map.count(L"_isEnumerator"))
            return Value::Bool_(true);
        return Value::Bool_(false);
    });

    com->map[L"GetEnumerator"] = fnGetEnumerator;
    com->map[L"isEnumerator"] = fnIsEnumerator;
    globalsEnv.vars[L"GetEnumerator"] = fnGetEnumerator;
    globalsEnv.vars[L"isEnumerator"] = fnIsEnumerator;

    globalsEnv.vars[L"com"] = Value::Table_(com);
    // plain `import com` must resolve to this kernel namespace
    // instead of creating an empty fallback module that would shadow it
    roots[L"com"] = Value::Table_(com);
}
} // namespace NodioVM
