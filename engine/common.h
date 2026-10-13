// common.h — shared engine types, helper declarations and small inline
// utilities. Every engine translation unit includes this; the interpreter
// object itself is declared in interp.h, the Parser in parser.h.
#pragma once

#include "pch.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <algorithm>
#include <functional>
#include <string>
#include <map>
#include <set>
#include <thread>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <memory>
#include <wininet.h>

namespace NodioVM
{
    using std::wstring;
    using std::shared_ptr;
    using std::vector;

    // ---------------- lexer ----------------
    enum TokType
    {
        TEnd, TNL, TNum, TStr, TIdent, TKw,
        // punct / operators
        TPunct
    };

    struct Tok
    {
        TokType t = TEnd;
        wstring text;        // ident/keyword/string value/number text
        double num = 0;
        int line = 1;
    };

    // aardio compiled (bytecode) modules begin with the literal '=aardio'
    // followed by a binary body — lib/ide/aifix.aardio ships this way. Such
    // a file is not source text, so a source-syntax check does not apply;
    // loading it as source must report a format error rather than a
    // misleading syntax error.
    inline bool IsCompiledModule(const wstring& src)
    {
        return src.size() >= 7 && src.compare(0, 7, L"=aardio") == 0;
    }

    inline const wchar_t* const KEYWORDS[] = {
        L"var", L"function", L"if", L"else", L"elseif", L"while", L"for",
        L"in", L"break", L"continue", L"return", L"null", L"true", L"false",
        L"and", L"or", L"not", L"do", L"end", L"import", L"namespace",
        L"try", L"catch", L"class", L"lambda", L"select", L"case", L"with",
        nullptr
    };

    struct Node;
    using PNode = shared_ptr<Node>;

    struct Value;
    struct Table
    {
        std::unordered_map<wstring, Value> map;
        vector<Value> arr;
        std::unordered_map<wstring, Value> scopeVars; // private namespace/module scope variables (var x)
    };

    struct Value
    {
        enum T { Null, Bool, Num, Str, Func, TableT, Class, Pointer } t = Null;
        bool b = false;
        double n = 0;
        wstring s;
        void* ptr = nullptr;
        PNode fn;                       // function literal body
        PNode classBody;                // class body statement block
        vector<wstring> params;
        wstring fname;                  // debug
        shared_ptr<Table> tab;
        shared_ptr<Table> nsClosure;    // defining namespace (aardio upvalue)
        shared_ptr<vector<PNode>> pdefaults;  // FuncLit param defaults
        shared_ptr<vector<Value>> mrets;      // aardio extra return values
        shared_ptr<Value> thisVal;            // lexical this captured at function definition
        // native builtin (round 19): C++-backed callable surfaced as a Func
        using NativeFn = std::function<Value(std::vector<Value>&)>;
        NativeFn native;
        static Value Num_(double v) { Value x; x.t = Num; x.n = v; return x; }
        static Value Bool_(bool v) { Value x; x.t = Bool; x.b = v; return x; }
        static Value Str_(const wstring& v) { Value x; x.t = Str; x.s = v; return x; }
        static Value Table_(shared_ptr<Table> tb) { Value x; x.t = TableT; x.tab = tb; return x; }
        static Value Native_(NativeFn f) { Value x; x.t = Func; x.native = std::move(f); return x; }
        static Value Pointer_(void* p) { Value x; x.t = Pointer; x.ptr = p; return x; }
        bool Truthy() const;
        wstring ToString() const;
    };

    // builtin-writing helpers (round 19)
    inline wstring BStr(const Value& v) { return v.ToString(); }
    inline double BNum(const Value& v)
    {
        if (v.t == Value::Num) return v.n;
        if (v.t == Value::Pointer) return (double)(__int64)(INT_PTR)v.ptr;
        if (v.t == Value::Str) { wchar_t* e = nullptr; return wcstod(v.s.c_str(), &e); }
        if (v.t == Value::Bool) return v.b ? 1 : 0;
        return 0;
    }
    // round 43: codepage argument — accepts a number (0=ACP, 65001=UTF-8,
    // 936=GBK, 54936=GB18030) or a name ("utf8"/"gbk"/"gb18030"/"ansi");
    // "" defaults to UTF-8
    inline UINT CodepageOf(const Value& v, UINT defCp = CP_UTF8)
    {
        if (v.t == Value::Num)
        {
            UINT cp = (UINT)(INT_PTR)(long long)v.n;
            if (cp == 0) return CP_ACP;
            return cp;
        }
        wstring w = v.t == Value::Null ? L"" : BStr(v);
        if (w.empty() || w == L"utf8" || w == L"utf-8") return CP_UTF8;
        if (w == L"ansi" || w == L"acp") return CP_ACP;
        if (w == L"gbk" || w == L"gb2312" || w == L"gb18030")
            return 54936;
        return defCp;
    }
    inline Value NF(Value::NativeFn f) { return Value::Native_(std::move(f)); }

    // round M2: struct field type names recognized in class/table member
    // declarations (int x; WORD name[32];)
    inline bool IsStructType(const wstring& w)
    {
        static const wchar_t* const kTypes[] = {
            L"int", L"INT", L"uint", L"UINT", L"byte", L"BYTE", L"word", L"WORD",
            L"dword", L"DWORD", L"long", L"LONG", L"int64", L"INT64", L"uint64", L"UINT64", L"long64",
            L"pointer", L"PTR", L"ptr", L"PTR", L"addr", L"ADDR", L"ustring",
            L"string", L"bool", L"BOOL", L"float", L"FLOAT", L"double", L"DOUBLE",
            L"struct", L"ustr", L"bstr", L"wstr", L"lpcstr", L"lpwstr",
            L"short", L"SHORT", L"ushort", L"USHORT", L"wchar", L"char", L"CHAR",
            L"uchar", L"UCHAR", L"int8", L"i64", L"qword", L"handle",
            L"hwnd", L"hkey", L"lpvoid", L"strb",
            nullptr };
        for (int k = 0; kTypes[k]; k++)
            if (w == kTypes[k]) return true;
        return false;
    }

    // ---------------- round 44: raw API (P/Invoke) ----------------
    //
    // raw.loadDll(path).api(name, "ret(p1,p2,...[,cdecl])") returns a
    // callable. Types: void int int64 word byte bool pointer str(A)
    // wstr(W) double. Param slots are integer-width; float/double params
    // are not supported in this level (the common user32/gdi32/kernel32
    // surface is integer+pointer). Default convention __stdcall;
    // ",cdecl" switches (no-op on x64).
#if defined(_M_IX86)
    typedef DWORD SlotT;
#else
    typedef unsigned __int64 SlotT;
#endif

    enum ApiRet
    {
        ARVoid, ARI64, ARInt, ARWord, ARByte, ARBool, ARDbl,
        ARStrA, ARStrW, ARPtr
    };
    struct ApiSig
    {
        ApiRet ret = ARI64;
        // type ids 0=int 1=int64 2=word 3=byte 4=bool 5=pointer 6=strA
        // 7=strW 8=struct; OR 0x100 = by-reference (&) out param
        vector<int> params;
        bool bCdecl = false;
        bool bRetFloat = false;   // declared "float" return (x64 XMM decode)
    };
    // aardio root for compile-time embed paths ("~/...") — parent of
    // NODIO_LIBDIR; defined near RunSource, used by DoImport
    struct Interp;   // (fwd — real struct defined later)
    struct CbCtx
    {
        Value fn;               // aardio function to invoke
        Value owner;            // owner/this receiver for callback invocation
        ApiSig sig;             // declared signature
        Interp* interp = nullptr;
        void* code = nullptr;   // executable trampoline
        size_t codeSize = 0;
    };
    extern std::vector<CbCtx*> g_cbs;

    // C++ shim shared by all trampolines. x86 __fastcall(ctx, stack):
    // caller args begin at stack+4 (after return address). x64
    // CallbackShim(ctx, rspAtEntry, xmm): integer args 1..4 were spilled
    // by the trampoline into the caller's shadow slots (stack+8..+32),
    // args 5+ live at stack+40 (8 bytes each); xmm points at four
    // 8-byte cells holding the bit patterns of XMM0..XMM3 taken before
    // any spilling.
    typedef __int64 (*CbDispatchFn)(CbCtx* ctx, vector<Value>& args);
    extern CbDispatchFn g_cbDispatch;   // set by InitBuiltins
    extern std::wstring g_curModuleFile;          // debug: module under exec
    template <typename R>
    inline R InvokeStd(void* fn, const vector<SlotT>& s)
    {
        switch (s.size())
        {
        case 0: { typedef R(__stdcall *FP)(); return ((FP)fn)(); }
        case 1: { typedef R(__stdcall *FP)(SlotT); return ((FP)fn)(s[0]); }
        case 2: { typedef R(__stdcall *FP)(SlotT, SlotT);
                  return ((FP)fn)(s[0], s[1]); }
        case 3: { typedef R(__stdcall *FP)(SlotT, SlotT, SlotT);
                  return ((FP)fn)(s[0], s[1], s[2]); }
        case 4: { typedef R(__stdcall *FP)(SlotT, SlotT, SlotT, SlotT);
                  return ((FP)fn)(s[0], s[1], s[2], s[3]); }
        case 5: { typedef R(__stdcall *FP)(SlotT, SlotT, SlotT, SlotT,
                                           SlotT);
                  return ((FP)fn)(s[0], s[1], s[2], s[3], s[4]); }
        case 6: { typedef R(__stdcall *FP)(SlotT, SlotT, SlotT, SlotT,
                                           SlotT, SlotT);
                  return ((FP)fn)(s[0], s[1], s[2], s[3], s[4], s[5]); }
        case 7: { typedef R(__stdcall *FP)(SlotT, SlotT, SlotT, SlotT,
                                           SlotT, SlotT, SlotT);
                  return ((FP)fn)(s[0], s[1], s[2], s[3], s[4], s[5],
                                  s[6]); }
        case 8: { typedef R(__stdcall *FP)(SlotT, SlotT, SlotT, SlotT,
                                           SlotT, SlotT, SlotT, SlotT);
                  return ((FP)fn)(s[0], s[1], s[2], s[3], s[4], s[5],
                                  s[6], s[7]); }
        case 9: { typedef R(__stdcall *FP)(SlotT, SlotT, SlotT, SlotT,
                                           SlotT, SlotT, SlotT, SlotT,
                                           SlotT);
                  return ((FP)fn)(s[0], s[1], s[2], s[3], s[4], s[5],
                                  s[6], s[7], s[8]); }
        case 10: { typedef R(__stdcall *FP)(SlotT, SlotT, SlotT, SlotT,
                                            SlotT, SlotT, SlotT, SlotT,
                                            SlotT, SlotT);
                   return ((FP)fn)(s[0], s[1], s[2], s[3], s[4], s[5],
                                   s[6], s[7], s[8], s[9]); }
        case 11: { typedef R(__stdcall *FP)(SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT);
                   return ((FP)fn)(s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[9], s[10]); }
        case 12: { typedef R(__stdcall *FP)(SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT);
                   return ((FP)fn)(s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[9], s[10], s[11]); }
        case 13: { typedef R(__stdcall *FP)(SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT);
                   return ((FP)fn)(s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[9], s[10], s[11], s[12]); }
        case 14: { typedef R(__stdcall *FP)(SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT);
                   return ((FP)fn)(s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[9], s[10], s[11], s[12], s[13]); }
        }
        return R();
    }

    template <typename R>
    inline R InvokeCdl(void* fn, const vector<SlotT>& s)
    {
        switch (s.size())
        {
        case 0: { typedef R(__cdecl *FP)(); return ((FP)fn)(); }
        case 1: { typedef R(__cdecl *FP)(SlotT); return ((FP)fn)(s[0]); }
        case 2: { typedef R(__cdecl *FP)(SlotT, SlotT);
                  return ((FP)fn)(s[0], s[1]); }
        case 3: { typedef R(__cdecl *FP)(SlotT, SlotT, SlotT);
                  return ((FP)fn)(s[0], s[1], s[2]); }
        case 4: { typedef R(__cdecl *FP)(SlotT, SlotT, SlotT, SlotT);
                  return ((FP)fn)(s[0], s[1], s[2], s[3]); }
        case 5: { typedef R(__cdecl *FP)(SlotT, SlotT, SlotT, SlotT,
                                         SlotT);
                  return ((FP)fn)(s[0], s[1], s[2], s[3], s[4]); }
        case 6: { typedef R(__cdecl *FP)(SlotT, SlotT, SlotT, SlotT,
                                         SlotT, SlotT);
                  return ((FP)fn)(s[0], s[1], s[2], s[3], s[4], s[5]); }
        case 7: { typedef R(__cdecl *FP)(SlotT, SlotT, SlotT, SlotT,
                                         SlotT, SlotT, SlotT);
                  return ((FP)fn)(s[0], s[1], s[2], s[3], s[4], s[5],
                                  s[6]); }
        case 8: { typedef R(__cdecl *FP)(SlotT, SlotT, SlotT, SlotT,
                                         SlotT, SlotT, SlotT, SlotT);
                  return ((FP)fn)(s[0], s[1], s[2], s[3], s[4], s[5],
                                  s[6], s[7]); }
        case 9: { typedef R(__cdecl *FP)(SlotT, SlotT, SlotT, SlotT,
                                         SlotT, SlotT, SlotT, SlotT,
                                         SlotT);
                  return ((FP)fn)(s[0], s[1], s[2], s[3], s[4], s[5],
                                  s[6], s[7], s[8]); }
        case 10: { typedef R(__cdecl *FP)(SlotT, SlotT, SlotT, SlotT,
                                          SlotT, SlotT, SlotT, SlotT,
                                          SlotT, SlotT);
                   return ((FP)fn)(s[0], s[1], s[2], s[3], s[4], s[5],
                                   s[6], s[7], s[8], s[9]); }
        case 11: { typedef R(__cdecl *FP)(SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT);
                   return ((FP)fn)(s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[9], s[10]); }
        case 12: { typedef R(__cdecl *FP)(SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT);
                   return ((FP)fn)(s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[9], s[10], s[11]); }
        case 13: { typedef R(__cdecl *FP)(SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT);
                   return ((FP)fn)(s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[9], s[10], s[11], s[12]); }
        case 14: { typedef R(__cdecl *FP)(SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT);
                   return ((FP)fn)(s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[9], s[10], s[11], s[12], s[13]); }
        }
        return R();
    }

#if defined(_WIN64)
    // Phase 1.1: per-signature x64 call stubs — the typed InvokeStd
    // pointers classify every argument as integer so float/double params
    // would land in RCX/RDX instead of XMM0..3. The stub is emitted once
    // per dll.api() bind: it loads integer-class params into
    // RCX,RDX,R8,R9 (then the stack just past the 32-byte shadow space),
    // float-class params into XMM0..3 (then 8-byte stack cells), calls
    // frame->fn, and funnels a float/double return from XMM0 back into
    // RAX so the C++ side can bit-cast it.
    //
    // frame layout (RCX at stub entry):
    //   [0x00] target function pointer
    //   [0x08 + 8*i] argument cell i — bit pattern as packed by the
    //   marshal code (integers scaled, floats/doubles IEEE bits)
    inline __int64 InvokeX64Stub(void* stub, void* fn, const vector<SlotT>& s)
    {
        vector<unsigned __int64> frame(1 + s.size());
        frame[0] = (unsigned __int64)(INT_PTR)fn;
        for (size_t i = 0; i < s.size(); i++)
            frame[1 + i] = (unsigned __int64)s[i];
        typedef __int64 (*StubFn)(void*);
        return ((StubFn)stub)(frame.data());
    }
#endif

    // reads a NUL-terminated string through a raw pointer
    inline wstring ReadStrA(const char* p)
    {
        if (!p) return L"";
        wstring out;
        while (*p)
        {
            out += (wchar_t)(unsigned char)*p;
            p++;
        }
        return out;
    }
    inline wstring ReadStrW(const wchar_t* p)
    {
        if (!p) return L"";
        wstring out;
        while (*p) { out += *p; p++; }
        return out;
    }

    // round 36: deep copy for thread boundaries — tables are duplicated so
    // the worker and the main VM never share mutable state (aardio: threads
    // receive value copies); functions/natives do not cross threads
    struct ThreadState
    {
        std::thread th;
        Value result;
        wstring error;
        bool done = false;
    };
    extern std::map<wstring, Value> g_threadSharedMap;
    extern std::mutex g_threadSharedMutex;
    struct RE
    {
        int k;                 // 0 char, 1 class, 2 dot, 3 group, 4 $ end
        wchar_t ch = 0;
        unsigned cls = 0;      // 1=\d 2=\D 4=\w 8=\W 16=\s 32=\S
        vector<std::pair<wchar_t, wchar_t>> ranges;
        bool neg = false;
        int gi = -1;           // capture index (group)
        vector<RE> sub;
        int min = 1, max = 1;  // max -1 = unbounded
        bool lazy = false;
    };

    inline constexpr size_t RENPOS = (size_t)-1;

    long long REMatchRep(const vector<RE>& els, size_t idx,
                         const wstring& s, size_t pos, int count,
                         vector<wstring>& caps, bool anchored);

    // regex.cpp — pattern engine surface used by string.match/replace etc.
    bool RECompile(const wstring& pat, vector<vector<RE>>& branches,
                   bool* pAnchored, int* pGroups, wstring* err);
    bool RETry(const vector<vector<RE>>& branches, const wstring& s,
               size_t pos, bool anchored, vector<wstring>& caps,
               size_t* pEnd);

    struct JParser
    {
        const wstring& s;
        size_t i;
        wstring* err;
        void Skip()
        {
            while (i < s.size() && iswspace(s[i])) i++;
        }
        bool Eat(wchar_t c)
        {
            Skip();
            if (i < s.size() && s[i] == c) { i++; return true; }
            return false;
        }
        Value Parse()
        {
            Value v = PVal();
            if (err->empty()) Skip();
            if (err->empty() && i < s.size())
                (*err) = L"trailing chars";
            return v;
        }
        Value PVal()
        {
            Skip();
            if (i >= s.size())
            {
                (*err) = L"eof";
                return Value();
            }
            wchar_t c = s[i];
            if (c == L'{') return PObj();
            if (c == L'[') return PArr();
            if (c == L'"') return Value::Str_(PStr());
            if (c == L't' && s.compare(i, 4, L"true") == 0)
            {
                i += 4;
                return Value::Bool_(true);
            }
            if (c == L'f' && s.compare(i, 5, L"false") == 0)
            {
                i += 5;
                return Value::Bool_(false);
            }
            if (c == L'n' && s.compare(i, 4, L"null") == 0)
            {
                i += 4;
                return Value();
            }
            return PNum();
        }
        wstring PStr()
        {
            i++;
            wstring out;
            while (i < s.size() && s[i] != L'"')
            {
                if (s[i] == L'\\' && i + 1 < s.size())
                {
                    i++;
                    wchar_t e = s[i];
                    switch (e)
                    {
                    case L'n': out += L'\n'; break;
                    case L'r': out += L'\r'; break;
                    case L't': out += L'\t'; break;
                    case L'b': out += L'\b'; break;
                    case L'f': out += L'\f'; break;
                    case L'/': out += L'/'; break;
                    case L'u':
                        if (i + 4 < s.size())
                        {
                            out += (wchar_t)wcstoul(s.substr(i + 1, 4).c_str(),
                                                    nullptr, 16);
                            i += 4;
                        }
                        break;
                    default: out += e; break;
                    }
                    i++;
                }
                else
                {
                    out += s[i];
                    i++;
                }
            }
            i++;
            return out;
        }
        Value PNum()
        {
            size_t b = i;
            while (i < s.size() && (iswdigit(s[i]) || s[i] == L'.' ||
                   s[i] == L'e' || s[i] == L'E' || s[i] == L'+' || s[i] == L'-'))
                i++;
            return Value::Num_(wcstod(s.substr(b, i - b).c_str(), nullptr));
        }
        Value PObj()
        {
            i++;
            auto tb = std::make_shared<Table>();
            if (Eat(L'}'))
                return Value::Table_(tb);
            while (true)
            {
                Skip();
                if (i >= s.size() || s[i] != L'"')
                {
                    (*err) = L"expect key";
                    return Value();
                }
                wstring k = PStr();
                if (!Eat(L':'))
                {
                    (*err) = L"expect :";
                    return Value();
                }
                tb->map[k] = PVal();
                if (Eat(L',')) continue;
                if (Eat(L'}')) break;
                (*err) = L"expect , or }";
                return Value();
            }
            return Value::Table_(tb);
        }
        Value PArr()
        {
            i++;
            auto tb = std::make_shared<Table>();
            if (Eat(L']'))
                return Value::Table_(tb);
            while (true)
            {
                tb->arr.push_back(PVal());
                if (Eat(L','))
                {
                    Skip();
                    if (i < s.size() && s[i] == L']') { i++; break; }
                    continue;
                }
                if (Eat(L']')) break;
                (*err) = L"expect , or ]";
                return Value();
            }
            return Value::Table_(tb);
        }
    };

    struct StructFieldDef
    {
        wstring name;
        wstring type;
        int arrCount = 1;
        bool bIsArray = false;
    };

    struct Env
    {
        Env* parent = nullptr;
        std::unordered_map<wstring, Value> vars;
        shared_ptr<Table> nsTable;   // namespace scope: bare names land here
        shared_ptr<Table> modTable;  // file/module scope table: scopeVars
        Value* Find(const wstring& k)
        {
            for (Env* e = this; e; e = e->parent)
            {
                auto it = e->vars.find(k);
                if (it != e->vars.end())
                    return &it->second;
                // aardio: `type` is a built-in primitive keyword/operator, never shadowed by namespace members
                if (e->nsTable && k != L"type")
                {
                    auto itS = e->nsTable->scopeVars.find(k);
                    if (itS != e->nsTable->scopeVars.end())
                        return &itS->second;
                    auto it2 = e->nsTable->map.find(k);
                    if (it2 != e->nsTable->map.end())
                        return &it2->second;
                }
                if (e->modTable && k != L"type")
                {
                    auto itS = e->modTable->scopeVars.find(k);
                    if (itS != e->modTable->scopeVars.end())
                        return &itS->second;
                }
            }
            return nullptr;
        }
    };

    struct Interp;
    extern Interp* g_interpCurrent;   // M2: active interp for meta hooks
    struct Node
    {
        enum K
        {
            Num, Str, Bool, NullK, Ident, Member, Index, Call, Bin, Un,
            FuncLit, TableCtor,
            ExprStmt, Assign, If, While, ForNum, ForIn, Try, Return, Break,
            Continue, VarDecl, Import, Namespace, Class, Block, With, DoWhile
        } k;
        double num = 0;
        wstring str;
        PNode a, b, c, d;
        vector<PNode> list;          // statements / args / pairs
        vector<wstring> names;       // params / var names
        vector<PNode> defaults;      // FuncLit: param default expressions
        bool bRoot = false;          // Member: '..' prefix
        bool bSafe = false;          // Member/Index: '?.' null-conditional
        bool bDirect = false;        // Index: [["k"]] skips metatables
        bool bMeta = false;          // Postfix: 'tab@' metatable access
        int line = 0;                // source line (round 30 error reporting)
    };

    // ---------------- parser ----------------

    struct FormState
    {
        Interp* it = nullptr;
        bool bModalDone = false;
        int nModalResult = 0;
        std::map<int, Value> callbacks;     // control id -> click dispatch
        std::map<int, Value> changes;       // control id -> change dispatch
        std::map<int, Value> selchgs;       // control id -> selchange dispatch
        std::map<int, Value> ctrls;         // control id -> control table
        std::map<int, HWND> hCtrl;          // control id -> handle
        int nNextId = 2000;
        // round 40: paint/timer — the form table is kept so WndProc can
        // read "onpaint"/"ontimer" live (scripts assign plain members)
        Value formVal;
        bool bInPaint = false;      // invalidate() re-entrancy guard
        int nNextTimer = 100;       // SetTimer ids (control ids start at 2000)
        // round 41: menu bar — command ids from 5000 (controls 2000+,
        // timers 100+), dispatched through the same WM_COMMAND handler
        HMENU hMenuBar = nullptr;
        std::map<int, Value> menus;         // command id -> dispatch stub
        std::map<int, Value> menuItems;     // command id -> item table
        int nNextMenuId = 5000;
    };
    extern std::map<HWND, FormState*> g_forms;
    LRESULT CALLBACK FormWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);


    // ---------------- module surface ----------------
    // lexer.cpp
    bool IsIdentChar(wchar_t c);
    vector<Tok> Lex(const wstring& src, wstring* strError);

    // util.cpp
    Value DeepCopy(const Value& v);
    wstring HttpFetch(const wstring& strUrl, const wchar_t* szMethod,
                      const wstring& strBody, const wstring& strCType,
                      wstring* strErr);
    void WriteU8(FILE* f, const wstring& s);

    // value.cpp
    // (Value::Truthy / Value::ToString defined there)

    // ffi.cpp
    int ApiTypeOf(const wstring& tRaw, bool* pByRef);
    bool ParseApiSig(const wstring& sig, ApiSig* out);
    void* EmitX64CallStub(const ApiSig& sig);
    void CallbackTrampoline(CbCtx* ctx);
    int GetStructFieldSize(const wstring& type, int arrCount,
                           bool bIsArray = false);
    std::vector<StructFieldDef> ParseStructDef(const Value& vTab);
    int CalcStructTotalSize(const Value& vTab);
    void* StructSerialize(const Value& vTab, int* pOutTotal = nullptr);
    void StructDeserialize(Value& vTab, const void* pBuf);

    // memdll.cpp — memory DLL loading (raw.loadDll with in-memory bytes).
    // MemDll owns the mapped image and its import dependencies; it must
    // outlive the module because free() releases everything.
    struct MemDll
    {
        void* pImage = nullptr;      // mapped image base
        size_t cbImage = 0;
        HMODULE hSelf = nullptr;     // same as pImage for a mapped module
        std::vector<HMODULE> vDeps;  // libraries loaded for imports
        bool bOk = false;
    };
    bool MemDllLoad(const wstring& bytes, void** phMod, MemDll* pOut);
    void MemDllUnload(MemDll* p);
    // Resolves an export from a manually mapped image by walking its
    // export directory. GetProcAddress only works for loader-mapped
    // modules, so memory DLLs must resolve this way. Returns null when
    // the image has no exports or the name is absent.
    FARPROC MemDllGetProc(void* pImage, const char* szName);

    // regex.cpp
    bool RECompile(const wstring& pat, vector<vector<RE>>& branches,
                   bool* pAnchored, int* pGroups, wstring* err);

    // json.cpp
    wstring JQuote(const wstring& str);
    wstring JWrite(const Value& v, int indent, int depth);

    // gdi.cpp
    struct GdiCtx;
    Value MakeGdiTable(const std::shared_ptr<GdiCtx>& g);
    bool RegisterFormClass();

    // entry.cpp
    wstring EmbedRoot();
    int CheckSyntax(const wstring& src, wstring* strError);
    int RunSource(const wstring& src, wstring* strError,
                  const wstring* pBaseDir,
                  const std::unordered_map<wstring, wstring>* pEmbedded,
                  bool bDebug, const wstring* pBreakLines);

    // Resolve an aardio '~'-rooted path to a real disk path. '~' is the
    // aardio install root: the parent of NODIO_LIBDIR when set (dev/F5),
    // otherwise the running exe's own directory (a published app). The
    // library loads bundled DLLs as raw.loadDll("~\lib\...") — resolving
    // '~' to the exe dir alone misses them.
    inline wstring ResolveTildeDisk(const wstring& strPath)
    {
        if (strPath.empty() || strPath[0] != L'~') return strPath;
        wstring rel = strPath.substr(1);
        while (!rel.empty() && (rel[0] == L'\\' || rel[0] == L'/'))
            rel = rel.substr(1);
        wstring root = EmbedRoot();
        if (root.empty())
        {
            wchar_t szExe[MAX_PATH] = L"";
            GetModuleFileNameW(nullptr, szExe, MAX_PATH);
            wchar_t* p = wcsrchr(szExe, L'\\');
            if (p) *p = 0;
            root = szExe;
        }
        return rel.empty() ? root : root + L"\\" + rel;
    }
}
