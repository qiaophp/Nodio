// autoComplete64.cpp - Nodio x64 clean-room implementation of autoComplete.dll
// Replaces official 32-bit autoComplete.dll for x64 processes.
// Exports CreateIDispatchObject(HWND hwndEdit, DWORD dwOptions) with cdecl convention.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ole2.h>
#include <shldisp.h>
#include <shlobj.h>
#include <vector>
#include <string>

// CEnumString: thread-safe IEnumString implementation wrapping std::vector<std::wstring>
class CEnumString : public IEnumString
{
private:
    LONG m_cRef;
    std::vector<std::wstring> m_strings;
    size_t m_nCurrent;
    CRITICAL_SECTION m_cs;

public:
    CEnumString() : m_cRef(1), m_nCurrent(0)
    {
        InitializeCriticalSection(&m_cs);
    }

    virtual ~CEnumString()
    {
        DeleteCriticalSection(&m_cs);
    }

    // IUnknown
    STDMETHOD(QueryInterface)(REFIID riid, void **ppvObject)
    {
        if (!ppvObject) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IEnumString)
        {
            *ppvObject = static_cast<IEnumString*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObject = NULL;
        return E_NOINTERFACE;
    }

    STDMETHOD_(ULONG, AddRef)()
    {
        return (ULONG)InterlockedIncrement(&m_cRef);
    }

    STDMETHOD_(ULONG, Release)()
    {
        LONG cRef = InterlockedDecrement(&m_cRef);
        if (cRef == 0)
        {
            delete this;
            return 0;
        }
        return (ULONG)cRef;
    }

    // IEnumString
    STDMETHOD(Next)(ULONG celt, LPOLESTR *rgelt, ULONG *pceltFetched)
    {
        if (!rgelt) return E_POINTER;
        if (celt > 1 && !pceltFetched) return E_POINTER;

        EnterCriticalSection(&m_cs);
        ULONG cFetched = 0;
        while (m_nCurrent < m_strings.size() && cFetched < celt)
        {
            const std::wstring& str = m_strings[m_nCurrent];
            size_t bytes = (str.length() + 1) * sizeof(WCHAR);
            LPOLESTR pCopy = (LPOLESTR)CoTaskMemAlloc(bytes);
            if (!pCopy)
            {
                LeaveCriticalSection(&m_cs);
                return E_OUTOFMEMORY;
            }
            memcpy(pCopy, str.c_str(), bytes);
            rgelt[cFetched] = pCopy;
            cFetched++;
            m_nCurrent++;
        }
        LeaveCriticalSection(&m_cs);

        if (pceltFetched) *pceltFetched = cFetched;
        return (cFetched == celt) ? S_OK : S_FALSE;
    }

    STDMETHOD(Skip)(ULONG celt)
    {
        EnterCriticalSection(&m_cs);
        m_nCurrent += celt;
        if (m_nCurrent > m_strings.size())
            m_nCurrent = m_strings.size();
        LeaveCriticalSection(&m_cs);
        return S_OK;
    }

    STDMETHOD(Reset)()
    {
        EnterCriticalSection(&m_cs);
        m_nCurrent = 0;
        LeaveCriticalSection(&m_cs);
        return S_OK;
    }

    STDMETHOD(Clone)(IEnumString **ppenum)
    {
        if (!ppenum) return E_POINTER;
        CEnumString *pNew = new CEnumString();
        if (!pNew) return E_OUTOFMEMORY;

        EnterCriticalSection(&m_cs);
        pNew->m_strings = m_strings;
        pNew->m_nCurrent = m_nCurrent;
        LeaveCriticalSection(&m_cs);

        *ppenum = pNew;
        return S_OK;
    }

    void SetStrings(const std::vector<std::wstring>& strings)
    {
        EnterCriticalSection(&m_cs);
        m_strings = strings;
        m_nCurrent = 0;
        LeaveCriticalSection(&m_cs);
    }
};

// CAutoCompleteObject: IDispatch object providing setStrings, setOption, enable, delete
class CAutoCompleteObject : public IDispatch
{
private:
    LONG m_cRef;
    HWND m_hwndEdit;
    IAutoComplete2* m_pAutoComplete;
    CEnumString* m_pEnumString;
    bool m_bInitialized;
    DWORD m_dwOptions;

public:
    CAutoCompleteObject()
        : m_cRef(1), m_hwndEdit(NULL), m_pAutoComplete(NULL),
          m_pEnumString(NULL), m_bInitialized(false), m_dwOptions(0)
    {
    }

    virtual ~CAutoCompleteObject()
    {
        Delete();
    }

    BOOL Initialize(HWND hwndEdit, DWORD dwOptions, LPCWSTR formatString)
    {
        if (!IsWindow(hwndEdit)) return FALSE;
        m_hwndEdit = hwndEdit;
        m_dwOptions = dwOptions;

        HRESULT hr = CoCreateInstance(CLSID_AutoComplete, NULL, CLSCTX_INPROC_SERVER,
                                      IID_IAutoComplete2, (void**)&m_pAutoComplete);
        if (FAILED(hr) || !m_pAutoComplete)
        {
            // Fallback to IAutoComplete if IAutoComplete2 is not available
            IAutoComplete* pAC1 = NULL;
            hr = CoCreateInstance(CLSID_AutoComplete, NULL, CLSCTX_INPROC_SERVER,
                                  IID_IAutoComplete, (void**)&pAC1);
            if (FAILED(hr) || !pAC1) return FALSE;
            // Query for IAutoComplete2 from AC1
            hr = pAC1->QueryInterface(IID_IAutoComplete2, (void**)&m_pAutoComplete);
            pAC1->Release();
            if (FAILED(hr) || !m_pAutoComplete) return FALSE;
        }

        m_pEnumString = new CEnumString();
        if (!m_pEnumString)
        {
            m_pAutoComplete->Release();
            m_pAutoComplete = NULL;
            return FALSE;
        }

        hr = m_pAutoComplete->Init(m_hwndEdit, m_pEnumString, NULL, formatString);
        if (FAILED(hr))
        {
            m_pEnumString->Release();
            m_pEnumString = NULL;
            m_pAutoComplete->Release();
            m_pAutoComplete = NULL;
            return FALSE;
        }

        m_pAutoComplete->SetOptions(m_dwOptions);
        m_pAutoComplete->Enable(TRUE);
        m_bInitialized = true;
        return TRUE;
    }

    void SetStrings(const VARIANT& items)
    {
        if (!m_bInitialized || !m_pEnumString) return;

        const VARIANT* pVar = &items;
        while (pVar->vt == (VT_BYREF | VT_VARIANT) && pVar->pvarVal)
        {
            pVar = pVar->pvarVal;
        }

        std::vector<std::wstring> stringList;

        if (pVar->vt & VT_ARRAY)
        {
            SAFEARRAY* psa = (pVar->vt & VT_BYREF) ? *(pVar->pparray) : pVar->parray;
            if (psa)
            {
                LONG lBound = 0, uBound = -1;
                SafeArrayGetLBound(psa, 1, &lBound);
                SafeArrayGetUBound(psa, 1, &uBound);

                VARTYPE vtElement;
                if (FAILED(SafeArrayGetVartype(psa, &vtElement)))
                {
                    vtElement = (pVar->vt & ~VT_ARRAY & ~VT_BYREF);
                }

                if (vtElement == VT_BSTR)
                {
                    for (LONG i = lBound; i <= uBound; i++)
                    {
                        BSTR bstr = NULL;
                        SafeArrayGetElement(psa, &i, &bstr);
                        if (bstr)
                        {
                            stringList.push_back(std::wstring(bstr, SysStringLen(bstr)));
                            SysFreeString(bstr);
                        }
                    }
                }
                else if (vtElement == VT_VARIANT)
                {
                    for (LONG i = lBound; i <= uBound; i++)
                    {
                        VARIANT var;
                        VariantInit(&var);
                        SafeArrayGetElement(psa, &i, &var);

                        VARIANT varStr;
                        VariantInit(&varStr);
                        if (SUCCEEDED(VariantChangeType(&varStr, &var, 0, VT_BSTR)) && varStr.bstrVal)
                        {
                            stringList.push_back(std::wstring(varStr.bstrVal, SysStringLen(varStr.bstrVal)));
                            VariantClear(&varStr);
                        }
                        VariantClear(&var);
                    }
                }
            }
        }

        m_pEnumString->SetStrings(stringList);
        if (m_pAutoComplete)
        {
            m_pEnumString->Reset();
        }
    }

    void SetOption(DWORD dwOptions)
    {
        if (!m_bInitialized || !m_pAutoComplete) return;
        m_dwOptions = dwOptions;
        m_pAutoComplete->SetOptions(m_dwOptions);
    }

    void Enable(BOOL bEnable)
    {
        if (!m_bInitialized || !m_pAutoComplete) return;
        m_pAutoComplete->Enable(bEnable);
    }

    void Delete()
    {
        if (m_pAutoComplete)
        {
            m_pAutoComplete->Enable(FALSE);
            m_pAutoComplete->Release();
            m_pAutoComplete = NULL;
        }
        if (m_pEnumString)
        {
            m_pEnumString->Release();
            m_pEnumString = NULL;
        }
        m_bInitialized = false;
    }

    // IUnknown
    STDMETHOD(QueryInterface)(REFIID riid, void **ppvObject)
    {
        if (!ppvObject) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDispatch)
        {
            *ppvObject = static_cast<IDispatch*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObject = NULL;
        return E_NOINTERFACE;
    }

    STDMETHOD_(ULONG, AddRef)()
    {
        return (ULONG)InterlockedIncrement(&m_cRef);
    }

    STDMETHOD_(ULONG, Release)()
    {
        LONG cRef = InterlockedDecrement(&m_cRef);
        if (cRef == 0)
        {
            delete this;
            return 0;
        }
        return (ULONG)cRef;
    }

    // IDispatch
    STDMETHOD(GetTypeInfoCount)(UINT *pctinfo)
    {
        if (!pctinfo) return E_POINTER;
        *pctinfo = 0;
        return S_OK;
    }

    STDMETHOD(GetTypeInfo)(UINT iTInfo, LCID lcid, ITypeInfo **ppTInfo)
    {
        return E_NOTIMPL;
    }

    STDMETHOD(GetIDsOfNames)(REFIID riid, LPOLESTR *rgszNames, UINT cNames, LCID lcid, DISPID *rgDispId)
    {
        if (!rgszNames || !rgDispId) return E_POINTER;
        HRESULT hr = S_OK;
        for (UINT i = 0; i < cNames; i++)
        {
            if (_wcsicmp(rgszNames[i], L"setStrings") == 0)
                rgDispId[i] = 1;
            else if (_wcsicmp(rgszNames[i], L"setOption") == 0)
                rgDispId[i] = 2;
            else if (_wcsicmp(rgszNames[i], L"enable") == 0)
                rgDispId[i] = 3;
            else if (_wcsicmp(rgszNames[i], L"delete") == 0)
                rgDispId[i] = 4;
            else
            {
                rgDispId[i] = DISPID_UNKNOWN;
                hr = DISP_E_UNKNOWNNAME;
            }
        }
        return hr;
    }

    STDMETHOD(Invoke)(DISPID dispIdMember, REFIID riid, LCID lcid, WORD wFlags,
                      DISPPARAMS *pDispParams, VARIANT *pVarResult,
                      EXCEPINFO *pExcepInfo, UINT *puArgErr)
    {
        if (pVarResult) VariantInit(pVarResult);

        switch (dispIdMember)
        {
        case 1: // setStrings(items)
        {
            if (!pDispParams || pDispParams->cArgs < 1) return DISP_E_BADPARAMCOUNT;
            // Parameters in DISPPARAMS are passed in reverse order
            VARIANT& arg = pDispParams->rgvarg[pDispParams->cArgs - 1];
            SetStrings(arg);
            return S_OK;
        }
        case 2: // setOption(dwOptions)
        {
            if (!pDispParams || pDispParams->cArgs < 1) return DISP_E_BADPARAMCOUNT;
            VARIANT& arg = pDispParams->rgvarg[pDispParams->cArgs - 1];
            VARIANT vDword;
            VariantInit(&vDword);
            if (SUCCEEDED(VariantChangeType(&vDword, &arg, 0, VT_UI4)))
            {
                SetOption(vDword.ulVal);
                VariantClear(&vDword);
                return S_OK;
            }
            return DISP_E_TYPEMISMATCH;
        }
        case 3: // enable(bEnable)
        {
            if (!pDispParams || pDispParams->cArgs < 1) return DISP_E_BADPARAMCOUNT;
            VARIANT& arg = pDispParams->rgvarg[pDispParams->cArgs - 1];
            VARIANT vBool;
            VariantInit(&vBool);
            if (SUCCEEDED(VariantChangeType(&vBool, &arg, 0, VT_BOOL)))
            {
                Enable(vBool.boolVal ? TRUE : FALSE);
                VariantClear(&vBool);
                return S_OK;
            }
            return DISP_E_TYPEMISMATCH;
        }
        case 4: // delete()
        {
            Delete();
            return S_OK;
        }
        default:
            return DISP_E_MEMBERNOTFOUND;
        }
    }
};

extern "C" __declspec(dllexport) IDispatch* __cdecl CreateIDispatchObject(HWND hwndEdit, DWORD dwOptions)
{
    if (!IsWindow(hwndEdit)) return NULL;

    CAutoCompleteObject* pObj = new CAutoCompleteObject();
    if (!pObj) return NULL;

    if (!pObj->Initialize(hwndEdit, dwOptions, NULL))
    {
        delete pObj;
        return NULL;
    }

    // Return with refcount = 1, transferred to the caller
    return pObj;
}
