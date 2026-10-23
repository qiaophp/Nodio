// com.cpp — kernel COM host (IDispatch dynamic invocation)
// global.com: com.CreateObject / GetObject /
// IsObject / Release ... Wrappers are tables carrying
// {_object = IDispatch*, _meta = comMeta} where comMeta._get/_set
// dispatch through IDispatch::Invoke.
#include "pch.h"
#include <ole2.h>
#include <ocidl.h>
#include "interp.h"
#include "engine/comsig.h"

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

    // ComGuidString 定义在类型信息那一段（后面），这里先声明
    static wstring ComGuidString(const GUID& g);

    // 源接口类型信息（定义在事件接收器那一段，后面）
    static ITypeInfo* FindSourceTypeInfo(IUnknown* pUnk, REFIID srcIid,
                                         bool bAllowRaw);

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
    static void VariantFromValue(const Value& v, VARIANT& out, wstring* pErr, VARTYPE forcedArrayVt = VT_EMPTY);

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

    static std::string BytesOf(const wstring& ws)
    {
        std::string out;
        out.reserve(ws.size());
        for (wchar_t c : ws) out.push_back((char)(unsigned char)c);
        return out;
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
                Value vtCtor = g_interpCurrent ? g_interpCurrent->globals.vars[NameId(L"time")] : Value();
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
        VARTYPE m_preferredArrayType{ VT_EMPTY };

    public:
        NodioTableDispatch(const Value& tab, VARTYPE prefArrayType = VT_EMPTY)
            : m_tab(tab), m_preferredArrayType(prefArrayType)
        {
            if (m_preferredArrayType == VT_EMPTY && m_tab.t == Value::TableT && m_tab.tab)
            {
                auto ipat = m_tab.tab->map.find(L"_preferredArrayType");
                if (ipat != m_tab.tab->map.end() && ipat->second.t == Value::Num)
                    m_preferredArrayType = (VARTYPE)ipat->second.n;
            }
        }

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
                        if (!m_tab.tab->arr.empty())
                        {
                            for (const auto& item : m_tab.tab->arr)
                                items.push_back(item);
                        }
                        else
                        {
                            for (const auto& kv : m_tab.tab->map)
                            {
                                if (!kv.first.empty() && kv.first[0] == L'_') continue;
                                items.push_back(Value::Str_(kv.first));
                            }
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
                    Value ret;
                    bool bHandled = false;
                    if (m_tab.t == Value::TableT && m_tab.tab)
                    {
                        auto im = m_tab.tab->map.find(L"_meta");
                        bool hasCall = (im != m_tab.tab->map.end() && im->second.t == Value::TableT &&
                                        im->second.tab && im->second.tab->map.count(L"_call"));
                        if (!hasCall && !args.empty())
                        {
                            if (args[0].t == Value::Num)
                            {
                                int idx = (int)args[0].n;
                                if (idx >= 0 && idx < (int)m_tab.tab->arr.size())
                                {
                                    ret = m_tab.tab->arr[idx];
                                    bHandled = true;
                                }
                                else if (idx >= 1 && idx <= (int)m_tab.tab->arr.size())
                                {
                                    ret = m_tab.tab->arr[idx - 1];
                                    bHandled = true;
                                }
                            }
                            else if (args[0].t == Value::Str)
                            {
                                auto itm = m_tab.tab->map.find(args[0].s);
                                if (itm != m_tab.tab->map.end())
                                {
                                    ret = itm->second;
                                    bHandled = true;
                                }
                            }
                        }
                    }
                    if (!bHandled)
                    {
                        ret = it->CallOwner(m_tab, m_tab, args);
                    }
                    if (pVarResult)
                    {
                        wstring err;
                        VariantFromValue(ret, *pVarResult, &err, m_preferredArrayType);
                    }
                    return S_OK;
                }
                else if (wFlags & (DISPATCH_PROPERTYPUT | DISPATCH_PROPERTYPUTREF))
                {
                    if (pDispParams && pDispParams->cArgs > 0)
                    {
                        wstring errVal;
                        Value val = VariantToValue(pDispParams->rgvarg[0], &errVal);
                        if (pDispParams->cArgs == 1)
                        {
                            m_tab = val;
                            return S_OK;
                        }
                        wstring errIdx;
                        Value idxVal = VariantToValue(pDispParams->rgvarg[pDispParams->cArgs - 1], &errIdx);
                        if (m_tab.t == Value::TableT && m_tab.tab)
                        {
                            if (idxVal.t == Value::Num)
                            {
                                int idx = (int)idxVal.n;
                                if (idx >= 0 && idx < (int)m_tab.tab->arr.size())
                                {
                                    m_tab.tab->arr[idx] = val;
                                    return S_OK;
                                }
                                else if (idx == (int)m_tab.tab->arr.size())
                                {
                                    m_tab.tab->arr.push_back(val);
                                    return S_OK;
                                }
                                else if (idx >= 1 && idx <= (int)m_tab.tab->arr.size())
                                {
                                    m_tab.tab->arr[idx - 1] = val;
                                    return S_OK;
                                }
                            }
                            else if (idxVal.t == Value::Str)
                            {
                                m_tab.tab->map[idxVal.s] = val;
                                return S_OK;
                            }
                        }
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
                        VariantFromValue(ret, *pVarResult, &err, m_preferredArrayType);
                    }
                    return S_OK;
                }
                else
                {
                    if (pVarResult)
                    {
                        wstring err;
                        VariantFromValue(memberVal, *pVarResult, &err, m_preferredArrayType);
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

    // ---------- COM 事件接收器（com.Connect / com.AddConnection / com.ReleaseConnection）----------
    //
    // 官方契约（D:\aardio\lib\builtin\com.aardio:197-216）：
    //   com.Connect(COM对象, 事件监听表) -> sink, cookie
    //     把 @2 的表对象转换为 COM 对象的**默认事件监听器**并实现默认事件接口；
    //     返回 sink（事件接收器）与 cookie（连接点标识）。
    //   com.AddConnection(COM对象, 事件接收器) -> cookie
    //     事件接收器可以是 com.ImplInterface 创建的动态事件接口（已是 COM 对象）。
    //   com.ReleaseConnection(COM对象[, 事件接收器, cookie])
    //
    // 官方 builtin/com.aardio 的 ConnectWeak / CreateEmbedEx 都是 aardio 源码，
    // 但都调用引擎原生的 Connect —— 所以 Connect 必须由内核提供。
    //
    // 实现范围（实测边界）：
    //   支持 **IDispatch 型连接点**（IConnectionPointContainer + IID_IDispatch，
    //   即 HTML / ActiveX / OCX 控件的默认事件接口，官方 web.form、
    //   win.ui.ctrl.metaProperty 走的就是这条）。
    //   原始（非 IDispatch）接口的连接点需要按 ITypeInfo 动态构造 vtable，
    //   本轮不实现（官方 com.wmi 的 SWbemSink 属于这一类）。

    // 从 aardio 值取 COM 的 IUnknown 指针（COM 对象包装表 / 绑定成员 / 裸指针）
    static IUnknown* ComUnknownPtr(const Value& v)
    {
        if (v.t == Value::Pointer && v.ptr)
            return reinterpret_cast<IUnknown*>(v.ptr);
        if (v.t == Value::TableT && v.tab)
        {
            auto io = v.tab->map.find(L"_object");
            if (io != v.tab->map.end() && io->second.t == Value::Pointer && io->second.ptr)
                return reinterpret_cast<IUnknown*>(io->second.ptr);
            auto ib = v.tab->map.find(L"_boundDispatch");
            if (ib != v.tab->map.end() && ib->second.t == Value::Pointer && ib->second.ptr)
                return reinterpret_cast<IUnknown*>(ib->second.ptr);
            // 原始接口接收器（com.ImplInterface 的结果）
            auto ir = v.tab->map.find(L"_rawSink");
            if (ir != v.tab->map.end() && ir->second.t == Value::Pointer && ir->second.ptr)
                return reinterpret_cast<IUnknown*>(ir->second.ptr);
        }
        return nullptr;
    }

    // 事件接收器：把 COM 事件（源接口的 DISPID）转发到 aardio 事件表的同名成员
    class NodioEventSink : public IDispatch
    {
        LONG m_ref{ 1 };
        Value m_sink;                // aardio 事件监听表
        ITypeInfo* m_srcTI{ nullptr };// 源（事件）接口类型信息

    public:
        NodioEventSink(const Value& sink, ITypeInfo* srcTI) : m_sink(sink)
        {
            m_srcTI = srcTI;
            if (m_srcTI) m_srcTI->AddRef();
        }
        ~NodioEventSink()
        {
            if (m_srcTI) m_srcTI->Release();
        }

        // DISPID -> 事件名（用源接口的类型信息反查）
        wstring DispIdName(DISPID id)
        {
            if (!m_srcTI || id == DISPID_UNKNOWN) return wstring();
            TYPEATTR* pAttr = nullptr;
            if (FAILED(m_srcTI->GetTypeAttr(&pAttr)) || !pAttr) return wstring();
            wstring name;
            for (int i = 0; i < pAttr->cFuncs; i++)
            {
                FUNCDESC* fd = nullptr;
                if (FAILED(m_srcTI->GetFuncDesc(i, &fd)) || !fd) continue;
                if (fd->memid == id)
                {
                    BSTR bName = nullptr;
                    if (SUCCEEDED(m_srcTI->GetDocumentation(fd->memid, &bName,
                                                           nullptr, nullptr, nullptr)) &&
                        bName)
                        name = bName;
                    if (bName) SysFreeString(bName);
                }
                m_srcTI->ReleaseFuncDesc(fd);
                if (!name.empty()) break;
            }
            m_srcTI->ReleaseTypeAttr(pAttr);
            return name;
        }

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
        STDMETHODIMP GetIDsOfNames(REFIID, LPOLESTR* rgszNames, UINT cNames,
                                  LCID, DISPID* prgdispids) override
        {
            if (!rgszNames || !prgdispids || cNames == 0) return E_INVALIDARG;
            for (UINT i = 0; i < cNames; i++)
            {
                DISPID id = DISPID_UNKNOWN;
                if (m_srcTI)
                {
                    LPOLESTR nm = rgszNames[i];
                    LPOLESTR* pnm = &nm;
                    MEMBERID* pmid = &id;
                    if (FAILED(m_srcTI->GetIDsOfNames(pnm, 1, pmid)))
                        id = DISPID_UNKNOWN;
                }
                if (id == DISPID_UNKNOWN) return DISP_E_UNKNOWNNAME;
                prgdispids[i] = id;
            }
            return S_OK;
        }
        STDMETHODIMP Invoke(DISPID dispIdMember, REFIID, LCID, WORD wFlags,
                            DISPPARAMS* pDispParams, VARIANT* pVarResult,
                            EXCEPINFO*, UINT*) override
        {
            Interp* it = g_interpCurrent;
            if (!it) return E_FAIL;
            wstring name = DispIdName(dispIdMember);
            if (name.empty()) return S_OK;
            // 事件名 -> aardio 事件表成员（走 _meta._get，官方 ConnectWeak 的
            // 接收表就是用 _get 转发的）
            Value memberVal = it->GetMember(m_sink, Value::Str_(name), false, 1);
            if (memberVal.t != Value::Func && !memberVal.native)
                return S_OK;   // 没挂处理函数的事件：静默忽略
            // 零参 + PROPERTYGET 是 Nodio 读取成员时的「探测」调用
            // （comMeta._get 会先探一次再决定是否返回可调用的绑定成员）：
            // 这种调用不能触发事件，只把事件处理函数本身交回去。
            bool bZeroArg = (pDispParams == nullptr || pDispParams->cArgs == 0);
            bool bProbe = bZeroArg && (wFlags & DISPATCH_PROPERTYGET);
            if (bProbe)
            {
                if (pVarResult)
                {
                    VariantInit(pVarResult);
                    pVarResult->vt = VT_DISPATCH;
                    pVarResult->pdispVal = new NodioCallbackDispatch(memberVal);
                }
                return S_OK;
            }
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
            Value ret = it->CallOwner(memberVal, m_sink, args);
            if (pVarResult)
            {
                wstring err;
                VariantFromValue(ret, *pVarResult, &err);
            }
            return S_OK;
        }
    };

    // ---------- 原始（非 IDispatch）接口事件接收器 ----------
    //
    // 官方契约（D:\aardio\lib\builtin\com.aardio:205-207）：
    //   com.ImplInterface(implTable, progID, interfaceName)
    //     例：com.ImplInterface(ocxEvents2,"WMPlayer.OCX.7","_WMPOCXEvents")
    //   com.ImplInterfaceFromTypelib(implTable, typelibPath, interfaceName, coclassName)
    //     用类型库实现 com 接口
    // 两者的结果都是一个「实现了指定接口」的 COM 对象，交给 com.AddConnection
    // 挂到目标对象的连接点上（官方 com.md / event.md 的用法）。
    //
    // 实现方式：按 ITypeInfo 动态生成 vtable。
    //   槽位顺序 = IUnknown 的 3 个方法 + 继承接口的方法 + 本接口的方法；
    //   本接口内部按 GetFuncDesc 的下标顺序 —— 这正是官方
    //   com.interface.loadTypelibrary 生成接口声明类时用的顺序
    //   （D:\aardio\lib\com\interface\_.aardio:143-168）。
    //   每个槽位一个 stdcall 跳板（engine/ffi.cpp 的回调机制），
    //   参数按 ELEMDESC 转换（BSTR / VARIANT / 接口指针…），
    //   本接口的方法按名字转发到 aardio 事件表的同名成员。

    // vtable 里的一个方法
    struct ComMethodInfo
    {
        wstring name;
        std::vector<int> params;   // 回调跳板的参数类型码
        int kind = 0;              // 0=本接口 1=IUnknown 2=IDispatch 3=其它继承接口
    };

    // 接收器的共享状态：事件表 + 每个方法的跳板
    struct RawSinkState
    {
        Value impl;                              // aardio 事件表
        IID  iid{};                              // 目标接口 IID
        ITypeInfo* ti{ nullptr };                 // 接口类型信息（DISPID -> 事件名）
        std::vector<std::shared_ptr<CbCtx>> cbs; // 方法跳板
        bool hasDispatch = false;                // vtable 里是否含 IDispatch 的四个槽位
        ~RawSinkState()
        {
            if (ti) ti->Release();
        }
    };

    // DISPID -> 接口成员名（用源接口的类型信息反查）
    static wstring ComDispIdToName(ITypeInfo* pTI, DISPID id)
    {
        if (!pTI || id == DISPID_UNKNOWN) return wstring();
        TYPEATTR* attr = nullptr;
        if (FAILED(pTI->GetTypeAttr(&attr)) || !attr) return wstring();
        wstring name;
        for (int i = 0; i < attr->cFuncs && name.empty(); i++)
        {
            FUNCDESC* fd = nullptr;
            if (FAILED(pTI->GetFuncDesc(i, &fd)) || !fd) continue;
            if (fd->memid == id)
            {
                BSTR bn = nullptr;
                if (SUCCEEDED(pTI->GetDocumentation(fd->memid, &bn, nullptr,
                                                  nullptr, nullptr)) && bn)
                    name = bn;
                if (bn) SysFreeString(bn);
            }
            pTI->ReleaseFuncDesc(fd);
        }
        pTI->ReleaseTypeAttr(attr);
        return name;
    }

    // COM 要求 vtable 指针位于对象首地址
    struct RawSinkObj
    {
        void** vtbl = nullptr;
        LONG   ref = 1;
        std::shared_ptr<RawSinkState> st;
        ~RawSinkObj()
        {
            for (auto& ctx : st->cbs)
                if (ctx) ReleaseCallback(ctx.get());
            st->cbs.clear();
            delete[] vtbl;
        }
    };

    // 引用计数归零的接收器延迟销毁：Release 跳板正在执行时不能销毁自己
    // （销毁会把跳板所在的页重写 neutralizing 掉）。
    static std::vector<RawSinkObj*> g_rawSinkPending;

    static void ComReapRawSinks()
    {
        if (g_rawSinkPending.empty()) return;
        std::vector<RawSinkObj*> dead;
        dead.swap(g_rawSinkPending);
        for (auto* p : dead) delete p;
    }

    // 用户定义类型（hreftype）：接口类型 -> 接口指针码，其它 -> 原始指针
    static int ComUserTypeCode(ITypeInfo* pTI, HREFTYPE hr);

    // 参数类型码：ELEMDESC -> 回调跳板的参数类型
    //
    // 关键区分：VT_PTR|VT_USERDEFINED 既可能是**接口指针**（要转成 COM 对象），
    // 也可能只是指向一个结构体（例如 REFIID 就是一个 GUID 结构体的指针）。
    // 靠类型信息的 typekind 判定：TKIND_INTERFACE / TKIND_DISPATCH 才是接口。
    static int ComParamCodeOf(ITypeInfo* pTI, const ELEMDESC* ed)
    {
        if (!ed) return 5;
        VARTYPE vt = ed->tdesc.vt;
        if (vt == VT_PTR)
        {
            // 指针参数：看被指向的类型
            const TYPEDESC* pt = ed->tdesc.lptdesc;
            VARTYPE bvt = pt ? pt->vt : VT_EMPTY;
            if (bvt == VT_USERDEFINED)
                return ComUserTypeCode(pTI, pt ? pt->hreftype : 0);
            switch (bvt)
            {
            case VT_BSTR:        return kComBstr;
            case VT_VARIANT:     return kComVariant;
            case VT_DISPATCH:    return kComDispatch;
            case VT_UNKNOWN:     return kComUnknown;
            default:             return 5;           // 其它指针按原始指针传
            }
        }
        if (vt == VT_USERDEFINED)
            return ComUserTypeCode(pTI, ed->tdesc.hreftype);
        switch (vt)
        {
        case VT_BSTR:     return kComBstr;
        case VT_VARIANT:  return kComVariant;
        case VT_DISPATCH: return kComDispatch;
        case VT_UNKNOWN:  return kComUnknown;
        case VT_I8: case VT_UI8: return 1;           // int64（占 8 字节）
        case VT_R4:       return 9;                  // float
        case VT_R8:
#if defined(_M_IX86)
            return 1;                               // x86 上双精度占 8 字节（栈平衡）
#else
            return 10;
#endif
        case VT_BOOL:     return 4;
        case VT_CY: case VT_DECIMAL: case VT_DATE: return 10;
        default:          return 0;                  // 整数（含 VT_CLSID 等）
        }
    }

    // 用户定义类型（hreftype）：接口类型 -> 接口指针码，其它 -> 原始指针
    static int ComUserTypeCode(ITypeInfo* pTI, HREFTYPE hr)
    {
        if (!pTI || !hr) return 5;
        ITypeInfo* pt = nullptr;
        if (FAILED(pTI->GetRefTypeInfo(hr, &pt)) || !pt) return 5;
        int code = 5;
        TYPEATTR* a = nullptr;
        if (SUCCEEDED(pt->GetTypeAttr(&a)) && a)
        {
            if (a->typekind == TKIND_INTERFACE || a->typekind == TKIND_DISPATCH)
                code = kComIface;
            pt->ReleaseTypeAttr(a);
        }
        pt->Release();
        return code;
    }

    // 收集接口（含继承）的全部方法，顺序即 vtable 顺序
    //
    // bIncludeDispatchBase：基接口里如果继承了 IDispatch，是否把 IDispatch 的
    // 四个方法也放进 vtable。
    //   原始接口（TKIND_INTERFACE）按 COM 规则：继承 IDispatch 的接口，
    //     vtable = IUnknown(3) + IDispatch(4) + 自己的方法。
    //   dispinterface（TKIND_DISPATCH）被控件当作**原始接口**实现时
    //     （典型：Windows Media Player 的 _WMPOCXEvents），
    //     vtable = IUnknown(3) + 声明的成员方法，没有 IDispatch 的四个槽位
    //     —— 这一点是实测出来的：WMP 的服务器按接口 IID 查 sink，
    //     再直接调用 vtable 槽位。
    static void ComCollectMethods(ITypeInfo* pTI, std::vector<ComMethodInfo>& out,
                                  int kind, bool bIncludeDispatchBase,
                                  int depth = 0)
    {
        if (!pTI || depth > 8) return;
        TYPEATTR* attr = nullptr;
        if (FAILED(pTI->GetTypeAttr(&attr)) || !attr) return;
        for (int i = 0; i < attr->cImplTypes; i++)
        {
            HREFTYPE hr = 0;
            if (SUCCEEDED(pTI->GetRefTypeOfImplType(i, &hr)))
            {
                ITypeInfo* base = nullptr;
                if (SUCCEEDED(pTI->GetRefTypeInfo(hr, &base)) && base)
                {
                    int bkind = 3;
                    TYPEATTR* ba = nullptr;
                    if (SUCCEEDED(base->GetTypeAttr(&ba)) && ba)
                    {
                        if (IsEqualGUID(ba->guid, IID_IUnknown)) bkind = 1;
                        else if (IsEqualGUID(ba->guid, IID_IDispatch)) bkind = 2;
                        base->ReleaseTypeAttr(ba);
                    }
                    if (bkind != 2 || bIncludeDispatchBase)
                        ComCollectMethods(base, out, bkind, bIncludeDispatchBase, depth + 1);
                    base->Release();
                }
            }
        }
        for (int i = 0; i < attr->cFuncs; i++)
        {
            FUNCDESC* fd = nullptr;
            if (FAILED(pTI->GetFuncDesc(i, &fd)) || !fd) continue;
            ComMethodInfo mi;
            mi.kind = kind;
            BSTR bn = nullptr;
            if (SUCCEEDED(pTI->GetDocumentation(fd->memid, &bn, nullptr, nullptr, nullptr)) && bn)
                mi.name = bn;
            if (bn) SysFreeString(bn);
            for (int k = 0; k < fd->cParams; k++)
                mi.params.push_back(ComParamCodeOf(pTI, &fd->lprgelemdescParam[k]));
            out.push_back(mi);
            pTI->ReleaseFuncDesc(fd);
        }
        pTI->ReleaseTypeAttr(attr);
    }

    // vtable 的前 3 槽必须是 IUnknown 的 QI/AddRef/Release
    static void ComEnsureIUnknownSlots(std::vector<ComMethodInfo>& v)
    {
        if (!v.empty() && v[0].kind == 1) return;
        ComMethodInfo qi; qi.name = L"QueryInterface"; qi.kind = 1;
        qi.params = { 5, 5 };
        ComMethodInfo ar; ar.name = L"AddRef"; ar.kind = 1;
        ComMethodInfo rl; rl.name = L"Release"; rl.kind = 1;
        v.insert(v.begin(), { qi, ar, rl });
    }

    // dispinterface（TKIND_DISPATCH）必须补齐 IDispatch 的四个槽位
    //
    // 实测：同一个接口在不同的类型库副本里 cImplTypes 可能不同 ——
    // 有的副本登记了 IDispatch 基接口（vtable 自带四个槽），有的副本
    // cImplTypes 为空（dispinterface 的成员本来就不占 vtable 槽）。
    // 缺了这四个槽，服务器按 IDispatch 派发事件时命中的就是自身方法槽
    // —— 实测会崩。所以这里无条件补齐。
    static void ComEnsureDispatchSlots(std::vector<ComMethodInfo>& v)
    {
        for (const auto& m : v)
            if (m.kind == 2) return;           // 已经有了
        ComMethodInfo gtc; gtc.name = L"GetTypeInfoCount"; gtc.kind = 2;
        ComMethodInfo gti; gti.name = L"GetTypeInfo"; gti.kind = 2;
        ComMethodInfo gion; gion.name = L"GetIDsOfNames"; gion.kind = 2;
        ComMethodInfo inv; inv.name = L"Invoke"; inv.kind = 2;
        // 插到 IUnknown 三槽之后
        size_t pos = 3;
        if (v.size() < pos) pos = v.size();
        v.insert(v.begin() + pos, { gtc, gti, gion, inv });
    }

    // IDispatch 四个方法的参数类型码（**硬编码**，不能用类型信息推导）
    //
    // 实测：服务器把我们的原始接口接收器当成 IDispatch 使用，
    // 直接调用 vtable 里 IDispatch 的四个槽位（Invoke 在 slot 6）。
    // IDispatch::Invoke 的 pVarResult 是 VARIANT* **出参**，
    // 按类型信息推导会被误判成 kComVariant（按值读 VARIANT），
    // 服务器传进来的是未初始化的 VARIANT 指针 —— 读它会拿到
    // 垃圾 vt（例如 VT_DISPATCH + 垃圾指针）再 AddRef，直接崩溃。
    // 所以这里按 IDispatch 的真实签名写死。
    static std::vector<int> ComDispatchParams(const wstring& name)
    {
        if (name == L"GetTypeInfoCount") return { 5 };                              // UINT*
        if (name == L"GetTypeInfo")      return { 0, 0, 5 };                        // UINT,LCID,ITypeInfo**
        if (name == L"GetIDsOfNames")    return { 5, 5, 0, 0, 5 };                  // REFIID,LPOLESTR*,UINT,LCID,DISPID*
        if (name == L"Invoke")           return { 0, 5, 0, 0, 5, 5, 5, 5 };        // DISPID,REFIID,LCID,WORD,DISPARAMS*,VARIANT*,EXCEPINFO*,UINT*
        return {};
    }

    // VARIANT 参数 -> aardio 值（处理 Automation 常见的按引用嵌套）
    static Value ComVariantArgToValue(const VARIANT& v)
    {
        // VT_VARIANT / VT_VARIANT|VT_BYREF 里存的是指向另一个 VARIANT 的指针
        if ((v.vt == VT_VARIANT || v.vt == (VT_VARIANT | VT_BYREF)) && v.pvarVal)
            return ComVariantArgToValue(*v.pvarVal);
        wstring err;
        return VariantToValue(v, &err);
    }

    // 生成一个方法的 stdcall 跳板
    static void* ComMakeStub(const std::shared_ptr<RawSinkState>& st,
                             const ComMethodInfo& mi, Interp* it)
    {
        auto spCtx = std::make_shared<CbCtx>();
        CbCtx* ctx = spCtx.get();
        ctx->sig.ret = ARInt;          // COM 方法一律按 HRESULT 返回
        ctx->sig.params.push_back(5);   // this
        if (mi.kind == 2)
        {
            // IDispatch 四个方法：参数码硬编码（见 ComDispatchParams 的说明）
            for (int c : ComDispatchParams(mi.name)) ctx->sig.params.push_back(c);
        }
        else
        {
            for (int c : mi.params) ctx->sig.params.push_back(c);
        }
        ctx->interp = it;
        ctx->owner = st->impl;         // 事件表由接收器持有
        ctx->rooted = true;            // 事件表不能被回收（GC 标记 ctx->owner）

        const wstring name = mi.name;
        if (mi.kind == 1 && name == L"QueryInterface")
        {
            ctx->fn = NF([st](vector<Value>& a) -> Value {
                ComRawTrace(L"[comraw] QI argc=%d\n", (int)a.size());
                if (a.size() < 3) return Value::Num_((double)(long long)E_POINTER);
                void* self = (a[0].t == Value::Pointer) ? a[0].ptr : nullptr;
                void* riidp = (a[1].t == Value::Pointer) ? a[1].ptr : nullptr;
                void** ppv = (a[2].t == Value::Pointer) ? (void**)a[2].ptr : nullptr;
                if (!self || !ppv) return Value::Num_((double)(long long)E_POINTER);
                IID riid{};
                if (riidp) memcpy(&riid, riidp, sizeof(IID));
                ComRawTrace(L"[comraw] QI self=%p riid=%08X target=%08X\n",
                            self, (unsigned long)riid.Data1,
                            (unsigned long)st->iid.Data1);
                // vtable 布局是 IUnknown(3) + IDispatch(4) + 自身方法时，
                // IDispatch 指针与 IUnknown 指针相同（IDispatch 继承 IUnknown，
                // 派生接口的第一个接口共用同一个 vtable 头）。
                if (IsEqualGUID(riid, IID_IUnknown) ||
                    (st->hasDispatch && IsEqualGUID(riid, IID_IDispatch)) ||
                    IsEqualGUID(riid, st->iid))
                {
                    *ppv = self;
                    InterlockedIncrement(&((RawSinkObj*)self)->ref);
                    return Value::Num_(0);          // S_OK
                }
                *ppv = nullptr;
                return Value::Num_((double)(long long)E_NOINTERFACE);
            });
        }
        else if (mi.kind == 1 && (name == L"AddRef" || name == L"Release"))
        {
            bool bAdd = (name == L"AddRef");
            ctx->fn = NF([bAdd](vector<Value>& a) -> Value {
                ComRawTrace(L"[comraw] %ls self=%p\n", bAdd ? L"AddRef" : L"Release",
                            a.empty() ? nullptr : (void*)(a[0].t == Value::Pointer ? a[0].ptr : nullptr));
                void* self = a.empty() ? nullptr
                    : ((a[0].t == Value::Pointer) ? a[0].ptr : nullptr);
                if (!self) return Value::Num_(0);
                auto* obj = (RawSinkObj*)self;
                if (bAdd) return Value::Num_((double)InterlockedIncrement(&obj->ref));
                LONG r = InterlockedDecrement(&obj->ref);
                if (r == 0)
                {
                    // 不能就地销毁：当前正在执行的就是这个跳板
                    g_rawSinkPending.push_back(obj);
                }
                return Value::Num_((double)(unsigned long)r);
            });
        }
        else if (mi.kind == 2)
        {
            // IDispatch 的四个方法：真实实现（服务器会用 Invoke 派发事件）
            const wstring dname = name;
            if (dname == L"GetTypeInfoCount")
            {
                ctx->fn = NF([](vector<Value>& a) -> Value {
                    if (a.size() > 1 && a[1].t == Value::Pointer && a[1].ptr)
                        *(UINT*)a[1].ptr = 1;
                    return Value::Num_(0);
                });
            }
            else if (dname == L"GetTypeInfo")
            {
                ctx->fn = NF([st](vector<Value>& a) -> Value {
                    if (a.size() > 3 && a[3].t == Value::Pointer && a[3].ptr)
                    {
                        ITypeInfo** pp = (ITypeInfo**)a[3].ptr;
                        if (st->ti) { st->ti->AddRef(); *pp = st->ti; return Value::Num_(0); }
                        *pp = nullptr;
                    }
                    return Value::Num_(0);
                });
            }
            else if (dname == L"GetIDsOfNames")
            {
                ctx->fn = NF([st](vector<Value>& a) -> Value {
                    if (a.size() < 6) return Value::Num_((double)(long long)E_INVALIDARG);
                    LPOLESTR* names = (a[2].t == Value::Pointer) ? (LPOLESTR*)a[2].ptr : nullptr;
                    UINT cNames = (UINT)(long long)a[3].n;
                    DISPID* out = (a[5].t == Value::Pointer) ? (DISPID*)a[5].ptr : nullptr;
                    if (!names || !out || cNames == 0)
                        return Value::Num_((double)(long long)E_INVALIDARG);
                    if (!st->ti) return Value::Num_((double)(long long)DISP_E_UNKNOWNNAME);
                    if (FAILED(st->ti->GetIDsOfNames(names, cNames, out)))
                        return Value::Num_((double)(long long)DISP_E_UNKNOWNNAME);
                    return Value::Num_(0);
                });
            }
            else
            {
                // Invoke(DISPID, REFIID, LCID, WORD, DISPPARAMS*, VARIANT*, EXCEPINFO*, UINT*)
                //
                // 服务器把我们的原始接口接收器当作 IDispatch 使用，
                // 用 Invoke 派发事件（实测 Windows Media Player 的
                // _WMPOCXEvents 走的就是这条路）。
                // DISPID -> 事件名：用**源接口的类型信息**反查
                // （dispinterface 的 FUNCDESC.memid 就是 DISPID）。
                ctx->fn = NF([st, it](vector<Value>& a) -> Value {
                    ComRawTrace(L"[comraw] Invoke argc=%d\n", (int)a.size());
                    if (!it || a.size() < 9) return Value::Num_((double)(long long)E_FAIL);
                    // DISPID 是 32 位整数：x64 下整型参数按 64 位读取，
                    // 高位可能是符号扩展的垃圾值 —— 取低 32 位。
                    DISPID dispid = (DISPID)(int32_t)(long long)a[1].n;
                    void* pDispParams = (a[5].t == Value::Pointer) ? a[5].ptr : nullptr;
                    void* pVarResult  = (a[6].t == Value::Pointer) ? a[6].ptr : nullptr;
                    wstring name = ComDispIdToName(st->ti, dispid);
                    ComRawTrace(L"[comraw] Invoke dispid=%d name=%ls\n",
                                (int)dispid, name.c_str());
                    if (name.empty()) return Value::Num_(0);   // 未知成员：静默忽略
                    Value fn = it->GetMember(st->impl, Value::Str_(name), false, 1);
                    if (fn.t != Value::Func && !fn.native)
                        return Value::Num_(0);                  // 未定义成员：忽略
                    std::vector<Value> args;
                    if (pDispParams)
                    {
                        DISPPARAMS* dp = (DISPPARAMS*)pDispParams;
                        if (dp->cArgs > 0 && dp->rgvarg)
                        {
                            args.resize(dp->cArgs);
                            for (UINT i = 0; i < dp->cArgs; i++)
                            {
                                // rgvarg 是逆序存放的
                                args[i] = ComVariantArgToValue(dp->rgvarg[dp->cArgs - 1 - i]);
                            }
                        }
                    }
                    Value ret = it->CallOwner(fn, st->impl, args);
                    if (pVarResult)
                    {
                        // 事件处理函数通常没有返回值：留 VT_EMPTY，
                        // 不要写 DISP_E_PARAMNOTFOUND（服务器会当成失败）。
                        VARIANT* vr = (VARIANT*)pVarResult;
                        VariantInit(vr);
                        if (ret.t != Value::Null)
                        {
                            wstring err;
                            VariantFromValue(ret, *vr, &err);
                        }
                    }
                    return Value::Num_(0);
                });
            }
        }
        else if (mi.kind == 3)
        {
            // 其它继承接口的方法：不实现（栈清理仍按真实参数个数）
            ctx->fn = NF([](vector<Value>&) -> Value {
                return Value::Num_((double)(long long)E_NOTIMPL);
            });
        }
        else
        {
            // 本接口的方法：按名字转发到 aardio 事件表的同名成员
            ctx->fn = NF([st, name, it](vector<Value>& a) -> Value {
                ComRawTrace(L"[comraw] method %ls argc=%d\n", name.c_str(), (int)a.size());
                if (!it) return Value::Num_((double)(long long)E_FAIL);
                Value fn = it->GetMember(st->impl, Value::Str_(name), false, 1);
                if (fn.t != Value::Func && !fn.native)
                    return Value::Num_((double)(long long)E_NOTIMPL);
                std::vector<Value> args;
                for (size_t i = 1; i < a.size(); i++) args.push_back(a[i]);
                Value ret = it->CallOwner(fn, st->impl, args);
                if (ret.t == Value::Num) return ret;
                return Value::Num_(0);      // S_OK
            });
        }
        CallbackTrampoline(ctx);
        if (!ctx->code) return nullptr;
        ComRawTrace(L"[comraw] stub %ls kind=%d code=%p interp=%p\n",
                    mi.name.c_str(), mi.kind, ctx->code, (void*)ctx->interp);
        g_cbs.push_back(ctx);
        st->cbs.push_back(spCtx);
        return ctx->code;
    }

    // 按接口类型信息生成一个实现了该接口的 COM 接收器
    static RawSinkObj* BuildRawSink(ITypeInfo* pTI, const Value& impl, wstring* pErr)
    {
        if (!pTI) { if (pErr) *pErr = L"无效的接口类型信息"; return nullptr; }
        TYPEATTR* attr = nullptr;
        if (FAILED(pTI->GetTypeAttr(&attr)) || !attr)
        {
            if (pErr) *pErr = L"读取接口属性失败";
            return nullptr;
        }
        IID iid = attr->guid;
        TYPEKIND kind = attr->typekind;
        pTI->ReleaseTypeAttr(attr);

        std::vector<ComMethodInfo> methods;
        // vtable = IUnknown(3) + 继承的接口（含 IDispatch 的四个方法）+ 自己的方法
        // —— 实测：WMP 的 _WMPOCXEvents 在类型库里登记为 dispinterface，
        //    服务器按接口 IID 查 sink 后直接调用 vtable 槽位，
        //    槽位里必须包含 IDispatch 的四个方法（少了就崩）。
        ComCollectMethods(pTI, methods, 0, true);
        ComEnsureIUnknownSlots(methods);
        // dispinterface 必须带 IDispatch 的四个槽（服务器用 Invoke 派发事件）
        if (kind == TKIND_DISPATCH) ComEnsureDispatchSlots(methods);
        if (methods.size() <= 3)
        {
            if (pErr) *pErr = L"该接口没有可实现的方法";
            return nullptr;
        }

        auto st = std::make_shared<RawSinkState>();
        st->impl = impl;
        st->iid = iid;
        st->ti = pTI;
        pTI->AddRef();              // 源接口类型信息（DISPID -> 事件名）
        for (const auto& m : methods)
            if (m.kind == 2) { st->hasDispatch = true; break; }

        auto* obj = new RawSinkObj();
        obj->st = st;
        obj->vtbl = new void*[methods.size()];
        memset(obj->vtbl, 0, sizeof(void*) * methods.size());
        Interp* it = g_interpCurrent;
        for (size_t i = 0; i < methods.size(); i++)
        {
            void* stub = ComMakeStub(st, methods[i], it);
            if (!stub)
            {
                if (pErr) *pErr = L"生成方法跳板失败";
                delete obj;
                return nullptr;
            }
            obj->vtbl[i] = stub;
        }
        return obj;
    }

    // 释放接收器的一次引用
    static void ComReleaseRawSink(RawSinkObj* obj)
    {
        if (!obj) return;
        if (InterlockedDecrement(&obj->ref) == 0)
            g_rawSinkPending.push_back(obj);
    }

    // 把接收器包装成 aardio 值（可交给 com.AddConnection）
    static Value WrapRawSink(RawSinkObj* obj)
    {
        auto w = std::make_shared<Table>();
        w->map[L"_rawSink"] = Value::Pointer_((void*)obj);
        w->map[L"_isComObject"] = Value::Bool_(true);
        w->map[L"_rawSinkIid"] = Value::Str_(ComGuidString(obj->st->iid));
        // 包装表持有最后一个引用：回收时释放
        w->map[L"_gc"] = NF([obj](vector<Value>&) -> Value {
            ComReleaseRawSink(obj);
            return Value();
        });
        return Value::Table_(w);
    }

    static bool IsRawSinkValue(const Value& v)
    {
        return v.t == Value::TableT && v.tab &&
               v.tab->map.count(L"_rawSink") > 0;
    }

    static IID RawSinkIid(const Value& v)
    {
        IID iid{};
        if (v.t == Value::TableT && v.tab)
        {
            auto it = v.tab->map.find(L"_rawSinkIid");
            if (it != v.tab->map.end() && it->second.t == Value::Str)
                IIDFromString(BStr(it->second).c_str(), &iid);
        }
        return iid;
    }

    // 找原始（非 IDispatch）事件连接点：返回连接点与源接口的类型信息
    //
    // IConnectionPoint 只暴露**源接口** IID（GetConnectionInterface），
    // 源接口的类型信息从组件的类型库里找（IProvideClassInfo ->
    // GetClassInfo -> GetContainingTypeLib）——这与 dispinterface 路径同源。
    static IConnectionPoint* FindRawConnectionPoint(IUnknown* pUnk, ITypeInfo** ppSrcTI)
    {
        *ppSrcTI = nullptr;
        IConnectionPointContainer* pCpc = nullptr;
        if (!pUnk || FAILED(pUnk->QueryInterface(IID_IConnectionPointContainer,
                                                 (void**)&pCpc)) || !pCpc)
            return nullptr;
        IConnectionPoint* pFound = nullptr;
        IEnumConnectionPoints* pEnum = nullptr;
        if (SUCCEEDED(pCpc->EnumConnectionPoints(&pEnum)) && pEnum)
        {
            IConnectionPoint* pTmp = nullptr;
            ULONG fetched = 0;
            while (pEnum->Next(1, &pTmp, &fetched) == S_OK && pTmp)
            {
                IID iid{};
                if (SUCCEEDED(pTmp->GetConnectionInterface(&iid)))
                {
                    ITypeInfo* ti = FindSourceTypeInfo(pUnk, iid, true);
                    if (ti)
                    {
                        TYPEATTR* attr = nullptr;
                        bool bRaw = false;
                        if (SUCCEEDED(ti->GetTypeAttr(&attr)) && attr)
                        {
                            bRaw = (attr->typekind == TKIND_INTERFACE);
                            ti->ReleaseTypeAttr(attr);
                        }
                        if (bRaw)
                        {
                            *ppSrcTI = ti;
                            pFound = pTmp;
                            break;
                        }
                        ti->Release();
                    }
                }
                pTmp->Release();
            }
            pEnum->Release();
        }
        pCpc->Release();
        return pFound;
    }

    // 在类型库里按名字找接口（可选先在指定 coclass 的嵌套接口里找）
    static ITypeInfo* ComFindInterfaceInLib(ITypeLib* pTL, const wstring& name,
                                            const wstring& coclassName, TYPEKIND* pKind)
    {
        if (!pTL) return nullptr;
        auto typeName = [](ITypeInfo* ti) -> wstring {
            BSTR bn = nullptr;
            wstring n;
            if (SUCCEEDED(ti->GetDocumentation(-1, &bn, nullptr, nullptr, nullptr)) && bn)
                n = bn;
            if (bn) SysFreeString(bn);
            return n;
        };
        // 1) 指定了 coclass：在它的实现接口（嵌套类型）里找
        if (!coclassName.empty())
        {
            UINT n = pTL->GetTypeInfoCount();
            for (UINT i = 0; i < n; i++)
            {
                ITypeInfo* ti = nullptr;
                if (FAILED(pTL->GetTypeInfo(i, &ti)) || !ti) continue;
                TYPEATTR* attr = nullptr;
                bool isCoc = false;
                if (SUCCEEDED(ti->GetTypeAttr(&attr)) && attr)
                {
                    isCoc = (attr->typekind == TKIND_COCLASS);
                    ti->ReleaseTypeAttr(attr);
                }
                if (!isCoc || typeName(ti) != coclassName) { ti->Release(); continue; }
                for (int k = 0; ; k++)
                {
                    HREFTYPE hr = 0;
                    if (FAILED(ti->GetRefTypeOfImplType(k, &hr))) break;
                    ITypeInfo* nested = nullptr;
                    if (SUCCEEDED(ti->GetRefTypeInfo(hr, &nested)) && nested)
                    {
                        if (typeName(nested) == name)
                        {
                            ti->Release();
                            if (pKind)
                            {
                                TYPEATTR* na = nullptr;
                                *pKind = TKIND_INTERFACE;
                                if (SUCCEEDED(nested->GetTypeAttr(&na)) && na)
                                {
                                    *pKind = na->typekind;
                                    nested->ReleaseTypeAttr(na);
                                }
                            }
                            return nested;
                        }
                        nested->Release();
                    }
                }
                ti->Release();
            }
        }
        // 2) 顶层类型
        UINT n = pTL->GetTypeInfoCount();
        for (UINT i = 0; i < n; i++)
        {
            ITypeInfo* ti = nullptr;
            if (FAILED(pTL->GetTypeInfo(i, &ti)) || !ti) continue;
            if (typeName(ti) == name)
            {
                if (pKind)
                {
                    TYPEATTR* attr = nullptr;
                    *pKind = TKIND_INTERFACE;
                    if (SUCCEEDED(ti->GetTypeAttr(&attr)) && attr)
                    {
                        *pKind = attr->typekind;
                        ti->ReleaseTypeAttr(attr);
                    }
                }
                return ti;
            }
            ti->Release();
        }
        return nullptr;
    }

    // ProgID / CLSID -> 类型库
    static ITypeLib* ComLoadTypeLibForProgId(const wstring& progId)
    {
        if (progId.empty()) return nullptr;
        CLSID clsid{};
        bool bHasCls = false;
        if (SUCCEEDED(CLSIDFromString(progId.c_str(), &clsid))) bHasCls = true;
        else if (SUCCEEDED(CLSIDFromProgID(progId.c_str(), &clsid))) bHasCls = true;
        if (!bHasCls) return nullptr;
        wstring clsidStr = ComGuidString(clsid);
        // 1) 注册表 CLSID\{...}\TypeLib -> TypeLib\{guid}\<版本>
        HKEY hk = nullptr;
        wstring tlKey = L"CLSID\\" + clsidStr + L"\\TypeLib";
        if (RegOpenKeyExW(HKEY_CLASSES_ROOT, tlKey.c_str(), 0, KEY_READ, &hk) == ERROR_SUCCESS)
        {
            wchar_t buf[64] = {};
            DWORD cb = sizeof(buf);
            DWORD type = 0;
            LONG rc = RegQueryValueExW(hk, nullptr, nullptr, &type,
                                       (LPBYTE)buf, &cb);
            RegCloseKey(hk);
            if (rc == ERROR_SUCCESS && type == REG_SZ)
            {
                IID tlid{};
                if (SUCCEEDED(IIDFromString(buf, &tlid)))
                {
                    // 版本号在子键名里（1.0 / 2.0 …），也可能写在 Version 值里
                    wstring vKey = L"TypeLib\\" + wstring(buf);
                    HKEY hv = nullptr;
                    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, vKey.c_str(), 0,
                                      KEY_READ, &hv) == ERROR_SUCCESS)
                    {
                        for (DWORD idx = 0; ; idx++)
                        {
                            wchar_t sub[64] = {};
                            DWORD scb = sizeof(sub) * sizeof(wchar_t);
                            LONG nr = RegEnumKeyExW(hv, idx, sub, &scb,
                                                   nullptr, nullptr, nullptr, nullptr);
                            if (nr != ERROR_SUCCESS) break;
                            int maj = 0, min = 0;
                            if (swscanf(sub, L"%d.%d", &maj, &min) != 2) continue;
                            ITypeLib* pTL = nullptr;
                            if (SUCCEEDED(LoadRegTypeLib(tlid, (WORD)maj, (WORD)min,
                                                         LOCALE_USER_DEFAULT, &pTL)))
                            {
                                RegCloseKey(hv);
                                return pTL;
                            }
                        }
                        // 没有版本子键：退回 Version 值
                        wchar_t ver[32] = {};
                        DWORD vt = 0, vc = sizeof(ver);
                        if (RegQueryValueExW(hv, L"Version", nullptr, &vt,
                                             (LPBYTE)ver, &vc) == ERROR_SUCCESS)
                        {
                            int maj = 0, min = 0;
                            swscanf(ver, L"%d.%d", &maj, &min);
                            ITypeLib* pTL = nullptr;
                            if (SUCCEEDED(LoadRegTypeLib(tlid, (WORD)maj, (WORD)min,
                                                         LOCALE_USER_DEFAULT, &pTL)))
                            {
                                RegCloseKey(hv);
                                return pTL;
                            }
                        }
                        RegCloseKey(hv);
                    }
                }
            }
        }
        // 2) 组件路径里的内嵌类型库
        wstring inKey = L"CLSID\\" + clsidStr + L"\\InprocServer32";
        if (RegOpenKeyExW(HKEY_CLASSES_ROOT, inKey.c_str(), 0, KEY_READ, &hk) == ERROR_SUCCESS)
        {
            wchar_t path[MAX_PATH] = {};
            DWORD cb = sizeof(path);
            DWORD type = 0;
            LONG rc = RegQueryValueExW(hk, nullptr, nullptr, &type,
                                       (LPBYTE)path, &cb);
            RegCloseKey(hk);
            if (rc == ERROR_SUCCESS)
            {
                ITypeLib* pTL = nullptr;
                if (SUCCEEDED(LoadTypeLibEx(path, REGKIND_NONE, &pTL)))
                    return pTL;
            }
        }
        return nullptr;
    }

    // 按类型信息生成一个「实现了该接口」的 COM 对象
    //
    // 官方 com.ImplInterface 的语义：把 aardio 表变成一个实现了指定接口的
    // COM 对象，方法按名字转发到表的同名成员。无论类型库里把它登记成
    // 原始接口（TKIND_INTERFACE）还是 dispinterface（TKIND_DISPATCH），
    // 都按 vtable 实现 —— 实测 Windows Media Player 的 _WMPOCXEvents
    // 在类型库里是 dispinterface，但服务器是按接口 IID 查 sink 再直接调用
    // vtable 槽位的。
    static Value ComMakeSinkValue(ITypeInfo* pTI, TYPEKIND kind,
                                  const Value& impl, wstring* pErr)
    {
        RawSinkObj* obj = BuildRawSink(pTI, impl, pErr);
        if (!obj) return Value();
        return WrapRawSink(obj);
    }

    // 连接点登记表：挂在 COM 对象包装表的隐藏成员上，供 ReleaseConnection 找到
    // cookie -> (连接点, 接收器)
    static Table* ComConnStoreOf(const Value& obj, bool bCreate)
    {
        if (obj.t != Value::TableT || !obj.tab) return nullptr;
        auto it = obj.tab->map.find(L"\x01comConn");
        if (it != obj.tab->map.end() && it->second.t == Value::TableT && it->second.tab)
            return it->second.tab.get();
        if (!bCreate) return nullptr;
        auto store = std::make_shared<Table>();
        obj.tab->map[L"\x01comConn"] = Value::Table_(store);
        return store.get();
    }

    static void ComConnStoreAdd(Table* store, DWORD cookie,
                                IConnectionPoint* cp, IUnknown* sink)
    {
        if (!store) return;
        auto rec = std::make_shared<Table>();
        rec->map[L"cookie"] = Value::Num_((double)(unsigned long)cookie);
        rec->map[L"cp"] = Value::Pointer_((void*)cp);
        rec->map[L"sink"] = Value::Pointer_((void*)sink);
        store->map[std::to_wstring((unsigned long)cookie)] = Value::Table_(rec);
    }

    // 找 COM 对象的默认事件连接点（只取 dispinterface 型）
    // 连接点只暴露源接口 IID，源接口的类型信息要从类型库里找
    // （源接口在类型库里注册为 dispinterface，只能枚举类型库）。
    // bAllowRaw=true 时也接受原始（非 IDispatch）接口。
    static ITypeInfo* FindSourceTypeInfo(IUnknown* pUnk, REFIID srcIid,
                                         bool bAllowRaw = false)
    {
        if (!pUnk) return nullptr;
        IProvideClassInfo* pPCI = nullptr;
        if (FAILED(pUnk->QueryInterface(IID_IProvideClassInfo, (void**)&pPCI)) || !pPCI)
            return nullptr;
        ITypeInfo* result = nullptr;
        ITypeInfo* clsTI = nullptr;
        if (SUCCEEDED(pPCI->GetClassInfo(&clsTI)) && clsTI)
        {
            ITypeLib* pTL = nullptr;
            UINT tlIndex = 0;
            if (SUCCEEDED(clsTI->GetContainingTypeLib(&pTL, &tlIndex)) && pTL)
            {
                UINT n = pTL->GetTypeInfoCount();
                for (UINT i = 0; i < n && !result; i++)
                {
                    ITypeInfo* ti = nullptr;
                    if (SUCCEEDED(pTL->GetTypeInfo(i, &ti)) && ti)
                    {
                        TYPEATTR* attr = nullptr;
                        bool match = false;
                        if (SUCCEEDED(ti->GetTypeAttr(&attr)) && attr)
                        {
                            match = (IsEqualGUID(attr->guid, srcIid) &&
                                     (bAllowRaw || attr->typekind == TKIND_DISPATCH));
                            ti->ReleaseTypeAttr(attr);
                        }
                        if (match) result = ti;
                        else ti->Release();
                    }
                }
                pTL->Release();
            }
            clsTI->Release();
        }
        pPCI->Release();
        return result;
    }

    // 找默认事件连接点，同时给出源（事件）接口的类型信息
    static IConnectionPoint* FindEventConnectionPoint(IUnknown* pUnk, ITypeInfo** ppSrcTI)
    {
        *ppSrcTI = nullptr;
        IConnectionPointContainer* pCpc = nullptr;
        if (!pUnk || FAILED(pUnk->QueryInterface(IID_IConnectionPointContainer, (void**)&pCpc)) || !pCpc)
            return nullptr;
        // 1) 优先 IID_IDispatch 连接点
        IConnectionPoint* pCp = nullptr;
        if (SUCCEEDED(pCpc->FindConnectionPoint(IID_IDispatch, &pCp)) && pCp)
        {
            ITypeInfo* ti = FindSourceTypeInfo(pUnk, IID_IDispatch);
            if (ti)
            {
                *ppSrcTI = ti;
                pCpc->Release();
                return pCp;
            }
            pCp->Release();
            pCp = nullptr;
        }
        // 2) 枚举连接点：取源接口是 dispinterface 的那个
        IEnumConnectionPoints* pEnum = nullptr;
        if (SUCCEEDED(pCpc->EnumConnectionPoints(&pEnum)) && pEnum)
        {
            IConnectionPoint* pTmp = nullptr;
            ULONG fetched = 0;
            while (pEnum->Next(1, &pTmp, &fetched) == S_OK && pTmp)
            {
                IID iid{};
                if (SUCCEEDED(pTmp->GetConnectionInterface(&iid)))
                {
                    ITypeInfo* ti = FindSourceTypeInfo(pUnk, iid);
                    if (ti)
                    {
                        *ppSrcTI = ti;
                        pCp = pTmp;
                        break;
                    }
                }
                pTmp->Release();
            }
            pEnum->Release();
        }
        pCpc->Release();
        return pCp;
    }

    static void VariantFromValue(const Value& v, VARIANT& out, wstring* pErr, VARTYPE forcedArrayVt)
    {
        VariantInit(&out);
        switch (v.t)
        {
        case Value::Null:
            out.vt = VT_ERROR;
            out.scode = DISP_E_PARAMNOTFOUND;
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
                // Check for _safearray / _type (from com.toSafeArray)
                auto isa = v.tab->map.find(L"_safearray");
                if (isa != v.tab->map.end())
                {
                    VARTYPE reqVt = VT_VARIANT;
                    auto ity = v.tab->map.find(L"_type");
                    if (ity != v.tab->map.end() && ity->second.t == Value::Num)
                        reqVt = (VARTYPE)ity->second.n;
                    if (isa->second.t == Value::Str)
                    {
                        std::string bytes = BytesOf(isa->second.s);
                        SAFEARRAYBOUND rgsabound[1];
                        rgsabound[0].lLbound = 0;
                        rgsabound[0].cElements = (ULONG)bytes.size();
                        SAFEARRAY* psa = SafeArrayCreate(VT_UI1, 1, rgsabound);
                        if (psa)
                        {
                            void* pData = nullptr;
                            if (SUCCEEDED(SafeArrayAccessData(psa, &pData)) && pData)
                            {
                                memcpy(pData, bytes.data(), bytes.size());
                                SafeArrayUnaccessData(psa);
                            }
                            out.vt = VT_ARRAY | VT_UI1;
                            out.parray = psa;
                            break;
                        }
                    }
                    else if (isa->second.t == Value::TableT && isa->second.tab)
                    {
                        VARTYPE elemVt = (reqVt == 0 || reqVt == VT_EMPTY) ? VT_VARIANT : reqVt;
                        SAFEARRAY* psa = CreateSafeArrayFromVector(isa->second.tab->arr, elemVt);
                        if (psa)
                        {
                            out.vt = VT_ARRAY | elemVt;
                            out.parray = psa;
                            break;
                        }
                    }
                }

                // Check for _safearray_type in table or table's _meta
                auto isat = v.tab->map.find(L"_safearray_type");
                if (isat == v.tab->map.end())
                {
                    auto im = v.tab->map.find(L"_meta");
                    if (im != v.tab->map.end() && im->second.t == Value::TableT && im->second.tab)
                    {
                        auto imsat = im->second.tab->map.find(L"_safearray_type");
                        if (imsat != im->second.tab->map.end())
                            isat = imsat;
                    }
                }
                if (isat != v.tab->map.end() && isat->second.t == Value::Num)
                {
                    VARTYPE elemVt = (VARTYPE)isat->second.n;
                    if (elemVt == 0 || elemVt == VT_EMPTY) elemVt = VT_VARIANT;
                    SAFEARRAY* psa = CreateSafeArrayFromVector(v.tab->arr, elemVt);
                    if (psa)
                    {
                        out.vt = VT_ARRAY | elemVt;
                        out.parray = psa;
                        break;
                    }
                }

                if (!v.tab->arr.empty())
                {
                    VARTYPE elemVt = forcedArrayVt;
                    SAFEARRAY* psa = CreateSafeArrayFromVector(v.tab->arr, elemVt);
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
                auto* pTableDisp = new NodioTableDispatch(v, forcedArrayVt);
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
            GcAddRootTable(s_meta);
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
                            vector<Value>& args, wstring* pErr,
                            VARTYPE prefArrayType = VT_EMPTY)
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
                VariantFromValue(args[args.size() - 1 - i], vars[i], pErr, prefArrayType);
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
        // If Invoke failed with E_INVALIDARG / DISP_E_TYPEMISMATCH, and any args were VT_ERROR (DISP_E_PARAMNOTFOUND),
        // retry with those arguments converted to VT_EMPTY
        if ((hr == E_INVALIDARG || hr == DISP_E_TYPEMISMATCH) && !vars.empty())
        {
            bool hasParamNotFound = false;
            for (auto& v : vars)
            {
                if (v.vt == VT_ERROR && v.scode == DISP_E_PARAMNOTFOUND)
                {
                    hasParamNotFound = true;
                    v.vt = VT_EMPTY;
                    v.scode = 0;
                }
            }
            if (hasParamNotFound)
            {
                memset(&ex, 0, sizeof(ex));
                argErr = 0;
                VariantInit(&ret);
                hr = pd->Invoke(dispid, IID_NULL, LOCALE_USER_DEFAULT,
                                wFlags, &dp, &ret, &ex, &argErr);
            }
        }
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

    // ---------- TypeLib & TypeInfo Wrappers ----------
    static Value WrapTypeInfo(ITypeInfo* pTI);
    static Value WrapTypeLib(ITypeLib* pTL);

    static Value WrapTypeLib(ITypeLib* pTL)
    {
        if (!pTL) return Value();
        pTL->AddRef();
        auto spTL = std::shared_ptr<ITypeLib>(pTL, [](ITypeLib* p) { if (p) p->Release(); });
        auto tb = std::make_shared<Table>();
        tb->map[L"_object"] = Value::Pointer_(pTL);
        tb->map[L"_isTypeLib"] = Value::Bool_(true);

        tb->map[L"GetTypeInfoCount"] = NF([spTL](vector<Value>&) -> Value {
            return Value::Num_((double)spTL->GetTypeInfoCount());
        });

        tb->map[L"GetTypeInfo"] = NF([spTL](vector<Value>& a) -> Value {
            if (a.empty() || a[0].t != Value::Num) return Value();
            UINT idx = (UINT)a[0].n;
            if (idx >= 1) idx -= 1; // 1-based index in aardio
            ITypeInfo* pTI = nullptr;
            if (FAILED(spTL->GetTypeInfo(idx, &pTI)) || !pTI)
                return Value();
            Value ret = WrapTypeInfo(pTI);
            pTI->Release();
            return ret;
        });

        tb->map[L"GetDocumentation"] = NF([spTL](vector<Value>& a) -> Value {
            MEMBERID memid = (a.empty() || a[0].t != Value::Num) ? MEMBERID_NIL : (MEMBERID)a[0].n;
            BSTR bstrName = nullptr, bstrDoc = nullptr, bstrHelp = nullptr;
            DWORD dwHelpContext = 0;
            if (FAILED(spTL->GetDocumentation(memid, &bstrName, &bstrDoc, &dwHelpContext, &bstrHelp)))
                return Value();
            auto doc = std::make_shared<Table>();
            doc->map[L"name"] = Value::Str_(bstrName ? bstrName : L"");
            doc->map[L"helpstring"] = Value::Str_(bstrDoc ? bstrDoc : L"");
            doc->map[L"helpfile"] = Value::Str_(bstrHelp ? bstrHelp : L"");
            doc->map[L"helpcontext"] = Value::Num_((double)dwHelpContext);
            if (bstrName) SysFreeString(bstrName);
            if (bstrDoc) SysFreeString(bstrDoc);
            if (bstrHelp) SysFreeString(bstrHelp);
            return Value::Table_(doc);
        });

        tb->map[L"ExportConstants"] = NF([spTL](vector<Value>&) -> Value {
            auto constants = std::make_shared<Table>();
            UINT count = spTL->GetTypeInfoCount();
            for (UINT i = 0; i < count; ++i)
            {
                ITypeInfo* pTI = nullptr;
                if (FAILED(spTL->GetTypeInfo(i, &pTI)) || !pTI) continue;
                TYPEATTR* pTA = nullptr;
                if (SUCCEEDED(pTI->GetTypeAttr(&pTA)) && pTA)
                {
                    for (WORD v = 0; v < pTA->cVars; ++v)
                    {
                        VARDESC* pVD = nullptr;
                        if (SUCCEEDED(pTI->GetVarDesc(v, &pVD)) && pVD)
                        {
                            if (pVD->varkind == VAR_CONST && pVD->lpvarValue)
                            {
                                BSTR bstrName = nullptr;
                                if (SUCCEEDED(pTI->GetDocumentation(pVD->memid, &bstrName, nullptr, nullptr, nullptr)) && bstrName)
                                {
                                    wstring err;
                                    Value val = VariantToValue(*pVD->lpvarValue, &err);
                                    constants->map[bstrName] = val;
                                    SysFreeString(bstrName);
                                }
                            }
                            pTI->ReleaseVarDesc(pVD);
                        }
                    }
                    pTI->ReleaseTypeAttr(pTA);
                }
                pTI->Release();
            }
            return Value::Table_(constants);
        });

        tb->map[L"ExportEnumerations"] = NF([spTL](vector<Value>&) -> Value {
            auto enums = std::make_shared<Table>();
            UINT count = spTL->GetTypeInfoCount();
            for (UINT i = 0; i < count; ++i)
            {
                ITypeInfo* pTI = nullptr;
                if (FAILED(spTL->GetTypeInfo(i, &pTI)) || !pTI) continue;
                TYPEATTR* pTA = nullptr;
                if (SUCCEEDED(pTI->GetTypeAttr(&pTA)) && pTA)
                {
                    if (pTA->typekind == TKIND_ENUM)
                    {
                        BSTR bstrEnumName = nullptr;
                        pTI->GetDocumentation(MEMBERID_NIL, &bstrEnumName, nullptr, nullptr, nullptr);
                        wstring enumName = bstrEnumName ? bstrEnumName : L"";
                        if (bstrEnumName) SysFreeString(bstrEnumName);

                        auto enumTable = std::make_shared<Table>();
                        for (WORD v = 0; v < pTA->cVars; ++v)
                        {
                            VARDESC* pVD = nullptr;
                            if (SUCCEEDED(pTI->GetVarDesc(v, &pVD)) && pVD)
                            {
                                if (pVD->varkind == VAR_CONST && pVD->lpvarValue)
                                {
                                    BSTR bstrValName = nullptr;
                                    if (SUCCEEDED(pTI->GetDocumentation(pVD->memid, &bstrValName, nullptr, nullptr, nullptr)) && bstrValName)
                                    {
                                        wstring err;
                                        Value val = VariantToValue(*pVD->lpvarValue, &err);
                                        enumTable->map[bstrValName] = val;
                                        SysFreeString(bstrValName);
                                    }
                                }
                                pTI->ReleaseVarDesc(pVD);
                            }
                        }
                        if (!enumName.empty())
                            enums->map[enumName] = Value::Table_(enumTable);
                    }
                    pTI->ReleaseTypeAttr(pTA);
                }
                pTI->Release();
            }
            return Value::Table_(enums);
        });

        return Value::Table_(tb);
    }

    // ---- type information helpers ------------------------------------
    // Official contract (lib/builtin/com.aardio intellisense):
    //   !comTypeAttr      Funcs / Vars / typekind / ImplTypes / flags.* / GUID
    //   !comTypeFunc      dispatchable / restricted / memid / invkind / Params /
    //                      ParamsOpt / name / description / helpfile /
    //                      helpcontext / parameters / type / vt
    //   !comTypeParameter default / in / opt / out / type / vt
    //   !comTypeVar       value / name
    // The official library reads these by name in lib/com/typeAttrs.aardio,
    // lib/com/tlbDoc.aardio and lib/com/interface/_.aardio.

    // TYPEATTR.wTypeFlags —— 本机 SDK 的 oaidl.h 是旧式 Automation 头，
    // 没有定义 TA_F*，这里按 Automation ABI 的固定取值补齐。
    // FUNCFLAGS 同理（wFuncFlags 是旧式 FUNCDESC 里的 WORD 字段）。
    enum
    {
        TA_FAPPONLY_ = 0x00010000,
        TA_FCANCREATE_ = 0x00020000,
        TA_FLICENSE_ = 0x00040000,
        TA_FPREDECLONLY_ = 0x00080000,
        TA_FHIDDEN_ = 0x00100000,
        TA_FCONTROL_ = 0x00200000,
        TA_FDUAL_ = 0x00400000,
        TA_FNONEXTENSIBLE_ = 0x00800000,
        TA_FOLEAUTOMATION_ = 0x01000000,
        TA_FDISPATCHABLE_ = 0x02000000,
        TA_FREVERSEBIND_ = 0x04000000
    };
    enum
    {
        FUNCFLAG_FRESTRICTED_ = 0x0001,
        FUNCFLAG_FSOURCE_ = 0x0002,
        FUNCFLAG_FBINDABLE_ = 0x0004,
        FUNCFLAG_FREQUESTEDIT_ = 0x0008,
        FUNCFLAG_FDISPATCHABLE_ = 0x0010,
        FUNCFLAG_FREVERSEBIND_ = 0x0020,
        FUNCFLAG_FHIDDEN_ = 0x0080,
        FUNCFLAG_FPROPGET_ = 0x0100,
        FUNCFLAG_FPROPPUT_ = 0x0200,
        FUNCFLAG_FPROPPUTREF_ = 0x0400
    };

    static wstring ComGuidString(const GUID& g)
    {
        wchar_t buf[64];
        swprintf_s(buf, 64,
            L"{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
            g.Data1, g.Data2, g.Data3,
            g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
            g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
        return wstring(buf);
    }

    static wstring ComTypeKindName(TYPEKIND k)
    {
        switch (k)
        {
        case TKIND_ENUM: return L"enumeration";
        case TKIND_RECORD: return L"record";
        case TKIND_MODULE: return L"module";
        case TKIND_INTERFACE: return L"interface";
        case TKIND_DISPATCH: return L"dispinterface";
        case TKIND_COCLASS: return L"coclass";
        case TKIND_ALIAS: return L"alias";
        case TKIND_UNION: return L"union";
        }
        return L"unknown";
    }

    // plain (uncommented) name of a base VARTYPE
    static wstring ComTypePlainName(VARTYPE base)
    {
        switch (base)
        {
        case VT_EMPTY: return L"empty";
        case VT_NULL: return L"null";
        case VT_I1: case VT_UI1: case VT_I2: case VT_UI2:
        case VT_I4: case VT_UI4: case VT_INT: case VT_UINT:
        case VT_ERROR: case VT_HRESULT:
            return L"int";
        case VT_I8: case VT_UI8: return L"long";
        case VT_R4: return L"float";
        case VT_R8: return L"double";
        case VT_CY: case VT_DECIMAL: return L"decimal";
        case VT_DATE: return L"date";
        case VT_BSTR: case VT_LPSTR: case VT_LPWSTR: return L"string";
        case VT_BOOL: return L"bool";
        case VT_VARIANT: return L"variant";
        case VT_DISPATCH: return L"pointer/*IDispatch*/";
        case VT_UNKNOWN: return L"pointer/*IUnknown*/";
        case VT_PTR: case VT_INT_PTR: case VT_UINT_PTR: return L"pointer";
        case VT_SAFEARRAY: return L"pointer/*SAFEARRAY*/";
        case VT_CARRAY: return L"pointer/*CARRAY*/";
        case VT_RECORD: return L"pointer/*RECORD*/";
        case VT_FILETIME: return L"pointer/*FILETIME*/";
        case VT_BLOB: return L"pointer/*BLOB*/";
        case VT_STREAM: return L"pointer/*IStream*/";
        case VT_STORAGE: return L"pointer/*IStorage*/";
        case VT_CF: return L"pointer/*CLIPDATA*/";
        case VT_CLSID: return L"pointer/*CLSID*/";
        case VT_VOID: return L"void";
        }
        return L"unknown";
    }

    // C style name used inside the /*...*/ comment
    static wstring ComTypeCName(VARTYPE base)
    {
        switch (base)
        {
        case VT_VARIANT: return L"VARIANT";
        case VT_BSTR: return L"BSTR";
        case VT_I1: return L"I1";
        case VT_UI1: return L"UI1";
        case VT_I2: return L"I2";
        case VT_UI2: return L"UI2";
        case VT_I4: return L"I4";
        case VT_UI4: return L"UI4";
        case VT_I8: return L"I8";
        case VT_UI8: return L"UI8";
        case VT_R4: return L"R4";
        case VT_R8: return L"R8";
        case VT_BOOL: return L"BOOL";
        case VT_DISPATCH: return L"IDispatch";
        case VT_UNKNOWN: return L"IUnknown";
        case VT_DATE: return L"DATE";
        case VT_CY: return L"CY";
        case VT_INT: return L"INT";
        case VT_UINT: return L"UINT";
        case VT_HRESULT: return L"HRESULT";
        case VT_ERROR: return L"SCODE";
        case VT_RECORD: return L"RECORD";
        case VT_FILETIME: return L"FILETIME";
        case VT_BLOB: return L"BLOB";
        case VT_STREAM: return L"IStream";
        case VT_STORAGE: return L"IStorage";
        case VT_CF: return L"CLIPDATA";
        case VT_CLSID: return L"CLSID";
        case VT_LPSTR: return L"LPSTR";
        case VT_LPWSTR: return L"LPWSTR";
        }
        return ComTypePlainName(base);
    }

    // Official type string of a parameter / return type. The shapes the
    // official library relies on are "pointer/*VARIANT*/" for a VARIANT*
    // and "pointer/*VARIANT*/[]" for a SAFEARRAY of VARIANT
    // (lib/com/tlbDoc.aardio strips the /*...*/ comment and maps the
    // VARIANT forms onto "variant" / "variant[]").
    static wstring ComTypeName(VARTYPE vt)
    {
        if (vt & VT_ARRAY)
            return L"pointer/*" + ComTypeCName(vt & VT_TYPEMASK) + L"*/[]";
        VARTYPE base = vt & VT_TYPEMASK;
        if (vt & VT_BYREF)
        {
            wstring inner = (base == VT_VARIANT) ? wstring(L"VARIANT")
                                                  : ComTypeCName(base);
            return L"pointer/*" + inner + L"*/" + ((vt & VT_PTR) ? L"*" : L"");
        }
        return ComTypePlainName(base);
    }

    static Value WrapTypeInfo(ITypeInfo* pTI)
    {
        if (!pTI) return Value();
        pTI->AddRef();
        auto spTI = std::shared_ptr<ITypeInfo>(pTI, [](ITypeInfo* p) { if (p) p->Release(); });
        auto tb = std::make_shared<Table>();
        tb->map[L"_object"] = Value::Pointer_(pTI);
        tb->map[L"_isTypeInfo"] = Value::Bool_(true);

        tb->map[L"GetTypeLib"] = NF([spTI](vector<Value>&) -> Value {
            ITypeLib* pTL = nullptr;
            UINT idx = 0;
            if (FAILED(spTI->GetContainingTypeLib(&pTL, &idx)) || !pTL)
                return Value();
            Value ret = WrapTypeLib(pTL);
            pTL->Release();
            return ret;
        });

        tb->map[L"GetDocumentation"] = NF([spTI](vector<Value>& a) -> Value {
            MEMBERID memid = (a.empty() || a[0].t != Value::Num) ? MEMBERID_NIL : (MEMBERID)a[0].n;
            BSTR bstrName = nullptr, bstrDoc = nullptr, bstrHelp = nullptr;
            DWORD dwHelpContext = 0;
            if (FAILED(spTI->GetDocumentation(memid, &bstrName, &bstrDoc, &dwHelpContext, &bstrHelp)))
                return Value();
            auto doc = std::make_shared<Table>();
            doc->map[L"name"] = Value::Str_(bstrName ? bstrName : L"");
            doc->map[L"helpstring"] = Value::Str_(bstrDoc ? bstrDoc : L"");
            doc->map[L"helpfile"] = Value::Str_(bstrHelp ? bstrHelp : L"");
            doc->map[L"helpcontext"] = Value::Num_((double)dwHelpContext);
            if (bstrName) SysFreeString(bstrName);
            if (bstrDoc) SysFreeString(bstrDoc);
            if (bstrHelp) SysFreeString(bstrHelp);
            return Value::Table_(doc);
        });

        // comTypeAttr —— 官方 !comTypeAttr
        tb->map[L"GetTypeAttr"] = NF([spTI](vector<Value>&) -> Value {
            TYPEATTR* pTA = nullptr;
            if (FAILED(spTI->GetTypeAttr(&pTA)) || !pTA)
                return Value();
            auto attr = std::make_shared<Table>();
            attr->map[L"Funcs"] = Value::Num_((double)pTA->cFuncs);
            attr->map[L"Vars"] = Value::Num_((double)pTA->cVars);
            attr->map[L"ImplTypes"] = Value::Num_((double)pTA->cImplTypes);
            attr->map[L"typekind"] = Value::Str_(ComTypeKindName(pTA->typekind));
            attr->map[L"GUID"] = Value::Str_(ComGuidString(pTA->guid));
            auto flags = std::make_shared<Table>();
            flags->map[L"cancreate"] =
                Value::Bool_((pTA->wTypeFlags & TA_FCANCREATE_) != 0);
            flags->map[L"dispatchable"] =
                Value::Bool_((pTA->wTypeFlags & TA_FDISPATCHABLE_) != 0);
            flags->map[L"control"] =
                Value::Bool_((pTA->wTypeFlags & TA_FCONTROL_) != 0);
            flags->map[L"appobject"] =
                Value::Bool_((pTA->wTypeFlags & TA_FAPPONLY_) != 0);
            flags->map[L"hidden"] =
                Value::Bool_((pTA->wTypeFlags & TA_FHIDDEN_) != 0);
            flags->map[L"oleautomation"] =
                Value::Bool_((pTA->wTypeFlags & TA_FOLEAUTOMATION_) != 0);
            attr->map[L"flags"] = Value::Table_(flags);
            spTI->ReleaseTypeAttr(pTA);
            return Value::Table_(attr);
        });

        // comTypeFunc —— 官方 !comTypeFunc，起始索引为 1
        tb->map[L"GetFuncDesc"] = NF([spTI](vector<Value>& a) -> Value {
            if (a.empty() || a[0].t != Value::Num) return Value();
            UINT idx = (UINT)a[0].n;
            if (idx >= 1) idx -= 1;
            FUNCDESC* pFD = nullptr;
            if (FAILED(spTI->GetFuncDesc(idx, &pFD)) || !pFD)
                return Value();

            auto fn = std::make_shared<Table>();
            // GetNames returns the function name followed by the parameter
            // names — one call serves both.
            BSTR bstrNames[32];
            UINT cNames = 0;
            HRESULT hrN = spTI->GetNames(pFD->memid, bstrNames, 32, &cNames);
            if (FAILED(hrN)) cNames = 0;
            fn->map[L"name"] = Value::Str_(
                (cNames > 0 && bstrNames[0]) ? bstrNames[0] : L"");

            fn->map[L"memid"] = Value::Num_((double)pFD->memid);
            fn->map[L"dispatchable"] =
                Value::Bool_((pFD->wFuncFlags & FUNCFLAG_FDISPATCHABLE_) != 0);
            fn->map[L"restricted"] =
                Value::Bool_((pFD->wFuncFlags & FUNCFLAG_FRESTRICTED_) != 0);

            // invkind：官方取值为 function / _get / _set / _setByRef
            wstring invKind = L"function";
            if (pFD->invkind & INVOKE_PROPERTYGET) invKind = L"_get";
            else if (pFD->invkind & INVOKE_PROPERTYPUT) invKind = L"_set";
            else if (pFD->invkind & INVOKE_PROPERTYPUTREF) invKind = L"_setByRef";
            fn->map[L"invkind"] = Value::Str_(invKind);

            VARTYPE vtRet = pFD->elemdescFunc.tdesc.vt;
            fn->map[L"Params"] = Value::Num_((double)pFD->cParams);
            fn->map[L"type"] = Value::Str_(ComTypeName(vtRet));
            fn->map[L"vt"] = Value::Num_((double)(long long)vtRet);

            // cParamsOpt 即官方的 ParamsOpt；变长参数函数这里是 -1
            // （官方 lib/com/tlbDoc.aardio 正是用 -1 判断「不定个数参数」）
            fn->map[L"ParamsOpt"] = Value::Num_((double)pFD->cParamsOpt);

            // 文档信息
            BSTR bstrDoc = nullptr, bstrHelp = nullptr;
            DWORD dwHelpCtx = 0;
            if (SUCCEEDED(spTI->GetDocumentation(pFD->memid, &bstrDoc,
                &bstrHelp, &dwHelpCtx, nullptr)))
            {
                fn->map[L"description"] = Value::Str_(bstrDoc ? bstrDoc : L"");
                fn->map[L"helpfile"] = Value::Str_(bstrHelp ? bstrHelp : L"");
                fn->map[L"helpcontext"] = Value::Num_((double)dwHelpCtx);
            }
            else
            {
                fn->map[L"description"] = Value::Str_(L"");
                fn->map[L"helpfile"] = Value::Str_(L"");
                fn->map[L"helpcontext"] = Value::Num_(0);
            }
            if (bstrDoc) SysFreeString(bstrDoc);
            if (bstrHelp) SysFreeString(bstrHelp);

            // 参数描述数组（comTypeParameter）
            auto params = std::make_shared<Table>();
            for (int i = 0; i < pFD->cParams; i++)
            {
                auto pd = std::make_shared<Table>();
                wstring pname;
                if ((UINT)(i + 1) < cNames && bstrNames[i + 1])
                    pname = bstrNames[i + 1];
                pd->map[L"name"] = Value::Str_(pname);

                // 旧式 Automation ABI：参数描述在 lprgelemdescParam 数组里
                VARTYPE pvt = VT_EMPTY;
                USHORT wFlags = 0;
                const PARAMDESCEX* pdex = nullptr;
                if (pFD->lprgelemdescParam)
                {
                    const ELEMDESC& ed = pFD->lprgelemdescParam[i];
                    pvt = ed.tdesc.vt;
                    wFlags = ed.paramdesc.wParamFlags;
                    pdex = ed.paramdesc.pparamdescex;
                }
                pd->map[L"in"] = Value::Bool_((wFlags & PARAMFLAG_FIN) != 0);
                pd->map[L"out"] = Value::Bool_((wFlags & PARAMFLAG_FOUT) != 0);
                pd->map[L"opt"] = Value::Bool_((wFlags & PARAMFLAG_FOPT) != 0);
                pd->map[L"type"] = Value::Str_(ComTypeName(pvt));
                pd->map[L"vt"] = Value::Num_((double)(long long)pvt);
                // 默认值
                Value defVal;
                if ((wFlags & PARAMFLAG_FHASDEFAULT) && pdex)
                {
                    wstring err;
                    defVal = VariantToValue(pdex->varDefaultValue, &err);
                }
                pd->map[L"default"] = defVal;
                params->arr.push_back(Value::Table_(pd));
            }
            fn->map[L"parameters"] = Value::Table_(params);

            for (UINT i = 0; i < cNames; i++)
                if (bstrNames[i]) SysFreeString(bstrNames[i]);
            spTI->ReleaseFuncDesc(pFD);
            return Value::Table_(fn);
        });

        // comTypeVar —— 官方 !comTypeVar，起始索引为 1
        tb->map[L"GetVarDesc"] = NF([spTI](vector<Value>& a) -> Value {
            if (a.empty() || a[0].t != Value::Num) return Value();
            UINT idx = (UINT)a[0].n;
            if (idx >= 1) idx -= 1;
            VARDESC* pVD = nullptr;
            if (FAILED(spTI->GetVarDesc(idx, &pVD)) || !pVD)
                return Value();
            auto v = std::make_shared<Table>();
            BSTR bstrName = nullptr;
            if (SUCCEEDED(spTI->GetDocumentation(pVD->memid, &bstrName,
                nullptr, nullptr, nullptr)) && bstrName)
            {
                v->map[L"name"] = Value::Str_(bstrName);
                SysFreeString(bstrName);
            }
            else
            {
                v->map[L"name"] = Value::Str_(L"");
            }
            Value val;
            if (pVD->lpvarValue)
            {
                wstring err;
                val = VariantToValue(*pVD->lpvarValue, &err);
            }
            v->map[L"value"] = val;
            spTI->ReleaseVarDesc(pVD);
            return Value::Table_(v);
        });

        // 官方 !comTypeInfo.GetImplType（起始索引为 1）
        // 旧式 Automation ABI：先取 HREFTYPE，再取类型信息
        tb->map[L"GetImplType"] = NF([spTI](vector<Value>& a) -> Value {
            if (a.empty() || a[0].t != Value::Num) return Value();
            UINT idx = (UINT)a[0].n;
            if (idx >= 1) idx -= 1;
            HREFTYPE hrt = 0;
            if (FAILED(spTI->GetRefTypeOfImplType(idx, &hrt)) || !hrt)
                return Value();
            ITypeInfo* pImpl = nullptr;
            if (FAILED(spTI->GetRefTypeInfo(hrt, &pImpl)) || !pImpl)
                return Value();
            Value ret = WrapTypeInfo(pImpl);
            pImpl->Release();
            return ret;
        });

        // 官方 !comTypeInfo.GetImplTypeFlags（起始索引为 1）
        tb->map[L"GetImplTypeFlags"] = NF([spTI](vector<Value>& a) -> Value {
            if (a.empty() || a[0].t != Value::Num) return Value();
            UINT idx = (UINT)a[0].n;
            if (idx >= 1) idx -= 1;
            INT f = 0;
            if (FAILED(spTI->GetImplTypeFlags(idx, &f)))
                return Value();
            auto t = std::make_shared<Table>();
            t->map[L"default"] =
                Value::Bool_((f & IMPLTYPEFLAG_FDEFAULT) != 0);
            t->map[L"source"] =
                Value::Bool_((f & IMPLTYPEFLAG_FSOURCE) != 0);
            t->map[L"restricted"] =
                Value::Bool_((f & IMPLTYPEFLAG_FRESTRICTED) != 0);
            t->map[L"defaultVTable"] =
                Value::Bool_((f & IMPLTYPEFLAG_FDEFAULTVTABLE) != 0);
            return Value::Table_(t);
        });

        tb->map[L"DumpTypeInfo"] = NF([spTI](vector<Value>&) -> Value {
            wstring out;
            TYPEATTR* pTA = nullptr;
            if (FAILED(spTI->GetTypeAttr(&pTA)) || !pTA)
                return Value::Str_(L"");
            BSTR bstrDoc = nullptr;
            spTI->GetDocumentation(MEMBERID_NIL, &bstrDoc, nullptr, nullptr, nullptr);
            if (bstrDoc)
            {
                out += wstring(L"// ") + bstrDoc + L"\n";
                SysFreeString(bstrDoc);
            }
            for (WORD f = 0; f < pTA->cFuncs; ++f)
            {
                FUNCDESC* pFD = nullptr;
                if (SUCCEEDED(spTI->GetFuncDesc(f, &pFD)) && pFD)
                {
                    BSTR bstrNames[16];
                    UINT cNames = 0;
                    if (SUCCEEDED(spTI->GetNames(pFD->memid, bstrNames, 16, &cNames)) && cNames > 0)
                    {
                        out += bstrNames[0];
                        out += L"(";
                        for (UINT i = 1; i < cNames; ++i)
                        {
                            if (i > 1) out += L", ";
                            out += bstrNames[i];
                        }
                        out += L")\n";
                        for (UINT i = 0; i < cNames; ++i) SysFreeString(bstrNames[i]);
                    }
                    spTI->ReleaseFuncDesc(pFD);
                }
            }
            spTI->ReleaseTypeAttr(pTA);
            return Value::Str_(out);
        });

        return Value::Table_(tb);
    }

    static Value LookupTypeLibConstant(IDispatch* pd, Value& targetObj, const wstring& key)
    {
        if (!targetObj.tab) return Value();
        auto itConsts = targetObj.tab->map.find(L"_typeLibConstants");
        if (itConsts != targetObj.tab->map.end() && itConsts->second.t == Value::TableT && itConsts->second.tab)
        {
            auto itVal = itConsts->second.tab->map.find(key);
            if (itVal != itConsts->second.tab->map.end())
                return itVal->second;
            return Value();
        }
        if (!pd) return Value();
        ITypeInfo* pTI = nullptr;
        if (FAILED(pd->GetTypeInfo(0, LOCALE_USER_DEFAULT, &pTI)) || !pTI)
            return Value();
        ITypeLib* pTL = nullptr;
        UINT uIdx = 0;
        HRESULT hrTL = pTI->GetContainingTypeLib(&pTL, &uIdx);
        pTI->Release();
        if (FAILED(hrTL) || !pTL)
            return Value();
        Value vTL = WrapTypeLib(pTL);
        pTL->Release();
        if (vTL.t == Value::TableT && vTL.tab)
        {
            auto itExp = vTL.tab->map.find(L"ExportConstants");
            if (itExp != vTL.tab->map.end() && (itExp->second.t == Value::Func || itExp->second.native))
            {
                vector<Value> noArgs;
                Interp* it = g_interpCurrent;
                if (it)
                {
                    Value vConsts = it->CallOwner(itExp->second, vTL, noArgs);
                    if (vConsts.t == Value::TableT && vConsts.tab)
                    {
                        targetObj.tab->map[L"_typeLibConstants"] = vConsts;
                        auto itVal = vConsts.tab->map.find(key);
                        if (itVal != vConsts.tab->map.end())
                            return itVal->second;
                    }
                }
            }
        }
        return Value();
    }

    // bound member: comObj.member(args) — a member read returns this
    // wrapper; calling it invokes the DISPID with the call args
    static Value WrapBoundMember(IDispatch* pd, DISPID dispid,
                                 const wstring& name,
                                 VARTYPE prefArrayType = VT_EMPTY)
    {
        auto w = std::make_shared<Table>();
        w->map[L"_boundDispatch"] = Value::Pointer_((void*)pd);
        w->map[L"_boundDispId"] = Value::Num_((double)dispid);
        w->map[L"_boundName"] = Value::Str_(name);
        if (prefArrayType != VT_EMPTY)
            w->map[L"_preferredArrayType"] = Value::Num_((double)prefArrayType);
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
            GcAddRootTable(s_meta);

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
                VARTYPE prefArrayType = VT_EMPTY;
                auto ipat = targetObj.tab->map.find(L"_preferredArrayType");
                if (ipat != targetObj.tab->map.end() && ipat->second.t == Value::Num)
                    prefArrayType = (VARTYPE)ipat->second.n;

                bool bOwnerCall = (a.size() >= 2 && a[1].t == Value::Bool && a[1].b);
                DISPID dispid = DISPID_UNKNOWN;
                if (targetObj.tab->comCache)
                {
                    auto it = targetObj.tab->comCache->find(key);
                    if (it != targetObj.tab->comCache->end())
                    {
                        if (bOwnerCall && it->second.boundMethod.t != Value::Null)
                            return it->second.boundMethod;
                        if (it->second.dispid != -1)
                        {
                            dispid = (DISPID)it->second.dispid;
                            goto have_dispid;
                        }
                    }
                }

                {
                    OLECHAR* name = const_cast<OLECHAR*>(key.c_str());
                    if (FAILED(pd->GetIDsOfNames(IID_NULL, &name, 1,
                                                 LOCALE_USER_DEFAULT, &dispid)))
                    {
                        // COM accessor dispatch: obj.getFoo / obj.setFoo gets the bound method
                        // e.g. "getCompile" -> "Compile", "getReference" -> "Reference", "getInvokeObjectMember" -> "InvokeObjectMember"
                        // Also parameterized property-put helper: obj.setFoo("key", val) maps to property put on "Foo"
                        // Official aardio also accepts all-lowercase accessor prefixes
                        // (doc: "在属性名字前加上get前缀、并以函数形式调用属性方法"),
                        // e.g. IXMLDOMDocument.getxml() / setText() / getText()
                        bool bAccessor = key.size() > 3 &&
                            (key.rfind(L"get", 0) == 0 || key.rfind(L"set", 0) == 0) &&
                            iswupper(key[3]);
                        if (!bAccessor && key.size() > 4 &&
                            (key.rfind(L"get", 0) == 0 || key.rfind(L"set", 0) == 0))
                        {
                            // lowercase accessor: verify the stripped name exists
                            wstring methodKey = key.substr(3);
                            OLECHAR* mname = const_cast<OLECHAR*>(methodKey.c_str());
                            DISPID idProbe;
                            if (SUCCEEDED(pd->GetIDsOfNames(IID_NULL, &mname, 1,
                                                            LOCALE_USER_DEFAULT, &idProbe)))
                                bAccessor = true;
                        }
                        if (bAccessor)
                        {
                            wstring methodKey = key.substr(3);
                            OLECHAR* mname = const_cast<OLECHAR*>(methodKey.c_str());
                            if (SUCCEEDED(pd->GetIDsOfNames(IID_NULL, &mname, 1,
                                                            LOCALE_USER_DEFAULT, &dispid)))
                            {
                                bool bIsSet = (key.rfind(L"set", 0) == 0);
                                auto w = std::make_shared<Table>();
                                w->map[L"_boundDispatch"] = Value::Pointer_((void*)pd);
                                w->map[L"_boundDispId"] = Value::Num_((double)dispid);
                                w->map[L"_boundName"] = Value::Str_(key);
                                if (bIsSet)
                                    w->map[L"_boundIsSet"] = Value::Bool_(true);
                                if (prefArrayType != VT_EMPTY)
                                    w->map[L"_preferredArrayType"] = Value::Num_((double)prefArrayType);
                                w->map[L"_meta"] = Value::Table_(
                                    std::shared_ptr<Table>(ComMeta(), [](Table*) {}));
                                Value bm = Value::Table_(w);
                                if (!targetObj.tab->comCache)
                                    targetObj.tab->comCache = std::make_shared<std::unordered_map<wstring, ComCacheEntry>>();
                                auto& cEntry = (*targetObj.tab->comCache)[key];
                                cEntry.dispid = (long)dispid;
                                cEntry.boundMethod = bm;
                                return bm;
                            }
                        }

                        // Type library constants lookup: obj.adChar etc.
                        Value vConst = LookupTypeLibConstant(pd, targetObj, key);
                        if (vConst.t != Value::Null)
                            return vConst;
                        return Value();
                    }
                }

                if (!targetObj.tab->comCache)
                    targetObj.tab->comCache = std::make_shared<std::unordered_map<wstring, ComCacheEntry>>();
                (*targetObj.tab->comCache)[key].dispid = (long)dispid;

                if (bOwnerCall)
                {
                    Value bm = WrapBoundMember(pd, dispid, key, prefArrayType);
                    (*targetObj.tab->comCache)[key].boundMethod = bm;
                    return bm;
                }

have_dispid:
                wstring err;
                vector<Value> noArgs;
                Value v = InvokeDisp(pd, dispid, DISPATCH_PROPERTYGET | DISPATCH_METHOD,
                                     noArgs, &err, prefArrayType);
                if (v.t == Value::TableT && v.tab && v.tab->map.count(L"_object") && prefArrayType != VT_EMPTY)
                {
                    v.tab->map[L"_preferredArrayType"] = Value::Num_((double)prefArrayType);
                }
                // a zero-arg probe that yields EMPTY is a Sub-style
                // method (e.g. IWshShortcut.Save) — keep it callable
                // a probe that yields a raw CCW thunk (_rawThunk) must stay
                // callable so late-binding calls on placeholder CCWs still work
                bool bNetThunk = v.t == Value::TableT && v.tab &&
                                 v.tab->map.count(L"_rawThunk") > 0;
                if (!err.empty() || v.t == Value::Null || bNetThunk)
                {
                    Value vConst = LookupTypeLibConstant(pd, targetObj, key);
                    if (vConst.t != Value::Null)
                        return vConst;
                    Value bm = WrapBoundMember(pd, dispid, key, prefArrayType);
                    if (targetObj.tab->comCache)
                        (*targetObj.tab->comCache)[key].boundMethod = bm;
                    return bm;
                }
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
                DISPID dispid = DISPID_UNKNOWN;
                if (targetObj.tab->comCache)
                {
                    auto it = targetObj.tab->comCache->find(key);
                    if (it != targetObj.tab->comCache->end() && it->second.dispid != -1)
                        dispid = (DISPID)it->second.dispid;
                }
                if (dispid == DISPID_UNKNOWN)
                {
                    OLECHAR* name = const_cast<OLECHAR*>(key.c_str());
                    if (FAILED(pd->GetIDsOfNames(IID_NULL, &name, 1,
                                                 LOCALE_USER_DEFAULT, &dispid)))
                    {
                        Interp* it = g_interpCurrent;
                        if (it)
                            *it->err = L"com: 成员不存在 " + key;
                        return Value();
                    }
                    if (!targetObj.tab->comCache)
                        targetObj.tab->comCache = std::make_shared<std::unordered_map<wstring, ComCacheEntry>>();
                    (*targetObj.tab->comCache)[key].dispid = (long)dispid;
                }
                VARTYPE prefArrayType = VT_EMPTY;
                auto ipat = targetObj.tab->map.find(L"_preferredArrayType");
                if (ipat != targetObj.tab->map.end() && ipat->second.t == Value::Num)
                    prefArrayType = (VARTYPE)ipat->second.n;
                vector<Value> putArgs{ a[1] };
                wstring err;
                InvokeDisp(pd, dispid, DISPATCH_PROPERTYPUT, putArgs, &err, prefArrayType);
                if (!err.empty())
                {
                    Interp* it = g_interpCurrent;
                    if (it)
                        *it->err = err;
                }
                return Value();
            });

            // _call(owner, ...) on a bound member or a COM dispatch object directly
            s_meta->map[L"_call"] = NF([](vector<Value>& a) -> Value {
                if (a.empty() || a[0].t != Value::TableT || !a[0].tab)
                    return Value();
                auto ib = a[0].tab->map.find(L"_boundDispatch");
                auto ii = a[0].tab->map.find(L"_boundDispId");
                IDispatch* pd = nullptr;
                DISPID dispid = DISPID_VALUE;
                if (ib != a[0].tab->map.end() &&
                    ib->second.t == Value::Pointer &&
                    ii != a[0].tab->map.end())
                {
                    pd = reinterpret_cast<IDispatch*>(ib->second.ptr);
                    dispid = (DISPID)(INT_PTR)BNum(ii->second);
                }
                else
                {
                    auto io = a[0].tab->map.find(L"_object");
                    if (io != a[0].tab->map.end() && io->second.t == Value::Pointer)
                    {
                        pd = reinterpret_cast<IDispatch*>(io->second.ptr);
                        dispid = DISPID_VALUE; // default member / indexer / collection invocation
                    }
                }
                if (!pd)
                    return Value();
                auto iname = a[0].tab->map.find(L"_boundName");
                wstring bname = iname != a[0].tab->map.end() ? iname->second.ToString() : L"";
                if (getenv("NODIO_TRACE"))
                    fwprintf(stderr, L"[ComMeta._call] boundName=%s dispid=%ld cArgs=%d\n", bname.c_str(), (long)dispid, (int)(a.size() - 1));
                WORD wFlags = DISPATCH_METHOD | DISPATCH_PROPERTYGET;
                if (a[0].tab->map.count(L"_boundIsSet"))
                    wFlags = DISPATCH_PROPERTYPUT;
                VARTYPE prefArrayType = VT_EMPTY;
                auto ipat = a[0].tab->map.find(L"_preferredArrayType");
                if (ipat != a[0].tab->map.end() && ipat->second.t == Value::Num)
                    prefArrayType = (VARTYPE)ipat->second.n;
                vector<Value> args(a.begin() + 1, a.end());
                wstring err;
                Value r = InvokeDisp(pd, dispid, wFlags, args, &err, prefArrayType);
                if (r.t == Value::TableT && r.tab && r.tab->map.count(L"_object") && prefArrayType != VT_EMPTY)
                {
                    r.tab->map[L"_preferredArrayType"] = Value::Num_((double)prefArrayType);
                }
                if (!err.empty())
                {
                    // Fallback for collections where DISPID_VALUE is not supported but item(index) is:
                    // e.g. DOM element collections heads(0) -> heads.item(0)
                    if (dispid == DISPID_VALUE && !args.empty())
                    {
                        DISPID dispidItem;
                        OLECHAR* itemName = const_cast<OLECHAR*>(L"item");
                        if (SUCCEEDED(pd->GetIDsOfNames(IID_NULL, &itemName, 1,
                                                        LOCALE_USER_DEFAULT, &dispidItem)))
                        {
                            wstring errItem;
                            Value rItem = InvokeDisp(pd, dispidItem, DISPATCH_METHOD | DISPATCH_PROPERTYGET, args, &errItem);
                            if (errItem.empty())
                                return rItem;
                        }
                    }
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
            GcAddRootTable(s_meta);

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

// ---- COM 调试追踪（写文件，避免控制台重定向吞掉输出） ----
    void ComRawTrace(const wchar_t* fmt, ...)
    {
        if (!ComTraceEnabled()) return;
        wchar_t buf[1024];
        va_list ap;
        va_start(ap, fmt);
        vswprintf(buf, 1024, fmt, ap);
        va_end(ap);
        FILE* f = nullptr;
        _wfopen_s(&f, L"C:\\Users\\QQ\\AppData\\Local\\Temp\\avtest\\comraw.log", L"a, ccs=UTF-8");
        if (f) { fputws(buf, f); fclose(f); }
    }

// ---- COM 回调参数转换（engine/comsig.h 的契约，ffi.cpp 的跳板调用） ----
//
// 原始接口的方法参数来自类型库，比通用 FFI 的 int/pointer 更丰富：
// BSTR 要变成 aardio 字符串，VARIANT* 要变成 aardio 值，
// 接口指针要变成 COM 对象（官方 com.ImplInterface 的事件处理函数
// 拿到的是 COM 对象，官方文档例子里直接写 item.sourceURL）。

    Value ComArgFromBstr(void* p)
    {
        BSTR b = (BSTR)p;
        if (!b) return Value();
        return Value::Str_(wstring(b, SysStringLen(b)));
    }

    Value ComArgFromDispatch(void* p)
    {
        if (!p) return Value();
        // 包成 COM 对象（官方 com.ImplInterface 的事件处理函数拿到的是
        // COM 对象，官方文档例子里直接写 item.sourceURL）
        return WrapDispatch(reinterpret_cast<IDispatch*>(p));
    }

    Value ComArgFromUnknown(void* p)
    {
        // 借用指针：调用方（COM 服务器）拥有它的生命周期
        if (!p) return Value();
        return Value::Pointer_(p);
    }

    Value ComArgFromVariant(void* p)
    {
        if (!p) return Value();
        VARIANT* v = (VARIANT*)p;
        wstring err;
        Value r = VariantToValue(*v, &err);
        return r;
    }

    Value ComArgFromIface(void* p)
    {
        if (!p) return Value();
        // 接口指针：能取到 IDispatch 就包成 COM 对象，否则给原始指针
        IDispatch* pd = nullptr;
        if (SUCCEEDED(reinterpret_cast<IUnknown*>(p)->QueryInterface(
                          IID_IDispatch, (void**)&pd)) && pd)
        {
            return WrapDispatch(pd);
        }
        return Value::Pointer_(p);
    }

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

    // ---- 官方引擎原生 API（第二十三轮补齐）----
    //
    // 官方 com 命名空间里有一批**引擎原生**函数：官方 lib/builtin/com.aardio
    // 只写 intellisense 声明（`NewObject(.(tab,progId) = ...`），实现藏在
    // aardio.exe 里。官方库的真实用法（lib/com/activeX.aardio）：
    //   appObject, event, e = ..com.NewObject(this._object, this.ProgID, this.TypeLib)
    //   if(!appObject) error("NewObject failed: " ++ e)
    //   this._object.externalEvent = event
    // 下面实现可以忠实落地的那几个（COM 事件接收器那一族需要连接点
    // IConnectionPoint 基础设施，暂缺，见 docs/aardio-stdlib-delivery.md §8）。

    // com.NewObject(tab, progId [, typeLibPath]) -> appObject, event, e
    // com.NewControl(tab, progId)               -> appObject, event, e
    auto comNewObject = [](vector<Value>& a, const wchar_t* apiName) -> Value {
        wstring prog = a.size() > 1 ? BStr(a[1]) : wstring();
        auto fail = [&](const wstring& msg) -> Value {
            Value r;                            // appObject = null（首值）
            auto ex = std::make_shared<vector<Value>>();
            ex->push_back(Value());            // event = null（mrets 只装首值之后的附加值）
            ex->push_back(Value::Str_(msg));   // e = 错误信息
            r.mrets = ex;
            return r;
        };
        if (prog.empty())
            return fail(wstring(apiName) + L": 缺少 ProgID 参数");
        CLSID clsid;
        if (FAILED(CLSIDFromProgID(prog.c_str(), &clsid)))
            return fail(wstring(apiName) + L": 无效 ProgID " + prog);
        IDispatch* pd = nullptr;
        HRESULT hr = CoCreateInstance(clsid, nullptr, CLSCTX_ALL, IID_IDispatch,
                                      (void**)&pd);
        if (FAILED(hr) || !pd)
        {
            wchar_t buf[16];
            swprintf(buf, 16, L"%08lX", (unsigned long)hr);
            return fail(wstring(apiName) + L": 创建失败 hr=0x" + buf);
        }
        Value obj = WrapDispatch(pd);
        // event：官方用它持有 COM 事件连接（外部事件对象）。
        // Nodio 的 COM 宿主还没有连接点设施，这里给出同形态的锚点对象，
        // 保证官方「三值返回」的形状一致。
        auto ev = std::make_shared<Table>();
        ev->map[L"_isComEvent"] = Value::Bool_(true);
        Value r = obj;
        auto ex = std::make_shared<vector<Value>>();
        ex->push_back(Value::Table_(ev));  // event
        ex->push_back(Value());            // e = null（无错误）
        r.mrets = ex;
        return r;
    };
    com->map[L"NewObject"] = NF([comNewObject](vector<Value>& a) -> Value {
        return comNewObject(a, L"com.NewObject");
    });
    com->map[L"NewControl"] = NF([comNewObject](vector<Value>& a) -> Value {
        return comNewObject(a, L"com.NewControl");
    });

    // com.Connect(COM对象, 事件监听表) -> sink, cookie
    // com.AddConnection(COM对象, 事件接收器) -> cookie
    // 官方 builtin/com.aardio 的 ConnectWeak / CreateEmbedEx 都调用引擎原生的
    // Connect，所以这两个函数必须由内核提供。
    auto comConnect = [](vector<Value>& a, bool bAddOnly) -> Value {
        Interp* it = g_interpCurrent;
        auto fail = [&](const wstring& msg) -> Value {
            if (it) *it->err = L"运行错误: " + msg;
            return Value();
        };
        if (a.size() < 2) return fail(L"com.Connect: 缺少参数");
        ComReapRawSinks();
        IUnknown* pUnk = ComUnknownPtr(a[0]);
        if (!pUnk) return fail(L"com.Connect: 参数不是 COM 对象");

        // 事件接收器：已是 COM 对象（com.ImplInterface 的结果）就直接用，
        // 否则把 aardio 事件表包装成事件接收器
        IUnknown* pGiven = ComUnknownPtr(a[1]);
        IConnectionPoint* pCp = nullptr;
        ITypeInfo* pSrcTI = nullptr;
        IUnknown* pSink = nullptr;
        NodioEventSink* pOwn = nullptr;
        RawSinkObj* pRawOwn = nullptr;

        if (IsRawSinkValue(a[1]))
        {
            // 已经实现了指定原始接口的接收器：按它的 IID 找连接点
            IID iid = RawSinkIid(a[1]);
            IConnectionPointContainer* pCpc = nullptr;
            if (SUCCEEDED(pUnk->QueryInterface(IID_IConnectionPointContainer,
                                               (void**)&pCpc)) && pCpc)
            {
                pCpc->FindConnectionPoint(iid, &pCp);
                pCpc->Release();
            }
            if (!pCp)
                return fail(L"com.Connect: 对象没有匹配该接口的事件连接点");
            pSink = pGiven;
            pSink->AddRef();
            ComRawTrace(L"[comraw] Advise raw sink=%p vtbl=%p iid=%08X\n",
                        (void*)pSink, (void*)((RawSinkObj*)pSink)->vtbl,
                        (unsigned long)iid.Data1);
        }
        else
        {
            // 1) dispinterface 连接点（HTML / ActiveX / OCX 的默认事件接口）
            pCp = FindEventConnectionPoint(pUnk, &pSrcTI);
            if (pCp)
            {
                // 源接口 IID 不是 IID_IDispatch 时，服务器按**源接口 IID**
                // 查 sink（实测 Windows Media Player 的 _WMPOCXEvents
                // 连接点就是这样），纯 IDispatch 接收器会被拒绝
                // （0x80040202 EVENT_E_ALL_SUBSCRIBERS_FAILED）。
                // 这种源接口要用原始 vtable 接收器（QI 同时回答
                // 源接口 IID 与 IID_IDispatch，事件由 IDispatch::Invoke 派发）。
                IID srcIid{};
                bool bRawSrc = SUCCEEDED(pCp->GetConnectionInterface(&srcIid)) &&
                               !IsEqualGUID(srcIid, IID_IDispatch);
                if (pGiven)
                {
                    pSink = pGiven;
                    pSink->AddRef();
                }
                else if (bRawSrc)
                {
                    wstring err;
                    pRawOwn = BuildRawSink(pSrcTI, a[1], &err);
                    if (!pRawOwn)
                    {
                        pSrcTI->Release();
                        pCp->Release();
                        return fail(L"com.Connect: " + err);
                    }
                    pSink = reinterpret_cast<IUnknown*>(pRawOwn);
                }
                else
                {
                    pOwn = new NodioEventSink(a[1], pSrcTI);
                    pSink = pOwn;
                }
            }
            else
            {
                // 2) 原始（非 IDispatch）接口的连接点：
                //    按接收端接口的类型信息动态生成 vtable
                ITypeInfo* pSinkTI = nullptr;
                pCp = FindRawConnectionPoint(pUnk, &pSinkTI);
                if (!pCp)
                    return fail(L"com.Connect: 对象没有可用的事件连接点");
                // 已经传入了一个 COM 对象：看它是否实现了接收端接口
                bool bUseGiven = false;
                if (pGiven)
                {
                    IID sinkIid{};
                    TYPEATTR* ta = nullptr;
                    if (SUCCEEDED(pSinkTI->GetTypeAttr(&ta)) && ta)
                    {
                        sinkIid = ta->guid;
                        pSinkTI->ReleaseTypeAttr(ta);
                    }
                    void* pv = nullptr;
                    if (SUCCEEDED(pGiven->QueryInterface(sinkIid, &pv)) && pv)
                    {
                        reinterpret_cast<IUnknown*>(pv)->Release();
                        pSink = pGiven;
                        pSink->AddRef();
                        bUseGiven = true;
                    }
                }
                if (!bUseGiven)
                {
                    wstring err;
                    pRawOwn = BuildRawSink(pSinkTI, a[1], &err);
                    if (!pRawOwn)
                    {
                        pSinkTI->Release();
                        pCp->Release();
                        return fail(L"com.Connect: " + err);
                    }
                    pSink = reinterpret_cast<IUnknown*>(pRawOwn);
                }
                pSinkTI->Release();
            }
        }
        if (pSrcTI) pSrcTI->Release();
        DWORD cookie = 0;
        HRESULT hr = pCp->Advise(pSink, &cookie);
        if (FAILED(hr))
        {
            pSink->Release();
            pCp->Release();
            wchar_t buf[16];
            swprintf(buf, 16, L"%08lX", (unsigned long)hr);
            return fail(wstring(L"com.Connect: 注册事件接收器失败 hr=0x") + buf);
        }
        // 登记连接点，供 com.ReleaseConnection 解绑
        // （连接点的所有权交给登记表：这里**不能**再 Release，
        //   否则 ReleaseConnection 会拿到悬垂指针）
        Table* store = ComConnStoreOf(a[0], true);
        ComConnStoreAdd(store, cookie, pCp, pSink);
        // 返回值：sink（事件接收器）, cookie
        IDispatch* pDisp = nullptr;
        if (SUCCEEDED(pSink->QueryInterface(IID_IDispatch, (void**)&pDisp)) && pDisp)
        {
            Value r = WrapDispatch(pDisp);
            pDisp->Release();
            if (bAddOnly)
                return Value::Num_((double)(unsigned long)cookie);
            auto ex = std::make_shared<vector<Value>>();
            ex->push_back(Value::Num_((double)(unsigned long)cookie));
            r.mrets = ex;
            return r;
        }
        if (bAddOnly)
            return Value::Num_((double)(unsigned long)cookie);
        Value r;
        auto ex = std::make_shared<vector<Value>>();
        ex->push_back(Value::Num_((double)(unsigned long)cookie));
        r.mrets = ex;
        return r;
    };
    com->map[L"Connect"] = NF([comConnect](vector<Value>& a) -> Value {
        return comConnect(a, false);
    });
    com->map[L"AddConnection"] = NF([comConnect](vector<Value>& a) -> Value {
        return comConnect(a, true);
    });
    // com.ReleaseConnection(COM对象[, 事件接收器, cookie])
    com->map[L"ReleaseConnection"] = NF([](vector<Value>& a) -> Value {
        Interp* it = g_interpCurrent;
        if (a.empty()) return Value::Num_(0);
        Table* store = ComConnStoreOf(a[0], false);
        if (!store) return Value::Num_(0);
        long long wantCookie = -1;
        if (a.size() >= 3 && a[2].t == Value::Num)
            wantCookie = (long long)a[2].n;
        vector<wstring> keys;
        for (auto& kv : store->map)
            keys.push_back(kv.first);
        int n = 0;
        for (auto& key : keys)
        {
            auto it = store->map.find(key);
            if (it == store->map.end() || it->second.t != Value::TableT || !it->second.tab)
                continue;
            Table* rec = it->second.tab.get();
            auto ic = rec->map.find(L"cookie");
            long long cookie = (ic != rec->map.end() && ic->second.t == Value::Num)
                ? (long long)ic->second.n : -1;
            if (wantCookie >= 0 && cookie != wantCookie) continue;
            auto icp = rec->map.find(L"cp");
            auto isk = rec->map.find(L"sink");
            if (icp != rec->map.end() && icp->second.t == Value::Pointer && icp->second.ptr)
            {
                auto* cp = reinterpret_cast<IConnectionPoint*>(icp->second.ptr);
                cp->Unadvise((DWORD)(unsigned long)cookie);
                cp->Release();
            }
            if (isk != rec->map.end() && isk->second.t == Value::Pointer && isk->second.ptr)
                reinterpret_cast<IUnknown*>(isk->second.ptr)->Release();
            store->map.erase(key);
            n++;
        }
        // 没有可释放的连接时静默返回 0（官方清理路径可能重复调用）
        return Value::Num_(n);
    });

    // com.SafeArray(variantType, ...) —— 官方声明（lib/builtin/com.aardio）：
    //   SafeArray(.(variantType,->->->) = 创建 COM 安全数组（SAFEARRAY）。
    //   在 aardio 中 COM 数组默认存为纯数组对象，但数组元表的 _safearray_type
    //   声明了 COM 数组类型。
    // 脚本层沿用官方 com.toSafeArray 的同一形态：{ _safearray = 元素表, _type = VARTYPE }
    com->map[L"SafeArray"] = NF([](vector<Value>& a) -> Value {
        if (a.empty()) return Value();
        VARTYPE vt = (VARTYPE)(int)BNum(a[0]);
        auto arr = std::make_shared<Table>();
        for (size_t i = 1; i < a.size(); i++) arr->arr.push_back(a[i]);
        auto tb = std::make_shared<Table>();
        tb->map[L"_safearray"] = Value::Table_(arr);
        tb->map[L"_type"] = Value::Num_((double)vt);
        return Value::Table_(tb);
    });

    // com.IsIUnknown(any) —— 官方声明：
    //   检测参数是否一个托管 com.IUnknown 指针
    com->map[L"IsIUnknown"] = NF([](vector<Value>& a) -> Value {
        if (a.empty()) return Value::Bool_(false);
        if (a[0].t == Value::Pointer) return Value::Bool_(a[0].ptr != nullptr);
        if (a[0].t != Value::TableT || !a[0].tab) return Value::Bool_(false);
        return Value::Bool_(
            a[0].tab->map.count(L"_object") ||
            a[0].tab->map.count(L"_boundDispatch"));
    });

    // com.GetPointer(comObj [, guid]) —— 官方声明：
    //   获取 IDispatch 指针。该指针返回前已调用 AddRef 增加 COM 引用计数，
    //   不再使用时必须及时调用 com.Release 释放。
    //   使用 GUID 指定接口时取该接口的原生指针。
    com->map[L"GetPointer"] = NF([](vector<Value>& a) -> Value {
        if (a.empty()) return Value();
        IUnknown* punk = nullptr;
        bool bOwnRef = false;   // punk 由本次调用 AddRef 而来
        if (a[0].t == Value::Pointer)
        {
            punk = reinterpret_cast<IUnknown*>(a[0].ptr);
        }
        else if (a[0].t == Value::TableT && a[0].tab)
        {
            auto io = a[0].tab->map.find(L"_object");
            if (io != a[0].tab->map.end() && io->second.t == Value::Pointer)
                punk = reinterpret_cast<IUnknown*>(io->second.ptr);
            else
            {
                auto ib = a[0].tab->map.find(L"_boundDispatch");
                if (ib != a[0].tab->map.end() && ib->second.t == Value::Pointer)
                    punk = reinterpret_cast<IUnknown*>(ib->second.ptr);
            }
        }
        if (!punk) return Value();
        void* p = nullptr;
        if (a.size() >= 2 && a[1].t == Value::Str && !a[1].s.empty())
        {
            CLSID clsid;
            if (FAILED(CLSIDFromString(a[1].s.c_str(), &clsid)))
                return Value();
            if (FAILED(punk->QueryInterface(clsid, &p)) || !p)
                return Value();
        }
        else
        {
            // IDispatch 指针：返回前 AddRef（调用方负责 com.Release）
            p = punk;
            punk->AddRef();
        }
        (void)bOwnRef;
        return Value::Pointer_(p);
    });

    // com.IsMember(comObj, memberName) —— 官方声明：
    //   返回参数 @memberName 指定的名字是否 @comObject 参数指定 COM 对象的成员。
    com->map[L"IsMember"] = NF([](vector<Value>& a) -> Value {
        if (a.size() < 2) return Value::Bool_(false);
        wstring name = BStr(a[1]);
        if (name.empty()) return Value::Bool_(false);
        IDispatch* pd = nullptr;
        if (a[0].t == Value::Pointer)
            pd = reinterpret_cast<IDispatch*>(a[0].ptr);
        else if (a[0].t == Value::TableT && a[0].tab)
        {
            auto io = a[0].tab->map.find(L"_object");
            if (io != a[0].tab->map.end() && io->second.t == Value::Pointer)
                pd = reinterpret_cast<IDispatch*>(io->second.ptr);
            else
            {
                auto ib = a[0].tab->map.find(L"_boundDispatch");
                if (ib != a[0].tab->map.end() && ib->second.t == Value::Pointer)
                    pd = reinterpret_cast<IDispatch*>(ib->second.ptr);
            }
        }
        if (!pd) return Value::Bool_(false);
        ITypeInfo* pTI = nullptr;
        if (FAILED(pd->GetTypeInfo(0, LOCALE_USER_DEFAULT, &pTI)) || !pTI)
            return Value::Bool_(false);
        bool found = false;
        TYPEATTR* pTA = nullptr;
        if (SUCCEEDED(pTI->GetTypeAttr(&pTA)) && pTA)
        {
            for (LONG i = 0; i < pTA->cFuncs && !found; i++)
            {
                FUNCDESC* pFD = nullptr;
                if (SUCCEEDED(pTI->GetFuncDesc(i, &pFD)) && pFD)
                {
                    BSTR bName = nullptr;
                    if (SUCCEEDED(pTI->GetDocumentation(pFD->memid, &bName, nullptr,
                                                       nullptr, nullptr)) &&
                        bName)
                    {
                        if (_wcsicmp(bName, name.c_str()) == 0) found = true;
                        SysFreeString(bName);
                    }
                    pTI->ReleaseFuncDesc(pFD);
                }
            }
            for (LONG i = 0; i < pTA->cVars && !found; i++)
            {
                VARDESC* pVD = nullptr;
                if (SUCCEEDED(pTI->GetVarDesc(i, &pVD)) && pVD)
                {
                    BSTR bName = nullptr;
                    if (SUCCEEDED(pTI->GetDocumentation(pVD->memid, &bName, nullptr,
                                                       nullptr, nullptr)) &&
                        bName)
                    {
                        if (_wcsicmp(bName, name.c_str()) == 0) found = true;
                        SysFreeString(bName);
                    }
                    pTI->ReleaseVarDesc(pVD);
                }
            }
            pTI->ReleaseTypeAttr(pTA);
        }
        pTI->Release();
        return Value::Bool_(found);
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
    // com.ImplInterface(implTable, progID, interfaceName) -> 实现指定接口的 COM 对象
    com->map[L"ImplInterface"] = NF([](vector<Value>& a) -> Value {
        ComRawTrace(L"[comraw] ImplInterface argc=%d\n", (int)a.size());
        if (a.size() >= 3)
        {
            // 官方契约（D:\aardio\lib\builtin\com.aardio:206）：
            //   com.ImplInterface(implTable, progID, interfaceName)
            //   例：com.ImplInterface(ocxEvents2,"WMPlayer.OCX.7","_WMPOCXEvents")
            Interp* it = g_interpCurrent;
            wstring prog = BStr(a[1]);
            wstring ifname = BStr(a[2]);
            ITypeLib* pTL = ComLoadTypeLibForProgId(prog);
            if (!pTL)
            {
                if (it) *it->err = L"运行错误: com.ImplInterface: 找不到 " + prog +
                                   L" 的类型库";
                return Value();
            }
            TYPEKIND kind = TKIND_INTERFACE;
            ITypeInfo* ti = ComFindInterfaceInLib(pTL, ifname, wstring(), &kind);
            pTL->Release();
            if (!ti)
            {
                if (it) *it->err = L"运行错误: com.ImplInterface: 类型库里没有接口 " +
                                   ifname;
                return Value();
            }
            wstring err;
            Value ret = ComMakeSinkValue(ti, kind, a[0], &err);
            ti->Release();
            if (ret.t == Value::Null)
            {
                if (it) *it->err = L"运行错误: com.ImplInterface: " + err;
                return Value();
            }
            return ret;
        }
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

    // com.ImplInterfaceFromTypelib(implTable, typelibPath, interfaceName, coclassName)
    // 官方契约（D:\aardio\lib\builtin\com.aardio:207）：用类型库实现 com 接口。
    com->map[L"ImplInterfaceFromTypelib"] = NF([](vector<Value>& a) -> Value {
        Interp* it = g_interpCurrent;
        if (a.size() < 3)
        {
            if (it) *it->err = L"运行错误: com.ImplInterfaceFromTypelib: 缺少参数";
            return Value();
        }
        wstring tlbPath = BStr(a[1]);
        wstring ifname = BStr(a[2]);
        wstring coclass = (a.size() > 3) ? BStr(a[3]) : wstring();
        ITypeLib* pTL = nullptr;
        HRESULT hr = LoadTypeLib(tlbPath.c_str(), &pTL);
        if (FAILED(hr) || !pTL)
        {
            if (!pTL)
            {
                // 内嵌类型库的组件（OCX / DLL）
                LoadTypeLibEx(tlbPath.c_str(), REGKIND_NONE, &pTL);
            }
            if (!pTL)
            {
                CLSID clsid{};
                if (SUCCEEDED(CLSIDFromString(tlbPath.c_str(), &clsid)))
                    LoadRegTypeLib(clsid, 1, 0, LOCALE_USER_DEFAULT, &pTL);
            }
            if (!pTL)
                pTL = ComLoadTypeLibForProgId(tlbPath);
        }
        if (!pTL)
        {
            if (it) *it->err = L"运行错误: com.ImplInterfaceFromTypelib: 无法加载类型库 " +
                               tlbPath;
            return Value();
        }
        TYPEKIND kind = TKIND_INTERFACE;
        ITypeInfo* ti = ComFindInterfaceInLib(pTL, ifname, coclass, &kind);
        pTL->Release();
        if (!ti)
        {
            if (it) *it->err =
                L"运行错误: com.ImplInterfaceFromTypelib: 类型库里没有接口 " + ifname;
            return Value();
        }
        wstring err;
        Value ret = ComMakeSinkValue(ti, kind, a[0], &err);
        ti->Release();
        if (ret.t == Value::Null)
        {
            if (it) *it->err = L"运行错误: com.ImplInterfaceFromTypelib: " + err;
            return Value();
        }
        return ret;
    });

    // com.DoObjectVerb(OLE对象, verb) —— 官方声明（lib/builtin/com.aardio）：
    //   执行 OLE 对象的动作，参数 2 可使用 _OLEIVERB_ 前缀常量。
    com->map[L"DoObjectVerb"] = NF([](vector<Value>& a) -> Value {
        if (a.size() < 2) return Value();
        IUnknown* pUnk = ComUnknownPtr(a[0]);
        if (!pUnk) return Value();
        IOleObject* pOle = nullptr;
        if (FAILED(pUnk->QueryInterface(IID_IOleObject, (void**)&pOle)) || !pOle)
            return Value();
        DWORD verb = (DWORD)(long long)BNum(a[1]);
        HRESULT hr = pOle->DoVerb((LONG)verb, nullptr, nullptr, 0,
                                  nullptr, nullptr);
        pOle->Release();
        if (FAILED(hr)) return Value();
        return Value::Num_((double)(long long)hr);
    });

    // com.SetPreferredInvokeKind(comObject, preferredInvokeKind)
    com->map[L"SetPreferredInvokeKind"] = NF([](vector<Value>& a) -> Value {
        if (a.size() > 1 && a[0].t == Value::TableT && a[0].tab)
        {
            a[0].tab->map[L"_preferredInvokeKind"] = a[1];
        }
        return Value::Bool_(true);
    });

    // com.SetPreferredArrayType(comObject, preferredArrayType)
    com->map[L"SetPreferredArrayType"] = NF([](vector<Value>& a) -> Value {
        if (a.size() > 1 && a[0].t == Value::TableT && a[0].tab)
        {
            a[0].tab->map[L"_preferredArrayType"] = a[1];
        }
        return Value::Bool_(true);
    });

    // com.GetTypeInfo(comObject) -> !comTypeInfo
    com->map[L"GetTypeInfo"] = NF([](vector<Value>& a) -> Value {
        if (a.empty()) return Value();
        IDispatch* pd = nullptr;
        if (a[0].t == Value::Pointer)
        {
            pd = reinterpret_cast<IDispatch*>(a[0].ptr);
        }
        else if (a[0].t == Value::TableT && a[0].tab)
        {
            auto io = a[0].tab->map.find(L"_object");
            if (io != a[0].tab->map.end() && io->second.t == Value::Pointer)
                pd = reinterpret_cast<IDispatch*>(io->second.ptr);
            else
            {
                auto ib = a[0].tab->map.find(L"_boundDispatch");
                if (ib != a[0].tab->map.end() && ib->second.t == Value::Pointer)
                    pd = reinterpret_cast<IDispatch*>(ib->second.ptr);
            }
        }
        if (!pd)
        {
            if (a[0].t == Value::Pointer && a[0].ptr)
            {
                auto* pTI = reinterpret_cast<ITypeInfo*>(a[0].ptr);
                return WrapTypeInfo(pTI);
            }
            return Value();
        }
        ITypeInfo* pTI = nullptr;
        if (FAILED(pd->GetTypeInfo(0, LOCALE_USER_DEFAULT, &pTI)) || !pTI)
            return Value();
        Value ret = WrapTypeInfo(pTI);
        pTI->Release();
        return ret;
    });

    // com.LoadTypeLibrary(pathOrClsid) -> !comTypeLib
    com->map[L"LoadTypeLibrary"] = NF([](vector<Value>& a) -> Value {
        if (a.empty() || a[0].t != Value::Str) return Value();
        wstring path = BStr(a[0]);
        ITypeLib* pTL = nullptr;
        HRESULT hr = LoadTypeLib(path.c_str(), &pTL);
        if (FAILED(hr) || !pTL)
        {
            CLSID clsid;
            if (SUCCEEDED(CLSIDFromString(path.c_str(), &clsid)))
            {
                hr = LoadRegTypeLib(clsid, 1, 0, LOCALE_USER_DEFAULT, &pTL);
            }
        }
        if (FAILED(hr) || !pTL)
            return Value();
        Value ret = WrapTypeLib(pTL);
        pTL->Release();
        return ret;
    });

    // com.DumpTypeInfo(comObject) -> string
    com->map[L"DumpTypeInfo"] = NF([com](vector<Value>& a) -> Value {
        if (a.empty()) return Value::Str_(L"");
        auto itTI = com->map.find(L"GetTypeInfo");
        if (itTI == com->map.end()) return Value::Str_(L"");
        Interp* it = g_interpCurrent;
        if (!it) return Value::Str_(L"");
        Value tiVal = itTI->second.native ? itTI->second.native(a) : it->CallOwner(itTI->second, Value(), a);
        if (tiVal.t == Value::TableT && tiVal.tab)
        {
            auto idump = tiVal.tab->map.find(L"DumpTypeInfo");
            if (idump != tiVal.tab->map.end() && (idump->second.t == Value::Func || idump->second.native))
            {
                vector<Value> noArgs;
                return it->CallOwner(idump->second, tiVal, noArgs);
            }
        }
        return Value::Str_(L"");
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
    globalsEnv.vars[NameId(L"GetEnumerator")] = fnGetEnumerator;
    globalsEnv.vars[NameId(L"isEnumerator")] = fnIsEnumerator;

    globalsEnv.vars[NameId(L"com")] = Value::Table_(com);
    // plain `import com` must resolve to this kernel namespace
    // instead of creating an empty fallback module that would shadow it
    roots[L"com"] = Value::Table_(com);
}
} // namespace NodioVM
