// NodioVM.cpp — lexer, parser and evaluator for the aardio subset
// described in NodioVM.h. Tree-walking, reference-counted AST via
// shared_ptr; values are a small variant. Builtins cover io.print and
// scalar conversions.

#include "pch.h"
#include "engine/NodioVM.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <algorithm>
#include <functional>
#include <string>
#include <map>
#include <thread>
#include <mutex>
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

    static const wchar_t* KEYWORDS[] = {
        L"var", L"function", L"if", L"else", L"elseif", L"while", L"for",
        L"in", L"break", L"continue", L"return", L"null", L"true", L"false",
        L"and", L"or", L"not", L"do", L"end", L"import", L"namespace",
        L"try", L"catch", L"class", L"lambda", L"select", L"case", L"with",
        nullptr
    };

    static bool IsIdentChar(wchar_t c)
    {
        return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
               (c >= L'0' && c <= L'9') || c == L'_' || c == L'$' || c > 0x7F;
    }

    static vector<Tok> Lex(const wstring& src, wstring* strError)
    {
        vector<Tok> out;
        size_t i = 0, n = src.size();
        int line = 1;
        while (i < n)
        {
            wchar_t c = src[i];
            if (c == 0xFEFF) { i++; continue; }
            if (c == L'\r') { i++; continue; }
            if (c == L'\n') { out.push_back({ TNL, L"", 0, line++ }); i++; continue; }
            if (c == L' ' || c == L'\t') { i++; continue; }
            // comments
            if (c == L'/' && i + 1 < n && src[i + 1] == L'/')
            {
                while (i < n && src[i] != L'\n') i++;
                continue;
            }
            if (c == L'/' && i + 1 < n && src[i + 1] == L'*')
            {
                i += 2;
                // '/**intellisense()' doc blocks (shipped lib convention):
                // bodies contain '*/' (e.g. __/*param*/), so they end only
                // at the literal terminator 'end intellisense' + '**/'
                // '/**intellisense(...)' (two stars) = doc block ending at
                // 'end intellisense'; '/*intellisense(::)*/' = normal block
                size_t iStar = i;
                while (iStar < n && src[iStar] == L'*') iStar++;
                // '/**word(...)' doc blocks (intellisense/details/...)
                // contain plain '*/' inside; they end at 'end word**/'
                bool bDoc = (iStar > i && iStar < n &&
                             src[iStar] >= L'a' && src[iStar] <= L'z');
                if (bDoc)
                {
                    size_t wLen = 0;
                    while (iStar + wLen < n &&
                           ((src[iStar + wLen] >= L'a' &&
                             src[iStar + wLen] <= L'z') ||
                            src[iStar + wLen] == L'_')) wLen++;
                    std::wstring word = src.substr(iStar, wLen);
                    std::wstring endMark = L"end " + word;
                    while (i + (int)endMark.size() < n)
                    {
                        if (src[i] == L'\n') line++;
                        if (src[i] == L'e' &&
                            src.compare(i, (int)endMark.size(),
                                        endMark.c_str(),
                                        (int)endMark.size()) == 0)
                            break;
                        i++;
                    }
                    while (i + 1 < n && !(src[i] == L'*' && src[i + 1] == L'/'))
                    { if (src[i] == L'\n') line++; i++; }
                }
                else
                {
                    while (i + 1 < n && !(src[i] == L'*' && src[i + 1] == L'/'))
                    { if (src[i] == L'\n') line++; i++; }
                }
                i += 2;
                continue;
            }
            // strings
            if (c == L'"' || c == L'`')
            {
                // aardio raw string: 双引号 = 原样字符串，不处理转义符
                // (original docs: "\" is a literal backslash; `...` same)
                wchar_t q = c;
                i++;
                wstring s;
                while (i < n && src[i] != q)
                {
                    if (src[i] == L'\n') line++;
                    s += src[i++];
                }
                i++;  // closing quote
                if (i < n && src[i] == L'#')
                {
                    i++;
                    std::string u8;
                    int len = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
                    if (len > 0)
                    {
                        u8.resize(len);
                        WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &u8[0], len, nullptr, nullptr);
                    }
                    double val = 0;
                    for (size_t k = 0; k < u8.size() && k < 4; k++)
                        val += (double)(((unsigned int)(unsigned char)u8[k]) << (k * 8));
                    out.push_back({ TNum, std::to_wstring((long long)val), val, line });
                    continue;
                }
                out.push_back({ TStr, s, 0, line });
                continue;
            }
            if (c == L'\'')
            {
                // aardio escaped string: 单引号 = 编译时转义字符串,
                // C-like \ escapes ('\\' == "\"" per the original docs)
                wchar_t q = c;
                i++;
                wstring s;
                while (i < n && src[i] != q)
                {
                    if (src[i] == L'\\' && i + 1 < n)
                    {
                        wchar_t e = src[++i];
                        switch (e)
                        {
                        case L'n': s += L'\n'; break;
                        case L'r': s += L'\r'; break;
                        case L't': s += L'\t'; break;
                        case L'0': s += L'\0'; break;
                        case L'\\': s += L'\\'; break;
                        case L'"': s += L'"'; break;
                        case L'\'': s += L'\''; break;
                        default:
                            if (e == L'x' && i + 2 < n)
                            {
                                s += (wchar_t)wcstoul(src.substr(i + 1, 2).c_str(), nullptr, 16);
                                i += 2;
                            }
                            else
                            {
                                // unknown escape: keep verbatim (patterns
                                // rely on '\d'/'\w' surviving the literal)
                                s += L'\\';
                                s += e;
                            }
                        }
                        i++;
                    }
                    else
                    {
                        if (src[i] == L'\n') line++;
                        s += src[i++];
                    }
                }
                i++;  // closing quote
                if (i < n && src[i] == L'#')
                {
                    i++;
                    std::string u8;
                    int len = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
                    if (len > 0)
                    {
                        u8.resize(len);
                        WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &u8[0], len, nullptr, nullptr);
                    }
                    double val = 0;
                    for (size_t k = 0; k < u8.size() && k < 4; k++)
                        val += (double)(((unsigned int)(unsigned char)u8[k]) << (k * 8));
                    out.push_back({ TNum, std::to_wstring((long long)val), val, line });
                    continue;
                }
                out.push_back({ TStr, s, 0, line });
                continue;
            }
            // numbers
            if ((c >= L'0' && c <= L'9') ||
                (c == L'.' && i + 1 < n && src[i + 1] >= L'0' && src[i + 1] <= L'9'))
            {
                size_t b = i;
                if (c == L'0' && i + 1 < n && (src[i + 1] == L'x' || src[i + 1] == L'X'))
                {
                    i += 2;
                    while (i < n && iswxdigit(src[i])) i++;
                    Tok t{ TNum, src.substr(b, i - b), 0, line };
                    t.num = (double)wcstoul(src.substr(b + 2, i - b - 2).c_str(), nullptr, 16);
                    out.push_back(t);
                    continue;
                }
                // aardio radix literal: <radix>#<digits> (e.g. 2#100, 8#77, 16#FF, 36#Z)
                size_t digEnd = i;
                while (digEnd < n && src[digEnd] >= L'0' && src[digEnd] <= L'9') digEnd++;
                if (digEnd < n && src[digEnd] == L'#' && digEnd + 1 < n && iswalnum(src[digEnd + 1]))
                {
                    int radix = _wtoi(src.substr(b, digEnd - b).c_str());
                    if (radix >= 2 && radix <= 36)
                    {
                        i = digEnd + 1;
                        size_t valStart = i;
                        auto isValidRadix = [radix](wchar_t ch) {
                            if (ch >= L'0' && ch <= L'9') return (ch - L'0') < radix;
                            if (ch >= L'a' && ch <= L'z') return (ch - L'a' + 10) < radix;
                            if (ch >= L'A' && ch <= L'Z') return (ch - L'A' + 10) < radix;
                            return false;
                        };
                        while (i < n && isValidRadix(src[i])) i++;
                        wstring numStr = src.substr(valStart, i - valStart);
                        wchar_t* endp = nullptr;
                        double val = (double)_wcstoui64(numStr.c_str(), &endp, radix);
                        Tok t{ TNum, src.substr(b, i - b), val, line };
                        out.push_back(t);
                        continue;
                    }
                }
                while (i < n && ((src[i] >= L'0' && src[i] <= L'9') || src[i] == L'.')) i++;
                if (i < n && (src[i] == L'e' || src[i] == L'E'))
                {
                    i++;
                    if (i < n && (src[i] == L'+' || src[i] == L'-')) i++;
                    while (i < n && src[i] >= L'0' && src[i] <= L'9') i++;
                }
                Tok t{ TNum, L"", 0, line };
                t.num = wcstod(src.substr(b, i - b).c_str(), nullptr);
                out.push_back(t);
                continue;
            }
            // identifiers / keywords
            if (IsIdentChar(c))
            {
                size_t b = i;
                while (i < n && IsIdentChar(src[i])) i++;
                wstring w = src.substr(b, i - b);
                for (int k = 0; KEYWORDS[k]; k++)
                    if (w == KEYWORDS[k]) { out.push_back({ TKw, w, 0, line }); goto next; }
                out.push_back({ TIdent, w, 0, line });
            next:
                continue;
            }
            // operators (longest match first)
            struct { const wchar_t* op; } OPS[] = {
                L"<<=", L">>>", L">>=", L"...", L"::=", L":=", L"::", L"++", L"--",
                L"..", L"!==", L"**=", L"**",
                L"===", L"==", L"!=", L"<=", L">=", L"&&", L"||", L"+=", L"|=",
                L"&=", L"^=", L"<<", L">>", L"-=", L"*=", L"/=", L"%=",
                L"?.",
                nullptr
            };
            bool bMatched = false;
            for (int k = 0; OPS[k].op; k++)
            {
                if (wcscmp(OPS[k].op, L"?.") == 0 && i + 2 < n && src[i + 2] == L'.')
                    continue;
                size_t l = wcslen(OPS[k].op);
                if (src.compare(i, l, OPS[k].op) == 0)
                {
                    out.push_back({ TPunct, OPS[k].op, 0, line });
                    i += l;
                    bMatched = true;
                    break;
                }
            }
            if (bMatched) continue;
            out.push_back({ TPunct, wstring(1, c), 0, line });
            i++;
        }
        out.push_back({ TEnd, L"", 0, line });
        return out;
    }

    // ---------------- AST ----------------
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
    static wstring BStr(const Value& v) { return v.ToString(); }
    static double BNum(const Value& v)
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
    static UINT CodepageOf(const Value& v, UINT defCp = CP_UTF8)
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
    static Value NF(Value::NativeFn f) { return Value::Native_(std::move(f)); }

    // round M2: struct field type names recognized in class/table member
    // declarations (int x; WORD name[32];)
    static bool IsStructType(const wstring& w)
    {
        static const wchar_t* const kTypes[] = {
            L"int", L"INT", L"uint", L"UINT", L"byte", L"BYTE", L"word", L"WORD",
            L"dword", L"DWORD", L"long", L"LONG", L"int64", L"INT64", L"uint64", L"UINT64", L"long64",
            L"pointer", L"PTR", L"addr", L"ADDR", L"ustring",
            L"string", L"bool", L"BOOL", L"float", L"FLOAT", L"double", L"DOUBLE",
            L"struct", nullptr };
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
    static int ApiTypeOf(const wstring& tRaw, bool* pByRef)
    {
        // tokens arrive as "INT", "addr", "ustring&", "struct &lpRect",
        // "int & resultult" — normalize case, split off names, keep & flag
        wstring t = tRaw;
        for (auto& c : t)
            c = towlower(c);
        *pByRef = t.find(L'&') != wstring::npos;
        // first word = the type
        size_t k = 0;
        while (k < t.size() && (t[k] == L' ' || t[k] == L'\t' || t[k] == L'\r' || t[k] == L'\n')) k++;
        size_t e = k;
        while (e < t.size() && t[e] != L' ' && t[e] != L'\t' && t[e] != L'\r' && t[e] != L'\n' &&
               t[e] != L'&')
            e++;
        wstring w = t.substr(k, e - k);
        if (w == L"int" || w == L"long" || w == L"dword" || w == L"uint")
            return 0;
        if (w == L"int64" || w == L"longlong") return 1;
        if (w == L"word") return 2;
        if (w == L"byte") return 3;
        if (w == L"bool" || w == L"boolean") return 4;
        if (w == L"addr" || w == L"ptr" || w == L"pointer" ||
            w == L"handle") return 5;
        if (w == L"str" || w == L"strb" || w == L"string") return 6;
        if (w == L"wstr" || w == L"ustring") return 7;
        if (w == L"struct") return 8;
        if (w == L"float") return 9;
        if (w == L"double") return 10;
        return -1;
    }


    // ---- M3-c: callbacks — aardio function -> API function pointer ----
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
    static std::vector<CbCtx*> g_cbs;

    // C++ shim shared by all trampolines. x86 __fastcall(ctx, stack):
    // caller args begin at stack+4 (after return address). x64
    // CallbackShim(ctx, rspAtEntry, xmm): integer args 1..4 were spilled
    // by the trampoline into the caller's shadow slots (stack+8..+32),
    // args 5+ live at stack+40 (8 bytes each); xmm points at four
    // 8-byte cells holding the bit patterns of XMM0..XMM3 taken before
    // any spilling.
    typedef __int64 (*CbDispatchFn)(CbCtx* ctx, vector<Value>& args);
    static CbDispatchFn g_cbDispatch = nullptr;   // set by InitBuiltins
    static std::wstring g_curModuleFile;          // debug: module under exec
#if defined(_M_IX86)
    static __int64 __fastcall CallbackShim(CbCtx* ctx, unsigned char* stack)
#else
    static __int64 CallbackShim(CbCtx* ctx, unsigned char* stack,
                                unsigned char* xmm)
#endif
    {
        // pack declared params from the caller's frame
        vector<Value> args;
        size_t nSlots = ctx->sig.params.size();
#if defined(_M_IX86)
        unsigned char* pArg = stack + 4;   // past return address
        for (size_t i = 0; i < nSlots; i++)
        {
            int pt = ctx->sig.params[i] & 0xFF;
            if (pt == 1)
            {
                long long v = *(__int64*)pArg;
                args.push_back(Value::Num_((double)v));
                pArg += 8;
            }
            else
            {
                int v = *(int*)pArg;
                args.push_back(Value::Num_((double)v));
                pArg += 4;
            }
        }
#else
        // Microsoft x64 ABI: position-based register assignment — arg i<4
        // uses RCX/RDX/R8/R9 for integer class, XMM0..3 for float class,
        // and the stack slot (i-4)*8 starting at [rsp_entry+40] for both
        // classes; float stack slots hold the value in the low 4 bytes.
        unsigned __int64 regs[4];
        regs[0] = *(unsigned __int64*)(stack + 8);
        regs[1] = *(unsigned __int64*)(stack + 16);
        regs[2] = *(unsigned __int64*)(stack + 24);
        regs[3] = *(unsigned __int64*)(stack + 32);
        unsigned __int64 xregs[4];
        if (xmm)
        {
            xregs[0] = *(unsigned __int64*)(xmm + 0);
            xregs[1] = *(unsigned __int64*)(xmm + 8);
            xregs[2] = *(unsigned __int64*)(xmm + 16);
            xregs[3] = *(unsigned __int64*)(xmm + 24);
        }
        else
            xregs[0] = xregs[1] = xregs[2] = xregs[3] = 0;
        unsigned char* pArg = stack + 40;             // 5th arg onward
        for (size_t i = 0; i < nSlots; i++)
        {
            int pt = ctx->sig.params[i] & 0xFF;
            if (pt == 9)          // float — XMM low 32 bits, bit pattern
            {
                unsigned int u = i < 4
                    ? (unsigned int)xregs[i]
                    : (unsigned int)(*(unsigned __int64*)pArg & 0xFFFFFFFFu);
                float f;
                memcpy(&f, &u, 4);
                args.push_back(Value::Num_((double)f));
                if (i >= 4) pArg += 8;
            }
            else if (pt == 10)    // double — full 64-bit bit pattern
            {
                unsigned __int64 u = i < 4 ? xregs[i]
                                           : *(unsigned __int64*)pArg;
                double d;
                memcpy(&d, &u, 8);
                args.push_back(Value::Num_(d));
                if (i >= 4) pArg += 8;
            }
            else                  // integer class (int/ptr/str...)
            {
                unsigned __int64 v;
                if (i < 4)
                    v = regs[i];
                else
                {
                    v = *(unsigned __int64*)pArg;
                    pArg += 8;
                }
                args.push_back(Value::Num_((double)(__int64)v));
            }
        }
#endif
        // interp methods are not declared yet at this point — dispatch
        // through a function pointer installed at InitBuiltins time
        if (!g_cbDispatch || !ctx)
            return 0;
        return g_cbDispatch(ctx, args);
    }

#if defined(_M_IX86)
    static __int64 __fastcall CallbackShimX86(CbCtx* ctx,
                                              unsigned char* stack)
    {
        return CallbackShim(ctx, stack);
    }
#endif

    static void CallbackTrampoline(CbCtx* ctx)
    {
#if defined(_M_IX86)
        // mov ecx, ctx ; mov edx, esp ; call shim ; ret N
        unsigned char code[32] = {};
        int n = 0;
        code[n++] = 0xB9;   // mov ecx, imm32
        unsigned int ctxAddr = (unsigned int)(INT_PTR)ctx;
        memcpy(code + n, &ctxAddr, 4); n += 4;
        code[n++] = 0x89; code[n++] = 0xE2;   // mov edx, esp
        code[n++] = 0xB8;   // mov eax, shim
        unsigned int shimAddr = (unsigned int)(INT_PTR)&CallbackShimX86;
        memcpy(code + n, &shimAddr, 4); n += 4;
        code[n++] = 0xFF; code[n++] = 0xD0;   // call eax
        int slots = 0;
        for (auto p : ctx->sig.params)
        {
            int pt = p & 0xFF;
            slots += (pt == 1) ? 2 : 1;
        }
        int popBytes = slots * 4;             // stdcall callee cleanup
        if (popBytes == 0)
            code[n++] = 0xC3;                 // ret
        else
        {
            code[n++] = 0xC2;                 // ret imm16
            code[n++] = (unsigned char)(popBytes & 0xFF);
            code[n++] = (unsigned char)((popBytes >> 8) & 0xFF);
        }
        ctx->codeSize = n;
        ctx->code = VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE,
                                 PAGE_EXECUTE_READWRITE);
        memcpy(ctx->code, code, n);
        FlushInstructionCache(GetCurrentProcess(), ctx->code, n);
#else
        // Microsoft x64 trampoline — entered with:
        //   [rsp]   = return address (rsp_entry ≡ 8 mod 16)
        //   RCX..R9 = integer/pointer args 1..4, XMM0..3 = float args
        //   [rsp+40..] = args 5+ (8 bytes each, caller cleans)
        // The caller's four shadow slots ([rsp+8..+32]) are callee
        // scratch per the ABI, so we spill RCX..R9 there, park XMM0..3
        // in our own frame, then invoke the C++ shim with
        //   RCX=ctx  RDX=rsp_entry  R8=&xmm_save
        //   sub rsp,0x78 (frame, 16-aligned)  →  sub rsp,0x30 (shadow)
        unsigned char* c = (unsigned char*)VirtualAlloc(
            nullptr, 160, MEM_COMMIT | MEM_RESERVE,
            PAGE_EXECUTE_READWRITE);
        int n = 0;
        auto put = [&c, &n](const unsigned char* p, int len)
        { memcpy(c + n, p, (size_t)len); n += len; };
        static const unsigned char sFrame[] = {
            0x48, 0x83, 0xEC, 0x78 };              // sub rsp,0x78
        static const unsigned char sXmmSave[] = {
            0x66, 0x0F, 0xD6, 0x04, 0x24,          // movq [rsp],xmm0
            0x66, 0x0F, 0xD6, 0x44, 0x24, 0x08,    // movq [rsp+8],xmm1
            0x66, 0x0F, 0xD6, 0x44, 0x24, 0x10,    // movq [rsp+16],xmm2
            0x66, 0x0F, 0xD6, 0x44, 0x24, 0x18 };  // movq [rsp+24],xmm3
        static const unsigned char sIntSave[] = {
            0x48, 0x89, 0x8C, 0x24, 0x80, 0x00, 0x00, 0x00, // mov [rsp+0x78+8],rcx
            0x48, 0x89, 0x94, 0x24, 0x88, 0x00, 0x00, 0x00, // mov [rsp+0x78+16],rdx
            0x4C, 0x89, 0x84, 0x24, 0x90, 0x00, 0x00, 0x00, // mov [rsp+0x78+24],r8
            0x4C, 0x89, 0x8C, 0x24, 0x98, 0x00, 0x00, 0x00 };// mov [rsp+0x78+32],r9
        static const unsigned char sGo[] = {
            0x48, 0x8D, 0x94, 0x24, 0x78, 0x00, 0x00, 0x00, // lea rdx,[rsp+0x78]
            0x4C, 0x8D, 0x04, 0x24,                         // lea r8,[rsp]
            0x48, 0x83, 0xEC, 0x30,                         // sub rsp,0x30
            0xFF, 0xD0,                                     // call rax
            0x48, 0x83, 0xC4, 0x30,                         // add rsp,0x30
            0x48, 0x81, 0xC4, 0x78, 0x00, 0x00, 0x00,       // add rsp,0x78
            0xC3 };                                         // ret (caller cleans)
        put(sFrame, 4);
        put(sXmmSave, (int)sizeof(sXmmSave));
        put(sIntSave, (int)sizeof(sIntSave));
        c[n++] = 0x48; c[n++] = 0xB8;               // movabs rax, &shim
        unsigned long long shimAddr =
            (unsigned long long)(INT_PTR)&CallbackShim;
        memcpy(c + n, &shimAddr, 8); n += 8;
        c[n++] = 0x48; c[n++] = 0xB9;               // movabs rcx, ctx
        unsigned long long ctxAddr = (unsigned long long)(INT_PTR)ctx;
        memcpy(c + n, &ctxAddr, 8); n += 8;
        put(sGo, (int)sizeof(sGo));
        ctx->codeSize = n;
        ctx->code = c;
        FlushInstructionCache(GetCurrentProcess(), c, n);
#endif
    }
    static bool ParseApiSig(const wstring& sig, ApiSig* out)
    {
        size_t pl = sig.find(L'(');
        size_t pr = sig.rfind(L')');
        if (pl == wstring::npos || pr == wstring::npos || pr < pl)
            return false;
        wstring strRet = sig.substr(0, pl);
        wstring strArgs = sig.substr(pl + 1, pr - pl - 1);
        wstring tail = sig.substr(pr + 1);
        {
            // returns share the alias table but map to ApiRet ids —
            // ApiTypeOf's numeric ids are PARAM types (0=int...) and
            // must not be cast straight to ApiRet
            wstring rt = strRet;
            for (auto& c : rt)
                c = towlower(c);
            while (!rt.empty() && (rt[0] == L' ' || rt[0] == L'\t' || rt[0] == L'\r' || rt[0] == L'\n'))
                rt.erase(0, 1);
            while (!rt.empty() && (rt.back() == L' ' || rt.back() == L'\t' || rt.back() == L'\r' || rt.back() == L'\n'))
                rt.pop_back();
            if (rt == L"void") { out->ret = ARVoid; }
            else if (rt == L"double")
            {
                out->ret = ARDbl;
                out->bRetFloat = false;
            }
            else if (rt == L"float")
            {
                out->ret = ARDbl;
                out->bRetFloat = true;
            }
            else if (rt == L"int64" || rt == L"longlong")
                out->ret = ARI64;
            else if (rt == L"word") out->ret = ARWord;
            else if (rt == L"byte") out->ret = ARByte;
            else if (rt == L"bool" || rt == L"boolean")
                out->ret = ARBool;
            else if (rt == L"str" || rt == L"strb" || rt == L"string")
                out->ret = ARStrA;
            else if (rt == L"wstr" || rt == L"ustring")
                out->ret = ARStrW;
            else if (rt == L"addr" || rt == L"ptr" ||
                     rt == L"pointer" || rt == L"handle" ||
                     rt == L"struct") out->ret = ARPtr;
            else if (rt == L"int" || rt == L"long" || rt == L"dword" ||
                     rt == L"uint") out->ret = ARInt;
            else return false;
        }
        out->bCdecl = tail.find(L"cdecl") != wstring::npos ||
                      tail.find(L"CDECL") != wstring::npos;
        size_t pos = 0;
        while (pos <= strArgs.size())
        {
            size_t comma = strArgs.find(L',', pos);
            if (comma == wstring::npos) comma = strArgs.size();
            wstring t = strArgs.substr(pos, comma - pos);
            while (!t.empty() && (t[0] == L' ' || t[0] == L'\t' || t[0] == L'\r' || t[0] == L'\n'))
                t.erase(0, 1);
            while (!t.empty() && (t.back() == L' ' || t.back() == L'\t' || t.back() == L'\r' || t.back() == L'\n'))
                t.pop_back();
            if (!t.empty())
            {
                bool bRef = false;
                int p = ApiTypeOf(t, &bRef);
                if (p < 0) return false;
                out->params.push_back(p | (bRef ? 0x100 : 0));
            }
            pos = comma + 1;
        }
        return true;
    }

    template <typename R>
    static R InvokeStd(void* fn, const vector<SlotT>& s)
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
    static R InvokeCdl(void* fn, const vector<SlotT>& s)
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
    static void* EmitX64CallStub(const ApiSig& sig)
    {
        unsigned char* c = (unsigned char*)VirtualAlloc(
            nullptr, 256, MEM_COMMIT | MEM_RESERVE,
            PAGE_EXECUTE_READWRITE);
        int n = 0;
        auto put = [&c, &n](const unsigned char* p, int len)
        { memcpy(c + n, p, (size_t)len); n += len; };
        auto put32 = [&c, &n](unsigned int v)
        { memcpy(c + n, &v, 4); n += 4; };
        // push rbx ; mov rbx, rcx ; sub rsp,0xC0 (16-aligned: entry ≡8,
        // push →0, 0xC0 ≡0 → call-site rsp ≡0 mod 16)
        static const unsigned char sPro[] = {
            0x53,
            0x48, 0x89, 0xCB,
            0x48, 0x81, 0xEC, 0xC0, 0x00, 0x00, 0x00 };
        put(sPro, (int)sizeof(sPro));
        // mov REG, [rbx+disp32] — mod 10, reg=dest, rm=011 (rbx)
        //   rcx: 48 8B 8B dd..  rdx: 48 8B 93 dd..
        //   r8 : 4C 8B 83 dd..  r9 : 4C 8B 8B dd..
        //   rax: 48 8B 83 dd..
        static const unsigned char kMovRbx[][3] = {
            { 0x48, 0x8B, 0x8B },   // rcx
            { 0x48, 0x8B, 0x93 },   // rdx
            { 0x4C, 0x8B, 0x83 },   // r8
            { 0x4C, 0x8B, 0x8B },   // r9
            { 0x48, 0x8B, 0x83 } }; // rax
        // movsd/movss XMMN, [rbx+disp32] — F2/F3 0F 10 mod(0x83|reg<<3)
        static const unsigned char kMsdX[][3] = {
            { 0xF2, 0x0F, 0x10 }, { 0xF2, 0x0F, 0x10 },
            { 0xF2, 0x0F, 0x10 }, { 0xF2, 0x0F, 0x10 } };
        (void)kMsdX;
        int nIntReg = 0, nXmmReg = 0, nStack = 0;
        for (size_t i = 0; i < sig.params.size() && i < 14; i++)
        {
            int pt = sig.params[i] & 0xFF;
            bool bFloat = (pt == 9 || pt == 10);
            unsigned int disp = (unsigned int)(8 + 8 * i);
            if (bFloat)
            {
                if (nXmmReg < 4)
                {
                    unsigned char o[3] = { pt == 10 ? 0xF2u : 0xF3u,
                                           0x0F, 0x10 };
                    c[n++] = o[0]; c[n++] = o[1]; c[n++] = o[2];
                    c[n++] = (unsigned char)(0x83 | (nXmmReg << 3));
                    put32(disp);
                    nXmmReg++;
                }
                else
                {
                    put(kMovRbx[4], 3);           // mov rax,[rbx+disp]
                    put32(disp);
                    unsigned char st[4] = {
                        0x48, 0x89, 0x44, 0x24 }; // mov [rsp+d],rax
                    put(st, 4);
                    c[n++] = (unsigned char)(0x20 + 8 * nStack);
                    nStack++;
                }
            }
            else
            {
                if (nIntReg < 4)
                {
                    put(kMovRbx[nIntReg], 3);
                    put32(disp);
                    nIntReg++;
                }
                else
                {
                    put(kMovRbx[4], 3);
                    put32(disp);
                    unsigned char st[4] = {
                        0x48, 0x89, 0x44, 0x24 };
                    put(st, 4);
                    c[n++] = (unsigned char)(0x20 + 8 * nStack);
                    nStack++;
                }
            }
        }
        c[n++] = 0xFF; c[n++] = 0x13;   // call qword [rbx]
        if (sig.ret == ARDbl)
        {
            if (sig.bRetFloat)
            {
                static const unsigned char cvt[] = {
                    0xF3, 0x0F, 0x5A, 0xC0 };   // cvtss2sd xmm0,xmm0
                put(cvt, 4);
            }
            static const unsigned char movq[] = {
                0x66, 0x48, 0x0F, 0x7E, 0xC0 }; // movq rax,xmm0
            put(movq, 5);
        }
        static const unsigned char sEp[] = {
            0x48, 0x81, 0xC4, 0xC0, 0x00, 0x00, 0x00, // add rsp,0xC0
            0x5B,                                     // pop rbx
            0xC3 };                                   // ret
        put(sEp, (int)sizeof(sEp));
        FlushInstructionCache(GetCurrentProcess(), c, n);
        return c;
    }
    static __int64 InvokeX64Stub(void* stub, void* fn, const vector<SlotT>& s)
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
    static wstring ReadStrA(const char* p)
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
    static wstring ReadStrW(const wchar_t* p)
    {
        if (!p) return L"";
        wstring out;
        while (*p) { out += *p; p++; }
        return out;
    }

    // round 36: deep copy for thread boundaries — tables are duplicated so
    // the worker and the main VM never share mutable state (aardio: threads
    // receive value copies); functions/natives do not cross threads
    static Value DeepCopy(const Value& v)
    {
        if (v.t == Value::TableT && v.tab)
        {
            auto nt = std::make_shared<Table>();
            for (auto& kv : v.tab->map)
            {
                if (kv.first == L"self") continue;
                nt->map[kv.first] = DeepCopy(kv.second);
            }
            for (auto& x : v.tab->arr)
                nt->arr.push_back(DeepCopy(x));
            Value res = Value::Table_(nt);
            if (v.tab->map.count(L"self"))
                nt->map[L"self"] = res;
            return res;
        }
        if (v.t == Value::Class && v.tab)
        {
            auto nt = std::make_shared<Table>();
            for (auto& kv : v.tab->map)
            {
                if (kv.first == L"self") continue;
                nt->map[kv.first] = DeepCopy(kv.second);
            }
            Value r = Value::Table_(nt);
            r.t = Value::Class;
            r.fn = v.fn;
            if (v.tab->map.count(L"self"))
                nt->map[L"self"] = r;
            return r;
        }
        Value x = v;
        x.tab.reset();
        x.nsClosure.reset();
        x.native = nullptr;
        x.fn.reset();
        return x;
    }

    // thread.create worker state (round 36)
    struct ThreadState
    {
        std::thread th;
        Value result;
        wstring error;
        bool done = false;
    };
    static std::map<wstring, Value> g_threadSharedMap;
    static std::mutex g_threadSharedMutex;

    // round 36: HTTP via WinINet — GET/POST with UTF-8 body and response
    static wstring HttpFetch(const wstring& strUrl, const wchar_t* szMethod,
                             const wstring& strBody,
                             const wstring& strCType, wstring* pErr)
    {
        wstring result;
        HINTERNET hOpen = InternetOpenW(L"aardio-re/43re",
                                        INTERNET_OPEN_TYPE_PRECONFIG,
                                        nullptr, nullptr, 0);
        if (!hOpen)
        {
            (*pErr) = L"InternetOpen 失败";
            return result;
        }
        wchar_t szHost[256] = L"";
        wchar_t szPath[2048] = L"";
        URL_COMPONENTSW uc = { sizeof(uc) };
        uc.lpszHostName = szHost;
        uc.dwHostNameLength = 255;
        uc.lpszUrlPath = szPath;
        uc.dwUrlPathLength = 2047;
        if (!InternetCrackUrlW(strUrl.c_str(), (DWORD)strUrl.size(), 0, &uc))
        {
            (*pErr) = L"URL 解析失败";
            InternetCloseHandle(hOpen);
            return result;
        }
        HINTERNET hConn = InternetConnectW(hOpen, szHost, uc.nPort, nullptr,
                                           nullptr, INTERNET_SERVICE_HTTP,
                                           0, 0);
        if (!hConn)
        {
            (*pErr) = L"连接失败: " + wstring(szHost);
            InternetCloseHandle(hOpen);
            return result;
        }
        wstring strPath = szPath;
        if (strPath.empty())
            strPath = L"/";
        HINTERNET hReq = HttpOpenRequestW(
            hConn, szMethod, strPath.c_str(), L"HTTP/1.1", nullptr, nullptr,
            uc.nScheme == INTERNET_SCHEME_HTTPS ? INTERNET_FLAG_SECURE : 0,
            0);
        if (!hReq)
        {
            (*pErr) = L"请求创建失败";
            InternetCloseHandle(hConn);
            InternetCloseHandle(hOpen);
            return result;
        }
        // wide string body -> UTF-8 bytes on the wire
        std::vector<char> body8;
        if (!strBody.empty())
        {
            int nU8 = WideCharToMultiByte(CP_UTF8, 0, strBody.c_str(),
                                          (int)strBody.size(), nullptr, 0,
                                          nullptr, nullptr);
            if (nU8 > 0)
            {
                body8.resize((size_t)nU8);
                WideCharToMultiByte(CP_UTF8, 0, strBody.c_str(),
                                    (int)strBody.size(), body8.data(), nU8,
                                    nullptr, nullptr);
            }
        }
        wstring strHeaders;
        if (!strCType.empty())
            strHeaders = L"Content-Type: " + strCType + L"\r\n";
        BOOL bOk = HttpSendRequestW(hReq, strHeaders.c_str(), -1L,
                                    body8.empty() ? nullptr : body8.data(),
                                    (DWORD)body8.size());
        if (!bOk)
        {
            (*pErr) = L"发送失败 (GetLastError=" +
                      std::to_wstring(GetLastError()) + L")";
        }
        else
        {
            std::string str8;
            char buf[4096];
            DWORD n = 0;
            while (InternetReadFile(hReq, buf, sizeof(buf), &n) && n > 0)
                str8.append(buf, n);
            int w = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                        str8.data(), (int)str8.size(),
                                        nullptr, 0);
            if (w <= 0)
                w = MultiByteToWideChar(CP_ACP, 0, str8.data(),
                                        (int)str8.size(), nullptr, 0);
            if (w > 0)
            {
                result.resize((size_t)w);
                MultiByteToWideChar(CP_UTF8, 0, str8.data(), (int)str8.size(),
                                    &result[0], w);
            }
        }
        InternetCloseHandle(hReq);
        InternetCloseHandle(hConn);
        InternetCloseHandle(hOpen);
        return result;
    }

    // UTF-8 byte writer: the CRT converts wide output via ACP for pipes,
    // which garbles CJK for UTF-8 consumers (F5 capture, test harness)
    static void WriteU8(FILE* f, const wstring& s)
    {
        int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(),
                                    nullptr, 0, nullptr, nullptr);
        if (n > 0)
        {
            std::string out((size_t)n, 0);
            WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0],
                                n, nullptr, nullptr);
            fwrite(out.data(), 1, out.size(), f);
        }
    }

    // ---------------- round 22: pattern matching ----------------
    // compact backtracking matcher: \d \D \w \W \s \S, '.', [...],
    // quantifiers * + ? {n} {n,} {n,m} (+ lazy variants), groups with
    // captures, top-level | alternation, ^ $ anchors. Greedy group
    // backtracking takes the first inner match (documented subset).
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

    static bool REClass(unsigned cls, wchar_t c)
    {
        bool r = false;
        if (cls & 1u) r = r || iswdigit(c) != 0;
        if (cls & 2u) r = r || !iswdigit(c);
        if (cls & 4u) r = r || (iswalnum(c) || c == L'_');
        if (cls & 8u) r = r || !(iswalnum(c) || c == L'_');
        if (cls & 16u) r = r || iswspace(c) != 0;
        if (cls & 32u) r = r || !iswspace(c);
        return (cls & 2u) || (cls & 8u) || (cls & 32u) ? r : r;
    }

    static bool REClassHit(const RE& el, wchar_t c)
    {
        bool hit = REClass(el.cls, c);
        for (auto& rg : el.ranges)
            if (c >= rg.first && c <= rg.second) hit = true;
        if (el.neg) return !hit;
        return hit;
    }

    struct REParser
    {
        const wstring& pat;
        size_t i = 0;
        int nGroups = 0;
        wstring* err;
        explicit REParser(const wstring& p, wstring* e) : pat(p), err(e) {}

        bool ParseSeq(vector<RE>& out, bool inGroup)
        {
            while (i < pat.size())
            {
                wchar_t c = pat[i];
                if (c == L'|' || c == L')')
                    return inGroup ? true : true;   // caller handles both
                RE el;
                if (c == L'(')
                {
                    i++;
                    if (i + 1 < pat.size() && pat[i] == L'?' && pat[i + 1] == L':')
                        i += 2;
                    else
                        el.gi = ++nGroups;
                    el.k = 3;
                    if (!ParseSeq(el.sub, true))
                        return false;
                    if (i >= pat.size() || pat[i] != L')')
                    {
                        (*err) = L"模式错误: 缺少 )";
                        return false;
                    }
                    i++;
                }
                else if (c == L'[')
                {
                    i++;
                    el.k = 1;
                    if (i < pat.size() && pat[i] == L'^') { el.neg = true; i++; }
                    bool first = true;
                    while (i < pat.size() && (pat[i] != L']' || first))
                    {
                        first = false;
                        wchar_t lo = pat[i];
                        if (lo == L'\\' && i + 1 < pat.size())
                        {
                            i++;
                            wchar_t e = pat[i];
                            unsigned bit = ClassBit(e);
                            if (bit) el.cls |= bit;
                            else if (e == L'n') lo = L'\n';
                            else if (e == L't') lo = L'\t';
                            else if (e == L'r') lo = L'\r';
                            else lo = e;
                        }
                        if (i + 2 < pat.size() && pat[i + 1] == L'-' &&
                            pat[i + 2] != L']')
                        {
                            wchar_t hi = pat[i + 2];
                            if (hi == L'\\' && i + 3 < pat.size())
                                hi = pat[i + 3];
                            el.ranges.push_back({ lo, hi });
                            i += 3;
                        }
                        else
                        {
                            el.ranges.push_back({ lo, lo });
                            i++;
                        }
                    }
                    if (i >= pat.size())
                    {
                        (*err) = L"模式错误: 缺少 ]";
                        return false;
                    }
                    i++;   // ']'
                }
                else if (c == L'.')
                {
                    i++;
                    el.k = 2;
                }
                else if (c == L'$')
                {
                    i++;
                    el.k = 4;
                }
                else if (c == L'^' && out.empty())
                {
                    i++;   // ^ anchor: only meaningful at pattern start; the
                           // search loop honors it by not scanning
                    continue;
                }
                else if (c == L'\\')
                {
                    i++;
                    if (i >= pat.size())
                    {
                        (*err) = L"模式错误: 悬挂的 \\";
                        return false;
                    }
                    wchar_t e = pat[i++];
                    unsigned bit = ClassBit(e);
                    if (bit) { el.k = 1; el.cls |= bit; }
                    else
                    {
                        el.k = 0;
                        if (e == L'n') el.ch = L'\n';
                        else if (e == L't') el.ch = L'\t';
                        else if (e == L'r') el.ch = L'\r';
                        else el.ch = e;
                    }
                }
                else
                {
                    i++;
                    el.k = 0;
                    el.ch = c;
                }
                // quantifier
                if (i < pat.size())
                {
                    wchar_t q = pat[i];
                    if (q == L'*') { el.min = 0; el.max = -1; i++; }
                    else if (q == L'+') { el.min = 1; el.max = -1; i++; }
                    else if (q == L'?') { el.min = 0; el.max = 1; i++; }
                    else if (q == L'{')
                    {
                        size_t j = pat.find(L'}', i);
                        if (j != wstring::npos)
                        {
                            wstring spec = pat.substr(i + 1, j - i - 1);
                            size_t comma = spec.find(L',');
                            el.min = _wtoi(spec.substr(0, comma).c_str());
                            if (comma == wstring::npos)
                                el.max = el.min;
                            else
                            {
                                wstring hi = spec.substr(comma + 1);
                                el.max = hi.empty() ? -1 : _wtoi(hi.c_str());
                            }
                            if (el.min < 0) el.min = 0;
                            i = j + 1;
                        }
                    }
                    if (i < pat.size() && pat[i] == L'?')
                    {
                        el.lazy = true;
                        i++;
                    }
                }
                out.push_back(std::move(el));
            }
            return true;
        }

        static unsigned ClassBit(wchar_t e)
        {
            switch (e)
            {
            case L'd': return 1;
            case L'D': return 2;
            case L'w': return 4;
            case L'W': return 8;
            case L's': return 16;
            case L'S': return 32;
            default: return 0;
            }
        }
    };

    static const size_t RENPOS = (size_t)-1;

    static long long REMatchRep(const vector<RE>& els, size_t idx,
                                const wstring& s, size_t pos, int count,
                                vector<wstring>& caps, bool anchored);

    static size_t REMatchOne(const RE& el, const wstring& s, size_t pos,
                             vector<wstring>& caps);

    static long long REMatchSeq(const vector<RE>& els, size_t idx,
                                const wstring& s, size_t pos,
                                vector<wstring>& caps, bool anchored)
    {
        if (idx == els.size())
            return (long long)pos;
        const RE& el = els[idx];
        if (el.k == 4)   // '$' — zero-width end anchor
        {
            if (pos != s.size()) return -1;
            return REMatchSeq(els, idx + 1, s, pos, caps, anchored);
        }
        return REMatchRep(els, idx, s, pos, 0, caps, anchored);
    }

    static long long REMatchRep(const vector<RE>& els, size_t idx,
                                const wstring& s, size_t pos, int count,
                                vector<wstring>& caps, bool anchored);

    static size_t REMatchOne(const RE& el, const wstring& s, size_t pos,
                             vector<wstring>& caps)
    {
        if (pos >= s.size())
            return RENPOS;
        switch (el.k)
        {
        case 0: return s[pos] == el.ch ? pos + 1 : RENPOS;
        case 2: return s[pos] != L'\n' ? pos + 1 : RENPOS;
        case 1: return REClassHit(el, s[pos]) ? pos + 1 : RENPOS;
        case 3:
        {
            vector<wstring> tmp = caps;
            long long e = REMatchSeq(el.sub, 0, s, pos, tmp, false);
            if (e < 0)
                return RENPOS;
            if (el.gi >= 0)
            {
                if ((size_t)el.gi >= caps.size())
                    caps.resize((size_t)el.gi + 1);
                caps[(size_t)el.gi] = s.substr(pos, (size_t)e - pos);
            }
            return (size_t)e;
        }
        default: return RENPOS;
        }
    }

    static long long REMatchRep(const vector<RE>& els, size_t idx,
                                const wstring& s, size_t pos, int count,
                                vector<wstring>& caps, bool anchored)
    {
        const RE& el = els[idx];
        bool canMore = el.max < 0 || count < el.max;
        if (el.lazy)
        {
            if (count >= el.min)
            {
                long long e = REMatchSeq(els, idx + 1, s, pos, caps, anchored);
                if (e >= 0) return e;
            }
            if (canMore)
            {
                size_t np = REMatchOne(el, s, pos, caps);
                if (np != RENPOS && np != pos)
                {
                    long long e = REMatchRep(els, idx, s, np, count + 1,
                                             caps, anchored);
                    if (e >= 0) return e;
                }
            }
            return -1;
        }
        // greedy: consume more first, then stop here
        if (canMore)
        {
            size_t np = REMatchOne(el, s, pos, caps);
            if (np != RENPOS && np != pos)
            {
                long long e = REMatchRep(els, idx, s, np, count + 1,
                                         caps, anchored);
                if (e >= 0) return e;
            }
        }
        if (count >= el.min)
        {
            long long e = REMatchSeq(els, idx + 1, s, pos, caps, anchored);
            if (e >= 0) return e;
        }
        return -1;
    }

    // one anchored attempt; fills caps and match end
    static bool RETry(const vector<vector<RE>>& branches, const wstring& s,
                      size_t pos, bool anchored, vector<wstring>& caps,
                      size_t* pEnd)
    {
        for (auto& els : branches)
        {
            vector<wstring> tmp;
            long long e = REMatchSeq(els, 0, s, pos, tmp, anchored);
            if (e >= 0)
            {
                caps = tmp;
                *pEnd = (size_t)e;
                return true;
            }
        }
        return false;
    }

    // ---------------- round 30: json ----------------
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

    static wstring JQuote(const wstring& str)
    {
        wstring out = L"\"";
        for (wchar_t c : str)
        {
            switch (c)
            {
            case L'"': out += L"\\\""; break;
            case L'\\': out += L"\\\\"; break;
            case L'\n': out += L"\\n"; break;
            case L'\r': out += L"\\r"; break;
            case L'\t': out += L"\\t"; break;
            default:
                if (c < 0x20)
                {
                    wchar_t buf[8];
                    swprintf(buf, 8, L"\\u%04x", (unsigned)c);
                    out += buf;
                }
                else out += c;
            }
        }
        out += L'"';
        return out;
    }

    static wstring JWrite(const Value& v, int indent, int depth)
    {
        wstring pad, padEnd;
        if (indent >= 0 && depth > 0)
        {
            pad.assign((size_t)indent * depth, L' ');
            padEnd.assign((size_t)indent * (depth - 1), L' ');
        }
        wstring nl = indent >= 0 ? L"\n" : L"";
        switch (v.t)
        {
        case Value::Null: return L"null";
        case Value::Bool: return v.b ? L"true" : L"false";
        case Value::Num:
        {
            if (v.n == (long long)v.n && fabs(v.n) < 1e15)
                return std::to_wstring((long long)v.n);
            wchar_t buf[32];
            swprintf(buf, 32, L"%.14g", v.n);
            return buf;
        }
        case Value::Str: return JQuote(v.s);
        case Value::TableT:
        {
            if (!v.tab)
                return L"null";
            bool asArray = !v.tab->arr.empty() && v.tab->map.empty();
            wstring out = asArray ? L"[" : L"{";
            bool bAny = !v.tab->arr.empty() || !v.tab->map.empty();
            if (indent >= 0 && bAny)
                out += nl;
            bool bFirst = true;
            if (asArray)
            {
                for (auto& e : v.tab->arr)
                {
                    if (!bFirst) { out += L","; if (indent >= 0) out += L" "; }
                    if (indent >= 0) out += nl;
                    out += pad + JWrite(e, indent, depth + 1);
                    bFirst = false;
                }
            }
            else
            {
                for (auto& kv : v.tab->map)
                {
                    if (!bFirst) { out += L","; if (indent >= 0) out += nl; }
                    out += pad + JQuote(kv.first) + L": " +
                           JWrite(kv.second, indent, depth + 1);
                    bFirst = false;
                }
            }
            if (indent >= 0 && bAny)
                out += nl + padEnd;
            out += asArray ? L"]" : L"}";
            return out;
        }
        default: return L"null";
        }
    }

    static bool RECompile(const wstring& pat, vector<vector<RE>>& branches,
                          bool* pAnchored, int* pGroups, wstring* err)
    {
        REParser ps(pat, err);
        bool anchored = false;
        size_t j = 0;
        if (!pat.empty() && pat[0] == L'^') { anchored = true; j = 1; }
        ps.i = j;
        while (true)
        {
            vector<RE> els;
            if (!ps.ParseSeq(els, false))
                return false;
            branches.push_back(std::move(els));
            if (ps.i < ps.pat.size() && ps.pat[ps.i] == L'|')
            {
                ps.i++;
                continue;
            }
            break;
        }
        if (!err->empty())
            return false;
        *pAnchored = anchored;
        *pGroups = ps.nGroups;
        return true;
    }

    bool Value::Truthy() const
    {
        switch (t)
        {
        case Null: return false;
        case Bool: return b;
        case Num: return n != 0;
        case Pointer: return ptr != nullptr;
        case Str: return !s.empty();
        default: return true;
        }
    }

    wstring Value::ToString() const
    {
        switch (t)
        {
        case Null: return L"null";
        case Bool: return b ? L"true" : L"false";
        case Num:
        {
            wchar_t buf[32];
            if (n == (long long)n)
                swprintf(buf, 32, L"%lld", (long long)n);
            else
                swprintf(buf, 32, L"%.14g", n);
            return buf;
        }
        case Pointer:
        {
            wchar_t buf[32];
            swprintf(buf, 32, L"%lld", (long long)(INT_PTR)ptr);
            return buf;
        }
        case Str: return s;
        case TableT: return L"table";
        case Class: return L"class";
        case Func: return L"function";
        }
        return L"?";
    }

    // ---------------- struct layout & serialization helpers ----------------
    struct StructFieldDef
    {
        wstring name;
        wstring type;
        int arrCount = 1;
        bool bIsArray = false;
    };

    static int GetStructFieldSize(const wstring& type, int arrCount, bool bIsArray = false)
    {
        wstring t = type;
        for (auto& c : t) c = towlower(c);
        if (arrCount <= 0) arrCount = 1;
        if (t == L"byte") return 1 * arrCount;
        if (t == L"word") return 2 * arrCount;
        if (t == L"int" || t == L"uint" || t == L"long" || t == L"dword" || t == L"bool") return 4 * arrCount;
        if (t == L"int64" || t == L"uint64" || t == L"long64") return 8 * arrCount;
        if (t == L"float") return 4 * arrCount;
        if (t == L"double") return 8 * arrCount;
        if (t == L"ustring") return bIsArray ? (2 * arrCount) : (int)sizeof(void*);
        if (t == L"string" || t == L"str") return bIsArray ? (1 * arrCount) : (int)sizeof(void*);
        if (t == L"pointer" || t == L"ptr" || t == L"addr") return (int)sizeof(void*) * arrCount;
        return 4 * arrCount;
    }

    static std::vector<StructFieldDef> ParseStructDef(const Value& vTab)
    {
        std::vector<StructFieldDef> fields;
        if (vTab.t != Value::TableT && vTab.t != Value::Class) return fields;
        if (!vTab.tab) return fields;
        auto is = vTab.tab->map.find(L"_struct");
        if (is == vTab.tab->map.end()) return fields;
        if (is->second.t == Value::Str)
        {
            const wstring& s = is->second.s;
            size_t pos = 0;
            while (pos < s.size())
            {
                size_t semi = s.find(L';', pos);
                if (semi == wstring::npos) semi = s.size();
                wstring item = s.substr(pos, semi - pos);
                pos = semi + 1;
                while (!item.empty() && (item[0] == L' ' || item[0] == L'\t' || item[0] == L'\r' || item[0] == L'\n'))
                    item.erase(0, 1);
                while (!item.empty() && (item.back() == L' ' || item.back() == L'\t' || item.back() == L'\r' || item.back() == L'\n'))
                    item.pop_back();
                if (item.empty()) continue;
                size_t sp = item.find_first_of(L" \t");
                if (sp == wstring::npos) continue;
                wstring stype = item.substr(0, sp);
                wstring srest = item.substr(sp + 1);
                while (!srest.empty() && (srest[0] == L' ' || srest[0] == L'\t'))
                    srest.erase(0, 1);
                int nArr = 1;
                bool bArr = false;
                size_t lb = srest.find(L'[');
                wstring sname = srest;
                if (lb != wstring::npos)
                {
                    bArr = true;
                    sname = srest.substr(0, lb);
                    size_t rb = srest.find(L']', lb);
                    wstring snum = (rb != wstring::npos) ? srest.substr(lb + 1, rb - lb - 1) : L"";
                    nArr = snum.empty() ? 1 : _wtoi(snum.c_str());
                    if (nArr <= 0) nArr = 1;
                }
                while (!sname.empty() && (sname.back() == L' ' || sname.back() == L'\t'))
                    sname.pop_back();
                fields.push_back({ sname, stype, nArr, bArr });
            }
        }
        else if (is->second.t == Value::TableT && is->second.tab)
        {
            for (auto& kv : is->second.tab->map)
            {
                wstring t = kv.second.ToString();
                int nArr = 1;
                bool bArr = false;
                size_t lb = t.find(L'[');
                wstring stype = t;
                if (lb != wstring::npos)
                {
                    bArr = true;
                    stype = t.substr(0, lb);
                    size_t rb = t.find(L']', lb);
                    wstring snum = (rb != wstring::npos) ? t.substr(lb + 1, rb - lb - 1) : L"";
                    nArr = snum.empty() ? 1 : _wtoi(snum.c_str());
                    if (nArr <= 0) nArr = 1;
                }
                fields.push_back({ kv.first, stype, nArr, bArr });
            }
        }
        return fields;
    }

    static int CalcStructTotalSize(const Value& vTab)
    {
        auto fields = ParseStructDef(vTab);
        int total = 0;
        for (auto& f : fields)
        {
            wstring t = f.type;
            for (auto& c : t) c = towlower(c);
            if (t == L"struct" && vTab.tab)
            {
                auto itM = vTab.tab->map.find(f.name);
                if (itM != vTab.tab->map.end() && (itM->second.t == Value::TableT || itM->second.t == Value::Class))
                {
                    int subSz = CalcStructTotalSize(itM->second);
                    total += (subSz > 0 ? subSz : 4) * (f.arrCount > 0 ? f.arrCount : 1);
                    continue;
                }
            }
            total += GetStructFieldSize(f.type, f.arrCount, f.bIsArray);
        }
        return total;
    }

    static void* StructSerialize(const Value& vTab, int* pOutTotal = nullptr)
    {
        if ((vTab.t != Value::TableT && vTab.t != Value::Class) || !vTab.tab)
            return nullptr;
        auto fields = ParseStructDef(vTab);
        if (fields.empty()) return nullptr;
        int total = CalcStructTotalSize(vTab);
        if (total <= 0) return nullptr;
        if (pOutTotal) *pOutTotal = total;
        char* buf = (char*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, total);
        if (!buf) return nullptr;
        int off = 0;
        static Value s_nullVal;
        for (auto& f : fields)
        {
            auto iv = vTab.tab->map.find(f.name);
            const Value& fv = (iv != vTab.tab->map.end()) ? iv->second : s_nullVal;
            wstring t = f.type;
            for (auto& c : t) c = towlower(c);
            int sz = GetStructFieldSize(f.type, f.arrCount, f.bIsArray);
            wstring lowerName = f.name;
            for (auto& c : lowerName) c = towlower(c);
            if ((lowerName == L"cbsize" || lowerName == L"dwsize") && (t == L"int" || t == L"uint" || t == L"dword"))
            {
                if (fv.n != total)
                {
                    vTab.tab->map[f.name] = Value::Num_(total);
                }
                *(int*)(buf + off) = total;
            }
            else if (t == L"struct")
            {
                if ((fv.t == Value::TableT || fv.t == Value::Class) && fv.tab)
                {
                    int subTotal = 0;
                    void* pSub = StructSerialize(fv, &subTotal);
                    if (pSub)
                    {
                        sz = subTotal;
                        memcpy(buf + off, pSub, subTotal);
                        HeapFree(GetProcessHeap(), 0, pSub);
                    }
                }
            }
            else if (t == L"ustring")
            {
                if (f.bIsArray)
                {
                    wstring s = BStr(fv);
                    wchar_t* pw = (wchar_t*)(buf + off);
                    for (int k = 0; k < f.arrCount; k++)
                        pw[k] = (k < (int)s.size()) ? s[k] : wchar_t(0);
                }
                else
                {
                    if (iv != vTab.tab->map.end() && iv->second.t == Value::Str)
                        *(const wchar_t**)(buf + off) = iv->second.s.c_str();
                    else if (fv.t == Value::Str)
                        *(const wchar_t**)(buf + off) = fv.s.c_str();
                    else if (fv.t == Value::Pointer)
                        *(void**)(buf + off) = fv.ptr;
                    else if (fv.t == Value::Num)
                        *(void**)(buf + off) = (void*)(INT_PTR)(long long)fv.n;
                    else
                        *(void**)(buf + off) = nullptr;
                }
            }
            else if (t == L"string" || t == L"str")
            {
                if (f.bIsArray)
                {
                    if (fv.t == Value::Str)
                    {
                        std::string sA((size_t)WideCharToMultiByte(CP_ACP, 0, fv.s.c_str(), (int)fv.s.size(), nullptr, 0, nullptr, nullptr), 0);
                        if (!sA.empty())
                            WideCharToMultiByte(CP_ACP, 0, fv.s.c_str(), (int)fv.s.size(), &sA[0], (int)sA.size(), nullptr, nullptr);
                        char* pc = buf + off;
                        for (int k = 0; k < f.arrCount; k++)
                            pc[k] = (k < (int)sA.size()) ? sA[k] : 0;
                    }
                }
                else
                {
                    if (fv.t == Value::Str)
                        *(const wchar_t**)(buf + off) = fv.s.c_str();
                    else if (fv.t == Value::Pointer)
                        *(void**)(buf + off) = fv.ptr;
                    else if (fv.t == Value::Num)
                        *(void**)(buf + off) = (void*)(INT_PTR)(long long)fv.n;
                    else
                        *(void**)(buf + off) = nullptr;
                }
            }
            else if (t == L"pointer" || t == L"ptr" || t == L"addr")
            {
                if (fv.t == Value::Pointer)
                    *(void**)(buf + off) = fv.ptr;
                else if (fv.ptr != nullptr)
                    *(void**)(buf + off) = fv.ptr;
                else
                    *(void**)(buf + off) = (void*)(INT_PTR)(long long)BNum(fv);
            }
            else if (t == L"float")
            {
                float flt = (float)BNum(fv);
                memcpy(buf + off, &flt, sizeof(float));
            }
            else if (t == L"double")
            {
                double dbl = BNum(fv);
                memcpy(buf + off, &dbl, sizeof(double));
            }
            else
            {
                long long v64 = (long long)BNum(fv);
                if (sz == 8) *(__int64*)(buf + off) = (__int64)v64;
                else if (sz == 2) *(unsigned short*)(buf + off) = (unsigned short)v64;
                else if (sz == 1) *(unsigned char*)(buf + off) = (unsigned char)v64;
                else *(int*)(buf + off) = (int)v64;
            }
            off += sz;
        }
        return buf;
    }

    static void StructDeserialize(Value& vTab, const void* pBuf)
    {
        if ((vTab.t != Value::TableT && vTab.t != Value::Class) || !vTab.tab || !pBuf)
            return;
        auto fields = ParseStructDef(vTab);
        const char* buf = (const char*)pBuf;
        int off = 0;
        for (auto& f : fields)
        {
            auto iv = vTab.tab->map.find(f.name);
            int sz = GetStructFieldSize(f.type, f.arrCount, f.bIsArray);
            wstring t = f.type;
            for (auto& c : t) c = towlower(c);
            if (t == L"struct")
            {
                if (iv != vTab.tab->map.end() && (iv->second.t == Value::TableT || iv->second.t == Value::Class) && iv->second.tab)
                {
                    sz = CalcStructTotalSize(iv->second);
                    StructDeserialize(iv->second, buf + off);
                }
            }
            else if (t == L"ustring")
            {
                if (f.bIsArray)
                {
                    const wchar_t* pw = (const wchar_t*)(buf + off);
                    int len = 0;
                    while (len < f.arrCount && pw[len]) len++;
                    vTab.tab->map[f.name] = Value::Str_(wstring(pw, len));
                }
                else
                {
                    const wchar_t* pw = *(const wchar_t**)(buf + off);
                    if (pw && !IsBadStringPtrW(pw, 260))
                        vTab.tab->map[f.name] = Value::Str_(wstring(pw));
                }
            }
            else if (t == L"string" || t == L"str")
            {
                if (f.bIsArray)
                {
                    const char* pc = (const char*)(buf + off);
                    int len = 0;
                    while (len < f.arrCount && pc[len]) len++;
                    vTab.tab->map[f.name] = Value::Str_(ReadStrA(std::string(pc, len).c_str()));
                }
                else
                {
                    const char* pc = *(const char**)(buf + off);
                    if (pc && !IsBadStringPtrA(pc, 260))
                        vTab.tab->map[f.name] = Value::Str_(ReadStrA(pc));
                }
            }
            else if (t == L"pointer" || t == L"ptr" || t == L"addr")
            {
                vTab.tab->map[f.name] = Value::Pointer_(*(void**)(buf + off));
            }
            else if (t == L"float")
            {
                float flt = 0;
                memcpy(&flt, buf + off, sizeof(float));
                vTab.tab->map[f.name] = Value::Num_(flt);
            }
            else if (t == L"double")
            {
                double dbl = 0;
                memcpy(&dbl, buf + off, sizeof(double));
                vTab.tab->map[f.name] = Value::Num_(dbl);
            }
            else
            {
                long long v64 = 0;
                if (sz == 8) v64 = *(__int64*)(buf + off);
                else if (sz == 2) v64 = *(unsigned short*)(buf + off);
                else if (sz == 1) v64 = *(unsigned char*)(buf + off);
                else v64 = *(int*)(buf + off);
                vTab.tab->map[f.name] = Value::Num_((double)v64);
            }
            off += sz;
        }
    }

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
    Interp* g_interpCurrent = nullptr;   // M2: active interp for meta hooks

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

    struct Parser
    {
        vector<Tok> toks;
        size_t p = 0;
        wstring* err;

        const Tok& Peek(int off = 0) const { return toks[min(p + off, toks.size() - 1)]; }
        Tok Next()
        {
            // NB: never write min(p++, ...) here — the Windows min macro
            // double-evaluates the argument and would skip a token.
            if (p >= toks.size())
                p = toks.size() - 1;
            return toks[p++];
        }
        bool IsPunct(const wchar_t* op) const
        {
            return Peek().t == TPunct && Peek().text == op;
        }
        bool IsKw(const wchar_t* kw) const
        {
            return Peek().t == TKw && Peek().text == kw;
        }
        bool EatPunct(const wchar_t* op)
        {
            if (IsPunct(op)) { p++; return true; }
            return false;
        }
        bool EatKw(const wchar_t* kw)
        {
            if (IsKw(kw)) { p++; return true; }
            return false;
        }
        void SkipNL()
        {
            while (Peek().t == TNL) p++;
        }
        void ExpectHelper(const wchar_t* op, int srcLine)
        {
            if (!EatPunct(op))
            {
                wstring context;
                int start = max(0, (int)p - 6);
                int end = min((int)toks.size(), (int)p + 4);
                for (int k = start; k < end; k++)
                {
                    if (k == (int)p) context += L" >>>";
                    context += L" [" + toks[k].text + L"]";
                    if (k == (int)p) context += L"<<< ";
                }
                (*err) = L"语法错误: 期望 " + wstring(op) + L" (from NodioVM.cpp:" + std::to_wstring(srcLine) + L", context:" + context + L") @ 行 " +
                         std::to_wstring(Peek().line);
            }
        }
#define Expect(op) ExpectHelper(op, __LINE__)

        PNode ParseBlock()   // until '}' or end
        {
            PNode blk = New(Node::Block);
            SkipNL();
            while (!IsPunct(L"}") && Peek().t != TEnd)
            {
                PNode st = ParseStatement();
                if (st) blk->list.push_back(st);
                SkipNL();
                if (!err->empty())
                {
                    fwprintf(stderr, L"[DEBUG ParseBlock err=%s p=%s line=%d]\n", err->c_str(), Peek().text.c_str(), Peek().line);
                    break;
                }
            }
            return blk;
        }

        PNode ParseStatement()
        {
            { static bool bT = GetEnvironmentVariableW(L"ATP", nullptr, 0) != 0; if (bT) fwprintf(stderr, L"%d ", Peek().line); }
            SkipNL();
            if (IsPunct(L";"))       // empty statement
            {
                p++;
                return nullptr;
            }
            if (IsPunct(L")"))
            {
                // stray close-paren or if-( closer appearing inside braced body
                p++;
                if (nIfParenOpen > 0)
                    nIfParenOpen--;
                return nullptr;
            }
                        if (IsPunct(L"{"))
            {
                // statement-leading '{' = bare block (scopes locals;
                // win/ui ctor bodies nest them)
                p++;
                PNode blk = ParseBlock();
                Expect(L"}");
                return blk;
            }
                        { static bool bDbg = GetEnvironmentVariableW(L"CMT", nullptr, 0) != 0; if (bDbg) fwprintf(stderr, L"%d ", Peek().line); }
            const Tok& t = Peek();
            if (t.t == TKw)
            {
                if (t.text == L"if") return ParseIf();
                if (t.text == L"try") return ParseTry();
                if (t.text == L"while") return ParseWhile();
                if (t.text == L"for") return ParseFor();
                if (t.text == L"select") return ParseSelect();
                if (t.text == L"with") return ParseWith();
                if (t.text == L"do") return ParseDoWhile();
                if (t.text == L"return")
                {
                    p++;
                    PNode n = New(Node::Return);
                    if (!PeekIsStmtEnd())
                    {
                        n->a = ParseExpr();
                        // aardio multi-return: return a, b, c
                        while (true)
                        {
                            size_t saveR = p;
                            SkipNL();
                            if (EatPunct(L","))
                            {
                                SkipNL();
                                if (PeekIsStmtEnd()) break;
                                n->list.push_back(ParseExpr());
                            }
                            else
                            {
                                p = saveR;
                                break;
                            }
                        }
                    }
                    return n;
                }
                if (t.text == L"break") { p++; return New(Node::Break); }
                if (t.text == L"continue") { p++; return New(Node::Continue); }
                if (t.text == L"var")
                {
                    p++;
                    PNode n = New(Node::VarDecl);
                    if (Peek().t == TIdent)
                        n->names.push_back(Next().text);
                    else
                        n->names.push_back(L"");
                    while (true)
                    {
                        size_t saveComma = p;
                        SkipNL();
                        if (EatPunct(L",") && !PeekIsAssignEnd())
                        {
                            SkipNL();
                            if (Peek().t == TIdent)
                                n->names.push_back(Next().text);
                            else
                                n->names.push_back(L"");
                        }
                        else
                        {
                            p = saveComma;
                            break;
                        }
                    }
                    SkipNL();
                    if (EatPunct(L"="))
                    {
                        SkipNL();
                        n->a = ParseExpr();
                        // var a,b = e1,e2 — init list per name; when the
                        // first expr yields multi-returns the extras fill
                        // the remaining names (VarDecl eval)
                        if (!n->b) n->b = New(Node::Block);
                        while (true)
                        {
                            size_t saveVal = p;
                            SkipNL();
                            if (EatPunct(L","))
                            {
                                SkipNL();
                                if (PeekIsStmtEnd()) break;
                                if (IsPunct(L","))
                                {
                                    n->b->list.push_back(New(Node::NullK));
                                    continue;
                                }
                                n->b->list.push_back(ParseExpr());
                            }
                            else
                            {
                                p = saveVal;
                                break;
                            }
                        }
                    }
                    return n;
                }
                if (t.text == L"import")
                {
                    // import a.b.c — builtin roots or <scriptdir>\a\b\c.aardio
                    p++;
                    PNode n = New(Node::Import);
                    n->names.push_back(ExpectIdent());
                    while (IsPunct(L".") && Peek(1).t == TIdent)
                    {
                        p++;
                        n->names.push_back(Next().text);
                    }
                    return n;
                }
                if (t.text == L"namespace")
                {
                    // namespace a.b { ... } — bare assignments inside the
                    // block land in the (resolved-or-created) namespace
                    // table; 'namespace a.b;' scopes the REST OF FILE
                    // aardio also supports 'namespace ..Name' (global root prefix)
                    p++;
                    PNode n = New(Node::Namespace);
                    if (IsPunct(L"..") || IsPunct(L"::"))
                    {
                        p++;
                        n->bRoot = true;
                    }
                    n->names.push_back(ExpectIdent());
                    while (IsPunct(L".") && Peek(1).t == TIdent)
                    {
                        p++;
                        n->names.push_back(Next().text);
                    }
                    // aardio: 'namespace a.b;' or 'namespace a.b\n' (newline/EOF/non-brace)
                    // scopes the rest of the file
                    bool bRestOfFile = false;
                    if (EatPunct(L";"))
                        bRestOfFile = true;
                    else if (Peek(-1).t == TNL || (!IsPunct(L"{") && Peek().t != TEnd))
                        bRestOfFile = true;
                    if (bRestOfFile)
                    {
                        // rest-of-file namespace
                        PNode blk = New(Node::Block);
                        while (Peek().t != TEnd)
                        {
                            PNode st = ParseStatement();
                            if (st) blk->list.push_back(st);
                            SkipNL();
                            if (!err->empty()) break;
                        }
                        n->a = blk;
                        return n;
                    }
                    Expect(L"{");
                    n->a = ParseBlock();
                    Expect(L"}");
                    return n;
                }
                if (t.text == L"class")
                {
                    // class Name { ctor(p){...}; member = v; fn = function(){} }
                    // aardio also allows dotted names: class win.form {...}
                    // (declares into the namespace path); 'class ::Name'
                    // declares on the global root
                    p++;
                    PNode n = New(Node::Class);
                    if (IsPunct(L"::") || IsPunct(L".."))
                    {
                        p++;
                        n->bRoot = true;
                    }
                    n->names.push_back(ExpectIdent());
                    while (IsPunct(L".") && Peek(1).t == TIdent)
                    {
                        p++;
                        n->names.push_back(Next().text);
                    }
                    SkipNL();
                    Expect(L"{");
                    n->a = New(Node::Block);
                    wstring structDesc;
                    while (!IsPunct(L"}") && Peek().t != TEnd)
                    {
                        SkipNL();
                        if (IsPunct(L"}")) break;
                        if (IsPunct(L";")) { p++; continue; }
                        // round M2: '@path;' — meta reference on the class
                        if (IsPunct(L"@"))
                        {
                            p++;   // '@'
                            PNode path = ParseExpr();
                            PNode asg = New(Node::Assign);
                            asg->str = L"=";
                            PNode key = New(Node::Str);
                            key->str = L"_meta";
                            PNode idx = New(Node::Index);
                            PNode th = New(Node::Ident);
                            th->str = L"this";
                            idx->a = th;
                            idx->b = key;
                            asg->a = idx;
                            asg->b = path;
                            n->a->list.push_back(asg);
                            SkipNL();
                            EatPunct(L";");
                            continue;
                        }
                        // round M2: struct member declaration
                        //   int h;  WORD name[32];  int x = 5;
                        if (Peek().t == TIdent && IsStructType(Peek().text) &&
                            Peek(1).t == TIdent)
                        {
                            wstring strType = Next().text;
                            wstring strName = Next().text;
                            int nArr = 0;
                            if (IsPunct(L"["))
                            {
                                p++;
                                if (IsPunct(L"]"))
                                    nArr = -1;
                                else
                                    nArr = (int)Peek().num, p++;
                                Expect(L"]");
                            }
                            structDesc += strType + L" " + strName +
                                (nArr ? (nArr < 0 ? L"[0]" :
                                         L"[" + std::to_wstring(nArr) + L"]")
                                      : L"") + L";";
                            PNode asgV = New(Node::Assign);
                            asgV->str = L"=";
                            PNode idxV = New(Node::Index);
                            PNode thS = New(Node::Ident);
                            thS->str = L"this";
                            PNode kV = New(Node::Str);
                            kV->str = strName;
                            idxV->a = thS;
                            idxV->b = kV;
                            asgV->a = idxV;
                            if (EatPunct(L"="))
                                asgV->b = ParseExpr();
                            else
                                asgV->b = New(Node::NullK);
                            n->a->list.push_back(asgV);
                            SkipNL();
                            EatPunct(L";");
                            EatPunct(L",");
                            SkipNL();
                            continue;
                        }
                        if (Peek().t == TIdent && Peek().text == L"ctor" &&
                            Peek(1).t == TPunct && Peek(1).text == L"(")
                        {
                            p++;                     // 'ctor'
                            n->b = ParseFuncRest();  // FuncLit: params+body
                        }
                        else
                        {
                            PNode st = ParseStatement();
                            if (st) n->a->list.push_back(st);
                        }
                        SkipNL();
                    }
                    Expect(L"}");
                    if (!structDesc.empty())
                    {
                        PNode asgS = New(Node::Assign);
                        asgS->str = L"=";
                        PNode idxS = New(Node::Index);
                        PNode thS = New(Node::Ident);
                        thS->str = L"this";
                        PNode kS = New(Node::Str);
                        kS->str = L"_struct";
                        idxS->a = thS;
                        idxS->b = kS;
                        PNode vT = New(Node::Str);
                        vT->str = structDesc;
                        asgS->a = idxS;
                        asgS->b = vT;
                        n->a->list.insert(n->a->list.begin(), asgS);
                    }
                    return n;
                }
                if (t.text == L"function")
                {
                    // function name(params) { } -> assignment
                    p++;
                    wstring name = ExpectIdent();
                    PNode fn = ParseFuncRest();
                    PNode asg = New(Node::Assign);
                    asg->str = L"=";   // NB: DoAssign keys off op; unset means compound
                    PNode id = New(Node::Ident);
                    id->str = name;
                    asg->a = id;
                    asg->b = fn;
                    return asg;
                }
            }
            // expression / assignment
            // multi-target assign: a,b,c = e — aardio names unpack
            if (Peek().t == TIdent)
            {
                size_t save = p;
                std::vector<wstring> names;
                names.push_back(Next().text);
                while (IsPunct(L",") && Peek(1).t == TIdent &&
                       (Peek(2).t == TIdent || (Peek(2).t == TPunct && Peek(2).text == L"=") || (Peek(2).t == TPunct && Peek(2).text == L"," && Peek(3).t == TIdent)))
                {
                    p++;
                    names.push_back(Next().text);
                }
                // member/index targets: x.dl,x.dt = a,b — aardio unpacks into
                // member paths too; detect when a target starts with . or [
                if (names.size() == 1 &&
                    (IsPunct(L".") || IsPunct(L"[") || IsPunct(L",")))
                {
                    PNode first = New(Node::Ident);
                    first->str = names[0];
                    first = ParsePostfixFrom(first);
                    std::vector<PNode> tgts;
                    tgts.push_back(first);
                    while (IsPunct(L",") && Peek(1).t == TIdent)
                    {
                        p++;
                        PNode t2 = New(Node::Ident);
                        t2->str = Next().text;
                        tgts.push_back(ParsePostfixFrom(t2));
                    }
                    if (IsPunct(L"=") && tgts.size() > 1)
                    {
                        p++;
                        PNode n = New(Node::Assign);
                        n->str = L"=";
                        n->a = tgts[0];           // first target
                        if (!n->b) n->b = New(Node::Block);
                        // positional extras after the first expr
                        SkipNL();
                        n->b->list.push_back(ParseExpr());
                        while (true)
                        {
                            size_t saveVal = p;
                            SkipNL();
                            if (EatPunct(L","))
                            {
                                SkipNL();
                                if (PeekIsStmtEnd()) break;
                                n->b->list.push_back(ParseExpr());
                            }
                            else
                            {
                                p = saveVal;
                                break;
                            }
                        }
                        // remaining targets live in c->list
                        if (!n->c) n->c = New(Node::Block);
                        for (size_t ti = 1; ti < tgts.size(); ti++)
                            n->c->list.push_back(tgts[ti]);
                        return n;
                    }
                    p = save;
                }
                if (IsPunct(L"=") && names.size() > 1)
                {
                    p++;
                    PNode n = New(Node::Assign);
                    n->str = L"=";
                    n->names = names;
                    SkipNL();
                    n->a = ParseExpr();
                    // a,b = e1,e2 — per-name init list
                    if (!n->b) n->b = New(Node::Block);
                    while (true)
                    {
                        size_t saveVal = p;
                        SkipNL();
                        if (EatPunct(L","))
                        {
                            SkipNL();
                            if (PeekIsStmtEnd()) break;
                            n->b->list.push_back(ParseExpr());
                        }
                        else
                        {
                            p = saveVal;
                            break;
                        }
                    }
                    return n;
                }
                p = save;
            }

            bInAssignLhs = true;
            PNode e = ParseExpr();
            bInAssignLhs = false;
            const wchar_t* ops[] = { L"=", L"::=", L":=", L"+=", L"-=",
                                     L"*=", L"/=", L"%=", L"^=", nullptr };
            for (int k = 0; ops[k]; k++)
            {
                if (Peek().t == TPunct && Peek().text == ops[k])
                {
                    p++;
                    PNode asg = New(Node::Assign);
                    asg->str = ops[k];
                    asg->a = e;
                    asg->b = ParseExpr();
                    return asg;
                }
            }
            if ((IsPunct(L"++") || IsPunct(L"--")) && Peek(1).t != TEnd &&
                (Peek(1).t == TNL || Peek(1).t == TEnd ||
                 (Peek(1).t == TPunct && (Peek(1).text == L";" ||
                                          Peek(1).text == L"}"))))
            {
                p++;   // consume the postfix operator
                PNode asg = New(Node::Assign);
                asg->str = (p >= 2 && toks[p - 1].text == L"++") ? L"+=" : L"-=";
                asg->a = e;
                PNode one = New(Node::Num);
                one->num = 1;
                asg->b = one;
                return asg;
            }
            PNode st = New(Node::ExprStmt);
            st->a = e;
            return st;
        }

        bool PeekIsAssignEnd() const
        {
            return Peek().t == TPunct && Peek().text == L"=";
        }
        bool PeekIsStmtEnd() const
        {
            return Peek().t == TEnd || Peek().t == TNL ||
                   IsPunct(L";") || IsPunct(L"}");
        }

        wstring ExpectIdent()
        {
            if (Peek().t == TIdent)
                return Next().text;
            (*err) = L"语法错误: 期望标识符 @ 行 " + std::to_wstring(Peek().line);
            return L"?";
        }

        PNode ParseFuncRest()
        {
            Expect(L"(");
            PNode fn = New(Node::FuncLit);
            if (!IsPunct(L")"))
            {
                // aardio varargs: function(f, ...) — "..." collects the
                // remaining arguments into a 1-based table
                if (IsPunct(L"..."))
                {
                    p++;
                    fn->names.push_back(L"...");
                    fn->defaults.push_back(nullptr);
                }
                else
                {
                    // aardio default params: function(a, b=1, c="x") — the
                    // default expression is evaluated at call time when the
                    // argument is missing
                    fn->names.push_back(ExpectIdent());
                    if (EatPunct(L"="))
                        fn->defaults.push_back(ParseExpr());
                    else
                        fn->defaults.push_back(nullptr);
                    while (EatPunct(L","))
                    {
                        SkipNL();
                        if (IsPunct(L"..."))
                        {
                            p++;
                            fn->names.push_back(L"...");
                            fn->defaults.push_back(nullptr);
                            break;
                        }
                        fn->names.push_back(ExpectIdent());
                        if (EatPunct(L"="))
                            fn->defaults.push_back(ParseExpr());
                        else
                            fn->defaults.push_back(nullptr);
                    }
                }
            }
            Expect(L")");
            SkipNL();
            if (IsPunct(L"{"))
            {
                Expect(L"{");
                fn->a = ParseBlock();
                Expect(L"}");
            }
            else
            {
                // brace-less body: function() stmt (e.g. for loop, assign, expr)
                PNode blk = New(Node::Block);
                PNode st = ParseStatement();
                if (st)
                {
                    if (st->k == Node::ExprStmt)
                    {
                        PNode r = New(Node::Return);
                        r->a = st->a;
                        blk->list.push_back(r);
                    }
                    else
                    {
                        blk->list.push_back(st);
                    }
                }
                fn->a = blk;
            }
            return fn;
        }

        // body: '{block}' or a single unbraced statement (aardio form)
        PNode ParseBody()
        {
            SkipNL();
            if (IsPunct(L"{"))
            {
                p++;
                PNode blk = ParseBlock();
                Expect(L"}");
                return blk;
            }
            PNode blk = New(Node::Block);
            PNode st = ParseStatement();
            if (st) blk->list.push_back(st);
            return blk;
        }

        PNode ParseIf()
        {
            p++;  // if
            PNode n = New(Node::If);
            n->c = New(Node::Block);   // elseif/else chain lives here
            bool bHadParen = EatPunct(L"(");
            // aardio lib sources span if-conds across lines with inline
            // `return` (win.ui): sequence exprs; last value is the cond,
            // an inline return unwinds during evaluation
            bool bIfParenClosed = false;
            {
                PNode seq = New(Node::Block);
                seq->list.push_back(ParseExpr());
                // shipped lib code writes `if( a = b )` (win.ui 2101) — a
                // lone '=' at cond level reads as equality
                while (IsPunct(L"=") && Peek(1).t != TEnd)
                {
                    PNode asgL = seq->list.back();
                    p++;   // '='
                    PNode n = New(Node::Bin);
                    n->str = L"==";
                    n->a = asgL;
                    n->b = ParseExpr();
                    seq->list[seq->list.size() - 1] = n;
                }
                while (IsPunct(L")") || IsPunct(L"("))
                {
                    // inline `return` continuation only applies while the
                    // if-( group is still open (win.ui multi-line conds);
                    // a paren-less `if cond return x` has return as BODY
                    SkipNL();
                    if (!(Peek().t == TKw && Peek().text == L"return"))
                        break;
                    seq->list.push_back(ParseExpr());
                }
                SkipNL();
                if (seq->list.size() > 1)
                    n->a = seq;
                else
                    n->a = seq->list[0];
            // aardio allows `if( a ) && ( b ) { }` — the if-( closer is
            // followed by && / || groups; fold them into the cond
            while (IsPunct(L")") &&
                   (Peek(1).t == TPunct && (Peek(1).text == L"&&" ||
                                            Peek(1).text == L"||")))
            {
                p++;   // ')'
                bIfParenClosed = true;
                wstring op = Next().text;   // && / ||
                SkipNL();
                PNode rhs;
                if (IsPunct(L"("))
                {
                    p++;
                    rhs = ParseExpr();
                    SkipNL();
                    Expect(L")");
                }
                else
                    rhs = ParseExpr();
                PNode bin = New(Node::Bin);
                bin->str = op;
                bin->a = n->a;
                bin->b = rhs;
                n->a = bin;
                SkipNL();
            }
            }
            // aardio sugar: if( cond { body } ) — the closing ')' may
            // trail the body (win.ui 421-432 uses this form)
            if (IsPunct(L"{"))
            {
                nIfParenOpen++;   // a stray ')' inside the body belongs to if(
                n->b = ParseBody();
                nIfParenOpen--;
                while (EatPunct(L";")) {}
                SkipNL();
                if (IsPunct(L")"))
                    p++;   // the if-( closer trailing the body
            }
            else
            {
                SkipNL();
                if (!bIfParenClosed && bHadParen)
                    Expect(L")");
                nIfParenOpen++;   // a stray ')' inside the body belongs to if(
                n->b = ParseBody();
                nIfParenOpen--;
                while (EatPunct(L";")) {}
                SkipNL();
            }
            // elseif/else chain in c->list: [cond, block, cond, block, ..., elseBlock]
            while (true)
            {
                while (EatPunct(L";")) {}
                SkipNL();
                if (!IsKw(L"elseif")) break;
                p++;
                Expect(L"(");
                n->c->list.push_back(ParseExpr());
                Expect(L")");
                n->c->list.push_back(ParseBody());
                while (EatPunct(L";")) {}
                SkipNL();
            }
            while (EatPunct(L";")) {}
            SkipNL();
            if (IsKw(L"else"))
            {
                p++;
                SkipNL();
                n->c->list.push_back(ParseBody());  // trailing = else
                while (EatPunct(L";")) {}
                SkipNL();
            }
            return n;
        }

        PNode ParseWhile()
        {
            p++;
            PNode n = New(Node::While);
            Expect(L"(");
            SkipNL();
            std::vector<PNode> condStmts;
            while (!IsPunct(L")") && Peek().t != TEnd)
            {
                PNode st = ParseStatement();
                if (st) condStmts.push_back(st);
                while (EatPunct(L";")) {}
                SkipNL();
                if (!err->empty()) break;
            }
            Expect(L")");
            if (condStmts.size() == 1)
                n->a = condStmts[0];
            else
            {
                PNode blk = New(Node::Block);
                blk->list = std::move(condStmts);
                n->a = blk;
            }
            n->b = ParseBody();
            return n;
        }

        // with(expr) { body } — bind expr's table members as locals for the
        // body scope; `owner` inside the body is the expr value (win.ui)
        PNode ParseWith()
        {
            p++;
            PNode n = New(Node::With);
            SkipNL();
            if (IsPunct(L"("))
            {
                p++;
                n->a = ParseExpr();
                SkipNL();
                Expect(L")");
            }
            else
                n->a = ParseExpr();
            n->b = ParseBody();
            return n;
        }

        // do { body } while(cond) - the C form; condition may be
        // parenthesized or bare
        PNode ParseDoWhile()
        {
            p++;
            PNode n = New(Node::DoWhile);
            SkipNL();
            n->b = ParseBody();
            SkipNL();
            EatKw(L"while");
            SkipNL();
            if (IsPunct(L"("))
            {
                p++;
                n->a = ParseExpr();
                SkipNL();
                Expect(L")");
            }
            else
                n->a = ParseExpr();
            while (EatPunct(L";")) {}
            SkipNL();
            return n;
        }

        // M7: select(v) { case <op?> v1[,v2..] {..} ... else {..} } —
        // identity compare by default (===); an optional leading operator
        // (== != > < >= <=) overrides the comparator; `case a;b` / `case
        // a to b` are inclusive numeric ranges (>= <=); first match wins;
        // `else` / `case else` is the default arm
        PNode ParseSelect()
        {
            p++;   // select
            PNode n = New(Node::If);
            PNode sel = New(Node::Num);   // sentinel: b->num==1 = select form
            sel->num = 1;
            n->b = sel;
            Expect(L"(");
            n->a = ParseExpr();          // the select value
            Expect(L")");
            SkipNL();
            n->c = New(Node::Block);
            Expect(L"{");
            SkipNL();
            while (!IsPunct(L"}") && Peek().t != TEnd)
            {
                SkipNL();
                if (IsPunct(L"}")) break;
                bool bElse = false;
                wstring op = L"===";
                if (IsKw(L"case"))
                {
                    p++;
                    // optional comparator
                    static const wchar_t* CMP[] = { L"===", L"!==", L"==",
                        L"!=", L">=", L"<=", L">", L"<", nullptr };
                    for (int k = 0; CMP[k]; k++)
                    {
                        if (IsPunct(CMP[k]))
                        {
                            op = CMP[k];
                            p++;
                            break;
                        }
                    }
                    if (IsKw(L"else"))
                    {
                        p++;
                        bElse = true;
                    }
                }
                else if (IsKw(L"else"))
                {
                    p++;
                    bElse = true;
                }
                else
                {
                    (*err) = L"语法错误: select 内期望 case @ 行 " +
                             std::to_wstring(Peek().line);
                    return n;
                }
                PNode blk;
                if (!bElse)
                {
                    // case values: v1[,v2...] and/or range a;b / a to b
                    PNode c1 = ParseExpr();
                    if (IsPunct(L";") ||
                        (Peek().t == TIdent && Peek().text == L"to"))
                    {
                        p++;
                        PNode c2 = ParseExpr();
                        // encode a range as a Bin node: str="range",
                        // a=v1, b=v2, c=Str(op) — eval applies >= <=
                        PNode rng = New(Node::Bin);
                        rng->str = L"range";
                        rng->a = c1;
                        rng->b = c2;
                        rng->c = nullptr;   // NB: null = chain tail (else arms misfire)
                        n->c->list.push_back(rng);
                    }
                    else
                    {
                        // first value (op may be an operator form); the
                        // multi-value chain continues via ','
                        PNode first = New(Node::Bin);
                        first->str = op;
                        first->a = nullptr;   // a = select value (filled at eval)
                        first->b = c1;
                        PNode chain = first;
                        while (EatPunct(L","))
                        {
                            SkipNL();
                            PNode nxt = New(Node::Bin);
                            nxt->str = op;
                            nxt->a = nullptr;
                            nxt->b = ParseExpr();
                            chain->c = nxt;   // list through c
                            chain = nxt;
                        }
                        n->c->list.push_back(first);   // NB: chain walked to the tail
                    }
                }
                else
                    n->c->list.push_back(nullptr);
                SkipNL();
                // arm body
                n->c->list.push_back(ParseBody());
                while (EatPunct(L";")) {}
                SkipNL();
            }
            Expect(L"}");
            return n;
        }

        PNode ParseFor()
        {
            p++;
            bool paren = EatPunct(L"(");
            // for-in: for k, v in tab { } (parenthesized form also accepted)
            if (Peek().t == TIdent &&
                (Peek(1).t == TKw && Peek(1).text == L"in" ||
                 (Peek(1).t == TPunct && Peek(1).text == L",")))
            {
                PNode n = New(Node::ForIn);
                n->names.push_back(Next().text);
                while (EatPunct(L","))
                    n->names.push_back(ExpectIdent());
                if (!EatKw(L"in"))
                {
                    (*err) = L"语法错误: for-in 缺少 in @ 行 " +
                             std::to_wstring(Peek().line);
                    return n;
                }
                n->a = ParseExpr();
                if (paren) Expect(L")");
                n->b = ParseBody();
                return n;
            }
            // numeric: for(i=1;cond;step){} — step/cond may be omitted;
            // 2-expr form for(i=n;limit) counts n..limit (1 if limit<n)
            PNode n = New(Node::ForNum);
            if (!paren)
                Expect(L"(");
            n->a = ParseStatement();     // init (assign)
            if (EatPunct(L";")) { /* optional */ }
            SkipNL();
            n->b = ParseExpr();          // cond
            if (EatPunct(L";")) { /* optional */ }
            SkipNL();
            if (!IsPunct(L")"))
                n->c = ParseStatement(); // step (may be absent)
            Expect(L")");
            n->names.push_back(L"limit");  // tag: middle is a limit
            n->d = ParseBody();
            return n;
        }

        PNode ParseTry()
        {
            // try { } catch(e) { } — e optional
            p++;   // try
            PNode n = New(Node::Try);
            SkipNL();
            Expect(L"{");
            n->a = ParseBlock();
            Expect(L"}");
            SkipNL();
            if (IsKw(L"catch"))
            {
                p++;
                SkipNL();
                if (EatPunct(L"("))
                {
                    n->names.push_back(ExpectIdent());
                    Expect(L")");
                    SkipNL();
                }
                Expect(L"{");
                n->b = ParseBlock();
                Expect(L"}");
            }
            return n;
        }

        PNode New(Node::K k)   // member: stamps the current token line
        {
            PNode n = std::make_shared<Node>();
            n->k = k;
            n->line = Peek().line;
            return n;
        }

        // expressions (precedence climbing)
        bool bNoCoalesce = false;   // inside a ternary's middle branch
        int nIfParenOpen = 0;       // aardio if( cond { } ) sugar
        bool bInAssignLhs = false;  // top-level statement assignment target
        PNode ParseExpr() { return ParseTernary(); }
        PNode ParseTernary()
        {
            PNode l = ParseOr();
            // aardio also lets the ? start the next line (win/_.aardio 315)
            size_t saveT = p;
            SkipNL();
            bool bQ = IsPunct(L"?");
            if (!bQ)
                p = saveT;
            if (bQ)
            {
                p++;
                PNode n = New(Node::Bin);
                n->str = L"?:";
                n->a = l;
                // the middle branch must not swallow the ternary's ':'
                bool bOld = bNoCoalesce;
                bNoCoalesce = true;
                n->b = ParseExpr();
                bNoCoalesce = bOld;
                SkipNL();
                if (IsPunct(L":"))
                {
                    p++;
                    n->c = ParseExpr();
                    return n;      // true ternary cond ? a : b
                }
                // aardio `a ? b` guard: null/false → false, else b (lazy);
                // encode as Bin "?" — eval: a truthy ? b : false
                n->str = L"?";
                return n;
            }
            return l;
        }
        PNode ParseOr()
        {
            PNode l = ParseAnd();
            while (true)
            {
                size_t save = p;
                SkipNL();
                if (!(IsPunct(L"||") || IsKw(L"or")))
                {
                    p = save;
                    break;
                }
                p++;
                PNode n = New(Node::Bin);
                n->str = L"||";
                n->a = l;
                n->b = ParseAnd();
                l = n;
            }
            // aardio a:b — null coalesce (b evaluated lazily)
            if (bNoCoalesce)
                return l;
            while (true)
            {
                size_t saveC = p;
                SkipNL();
                if (!(IsPunct(L":") && Peek(1).t != TEnd &&
                      !(Peek(1).t == TPunct && Peek(1).text == L"=")))
                {
                    p = saveC;
                    break;
                }
                p++;
                PNode n = New(Node::Bin);
                n->str = L":";
                n->a = l;
                n->b = ParseAnd();
                l = n;
            }
            return l;
        }
        PNode ParseAnd()
        {
            PNode l = ParseEq();
            while (true)
            {
                // aardio: a binary operator may start the NEXT line
                // aardio: a binary operator may start the NEXT line —
                // peek past newlines and restore when no operator follows
                size_t save = p;
                SkipNL();
                if (!(IsPunct(L"&&") || IsKw(L"and")))
                {
                    p = save;
                    break;
                }
                p++;
                PNode n = New(Node::Bin);
                n->str = L"&&";
                n->a = l;
                n->b = ParseEq();
                l = n;
            }
            return l;
        }
        PNode ParseBitOr()
        {
            PNode l = ParseBitXor();
            while (IsPunct(L"|") || IsPunct(L"|="))
            {
                wstring op = Next().text;
                if (op == L"|=")
                {
                    PNode n = New(Node::Bin);
                    n->str = L"|=";
                    n->a = l;
                    n->b = ParseBitXor();
                    l = n;
                }
                else
                {
                    PNode n = New(Node::Bin);
                    n->str = L"|";
                    n->a = l;
                    n->b = ParseBitXor();
                    l = n;
                }
            }
            return l;
        }
        PNode ParseBitXor()
        {
            PNode l = ParseBitAnd();
            while (IsPunct(L"^"))
            {
                p++;
                PNode n = New(Node::Bin);
                n->str = L"^x";
                n->a = l;
                n->b = ParseBitAnd();
                l = n;
            }
            return l;
        }
        PNode ParseBitAnd()
        {
            PNode l = ParseShift();
            while (IsPunct(L"&") || IsPunct(L"&="))
            {
                wstring op = Next().text;
                PNode n = New(Node::Bin);
                n->str = op;
                n->a = l;
                n->b = ParseShift();
                l = n;
            }
            return l;
        }
        PNode ParseShift()
        {
            PNode l = ParseCmp();
            while (true)
            {
                size_t save = p;
                SkipNL();
                if (!(IsPunct(L"<<") || IsPunct(L">>") || IsPunct(L">>>") ||
                      IsPunct(L"<<=") || IsPunct(L">>=")))
                {
                    p = save;
                    break;
                }
                wstring op = Next().text;
                PNode n = New(Node::Bin);
                n->str = op;
                n->a = l;
                SkipNL();
                n->b = ParseCmp();
                l = n;
            }
            return l;
        }
        PNode ParseEq()
        {
            PNode l = ParseBitOr();
            while (true)
            {
                size_t save = p;
                SkipNL();
                if (!(IsPunct(L"===") || IsPunct(L"==") || IsPunct(L"!=") ||
                      IsPunct(L"!==") || (!bInAssignLhs && IsPunct(L"="))))
                {
                    p = save;
                    break;
                }
                wstring op = Next().text;
                if (op == L"=") op = L"==";
                PNode n = New(Node::Bin);
                n->str = op;
                n->a = l;
                SkipNL();
                n->b = ParseBitOr();
                l = n;
            }
            return l;
        }
        PNode ParseCmp()
        {
            PNode l = ParseConcat();
            while (true)
            {
                size_t save = p;
                SkipNL();
                if (!(IsPunct(L"<") || IsPunct(L">") || IsPunct(L"<=") || IsPunct(L">=")))
                {
                    p = save;
                    break;
                }
                PNode n = New(Node::Bin);
                n->str = Next().text;
                n->a = l;
                SkipNL();
                n->b = ParseConcat();
                l = n;
            }
            return l;
        }
        PNode ParseConcat()   // '..'/'++' bind below + (concat level), right assoc
        {
            PNode l = ParseAdd();
            // aardio postfix 'expr++' (increment statement): when '++' is
            // followed by a statement end it binds to the left expr, not concat
           if ((IsPunct(L"++") || IsPunct(L"--")) &&
                (Peek(1).t == TNL || Peek(1).t == TEnd ||
                 (Peek(1).t == TPunct && (Peek(1).text == L";" ||
                                          Peek(1).text == L"}"))))
                return l;
            if (IsPunct(L"..") || IsPunct(L"++"))
            {
                PNode n = New(Node::Bin);
                n->str = Next().text;
                n->a = l;
                n->b = ParseConcat();
                return n;
            }
            return l;
        }
        PNode ParseAdd()
        {
            PNode l = ParseMul();
            while (true)
            {
                size_t save = p;
                SkipNL();
                if (!(IsPunct(L"+") || IsPunct(L"-")))
                {
                    p = save;
                    break;
                }
                PNode n = New(Node::Bin);
                n->str = Next().text;
                n->a = l;
                SkipNL();
                n->b = ParseMul();
                l = n;
            }
            return l;
        }
        PNode ParseMul()
        {
            PNode l = ParseUnary();
            while (true)
            {
                size_t save = p;
                SkipNL();
                if (!(IsPunct(L"*") || IsPunct(L"/") || IsPunct(L"%")))
                {
                    p = save;
                    break;
                }
                PNode n = New(Node::Bin);
                n->str = Next().text;
                n->a = l;
                SkipNL();
                n->b = ParseUnary();
                l = n;
            }
            return l;
        }
        PNode ParseUnary()
        {
            if (IsPunct(L"-") || IsPunct(L"!") || IsKw(L"not") ||
                IsPunct(L"#") || IsPunct(L"~"))
            {
                PNode n = New(Node::Un);
                n->str = Peek().text;
                p++;
                n->a = ParseUnary();
                return n;
            }
            return ParsePow();
        }
        PNode ParsePow()
        {
            PNode l = ParsePostfix();
            if (IsPunct(L"^") || IsPunct(L"**"))
            {
                wstring op = Next().text;
                PNode n = New(Node::Bin);
                n->str = op;
                n->a = l;
                n->b = ParseUnary();   // right assoc
                return n;
            }
            return l;
        }
        PNode ParsePostfix()
        {
            PNode e = ParsePrimary();
            while (true)
            {
                if (IsPunct(L".") && (Peek(1).t == TIdent || Peek(1).t == TKw || (Peek(1).t == TPunct && Peek(1).text == L"$")))
                {
                    p++;
                    PNode m = New(Node::Member);
                    m->a = e;
                    // '$name' members span two tokens ('$' + ident)
                    if (Peek().t == TPunct && Peek().text == L"$" &&
                        Peek(1).t == TIdent)
                    {
                        p++;
                        m->str = L"$" + Next().text;
                    }
                    else
                        m->str = Next().text;
                    e = m;
                }
                else if (IsPunct(L"?."))
                {
                    p++;
                    PNode m = New(Node::Member);
                    m->bSafe = true;
                    m->a = e;
                    if (Peek().t == TPunct && Peek().text == L"$" &&
                        Peek(1).t == TIdent)
                    {
                        p++;
                        m->str = L"$" + Next().text;
                    }
                    else
                        m->str = Next().text;
                    e = m;
                }
                else if (IsPunct(L"?["))
                {
                    p++;
                    PNode m = New(Node::Index);
                    m->bSafe = true;
                    m->a = e;
                    bool bOldLhs = bInAssignLhs;
                    bInAssignLhs = false;
                    m->b = ParseExpr();
                    bInAssignLhs = bOldLhs;
                    Expect(L"]");
                    e = m;
                }
                else if (IsPunct(L"[") && Peek(1).t == TPunct &&
                         Peek(1).text == L"[")
                {
                    // aardio t[["k"]] — direct-key index, skips metatables
                    p += 2;
                    PNode m = New(Node::Index);
                    m->a = e;
                    m->bDirect = true;
                    bool bOldLhs = bInAssignLhs;
                    bInAssignLhs = false;
                    m->b = ParseExpr();
                    bInAssignLhs = bOldLhs;
                    Expect(L"]");
                    Expect(L"]");
                    e = m;
                }
                else if (IsPunct(L"@") && Peek(1).t != TIdent)
                {
                    // aardio 'tab@' — metatable get/set (postfix @ not
                    // followed by an ident path; '@path' is the M2 form)
                    p++;
                    PNode m = New(Node::Member);
                    m->bMeta = true;
                    m->a = e;
                    m->str = L"_meta";
                    e = m;
                }
                else if (IsPunct(L"#"))
                {
                    // postfix # — aardio expr# = length (string chars /
                    // table array length)
                    p++;
                    PNode n = New(Node::Un);
                    n->str = L"#";
                    n->a = e;
                    e = ParsePostfixCont(n);
                }
                else if (IsPunct(L"["))
                {
                    p++;
                    PNode m = New(Node::Index);
                    m->a = e;
                    bool bOldLhs = bInAssignLhs;
                    bInAssignLhs = false;
                    m->b = ParseExpr();
                    bInAssignLhs = bOldLhs;
                    Expect(L"]");
                    e = m;
                }
                else if (IsPunct(L"("))
                {
                    p++;
                    PNode c = New(Node::Call);
                    c->a = e;
                    SkipNL();
                    bool bOldLhs = bInAssignLhs;
                    bInAssignLhs = false;
                    if (!IsPunct(L")"))
                    {
                        // aardio f(,,,,1): leading empty slots
                        if (IsPunct(L",") )
                            c->list.push_back(New(Node::NullK));
                        else if (IsPunct(L")"))
                            c->list.push_back(New(Node::NullK));  // empty slot before )
                        else if (Peek().t == TIdent && Peek(1).t == TPunct && Peek(1).text == L"=")
                        {
                            // named args: f(name = expr; ...) — one table arg
                            PNode tbl = New(Node::TableCtor);
                            while (Peek().t == TIdent && Peek(1).t == TPunct &&
                                   Peek(1).text == L"=")
                            {
                                PNode key = New(Node::Str);
                                key->str = Next().text;
                                p++;
                                tbl->list.push_back(key);
                                tbl->list.push_back(ParseExpr());
                                SkipNL();
                                EatPunct(L";");
                                SkipNL();
                                if (IsPunct(L",")) { p++; SkipNL(); continue; }
                            }
                            c->list.push_back(tbl);
                        }
                        else
                            c->list.push_back(ParseExpr());
                        SkipNL();
                        while (EatPunct(L","))
                            {
                                SkipNL();
                                if (IsPunct(L",") || IsPunct(L")"))
                                    c->list.push_back(New(Node::NullK));
                                else if (IsPunct(L")"))
                                    c->list.push_back(New(Node::NullK));  // empty slot before )
                                else
                                    c->list.push_back(ParseExpr());
                                SkipNL();
                            }
                    }
                    bInAssignLhs = bOldLhs;
                    Expect(L")");
                    e = c;
                }
                else if (IsPunct(L"..") && Peek(1).t == TIdent &&
                         !(Peek(1).line != Peek().line))   // root: '..' + ident
                {
                    // '..name' after expression start = global root member;
                    // handled in ParsePrimary for statement-leading form.
                    break;
                }
                else break;
            }
            return e;
        }
        PNode ParsePrimary()
        {
            const Tok& t = Peek();
            // "..." expression = the varargs table
            if (t.t == TPunct && t.text == L"...")
            {
                p++;
                PNode n = New(Node::Ident);
                n->str = L"...";
                return n;
            }
            if (t.t == TNum) { p++; PNode n = New(Node::Num); n->num = t.num; return n; }
            if (t.t == TStr) { p++; PNode n = New(Node::Str); n->str = t.text; return n; }
            if (t.t == TIdent)
            {
                p++;
                PNode n = New(Node::Ident);
                n->str = t.text;
                return n;
            }
            // root namespace '..ident' / '..io.print'
            if (IsPunct(L"..") && Peek(1).t == TIdent)
            {
                p += 2;
                PNode m = New(Node::Member);
                m->bRoot = true;
                PNode root = New(Node::Ident);
                root->str = L"..";
                m->a = root;
                m->str = Peek(-1).text;
                // continue postfix chain from the member
                return ParsePostfixFrom(m);
            }
            // '::ident' / '::io.print' — same root scope (aardio ::)
            if (IsPunct(L"::") && Peek(1).t == TIdent)
            {
                p += 2;
                PNode m = New(Node::Member);
                m->bRoot = true;
                PNode root = New(Node::Ident);
                root->str = L"..";
                m->a = root;
                m->str = Peek(-1).text;
                return ParsePostfixFrom(m);
            }
            if (IsPunct(L"("))
            {
                p++;
                SkipNL();
                // grouping restores null-coalesce freedom: a ternary
                // middle suppresses ':' but a nested '( a : b )'
                // inside it has its own scope (win/ui/ctrl 61)
                bool bOldC = bNoCoalesce;
                bNoCoalesce = false;
                bool bOldLhs = bInAssignLhs;
                bInAssignLhs = false;
                PNode e = ParseExpr();
                bInAssignLhs = bOldLhs;
                bNoCoalesce = bOldC;
                SkipNL();
                Expect(L")");
                return ParsePostfixFrom(e);
            }
            if (IsKw(L"true") || IsKw(L"false"))
            {
                p++;
                PNode n = New(Node::Bool);
                n->num = t.text == L"true" ? 1 : 0;
                return n;
            }
            if (IsKw(L"null")) { p++; return New(Node::NullK); }
            if (IsKw(L"function"))
            {
                p++;
                return ParseFuncRest();
            }
            // aardio lambda: lambda(a,b) expr — implicit return
            if (t.t == TKw && t.text == L"lambda")
            {
                p++;
                Expect(L"(");
                PNode fn = New(Node::FuncLit);
                if (!IsPunct(L")"))
                {
                    // lambda(f,...) - varargs param supported
                    if (IsPunct(L"..."))
                    {
                        p++;
                        fn->names.push_back(L"...");
                        fn->defaults.push_back(nullptr);
                    }
                    else
                    {
                    fn->names.push_back(ExpectIdent());
                    if (EatPunct(L"="))
                        fn->defaults.push_back(ParseExpr());
                    else
                        fn->defaults.push_back(nullptr);
                    while (EatPunct(L","))
                    {
                        SkipNL();
                        if (IsPunct(L"..."))
                        {
                            p++;
                            fn->names.push_back(L"...");
                            fn->defaults.push_back(nullptr);
                            break;
                        }
                        fn->names.push_back(ExpectIdent());
                        if (EatPunct(L"="))
                            fn->defaults.push_back(ParseExpr());
                        else
                            fn->defaults.push_back(nullptr);
                    }
                    }
                }
                Expect(L")");
                PNode body = New(Node::Block);
                PNode retSt = New(Node::Return);
                retSt->a = ParseExpr();
                // brace-less body may end in an assignment
                // (function(v) owner.x = v inside a table literal)
                if (Peek().t == TPunct &&
                    (Peek().text == L"=" || Peek().text == L":="))
                {
                    PNode asg = New(Node::Assign);
                    asg->str = Next().text;
                    asg->a = retSt->a;
                    asg->b = ParseExpr();
                    retSt->a = asg;
                }
                body->list.push_back(retSt);
                fn->a = body;
                return fn;
            }
            if (IsPunct(L"["))
            {
                // aardio array literal: [a,b,c] — 1-based table array
                // and [@meta] / [@meta, a, b]
                p++;
                PNode n = New(Node::TableCtor);
                SkipNL();
                int nArr = 0;
                while (!IsPunct(L"]") && Peek().t != TEnd)
                {
                    SkipNL();
                    if (IsPunct(L"]")) break;
                    if (IsPunct(L"@"))
                    {
                        p++;
                        PNode path = ParseExpr();
                        PNode key = New(Node::Str);
                        key->str = L"_meta";
                        n->list.push_back(key);
                        n->list.push_back(path);
                        SkipNL();
                        EatPunct(L";");
                        EatPunct(L",");
                        continue;
                    }
                    PNode kN = New(Node::Num);
                    kN->num = ++nArr;
                    n->list.push_back(kN);
                    n->list.push_back(ParseExpr());
                    SkipNL();
                    EatPunct(L",");
                }
                SkipNL();
                Expect(L"]");
                return n;
            }
            // aardio class expression: ::MSG = class{ ... } (no name)
            if (t.t == TKw && t.text == L"class")
            {
                p++;
                PNode n = New(Node::Class);
                if (IsPunct(L"::"))
                    p++;
                if (Peek().t == TIdent)
                    n->names.push_back(Next().text);
                SkipNL();
                Expect(L"{");
                n->a = New(Node::Block);
                wstring structDesc;
                while (!IsPunct(L"}") && Peek().t != TEnd)
                {
                    SkipNL();
                    if (IsPunct(L"}")) break;
                    if (IsPunct(L";")) { p++; continue; }
                    // member: name = expr | struct decl | '@path;'
                    if (IsPunct(L"@"))
                    {
                        p++;
                        PNode path = ParseExpr();
                        PNode asg = New(Node::Assign);
                        asg->str = L"=";
                        PNode key = New(Node::Str);
                        key->str = L"_meta";
                        PNode idx = New(Node::Index);
                        PNode th = New(Node::Ident);
                        th->str = L"this";
                        idx->a = th;
                        idx->b = key;
                        asg->a = idx;
                        asg->b = path;
                        n->a->list.push_back(asg);
                        SkipNL();
                        EatPunct(L";");
                        continue;
                    }
                    if (Peek().t == TIdent && IsStructType(Peek().text) &&
                        Peek(1).t == TIdent)
                    {
                        wstring strType = Next().text;
                        wstring strName = Next().text;
                        int nArr = 0;
                        if (IsPunct(L"["))
                        {
                            p++;
                            if (IsPunct(L"]"))
                                nArr = -1;
                            else
                                nArr = (int)Peek().num, p++;
                            Expect(L"]");
                        }
                        structDesc += strType + L" " + strName +
                            (nArr ? (nArr < 0 ? L"[0]" :
                                     L"[" + std::to_wstring(nArr) + L"]")
                                  : L"") + L";";
                        PNode asg = New(Node::Assign);
                        asg->str = L"=";
                        PNode key = New(Node::Str);
                        key->str = strName;
                        PNode idx = New(Node::Index);
                        PNode th = New(Node::Ident);
                        th->str = L"this";
                        idx->a = th;
                        idx->b = key;
                        asg->a = idx;
                        // struct member with initializer: INT cbSize = 48;
                        if (EatPunct(L"="))
                            asg->b = ParseExpr();
                        else
                        {
                            // default-init the struct member as null
                            PNode nn = New(Node::NullK);
                            asg->b = nn;
                        }
                        n->a->list.push_back(asg);
                        SkipNL();
                        EatPunct(L";");
                        EatPunct(L",");
                        SkipNL();
                        continue;
                    }
                    if ((Peek().t == TIdent || Peek().t == TKw) &&
                        Peek(1).t == TPunct && Peek(1).text == L"=")
                    {
                        PNode asg = New(Node::Assign);
                        asg->str = L"=";
                        PNode id = New(Node::Index);
                        PNode th = New(Node::Ident);
                        th->str = L"this";
                        PNode key = New(Node::Str);
                        key->str = Peek().text;
                        id->a = th;
                        id->b = key;
                        p += 2;
                        asg->a = id;
                        asg->b = ParseExpr();
                        n->a->list.push_back(asg);
                        SkipNL();
                        EatPunct(L";");
                        continue;
                    }
                    // ctor: function name(params){...} or ctor(...) - reuse
                    // the function-assignment path by falling back to
                    // ParseStatement for this member
                    PNode st = ParseStatement();
                    if (st) n->a->list.push_back(st);
                    SkipNL();
                }
                Expect(L"}");
                if (!structDesc.empty())
                {
                    PNode asgS = New(Node::Assign);
                    asgS->str = L"=";
                    PNode idxS = New(Node::Index);
                    PNode thS = New(Node::Ident);
                    thS->str = L"this";
                    PNode kS = New(Node::Str);
                    kS->str = L"_struct";
                    idxS->a = thS;
                    idxS->b = kS;
                    PNode vT = New(Node::Str);
                    vT->str = structDesc;
                    asgS->a = idxS;
                    asgS->b = vT;
                    n->a->list.insert(n->a->list.begin(), asgS);
                }
                return n;
            }
            // M7: `return` in expression position — aardio allows
            // `if( cond
            //      return f(x) )` in lib sources (win.ui): the inline
            // return exits the enclosing function with the value
            if (t.t == TKw && t.text == L"return")
            {
                p++;
                PNode n = New(Node::Return);
                if (!PeekIsStmtEnd() && !IsPunct(L")") && !IsPunct(L","))
                    n->a = ParseExpr();
                return n;
            }
            if (IsPunct(L"{"))
            {
                p++;
                PNode n = New(Node::TableCtor);
                SkipNL();
                wstring structDesc;
                while (!IsPunct(L"}") && Peek().t != TEnd)
                {
                    SkipNL();
                    if (IsPunct(L"}")) break;
                    // round M2: '@path;' — meta reference (stores under
                    // the "_meta" key; aardio tables/classes attach their
                    // metatable this way)
                    if (IsPunct(L"@"))
                    {
                        p++;   // '@'
                        PNode path = ParseExpr();
                        PNode key = New(Node::Str);
                        key->str = L"_meta";
                        n->list.push_back(key);
                        n->list.push_back(path);
                        SkipNL();
                        EatPunct(L";");
                        continue;
                    }
                    // round M2: struct member declaration —
                    //   int x;  WORD name[32];  int left = expr;
                    // emits a normal pair (init or null) plus a "_struct"
                    // layout string consumed by raw.struct/raw.api/raw.convert
                    if (Peek().t == TIdent && IsStructType(Peek().text) &&
                        Peek(1).t == TIdent)
                    {
                        wstring strType = Next().text;
                        wstring strName = Next().text;
                        int nArr = 0;
                        if (IsPunct(L"["))
                        {
                            p++;
                            if (IsPunct(L"]"))
                                nArr = -1;    // variable length [0]
                            else
                                nArr = (int)Peek().num, p++;
                            Expect(L"]");
                        }
                        structDesc += strType + L" " + strName +
                            (nArr ? (nArr < 0 ? L"[0]" :
                                     L"[" + std::to_wstring(nArr) + L"]")
                                 : L"") + L";";
                        // initializer or null value
                        PNode keyN = New(Node::Str);
                        keyN->str = strName;
                        n->list.push_back(keyN);
                        if (EatPunct(L"="))
                            n->list.push_back(ParseExpr());
                        else
                        {
                            PNode nn = New(Node::NullK);
                            n->list.push_back(nn);
                        }
                        SkipNL();
                        EatPunct(L";");
                        EatPunct(L",");
                        SkipNL();
                        continue;
                    }
                    // pair: name = expr | name: expr (aardio allows both separators;
                    // win/_.aardio uses {max: 3;min: 6}) | ["k"] = expr |
                    // expr (array item)
                    if ((Peek().t == TIdent || Peek().t == TKw) && Peek(1).t == TPunct &&
                        (Peek(1).text == L"=" || Peek(1).text == L":"))
                    {
                        bool bColonPair = (Peek(1).text == L":");
                        PNode key = New(Node::Str);
                        key->str = Peek().text;
                        p += 2;
                        n->list.push_back(key);
                        if (bColonPair)
                        {
                            // colon value must not eat the next pair
                            bool bOld = bNoCoalesce;
                            bNoCoalesce = true;
                            n->list.push_back(ParseExpr());
                            bNoCoalesce = bOld;
                        }
                        else
                            n->list.push_back(ParseExpr());
                    }
                    else if (IsPunct(L"["))
                    {
                        p++;
                        PNode key = ParseExpr();
                        Expect(L"]");
                        Expect(L"=");
                        n->list.push_back(key);
                        n->list.push_back(ParseExpr());
                    }
                    else
                    {
                        PNode idx = New(Node::Num);
                        idx->num = (double)n->list.size() / 2 + 1;   // 1-based
                        n->list.push_back(idx);
                        n->list.push_back(ParseExpr());
                    }
                    SkipNL();
                    EatPunct(L";");
                    if (!EatPunct(L","))
                    {
                        // newline separated pairs are fine
                    }
                    SkipNL();
                }
                Expect(L"}");
                if (!structDesc.empty())
                {
                    PNode keyS = New(Node::Str);
                    keyS->str = L"_struct";
                    PNode valS = New(Node::Str);
                    valS->str = structDesc;
                    n->list.push_back(keyS);
                    n->list.push_back(valS);
                }
                return n;
            }
            (*err) = L"语法错误: 意外的 " + (t.t == TEnd ? wstring(L"文件尾")
                     : t.text) + L" @ 行 " + std::to_wstring(t.line);
            if (Peek().t != TEnd)
                p++;   // progress guarantee — never loop forever
            return New(Node::NullK);
        }
        PNode ParsePostfixFrom(PNode e) { return ParsePostfixCont(e); }
        PNode ParsePostfixCont(PNode e)
        {
            while (true)
            {
                if (IsPunct(L".") && (Peek(1).t == TIdent || Peek(1).t == TKw || (Peek(1).t == TPunct && Peek(1).text == L"$")))
                {
                    p++;
                    PNode m = New(Node::Member);
                    m->a = e;
                    // '$name' members span two tokens ('$' + ident)
                    if (Peek().t == TPunct && Peek().text == L"$" &&
                        Peek(1).t == TIdent)
                    {
                        p++;
                        m->str = L"$" + Next().text;
                    }
                    else
                        m->str = Next().text;
                    e = m;
                }
                else if (IsPunct(L"?."))
                {
                    p++;
                    PNode m = New(Node::Member);
                    m->bSafe = true;
                    m->a = e;
                    if (Peek().t == TPunct && Peek().text == L"$" &&
                        Peek(1).t == TIdent)
                    {
                        p++;
                        m->str = L"$" + Next().text;
                    }
                    else
                        m->str = Next().text;
                    e = m;
                }
                else if (IsPunct(L"?["))
                {
                    p++;
                    PNode m = New(Node::Index);
                    m->bSafe = true;
                    m->a = e;
                    bool bOldLhs = bInAssignLhs;
                    bInAssignLhs = false;
                    m->b = ParseExpr();
                    bInAssignLhs = bOldLhs;
                    Expect(L"]");
                    e = m;
                }
                else if (IsPunct(L"[") && Peek(1).t == TPunct &&
                         Peek(1).text == L"[")
                {
                    // aardio t[["k"]] — direct-key index, skips metatables
                    p += 2;
                    PNode m = New(Node::Index);
                    m->a = e;
                    m->bDirect = true;
                    bool bOldLhs = bInAssignLhs;
                    bInAssignLhs = false;
                    m->b = ParseExpr();
                    bInAssignLhs = bOldLhs;
                    Expect(L"]");
                    Expect(L"]");
                    e = m;
                }
                else if (IsPunct(L"@") && Peek(1).t != TIdent)
                {
                    // aardio 'tab@' — metatable get/set (postfix @ not
                    // followed by an ident path; '@path' is the M2 form)
                    p++;
                    PNode m = New(Node::Member);
                    m->bMeta = true;
                    m->a = e;
                    m->str = L"_meta";
                    e = m;
                }
                else if (IsPunct(L"#"))
                {
                    // postfix # — aardio expr# = length (string chars /
                    // table array length)
                    p++;
                    PNode n = New(Node::Un);
                    n->str = L"#";
                    n->a = e;
                    e = ParsePostfixCont(n);
                }
                else if (IsPunct(L"["))
                {
                    p++;
                    PNode m = New(Node::Index);
                    m->a = e;
                    bool bOldLhs = bInAssignLhs;
                    bInAssignLhs = false;
                    m->b = ParseExpr();
                    bInAssignLhs = bOldLhs;
                    Expect(L"]");
                    e = m;
                }
                else if (IsPunct(L"("))
                {
                    p++;
                    PNode c = New(Node::Call);
                    c->a = e;
                    SkipNL();
                    bool bOldLhs = bInAssignLhs;
                    bInAssignLhs = false;
                    if (!IsPunct(L")"))
                    {
                        // aardio f(,,,,1): leading empty slots
                        if (IsPunct(L",") )
                            c->list.push_back(New(Node::NullK));
                        else if (IsPunct(L")"))
                            c->list.push_back(New(Node::NullK));  // empty slot before )
                        else if (Peek().t == TIdent && Peek(1).t == TPunct && Peek(1).text == L"=")
                        {
                            // named args: f(name = expr; ...) — one table arg
                            PNode tbl = New(Node::TableCtor);
                            while (Peek().t == TIdent && Peek(1).t == TPunct &&
                                   Peek(1).text == L"=")
                            {
                                PNode key = New(Node::Str);
                                key->str = Next().text;
                                p++;
                                tbl->list.push_back(key);
                                tbl->list.push_back(ParseExpr());
                                SkipNL();
                                EatPunct(L";");
                                SkipNL();
                                if (IsPunct(L",")) { p++; SkipNL(); continue; }
                            }
                            c->list.push_back(tbl);
                        }
                        else
                            c->list.push_back(ParseExpr());
                        SkipNL();
                        while (EatPunct(L","))
                            {
                                SkipNL();
                                if (IsPunct(L",") || IsPunct(L")"))
                                    c->list.push_back(New(Node::NullK));
                                else if (IsPunct(L")"))
                                    c->list.push_back(New(Node::NullK));  // empty slot before )
                                else
                                    c->list.push_back(ParseExpr());
                                SkipNL();
                            }
                    }
                    bInAssignLhs = bOldLhs;
                    Expect(L")");
                    e = c;
                }
                else break;
            }
            return e;
        }
    };

    // ---------------- interpreter ----------------
    // ---- round 38: win.form — minimal windowing --------------------------
    //
    // A real Win32 window with button/static/edit children. Callbacks run
    // VM code re-entrantly inside doModal's message loop (Interp state is
    // saved/restored around each dispatch). Forms register in a global
    // HWND map; the WndProc dispatches WM_COMMAND to control tables.

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
    static std::map<HWND, FormState*> g_forms;
    static LRESULT CALLBACK FormWndProc(HWND hwnd, UINT msg, WPARAM wp,
                                        LPARAM lp);
    static bool RegisterFormClass();


    struct Interp
    {
        wstring* err;
        Env globals;
        int nBreak = 0, nContinue = 0, nReturn = 0;
        Value ret;
        // M2: static self for meta callbacks fired from static helpers
        static Interp* g_self;
        Interp* self() { return this; }
        // round 19: aardio root namespaces + module loading
        // (GetRoot/BindModule/DoImport/InitBuiltins defined at the tail)
        int nErrLine = 0;   // last statement line entered (error reporting)
        // round 31: console mini-debugger
        bool bDebug = false;            // hooks armed
        bool bStep = false;             // stop at every statement
        std::set<int> breakpoints;      // armed lines
        std::unordered_map<wstring, Value> roots;    // string/math/io/...
        std::unordered_map<wstring, Value> modules;  // import cache by path
        wstring baseDir;                             // import file resolution
        std::unordered_map<wstring, wstring> embedded;   // payload v2 files

        Value Eval(const PNode& n, Env* env)
        {
            switch (n->k)
            {
            case Node::Num: return Value::Num_(n->num);
            case Node::Str: return Value::Str_(n->str);
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
                    if (v.t == Value::Str) return Value::Num_((double)v.s.size());
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

        double ToNum(const Value& v)
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
        static bool MetaGet(const Value& obj, const wstring& key,
                            Value* out, bool* pErr, wstring* pErrMsg,
                            bool bOwnerCall = false)
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

        static bool MetaSet(const Value& obj, const wstring& key,
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
        Value CallOwner(const Value& f, const Value& obj,
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
                    local.vars[L"..."] = Value::Table_(tbV);
                }
                else
                {
                    local.vars[L"..."] = Value();
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
        static Value CallVmFuncStatic(const Value& obj, const Value& f,
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
        Value GetMeta(const Value& obj)
        {
            if (obj.t != Value::TableT || !obj.tab) return Value();
            auto im = obj.tab->map.find(L"_meta");
            if (im != obj.tab->map.end())
                return im->second;
            return Value();
        }

        // M2: string conversion honoring a metatable _tostring
        wstring ToStringM(const Value& v)
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

        Value CreateDirectApiWrapper(HMODULE hMod, const wstring& funcName,
                                     bool bDllCdecl = false, bool bDllUnicode = false)
        {
            if (!hMod || funcName.empty())
                return Value();

            // 1. Convert funcName to ANSI string
            std::string strNameA((size_t)WideCharToMultiByte(CP_ACP, 0,
                funcName.c_str(), (int)funcName.size(), nullptr, 0, nullptr, nullptr), 0);
            if (!strNameA.empty())
                WideCharToMultiByte(CP_ACP, 0, funcName.c_str(), (int)funcName.size(),
                                   &strNameA[0], (int)strNameA.size(), nullptr, nullptr);

            // 2. Check suffix
            char suffix = 0;
            std::string bareName = strNameA;
            if (strNameA.size() > 1)
            {
                char last = strNameA.back();
                if (last == 'W' || last == 'A' || last == 'L' || last == 'P' ||
                    last == 'D' || last == 'F' || last == 'B')
                {
                    suffix = last;
                    bareName = strNameA.substr(0, strNameA.size() - 1);
                }
            }

            FARPROC fp = nullptr;
            bool bUnicode = bDllUnicode;
            ApiRet retType = ARInt;

            if (suffix == 'W') { bUnicode = true; }
            else if (suffix == 'A') { bUnicode = false; }
            else if (suffix == 'L') { retType = ARI64; }
            else if (suffix == 'P') { retType = ARPtr; }
            else if (suffix == 'D' || suffix == 'F') { retType = ARDbl; }
            else if (suffix == 'B') { retType = ARBool; }

            // Step 1: try exact name
            fp = ::GetProcAddress(hMod, strNameA.c_str());

            // Step 2: if not found and suffix exists, try bareName
            if (!fp && suffix)
            {
                fp = ::GetProcAddress(hMod, bareName.c_str());
            }

            // Step 3: if not found, try appending 'W'
            if (!fp)
            {
                std::string tryW = (suffix ? bareName : strNameA) + "W";
                fp = ::GetProcAddress(hMod, tryW.c_str());
                if (fp) bUnicode = true;
            }

            // Step 4: if not found, try appending 'A'
            if (!fp)
            {
                std::string tryA = (suffix ? bareName : strNameA) + "A";
                fp = ::GetProcAddress(hMod, tryA.c_str());
                if (fp) bUnicode = false;
            }

            if (!fp)
                return Value(); // not found

            void* fn = (void*)fp;
            bool bCdecl = bDllCdecl;

            Value fWrap = NF([this, fn, bUnicode, bCdecl, retType](vector<Value>& a) -> Value {
                vector<SlotT> slots;
                std::vector<std::string> tmpA;
                std::vector<wstring> tmpW;
                struct DirectRef {
                    size_t idx;
                    void* pBuf;
                    Value origVal;
                };
                std::vector<DirectRef> outStructs;

                for (size_t i = 0; i < a.size(); i++)
                {
                    Value v = a[i];
                    if (v.t == Value::Null)
                    {
                        slots.push_back(0);
                    }
                    else if (v.t == Value::Bool)
                    {
                        slots.push_back(v.b ? 1 : 0);
                    }
                    else if (v.t == Value::Num)
                    {
                        slots.push_back((SlotT)(INT_PTR)(int)(unsigned int)(long long)v.n);
                    }
                    else if (v.t == Value::Pointer)
                    {
                        slots.push_back((SlotT)(INT_PTR)v.ptr);
                    }
                    else if (v.t == Value::Str)
                    {
                        if (bUnicode)
                        {
                            tmpW.push_back(v.s);
                            slots.push_back((SlotT)(INT_PTR)tmpW.back().c_str());
                        }
                        else
                        {
                            std::string sA((size_t)WideCharToMultiByte(CP_ACP, 0, v.s.c_str(),
                                (int)v.s.size(), nullptr, 0, nullptr, nullptr), 0);
                            if (!sA.empty())
                                WideCharToMultiByte(CP_ACP, 0, v.s.c_str(), (int)v.s.size(),
                                                   &sA[0], (int)sA.size(), nullptr, nullptr);
                            tmpA.push_back(std::move(sA));
                            slots.push_back((SlotT)(INT_PTR)tmpA.back().c_str());
                        }
                    }
                    else if (v.t == Value::TableT || v.t == Value::Class)
                    {
                        int total = 0;
                        void* pBuf = StructSerialize(v, &total);
                        if (pBuf)
                        {
                            slots.push_back((SlotT)(INT_PTR)pBuf);
                            outStructs.push_back({ i, pBuf, v });
                        }
                        else
                        {
                            slots.push_back((SlotT)(INT_PTR)(long long)BNum(v));
                        }
                    }
                    else
                    {
                        slots.push_back((SlotT)(INT_PTR)(long long)BNum(v));
                    }
                }

                if (slots.size() > 14)
                {
                    if (err->empty()) (*err) = L"运行错误: API 参数超过 14 个";
                    return Value();
                }

                Value vRet;
                if (retType == ARDbl)
                {
                    double d = bCdecl ? InvokeCdl<double>(fn, slots)
                                      : InvokeStd<double>(fn, slots);
                    vRet = Value::Num_(d);
                }
                else
                {
                    __int64 r = bCdecl ? InvokeCdl<__int64>(fn, slots)
                                       : InvokeStd<__int64>(fn, slots);
                    if (retType == ARBool)
                        vRet = Value::Bool_(r != 0);
                    else if (retType == ARPtr)
                        vRet = Value::Num_((double)(INT_PTR)r);
                    else if (retType == ARI64)
                        vRet = Value::Num_((double)r);
                    else
                        vRet = Value::Num_((double)(int)r);
                }

                if (!outStructs.empty())
                {
                    auto extras = std::make_shared<vector<Value>>();
                    for (auto& ref : outStructs)
                    {
                        StructDeserialize(ref.origVal, ref.pBuf);
                        extras->push_back(ref.origVal);
                        HeapFree(GetProcessHeap(), 0, ref.pBuf);
                    }
                    vRet.mrets = extras;
                }

                return vRet;
            });
            fWrap.fname = funcName;
            fWrap.ptr = fn;
            return fWrap;
        }

        Value GetMember(const Value& obj, const wstring& key,
                        bool bDirect = false, bool bOwnerCall = false)
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

        Value EvalCall(const PNode& n, Env* env)
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
                            local.vars[L"..."] = Value::Table_(tbV);
                        }
                        else
                        {
                            local.vars[L"..."] = Value();
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
                (*err) = L"运行错误: 尝试调用非函数值";
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
            // "..." varargs: leftover arguments as a 1-based table
            if (f.params.size() && f.params.back() == L"..." &&
                f.fn)
            {
                if (args.size() > f.params.size() - 1)
                {
                    auto tbV = std::make_shared<Table>();
                    for (size_t k = f.params.size() - 1; k < args.size(); k++)
                        tbV->arr.push_back(args[k]);
                    local.vars[L"..."] = Value::Table_(tbV);
                }
                else
                {
                    local.vars[L"..."] = Value();
                }
            }
            Value old = ret;
            int oldR = nReturn;
            nReturn = 0;
            Value r = ExecBlock(f.fn, &local);
            nReturn = oldR;
            ret = old;
            return r;
        }

        Value EvalBin(const PNode& n, Env* env)
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
            if (op == L":")   // aardio null coalesce
            {
                Value l = Eval(n->a, env);
                return l.t == Value::Null ? Eval(n->b, env) : l;
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
            if (op == L"+" && l.t == Value::Str && r.t == Value::Str)
            {
                // aardio: arithmetic on two non-numeric strings is an error,
                // string concat is '++' (or the compat '..' above)
                wchar_t* e1 = nullptr, *e2 = nullptr;
                wcstod(l.s.c_str(), &e1);
                wcstod(r.s.c_str(), &e2);
                if ((e1 && *e1) || (e2 && *e2))
                {
                    (*err) = L"运行错误: 字符串连接请使用 ++ 运算符";
                    return Value();
                }
            }
            double a = ToNum(l), b = ToNum(r);
            if (op == L"+") return Value::Num_(a + b);
            if (op == L"-") return Value::Num_(a - b);
            if (op == L"*") return Value::Num_(a * b);
            if (op == L"/") return Value::Num_(b != 0 ? a / b : 0);
            if (op == L"%" || op == L"%=") return Value::Num_(b != 0 ? fmod(a, b) : 0);
            if (op == L"^" || op == L"**") return Value::Num_(pow(a, b));
            // M2: bitwise (32-bit width, aardio DWORD semantics)
            unsigned int ua = (unsigned int)(INT_PTR)(long long)a;
            unsigned int ub = (unsigned int)(INT_PTR)(long long)b;
            if (op == L"&") return Value::Num_((double)(int)(ua & ub));
            if (op == L"|") return Value::Num_((double)(int)(ua | ub));
            if (op == L"^x") return Value::Num_((double)(int)(ua ^ ub));
            if (op == L"<<") return Value::Num_((double)(int)(ua << (ub & 31)));
            if (op == L">>") return Value::Num_((double)(int)(ua >> (ub & 31)));
            if (op == L">>>")
                return Value::Num_((double)((long long)(ua >> (ub & 31))));
            if (op == L"<") return Value::Bool_(a < b && l.t == r.t ? true : CmpLess(l, r));
            if (op == L">") return Value::Bool_(CmpLess(r, l));
            if (op == L"<=") return Value::Bool_(!CmpLess(r, l));
            if (op == L">=") return Value::Bool_(!CmpLess(l, r));
            return Value();
        }

        static bool CmpLess(const Value& l, const Value& r)
        {
            if (l.t == Value::Str && r.t == Value::Str)
                return l.s < r.s;
            if (l.t == Value::Num && r.t == Value::Num)
                return l.n < r.n;
            return false;
        }

        Value ApplyOp(const wstring& op, const Value& l, const Value& r)
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
            if (op == L"^" || op == L"**") return Value::Num_(pow(a, b));
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
        void DoAssignSimple(const wstring& name, const Value& v)
        {
            globals.vars[name] = v;
        }

        void DoAssign(const PNode& target, const Value& v, Env* env,
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

        static PNode ExprToNode(const Value& v)
        {
            PNode n = std::make_shared<Node>();
            if (v.t == Value::Num) { n->k = Node::Num; n->num = v.n; }
            else if (v.t == Value::Str) { n->k = Node::Str; n->str = v.s; }
            else if (v.t == Value::Bool) { n->k = Node::Bool; n->num = v.b; }
            else n->k = Node::NullK;
            return n;
        }

        void DumpVars(Env* env)
        {
            int n = 0;
            static const wchar_t* kScalarNatives[] = {
                L"tostring", L"tonumber", L"sleep", L"call", L"topointer",
                L"splice", L"tocdecl", L"pop", L"tostdcall", L"isArray",
                L"isArrayLike", L"push", L"_HINSTANCE", L"_HINSTANSE",
                L"__waitForMultipleObjects", L"__messagePeek",
                L"_INVALID_HANDLE_VALUE", L"_HAPPICON", L"_WIN_64",
                L"_WINXP", L"_WIN7_LATER", L"_WIN10_LATER"
            };
            for (auto& kv : env->vars)
            {
                // built-in namespaces/functions bury the script's own vars; User32, Kernel32 and type kept for debugger
                if ((roots.count(kv.first) && kv.first != L"User32" && kv.first != L"Kernel32" && kv.first != L"type") ||
                    modules.count(kv.first))
                    continue;
                bool bNativ = false;
                for (auto* p : kScalarNatives)
                    if (kv.first == p) { bNativ = true; break; }
                if (bNativ) continue;
                if (n++ >= 20) { WriteU8(stdout, L"  ...\n"); break; }
                wstring val = kv.second.ToString();
                if (val.size() > 60) val = val.substr(0, 60) + L"...";
                WriteU8(stdout, L"  " + kv.first + L" = " + val + L"\n");
            }
            if (n == 0)
                WriteU8(stdout, L"  (本层无变量)\n");
            fflush(stdout);
        }

        // console mini-debugger: prompts on the script's console; commands —
        // enter=continue, s=step, c=continue w/o stepping, v=vars, q=abort
        void DebugStop(int nLine, Env* env)
        {
            while (true)
            {
                WriteU8(stdout, L"\n[断点] 行 " +
                                std::to_wstring(nLine) + L"\n");
                DumpVars(env);
                WriteU8(stdout, L"(回车=继续  s=单步  "
                                L"c=连续执行  q=退出) > ");
                fflush(stdout);
                wchar_t buf[64] = {0};
                if (!fgetws(buf, 64, stdin))
                {
                    (*err) = L"调试中断";
                    return;
                }
                wchar_t c = buf[0];
                if (c == L'\n' || c == 0) { return; }
                if (c == L's') { bStep = true; return; }
                if (c == L'c') { bStep = false; return; }
                if (c == L'v') continue;
                if (c == L'q') { (*err) = L"调试终止"; return; }
            }
        }

        Value ExecBlock(const PNode& blk, Env* env)
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

        Value EvalClass(const PNode& n, Env* env)
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

        Value Exec(const PNode& n, Env* env)
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
                bool bLimitForm = n->names.size() > 0 &&
                                  n->names[0] == L"limit";
                double dStep = 1;
                if (n->c && n->c->k == Node::ExprStmt && n->c->a &&
                    n->c->a->k == Node::Num)
                    dStep = n->c->a->num;
                int nDir = 1;
                if (n->a && n->a->k == Node::Assign && n->a->a &&
                    n->a->a->k == Node::Ident)
                {
                    Value* pv = env->Find(n->a->a->str);
                    Value lim = Eval(n->b, env);
                    if (pv && pv->t == Value::Num && lim.t == Value::Num)
                    {
                        nDir = BNum(*pv) > BNum(lim) ? -1 : 1;
                    }
                }
                if (n->c && n->c->k == Node::ExprStmt && n->c->a &&
                    n->c->a->k == Node::Un && n->c->a->str == L"-")
                {
                    dStep = -BNum(Eval(n->c->a->a, env));
                    nDir = -1;
                }
                if (!n->c)
                    dStep = (double)nDir;   // 2-expr form: derived step
                auto bContinue = [&]() -> bool {
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
                    bool bNumStep = n->c && n->c->k == Node::ExprStmt &&
                                    n->c->a && n->c->a->k == Node::Num;
                    if (n->c && !bNumStep)
                        Exec(n->c, env);
                    else if (n->a && n->a->k == Node::Assign &&
                             n->a->a && n->a->a->k == Node::Ident)
                    {
                        Value* pv = env->Find(n->a->a->str);
                        if (pv && pv->t == Value::Num)
                        {
                            *pv = Value::Num_(pv->n + dStep);
                        }
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
                    {
                        (*err) = msg;   // no handler: re-raise
                        return Value();
                    }
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

        Value GetRoot(const wstring& name)
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

        Value NsPath(const vector<wstring>& names, Env* env = nullptr, bool bRoot = false)
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

        Value BindModule(const vector<wstring>& names, const Value& mod)
        {
            static bool bDbg = GetEnvironmentVariableW(L"CMT", nullptr, 0) != 0;
            if (bDbg)
            {
                wstring full;
                for (size_t i = 0; i < names.size(); ++i) { if (i) full += L"."; full += names[i]; }
                fwprintf(stderr, L"[DEBUG BIND] names=%s mod.t=%d\n", full.c_str(), (int)mod.t);
                fflush(stderr);
            }
            // single-segment import: the module IS the root namespace
            if (names.size() == 1)
            {
                auto itOld = roots.find(names[0]);
                if (itOld != roots.end() && itOld->second.t == Value::TableT && itOld->second.tab &&
                    mod.t == Value::TableT && mod.tab && itOld->second.tab != mod.tab)
                {
                    for (auto& kv : itOld->second.tab->map)
                    {
                        if (!mod.tab->map.count(kv.first))
                            mod.tab->map[kv.first] = kv.second;
                    }
                }
                roots[names[0]] = mod;
                globals.vars[names[0]] = mod;
                return mod;
            }
            // bind a dotted import path into the root namespaces
            Value ns = GetRoot(names[0]);
            for (size_t i = 1; i < names.size(); i++)
            {
                if (ns.t != Value::TableT || !ns.tab)
                    break;
                if (i + 1 == names.size())
                {
                    auto itTarget = ns.tab->map.find(names[i]);
                    if (itTarget != ns.tab->map.end() && itTarget->second.t == Value::TableT && itTarget->second.tab &&
                        mod.t == Value::TableT && mod.tab && itTarget->second.tab != mod.tab)
                    {
                        for (auto& kv : itTarget->second.tab->map)
                        {
                            if (!mod.tab->map.count(kv.first))
                                mod.tab->map[kv.first] = kv.second;
                        }
                    }
                    ns.tab->map[names[i]] = mod;
                    break;
                }
                auto it = ns.tab->map.find(names[i]);
                if (it != ns.tab->map.end() && (it->second.t == Value::TableT || it->second.t == Value::Class))
                    ns = it->second;
                else
                {
                    auto nt = std::make_shared<Table>();
                    Value v = Value::Table_(nt);
                    ns.tab->map[names[i]] = v;
                    ns = v;
                }
            }
            return mod;
        }

        Value DoImport(const PNode& n, Env* env)
        {
            (void)env;
            // M7: global lib root — the ORIGINAL aardio install's lib\
            // (plaintext source), enabled via NODIO_LIBDIR; resolution
            // order: project baseDir first, then the lib root
            static const wstring strLibDir = []
            {
                wchar_t szLib[MAX_PATH] = L"";
                GetEnvironmentVariableW(L"NODIO_LIBDIR", szLib, MAX_PATH);
                return wstring(szLib);
            }();
            // import resolution accepts both .aardio (official ecosystem)
            // and .nodio (native) extensions; each root prefers the
            // folder-module layout (rel\_.ext) over the flat file
            // (rel.ext), and the aardio extension over nodio
            auto LibCand = [&](const wstring& relX, bool bAlt,
                               const wchar_t* ext) -> wstring
            {
                if (strLibDir.empty()) return L"";
                return strLibDir + L"\\" + relX +
                    (bAlt ? (wstring(L"\\_.") + ext)
                          : (wstring(L".") + ext));
            };
            auto Exists = [](const wstring& p) -> bool
            {
                return !p.empty() && _waccess(p.c_str(), 0) == 0;
            };
            // 1) pre-bound builtin namespaces resolve immediately —
            // but a matching lib FILE merges into the namespace first
            // (aardio: import X loads lib source into namespace X)
            {
                wstring relT;
                for (size_t i = 0; i < n->names.size(); i++)
                {
                    if (i) relT += L"\\";
                    relT += n->names[i];
                }
                bool bAnyFile =
                    Exists(baseDir.empty() ? relT + L".aardio"
                                           : baseDir + L"\\" + relT + L".aardio") ||
                    Exists(baseDir.empty() ? relT + L".nodio"
                                           : baseDir + L"\\" + relT + L".nodio") ||
                    Exists(baseDir.empty() ? relT + L"\\_.aardio"
                                           : baseDir + L"\\" + relT + L"\\_.aardio") ||
                    Exists(baseDir.empty() ? relT + L"\\_.nodio"
                                           : baseDir + L"\\" + relT + L"\\_.nodio") ||
                    Exists(LibCand(relT, false, L"aardio")) ||
                    Exists(LibCand(relT, false, L"nodio")) ||
                    Exists(LibCand(relT, true, L"aardio")) ||
                    Exists(LibCand(relT, true, L"nodio"));
                if (roots.count(n->names[0]) &&
                    !embedded.count(relT + L".aardio") &&
                    !embedded.count(relT + L"\\_.aardio") &&
                    !embedded.count(relT + L".nodio") &&
                    !embedded.count(relT + L"\\_.nodio") &&
                    !bAnyFile)
                    return roots[n->names[0]];
            }
            // 2) file module: baseDir\a\b\c.aardio for `import a.b.c`
            wstring rel;
            for (size_t i = 0; i < n->names.size(); i++)
            {
                if (i) rel += L"\\";
                rel += n->names[i];
            }
            // candidate priority: baseDir first, then the lib root;
            // within a root: rel\_.aardio > rel.aardio >
            //                rel\_.nodio > rel.nodio
            wstring file;
            {
                wstring c0 = baseDir.empty() ? wstring()
                    : baseDir + L"\\" + rel + L"\\_.aardio";
                wstring c1 = baseDir.empty() ? wstring()
                    : baseDir + L"\\" + rel + L".aardio";
                wstring c2 = baseDir.empty() ? wstring()
                    : baseDir + L"\\" + rel + L"\\_.nodio";
                wstring c3 = baseDir.empty() ? wstring()
                    : baseDir + L"\\" + rel + L".nodio";
                if (Exists(c0)) file = c0;
                else if (Exists(c1)) file = c1;
                else if (Exists(c2)) file = c2;
                else if (Exists(c3)) file = c3;
                else if (Exists(LibCand(rel, true, L"aardio")))
                    file = LibCand(rel, true, L"aardio");
                else if (Exists(LibCand(rel, false, L"aardio")))
                    file = LibCand(rel, false, L"aardio");
                else if (Exists(LibCand(rel, true, L"nodio")))
                    file = LibCand(rel, true, L"nodio");
                else if (Exists(LibCand(rel, false, L"nodio")))
                    file = LibCand(rel, false, L"nodio");
                else
                    file = baseDir.empty() ? rel + L".aardio"
                                           : baseDir + L"\\" + rel + L".aardio";
            }
            static bool bDbg = GetEnvironmentVariableW(L"CMT", nullptr, 0) != 0;
            if (bDbg)
            {
                fwprintf(stderr, L"[DEBUG DOIMPORT] file=%s in_modules=%d\n", file.c_str(), (int)(modules.find(file) != modules.end()));
                fflush(stderr);
            }
            auto it = modules.find(file);
            if (it != modules.end())
            {
                BindModule(n->names, it->second);
                return it->second;
            }
            // payload v2: embedded project files resolve before disk
            // (published exe: libEmbed closure ships under lib-relative
            // keys — "win\_.aardio" — so try both lib layouts, both
            // extensions)
            auto itE = embedded.find(rel + L".aardio");
            if (itE == embedded.end())
                itE = embedded.find(rel + L"\\_.aardio");
            if (itE == embedded.end())
                itE = embedded.find(rel + L".nodio");
            if (itE == embedded.end())
                itE = embedded.find(rel + L"\\_.nodio");
            if (itE != embedded.end())
            {
                wstring src = itE->second;
                wstring modErr;
                vector<Tok> toks = Lex(src, &modErr);
                Parser ps{ toks, 0, &modErr };
                PNode prog = ps.ParseBlock();
                if (!modErr.empty())
                {
                    (*err) = L"库 " + file + L" 编译失败: " + modErr;
                    return Value();
                }
                shared_ptr<Table> placeholder2;
                if (n->names.size() == 1)
                {
                    auto itR = roots.find(n->names[0]);
                    if (itR != roots.end() && itR->second.t == Value::TableT && itR->second.tab)
                        placeholder2 = itR->second.tab;
                }
                else if (n->names.size() > 1)
                {
                    auto itR = roots.find(n->names[0]);
                    if (itR != roots.end() && (itR->second.t == Value::TableT || itR->second.t == Value::Class) && itR->second.tab)
                    {
                        Value cur = itR->second;
                        for (size_t i = 1; i < n->names.size(); ++i)
                        {
                            if (cur.t != Value::TableT && cur.t != Value::Class) break;
                            if (!cur.tab) break;
                            auto itSub = cur.tab->map.find(n->names[i]);
                            if (itSub != cur.tab->map.end() && (itSub->second.t == Value::TableT || itSub->second.t == Value::Class) && itSub->second.tab)
                            {
                                if (i + 1 == n->names.size() && itSub->second.t == Value::TableT)
                                    placeholder2 = itSub->second.tab;
                                else
                                    cur = itSub->second;
                            }
                            else break;
                        }
                    }
                }
                if (!placeholder2)
                    placeholder2 = std::make_shared<Table>();
                Value modPlaceholder2 = Value::Table_(placeholder2);
                placeholder2->map[L"self"] = modPlaceholder2;
                modules[file] = modPlaceholder2;
                BindModule(n->names, modPlaceholder2);
                Env modEnv2;
                modEnv2.parent = &globals;
                modEnv2.modTable = placeholder2;
                wstring oldModFile2 = g_curModuleFile;
                g_curModuleFile = file;
                Value oldRet = ret;
                int oldR = nReturn, oldB = nBreak, oldC = nContinue;
                nReturn = nBreak = nContinue = 0;
                ret = Value();
                ExecBlock(prog, &modEnv2);
                for (auto& kv : modEnv2.vars)
                    placeholder2->scopeVars[kv.first] = kv.second;
                g_curModuleFile = oldModFile2;
                Value mod2 = ret;
                nReturn = oldR; nBreak = oldB; nContinue = oldC;
                ret = oldRet;
                if (!err->empty())
                    return Value();
                if (mod2.t != Value::TableT)
                {
                    Value curNs;
                    auto itFirst = globals.vars.find(n->names[0]);
                    if (itFirst != globals.vars.end())
                        curNs = itFirst->second;
                    for (size_t ni = 1; ni < n->names.size(); ++ni)
                    {
                        if (curNs.t == Value::TableT && curNs.tab)
                        {
                            auto itSub = curNs.tab->map.find(n->names[ni]);
                            if (itSub != curNs.tab->map.end())
                                curNs = itSub->second;
                            else
                            {
                                curNs = Value();
                                break;
                            }
                        }
                        else
                        {
                            curNs = Value();
                            break;
                        }
                    }
                    if ((curNs.t == Value::TableT && curNs.tab) || curNs.t == Value::Class || curNs.t == Value::Func)
                        mod2 = curNs;
                    else
                    {
                        for (auto& kv : modEnv2.vars)
                            placeholder2->map[kv.first] = kv.second;
                        mod2 = modPlaceholder2;
                    }
                }
                // lib convention: file a/b/C.aardio with 'namespace a.b;'
                // + 'class C { }' binds the CLASS, not the namespace
                {
                    size_t sl = file.find_last_of(L"\\/");
                    wstring base = (sl == wstring::npos) ? file
                        : file.substr(sl + 1);
                    if (base.size() > 7 &&
                        base.substr(base.size() - 7) == L".aardio")
                        base = base.substr(0, base.size() - 7);
                    if (base.size() > 6 &&
                        base.substr(base.size() - 6) == L".nodio")
                        base = base.substr(0, base.size() - 6);
                    if (mod2.t == Value::TableT && mod2.tab)
                    {
                        auto im = mod2.tab->map.find(base);
                        if (im != mod2.tab->map.end() &&
                            (im->second.t == Value::Class ||
                             im->second.t == Value::Func))
                            mod2 = im->second;
                    }
                }
                if (mod2.t == Value::TableT && mod2.tab && mod2.tab != placeholder2)
                {
                    for (auto& kv : mod2.tab->map)
                        placeholder2->map[kv.first] = kv.second;
                }
                modules[file] = mod2;
                return BindModule(n->names, mod2);
            }
            FILE* pf = nullptr;
            if (_wfopen_s(&pf, file.c_str(), L"rb") != 0 || !pf)
            {
                // pure-namespace import: no <name>.aardio / _\_.aardio
                // but a submodule folder exists (com/, util/...) in baseDir or strLibDir
                wstring dirCand = file.substr(0, file.size() - 7);
                DWORD attr = GetFileAttributesW(dirCand.c_str());
                if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY))
                {
                    if (!strLibDir.empty())
                    {
                        wstring dirLib = strLibDir + L"\\" + rel;
                        attr = GetFileAttributesW(dirLib.c_str());
                    }
                }
                if (attr != INVALID_FILE_ATTRIBUTES &&
                    (attr & FILE_ATTRIBUTE_DIRECTORY))
                {
                    auto tbN = std::make_shared<Table>();
                    Value modN = Value::Table_(tbN);
                    modules[file] = modN;
                    return BindModule(n->names, modN);
                }
                (*err) = L"找不到库: " + file;
                return Value();
            }
            std::string raw;
            {
                char buf[4096];
                size_t rd;
                while ((rd = fread(buf, 1, sizeof(buf), pf)) > 0)
                    raw.append(buf, rd);
                fclose(pf);
            }
            { static bool bDbg = GetEnvironmentVariableW(L"CMT", nullptr, 0) != 0;
              if (bDbg) fwprintf(stderr, L"[LOAD] %s\n", file.c_str()); }
            int off = (raw.size() >= 3 && (BYTE)raw[0] == 0xEF &&
                       (BYTE)raw[1] == 0xBB && (BYTE)raw[2] == 0xBF) ? 3 : 0;
            int w = MultiByteToWideChar(CP_UTF8, 0, raw.data() + off,
                                        (int)(raw.size() - off), nullptr, 0);
            wstring src((size_t)w, 0);
            if (w > 0)
                MultiByteToWideChar(CP_UTF8, 0, raw.data() + off,
                                    (int)(raw.size() - off), &src[0], w);
            wstring modErr;
            vector<Tok> toks = Lex(src, &modErr);
            Parser ps{ toks, 0, &modErr };
            PNode prog = ps.ParseBlock();
            if (!modErr.empty())
            {
                (*err) = L"库 " + file + L" 编译失败: " + modErr;
                return Value();
            }
            // Circular import guard: seed a placeholder table before execution
            shared_ptr<Table> placeholder;
            if (n->names.size() == 1)
            {
                auto itR = roots.find(n->names[0]);
                if (itR != roots.end() && itR->second.t == Value::TableT && itR->second.tab)
                    placeholder = itR->second.tab;
            }
            else if (n->names.size() > 1)
            {
                auto itR = roots.find(n->names[0]);
                if (itR != roots.end() && (itR->second.t == Value::TableT || itR->second.t == Value::Class) && itR->second.tab)
                {
                    Value cur = itR->second;
                    for (size_t i = 1; i < n->names.size(); ++i)
                    {
                        if (cur.t != Value::TableT && cur.t != Value::Class) break;
                        if (!cur.tab) break;
                        auto itSub = cur.tab->map.find(n->names[i]);
                        if (itSub != cur.tab->map.end() && (itSub->second.t == Value::TableT || itSub->second.t == Value::Class) && itSub->second.tab)
                        {
                            if (i + 1 == n->names.size() && itSub->second.t == Value::TableT)
                                placeholder = itSub->second.tab;
                            else
                                cur = itSub->second;
                        }
                        else break;
                    }
                }
            }
            if (!placeholder)
                placeholder = std::make_shared<Table>();
            Value modPlaceholder = Value::Table_(placeholder);
            placeholder->map[L"self"] = modPlaceholder;
            modules[file] = modPlaceholder;
            BindModule(n->names, modPlaceholder);

            // module runs in a child scope; its `return v` (or its collected
            // top-level globals) becomes the module value
            Env modEnv;
            modEnv.parent = &globals;
            modEnv.modTable = placeholder;
            wstring oldModFile = g_curModuleFile;
            g_curModuleFile = file;
            Value oldRet = ret;
            int oldR = nReturn, oldB = nBreak, oldC = nContinue;
            nReturn = nBreak = nContinue = 0;
            ret = Value();
            ExecBlock(prog, &modEnv);
            for (auto& kv : modEnv.vars)
                placeholder->scopeVars[kv.first] = kv.second;
            g_curModuleFile = oldModFile;
            Value mod = ret;
            nReturn = oldR; nBreak = oldB; nContinue = oldC;
            ret = oldRet;
            if (!err->empty())
                return Value();
            if (mod.t != Value::TableT)
            {
                Value curNs;
                auto itFirst = globals.vars.find(n->names[0]);
                if (itFirst != globals.vars.end())
                    curNs = itFirst->second;
                for (size_t ni = 1; ni < n->names.size(); ++ni)
                {
                    if (curNs.t == Value::TableT && curNs.tab)
                    {
                        auto itSub = curNs.tab->map.find(n->names[ni]);
                        if (itSub != curNs.tab->map.end())
                            curNs = itSub->second;
                        else
                        {
                            curNs = Value();
                            break;
                        }
                    }
                    else
                    {
                        curNs = Value();
                        break;
                    }
                }
                if ((curNs.t == Value::TableT && curNs.tab) || curNs.t == Value::Class || curNs.t == Value::Func)
                    mod = curNs;
                else
                {
                    for (auto& kv : modEnv.vars)
                        placeholder->map[kv.first] = kv.second;
                    mod = modPlaceholder;
                }
            }
            // lib convention: bind a same-named Class/Func member
            {
                size_t sl = file.find_last_of(L"\\/");
                wstring base = (sl == wstring::npos) ? file
                    : file.substr(sl + 1);
                if (base.size() > 7 &&
                    base.substr(base.size() - 7) == L".aardio")
                    base = base.substr(0, base.size() - 7);
                if (base.size() > 6 &&
                    base.substr(base.size() - 6) == L".nodio")
                    base = base.substr(0, base.size() - 6);
                // folder-module convention: a/b/C/_.aardio binds C
                // (the class declared inside), not '_'
                if (base == L"_")
                {
                    size_t sl2 = file.find_last_of(L"\\/");
                    if (sl2 != wstring::npos && sl2 > 0)
                    {
                        size_t sl3 = file.find_last_of(L"\\/", sl2 - 1);
                        if (sl3 != wstring::npos)
                            base = file.substr(sl3 + 1, sl2 - sl3 - 1);
                    }
                }
                Value cand = mod;
                if (cand.t == Value::TableT && cand.tab)
                {
                    auto im = cand.tab->map.find(base);
                    if (im != cand.tab->map.end() &&
                        (im->second.t == Value::Class ||
                         im->second.t == Value::Func))
                        mod = im->second;
                    else
                    {
                        // 'namespace X;' files declare into the global
                        // namespace chain: probe globals.X1.X2...base
                        Value ns = Value();
                        for (size_t ni = 0; ni + 1 < n->names.size(); ni++)
                        {
                            const wstring& nm = n->names[ni];
                            if (ns.t != Value::TableT || !ns.tab)
                            {
                                auto ig = globals.vars.find(nm);
                                if (ig != globals.vars.end())
                                    ns = ig->second;
                                else break;
                            }
                            else
                            {
                                auto it2 = ns.tab->map.find(nm);
                                if (it2 != ns.tab->map.end())
                                    ns = it2->second;
                                else break;
                            }
                        }
                        if (ns.t == Value::TableT && ns.tab)
                        {
                            auto im2 = ns.tab->map.find(base);
                            if (im2 != ns.tab->map.end() &&
                                (im2->second.t == Value::Class ||
                                 im2->second.t == Value::Func))
                                mod = im2->second;
                        }
                    }
                }
            }
            {
                static bool bDbg = GetEnvironmentVariableW(L"CMT", nullptr, 0) != 0;
                if (bDbg)
                {
                    wstring b2 = file;
                    size_t s2 = b2.find_last_of(L"\\/");
                    if (s2 != wstring::npos) b2 = b2.substr(s2 + 1);
                    fwprintf(stderr, L"[BIND] file=%s type=%d hasbase=%d\n",
                             file.c_str(), (int)mod.t,
                             (int)(mod.t == Value::TableT && mod.tab &&
                                   mod.tab->map.count(
                                       b2.substr(0, b2.size() - 7))));
                }
            }
            if (mod.t == Value::TableT && mod.tab && mod.tab != placeholder)
            {
                for (auto& kv : mod.tab->map)
                    placeholder->map[kv.first] = kv.second;
            }
            modules[file] = mod;
            return BindModule(n->names, mod);
        }

        // invoke a VM function value from native code (callback dispatch);
        // saves/restores interp state so re-entrant calls are safe
        Value CallVmFunc(const Value& f, vector<Value>& args)
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
                local.vars[L"..."] = Value::Table_(tbV);
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

        void InitBuiltins()
        {
            // ---- global call() builtin: call(fn, owner, ...)
            // returns ok, ret (metaProperty machinery depends on it)
            globals.vars[L"call"] =
                NF([this](vector<Value>& a) -> Value {
                auto finish = [this](bool ok, Value r) -> Value {
                    Value vOk = Value::Bool_(ok);
                    auto extras = std::make_shared<vector<Value>>();
                    extras->push_back(r);
                    if (r.mrets)
                    {
                        for (auto& item : *r.mrets)
                            extras->push_back(item);
                    }
                    vOk.mrets = extras;
                    return vOk;
                };
                if (a.empty() || (a[0].t != Value::Func &&
                                  a[0].t != Value::Class &&
                                  a[0].t != Value::TableT))
                    return finish(false, Value());
                try
                {
                    vector<Value> args;
                    for (size_t k = 2; k < a.size(); k++)
                        args.push_back(a[k]);
                    Value r = CallOwner(a[0],
                        a.size() > 1 ? a[1] : Value(), args);
                    if (err && !err->empty())
                    {
                        wstring e = *err;
                        err->clear();
                        return finish(false, Value::Str_(e));
                    }
                    return finish(true, r);
                }
                catch (...)
                {
                    return finish(false, Value());
                }
            });

            // ---- string (1-based, aardio/Lua style) ----
            {
                auto tb = GetRoot(L"string").tab;
                tb->map[L"len"] = NF([](vector<Value>& a) {
                    return Value::Num_((double)BStr(a[0]).size()); });
                tb->map[L"left"] = NF([](vector<Value>& a) {
                    wstring s = BStr(a[0]); size_t k = (size_t)BNum(a[1]);
                    return Value::Str_(s.substr(0, min(k, s.size()))); });
                tb->map[L"right"] = NF([](vector<Value>& a) {
                    wstring s = BStr(a[0]); size_t k = (size_t)BNum(a[1]);
                    return Value::Str_(k >= s.size() ? s : s.substr(s.size() - k)); });
                tb->map[L"mid"] = NF([](vector<Value>& a) {
                    wstring s = BStr(a[0]);
                    long long st = (long long)BNum(a[1]); if (st < 1) st = 1;
                    long long ct = a.size() > 2 ? (long long)BNum(a[2]) : -1;
                    if (ct < 0 || (size_t)(st - 1 + ct) > s.size())
                        ct = (long long)s.size() - (st - 1);
                    if (ct < 0) ct = 0;
                    return Value::Str_(s.substr((size_t)(st - 1), (size_t)ct)); });
                tb->map[L"upper"] = NF([](vector<Value>& a) {
                    wstring s = BStr(a[0]);
                    for (auto& c : s) c = (wchar_t)towupper(c);
                    return Value::Str_(s); });
                tb->map[L"lower"] = NF([](vector<Value>& a) {
                    wstring s = BStr(a[0]);
                    for (auto& c : s) c = (wchar_t)towlower(c);
                    return Value::Str_(s); });
                tb->map[L"trim"] = NF([](vector<Value>& a) {
                    wstring s = BStr(a[0]);
                    size_t b = s.find_first_not_of(L" \t\r\n");
                    if (b == wstring::npos) return Value::Str_(L"");
                    size_t e = s.find_last_not_of(L" \t\r\n");
                    return Value::Str_(s.substr(b, e - b + 1)); });
                tb->map[L"index"] = NF([](vector<Value>& a) {
                    wstring s = BStr(a[0]), sub = BStr(a[1]);
                    size_t from = a.size() > 2 ? (size_t)BNum(a[2]) : 1;
                    if (from < 1) from = 1;
                    size_t pos = s.find(sub, from - 1);
                    if (pos == wstring::npos) return Value();
                    return Value::Num_((double)(pos + 1)); });
                tb->map[L"find"] = NF([](vector<Value>& a) {
                    // pattern search (aardio string.find): {start,end} table
                    wstring s = BStr(a[0]), pat = BStr(a[1]);
                    size_t start = a.size() > 2 ? (size_t)BNum(a[2]) : 1;
                    if (start < 1) start = 1;
                    vector<vector<RE>> br; bool anchored = false;
                    int nGroups = 0; wstring perr;
                    if (!RECompile(pat, br, &anchored, &nGroups, &perr))
                        return Value();
                    vector<wstring> caps; size_t end = 0;
                    for (size_t p0 = start - 1; p0 <= s.size(); p0++)
                    {
                        if (RETry(br, s, p0, false, caps, &end))
                        {
                            auto tb2 = std::make_shared<Table>();
                            tb2->arr.push_back(Value::Num_((double)(p0 + 1)));
                            tb2->arr.push_back(Value::Num_((double)end));
                            tb2->map[L"start"] = Value::Num_((double)(p0 + 1));
                            tb2->map[L"end"] = Value::Num_((double)end);
                            return Value::Table_(tb2);
                        }
                        if (anchored) break;
                    }
                    return Value(); });
                tb->map[L"match"] = NF([](vector<Value>& a) {
                    // no groups -> whole match; 1 group -> it; more -> table
                    wstring s = BStr(a[0]), pat = BStr(a[1]);
                    size_t start = a.size() > 2 ? (size_t)BNum(a[2]) : 1;
                    if (start < 1) start = 1;
                    vector<vector<RE>> br; bool anchored = false;
                    int nGroups = 0; wstring perr;
                    if (!RECompile(pat, br, &anchored, &nGroups, &perr))
                        return Value();
                    vector<wstring> caps; size_t end = 0;
                    for (size_t p0 = start - 1; p0 <= s.size(); p0++)
                    {
                        if (RETry(br, s, p0, false, caps, &end))
                        {
                            if (nGroups == 0)
                                return Value::Str_(s.substr(p0, end - p0));
                            if (nGroups == 1)
                                return (size_t)1 < caps.size()
                                    ? Value::Str_(caps[1]) : Value::Str_(L"");
                            auto tb2 = std::make_shared<Table>();
                            for (size_t g = 1; g <= (size_t)nGroups &&
                                               g < caps.size(); g++)
                                tb2->arr.push_back(Value::Str_(caps[g]));
                            return Value::Table_(tb2);
                        }
                        if (anchored) break;
                    }
                    return Value(); });
                tb->map[L"gsub"] = NF([](vector<Value>& a) {
                    wstring s = BStr(a[0]), pat = BStr(a[1]);
                    wstring repl = a.size() > 2 ? BStr(a[2]) : L"";
                    int limit = a.size() > 3 ? (int)BNum(a[3]) : -1;
                    vector<vector<RE>> br; bool anchored = false;
                    int nGroups = 0; wstring perr;
                    if (!RECompile(pat, br, &anchored, &nGroups, &perr))
                        return Value::Str_(s);
                    wstring out;
                    size_t cp = 0, p0 = 0;
                    int done = 0;
                    while (p0 <= s.size() && (limit < 0 || done < limit))
                    {
                        vector<wstring> caps; size_t end = 0;
                        if (RETry(br, s, p0, false, caps, &end))
                        {
                            out += s.substr(cp, p0 - cp);
                            for (size_t r = 0; r < repl.size(); r++)
                            {
                                if ((repl[r] == L'$' || repl[r] == L'\\') &&
                                    r + 1 < repl.size() && iswdigit(repl[r + 1]))
                                {
                                    int g = repl[r + 1] - L'0';
                                    if (g == 0)
                                        out += s.substr(p0, end - p0);
                                    else if ((size_t)g < caps.size())
                                        out += caps[(size_t)g];
                                    r++;
                                }
                                else
                                    out += repl[r];
                            }
                            done++;
                            if (end == p0)
                            {
                                if (p0 < s.size()) out += s[p0];
                                p0++;
                            }
                            else
                                p0 = end;
                            cp = p0;
                            continue;
                        }
                        if (p0 >= s.size()) break;
                        p0++;
                    }
                    out += s.substr(cp);
                    return Value::Str_(out); });
                tb->map[L"replace"] = NF([](vector<Value>& a) {
                    wstring s = BStr(a[0]), f = BStr(a[1]), r = BStr(a[2]);
                    if (f.empty()) return Value::Str_(s);
                    size_t pos = 0;
                    while ((pos = s.find(f, pos)) != wstring::npos)
                    {
                        s.replace(pos, f.size(), r);
                        pos += r.size();
                    }
                    return Value::Str_(s); });
                tb->map[L"split"] = NF([](vector<Value>& a) {
                    wstring s = BStr(a[0]), d = a.size() > 1 ? BStr(a[1]) : L",";
                    auto tb2 = std::make_shared<Table>();
                    if (d.empty())
                    {
                        for (auto& c : s) tb2->arr.push_back(Value::Str_(wstring(1, c)));
                    }
                    else
                    {
                        size_t pos = 0;
                        while (true)
                        {
                            size_t nx = s.find(d, pos);
                            if (nx == wstring::npos)
                            {
                                tb2->arr.push_back(Value::Str_(s.substr(pos)));
                                break;
                            }
                            tb2->arr.push_back(Value::Str_(s.substr(pos, nx - pos)));
                            pos = nx + d.size();
                        }
                    }
                    return Value::Table_(tb2); });
                tb->map[L"count"] = NF([](vector<Value>& a) {
                    wstring s = BStr(a[0]), f = BStr(a[1]);
                    if (f.empty()) return Value::Num_(0);
                    vector<vector<RE>> br; bool anchored = false;
                    int nGroups = 0; wstring perr;
                    if (!RECompile(f, br, &anchored, &nGroups, &perr))
                        return Value::Num_(0);
                    long long c = 0;
                    size_t p0 = 0;
                    while (p0 <= s.size())
                    {
                        vector<wstring> caps; size_t end = 0;
                        if (RETry(br, s, p0, false, caps, &end))
                        {
                            c++;
                            p0 = end > p0 ? end : p0 + 1;
                        }
                        else
                        {
                            if (p0 >= s.size()) break;
                            p0++;
                        }
                    }
                    return Value::Num_((double)c); });
                tb->map[L"rep"] = NF([](vector<Value>& a) {
                    wstring s = BStr(a[0]); long long k = (long long)BNum(a[1]);
                    wstring r;
                    for (long long i = 0; i < k; i++) r += s;
                    return Value::Str_(r); });
                tb->map[L"toUtf16"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty()) return Value::Str_(L"");
                    return Value::Str_(BStr(a[0])); });
                tb->map[L"fromUtf16"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty()) return Value();
                    if (a[0].t == Value::Str) return a[0];
                    if (a[0].t == Value::Pointer || a[0].t == Value::Num)
                    {
                        const wchar_t* p = (const wchar_t*)(INT_PTR)(long long)BNum(a[0]);
                        if (!p) return Value();
                        if (a.size() > 1 && a[1].t == Value::Num)
                        {
                            size_t len = (size_t)BNum(a[1]);
                            return Value::Str_(wstring(p, len));
                        }
                        return Value::Str_(wstring(p));
                    }
                    return Value(); });
                tb->map[L"pack"] = NF([](vector<Value>& a) -> Value {
                    wstring out;
                    std::function<void(const Value&)> addVal = [&](const Value& v) {
                        if (v.t == Value::Num)
                        {
                            out += (wchar_t)(unsigned char)(unsigned int)(long long)v.n;
                        }
                        else if (v.t == Value::Str)
                        {
                            out += v.s;
                        }
                        else if (v.t == Value::TableT && v.tab)
                        {
                            for (auto& item : v.tab->arr)
                                addVal(item);
                        }
                    };
                    for (auto& arg : a)
                        addVal(arg);
                    return Value::Str_(out); });
                tb->map[L"unpack"] = NF([](vector<Value>& a) -> Value {
                    auto t = std::make_shared<Table>();
                    if (a.empty()) return Value::Table_(t);
                    wstring s = BStr(a[0]);
                    int len = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
                    std::string u8(len, 0);
                    if (len > 0)
                        WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &u8[0], len, nullptr, nullptr);
                    for (unsigned char c : u8)
                        t->arr.push_back(Value::Num_((double)c));
                    return Value::Table_(t); });
                tb->map[L"format"] = NF([](vector<Value>& a) {
                    wstring fmt = BStr(a[0]);
                    wstring out;
                    size_t ai = 1;
                    for (size_t i = 0; i < fmt.size(); i++)
                    {
                        if (fmt[i] != L'%' || i + 1 >= fmt.size())
                        {
                            out += fmt[i];
                            continue;
                        }
                        wchar_t c = fmt[++i];
                        if (c == L'%') { out += L'%'; continue; }
                        wstring spec = L"%";
                        while (i < fmt.size() && (wcschr(L".0123456789", fmt[i]) ||
                               fmt[i] == L'l'))
                            spec += fmt[i++];
                        if (i >= fmt.size()) break;
                        spec += fmt[i];
                        Value arg = ai < a.size() ? a[ai++] : Value();
                        wchar_t buf[128];
                        switch (fmt[i])
                        {
                        case L's': out += BStr(arg); break;
                        case L'd': case L'i': case L'u':
                            swprintf(buf, 128, spec.c_str(), (long long)BNum(arg));
                            out += buf; break;
                        case L'f':
                            swprintf(buf, 128, spec.c_str(), BNum(arg));
                            out += buf; break;
                        case L'x': case L'X':
                            swprintf(buf, 128, spec.c_str(), (unsigned)BNum(arg));
                            out += buf; break;
                        case L'c': out += (wchar_t)BNum(arg); break;
                        default: out += arg.ToString(); break;
                        }
                    }
                    return Value::Str_(out); });
                tb->map[L"load"] = NF([](vector<Value>& a) {
                    FILE* pf = nullptr;
                    if (_wfopen_s(&pf, BStr(a[0]).c_str(), L"rb") != 0 || !pf)
                        return Value();
                    std::string raw2;
                    char buf2[4096]; size_t rd;
                    while ((rd = fread(buf2, 1, sizeof(buf2), pf)) > 0)
                        raw2.append(buf2, rd);
                    fclose(pf);
                    int off2 = (raw2.size() >= 3 && (BYTE)raw2[0] == 0xEF &&
                                (BYTE)raw2[1] == 0xBB && (BYTE)raw2[2] == 0xBF) ? 3 : 0;
                    int w2 = MultiByteToWideChar(CP_UTF8, 0, raw2.data() + off2,
                                                 (int)(raw2.size() - off2), nullptr, 0);
                    wstring s2((size_t)w2, 0);
                    if (w2 > 0)
                        MultiByteToWideChar(CP_UTF8, 0, raw2.data() + off2,
                                            (int)(raw2.size() - off2), &s2[0], w2);
                    return Value::Str_(s2); });
                tb->map[L"hex"] = NF([](vector<Value>& a) {
                    wstring str = BStr(a[0]);
                    int n = WideCharToMultiByte(CP_UTF8, 0, str.c_str(),
                        (int)str.size(), nullptr, 0, nullptr, nullptr);
                    std::string raw((size_t)n, 0);
                    WideCharToMultiByte(CP_UTF8, 0, str.c_str(),
                        (int)str.size(), &raw[0], n, nullptr, nullptr);
                    static const wchar_t* kHex = L"0123456789ABCDEF";
                    wstring out;
                    for (unsigned char c : raw)
                    {
                        out += kHex[c >> 4];
                        out += kHex[c & 0xF];
                    }
                    return Value::Str_(out); });
                tb->map[L"unhex"] = NF([](vector<Value>& a) {
                    wstring h = BStr(a[0]);
                    std::string raw;
                    for (size_t j = 0; j + 1 < h.size(); j += 2)
                        raw.push_back((char)wcstoul(h.substr(j, 2).c_str(),
                                                    nullptr, 16));
                    int w = MultiByteToWideChar(CP_UTF8, 0, raw.data(),
                                                (int)raw.size(), nullptr, 0);
                    wstring out((size_t)w, 0);
                    if (w > 0)
                        MultiByteToWideChar(CP_UTF8, 0, raw.data(),
                                            (int)raw.size(), &out[0], w);
                    return Value::Str_(out); });
                tb->map[L"base64"] = NF([](vector<Value>& a) {
                    wstring str = BStr(a[0]);
                    int n = WideCharToMultiByte(CP_UTF8, 0, str.c_str(),
                        (int)str.size(), nullptr, 0, nullptr, nullptr);
                    std::string raw((size_t)n, 0);
                    WideCharToMultiByte(CP_UTF8, 0, str.c_str(),
                        (int)str.size(), &raw[0], n, nullptr, nullptr);
                    static const wchar_t* kB64 =
                        L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                    wstring out;
                    size_t j = 0;
                    while (j < raw.size())
                    {
                        unsigned b0 = (unsigned char)raw[j];
                        unsigned b1 = j + 1 < raw.size() ? (unsigned char)raw[j + 1] : 0;
                        unsigned b2 = j + 2 < raw.size() ? (unsigned char)raw[j + 2] : 0;
                        out += kB64[b0 >> 2];
                        out += kB64[((b0 & 3) << 4) | (b1 >> 4)];
                        out += j + 1 < raw.size() ? kB64[((b1 & 15) << 2) | (b2 >> 6)] : L'=';
                        out += j + 2 < raw.size() ? kB64[b2 & 63] : L'=';
                        j += 3;
                    }
                    return Value::Str_(out); });
                tb->map[L"save"] = NF([](vector<Value>& a) {
                    FILE* pf = nullptr;
                    if (_wfopen_s(&pf, BStr(a[0]).c_str(), L"wb") != 0 || !pf)
                        return Value::Bool_(false);
                    wstring s2 = BStr(a[1]);
                    int n2 = WideCharToMultiByte(CP_UTF8, 0, s2.c_str(),
                                                 (int)s2.size(), nullptr, 0,
                                                 nullptr, nullptr);
                    std::string out2((size_t)n2, 0);
                    if (n2 > 0)
                        WideCharToMultiByte(CP_UTF8, 0, s2.c_str(),
                                            (int)s2.size(), &out2[0], n2,
                                            nullptr, nullptr);
                    fwrite(out2.data(), 1, out2.size(), pf);
                    fclose(pf);
                    return Value::Bool_(true); });
                // round 43: fromto(s, from, to) — transcoding. Strings in
                // the VM are wide text; a "byte string" is a wide string
                // with every char <= 0xFF (one char = one raw byte).
                // Input with any char > 0xFF is TEXT (encoded with `from`
                // to get its bytes); otherwise input is a byte string.
                // Output: `to` empty/omitted -> decoded TEXT; otherwise
                // a byte string. from/to accept names or numeric
                // codepages (see CodepageOf); "" = UTF-8.
                tb->map[L"fromto"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty()) return Value();
                    wstring s = BStr(a[0]);
                    UINT cpFrom = CodepageOf(a.size() > 1 ? a[1] : Value());
                    bool bToText = a.size() < 3 ||
                                   a[2].t == Value::Null ||
                                   BStr(a[2]).empty();
                    UINT cpTo = bToText ? CP_UTF8
                        : CodepageOf(a[2]);
                    bool bTextIn = false;
                    for (wchar_t c : s)
                        if (c > 0xFF) { bTextIn = true; break; }
                    std::string raw;
                    if (bTextIn)
                    {
                        int nF = WideCharToMultiByte(cpFrom, 0, s.c_str(),
                            (int)s.size(), nullptr, 0, nullptr, nullptr);
                        if (nF < 0) return Value();
                        raw.resize((size_t)nF);
                    }
                    else
                    {
                        raw.resize(s.size());
                        for (size_t k = 0; k < s.size(); k++)
                            raw[k] = (char)(unsigned char)s[k];
                    }
                    if (bTextIn)
                        WideCharToMultiByte(cpFrom, 0, s.c_str(),
                            (int)s.size(), &raw[0], (int)raw.size(),
                            nullptr, nullptr);
                    int nWide = MultiByteToWideChar(cpFrom, 0, raw.data(),
                        (int)raw.size(), nullptr, 0);
                    if (nWide < 0) return Value();
                    wstring mid((size_t)nWide, 0);
                    if (nWide > 0)
                        MultiByteToWideChar(cpFrom, 0, raw.data(),
                            (int)raw.size(), &mid[0], nWide);
                    if (bToText)
                        return Value::Str_(mid);   // decoded text
                    int nTo = WideCharToMultiByte(cpTo, 0, mid.c_str(),
                        (int)mid.size(), nullptr, 0, nullptr, nullptr);
                    if (nTo < 0) return Value();
                    std::string out((size_t)nTo, 0);
                    if (nTo > 0)
                        WideCharToMultiByte(cpTo, 0, mid.c_str(),
                            (int)mid.size(), &out[0], nTo, nullptr,
                            nullptr);
                    wstring res((size_t)nTo, 0);
                    for (int k = 0; k < nTo; k++)
                        res[(size_t)k] =
                            (wchar_t)(unsigned char)out[(size_t)k];
                    return Value::Str_(res); });
            }
            // ---- math ----
            {
                auto tb = GetRoot(L"math").tab;
                tb->map[L"PI"] = Value::Num_(3.14159265358979323846);
                tb->map[L"E"] = Value::Num_(2.71828182845904523536);
                tb->map[L"abs"] = NF([](vector<Value>& a) {
                    return Value::Num_(fabs(BNum(a[0]))); });
                tb->map[L"ceil"] = NF([](vector<Value>& a) {
                    return Value::Num_(ceil(BNum(a[0]))); });
                tb->map[L"floor"] = NF([](vector<Value>& a) {
                    return Value::Num_(floor(BNum(a[0]))); });
                tb->map[L"sqrt"] = NF([](vector<Value>& a) {
                    return Value::Num_(sqrt(BNum(a[0]))); });
                tb->map[L"pow"] = NF([](vector<Value>& a) {
                    return Value::Num_(pow(BNum(a[0]), BNum(a[1]))); });
                tb->map[L"log"] = NF([](vector<Value>& a) {
                    return Value::Num_(log(BNum(a[0]))); });
                tb->map[L"exp"] = NF([](vector<Value>& a) {
                    return Value::Num_(exp(BNum(a[0]))); });
                tb->map[L"sin"] = NF([](vector<Value>& a) {
                    return Value::Num_(sin(BNum(a[0]))); });
                tb->map[L"cos"] = NF([](vector<Value>& a) {
                    return Value::Num_(cos(BNum(a[0]))); });
                tb->map[L"tan"] = NF([](vector<Value>& a) {
                    return Value::Num_(tan(BNum(a[0]))); });
                tb->map[L"max"] = NF([](vector<Value>& a) {
                    double m = a.empty() ? 0 : BNum(a[0]);
                    for (size_t i = 1; i < a.size(); i++)
                        m = max(m, BNum(a[i]));
                    return Value::Num_(m); });
                tb->map[L"min"] = NF([](vector<Value>& a) {
                    double m = a.empty() ? 0 : BNum(a[0]);
                    for (size_t i = 1; i < a.size(); i++)
                        m = min(m, BNum(a[i]));
                    return Value::Num_(m); });
                tb->map[L"random"] = NF([](vector<Value>& a) {
                    static bool seeded = false;
                    if (!seeded) { srand((unsigned)time(nullptr)); seeded = true; }
                    if (a.empty())
                        return Value::Num_((double)rand() / RAND_MAX);
                    long long lo = (long long)BNum(a[0]);
                    if (a.size() == 1)
                        return Value::Num_((double)(1 + rand() % lo));
                    long long hi = (long long)BNum(a[1]);
                    if (hi < lo) return Value::Num_((double)lo);
                    return Value::Num_((double)(lo + rand() % (hi - lo + 1))); });
            }
            // ---- io ----
            {
                auto tb = GetRoot(L"io").tab;
                auto writer = [](vector<Value>& a, const wchar_t* sep,
                                 bool nl) -> Value {
                    wstring out;
                    for (size_t i = 0; i < a.size(); i++)
                    {
                        if (i) out += sep;
                        out += a[i].ToString();
                    }
                    if (nl) out += L"\n";
                    WriteU8(stdout, out);
                    fflush(stdout);
                    return Value();
                };
                tb->map[L"print"] = NF([writer](vector<Value>& a) {
                    return writer(a, L"", false); });
                tb->map[L"println"] = NF([writer](vector<Value>& a) {
                    return writer(a, L"", true); });
                tb->map[L"open"] = NF([](vector<Value>& a) {
                    wstring path = BStr(a[0]);
                    wstring mode = a.size() > 1 ? BStr(a[1]) : L"r";
                    const wchar_t* m = wcschr(mode.c_str(), L'w') ? L"wb" :
                        wcschr(mode.c_str(), L'a') ? L"ab" : L"rb";
                    FILE* pf = nullptr;
                    if (_wfopen_s(&pf, path.c_str(), m) != 0 || !pf)
                        return Value();
                    auto fp = std::make_shared<FILE*>(pf);
                    auto tb2 = std::make_shared<Table>();
                    tb2->map[L"read"] = NF([fp](vector<Value>&) {
                        if (!*fp) return Value();
                        std::string raw3;
                        char buf3[4096]; size_t rd3;
                        while ((rd3 = fread(buf3, 1, sizeof(buf3), *fp)) > 0)
                            raw3.append(buf3, rd3);
                        int w3 = MultiByteToWideChar(CP_UTF8, 0, raw3.data(),
                                                     (int)raw3.size(), nullptr, 0);
                        wstring s3((size_t)w3, 0);
                        if (w3 > 0)
                            MultiByteToWideChar(CP_UTF8, 0, raw3.data(),
                                                (int)raw3.size(), &s3[0], w3);
                        return Value::Str_(s3); });
                    tb2->map[L"write"] = NF([fp](vector<Value>& a2) {
                        if (!*fp) return Value::Bool_(false);
                        wstring s3 = BStr(a2[0]);
                        int n3 = WideCharToMultiByte(CP_UTF8, 0, s3.c_str(),
                                                     (int)s3.size(), nullptr, 0,
                                                     nullptr, nullptr);
                        std::string out3((size_t)n3, 0);
                        if (n3 > 0)
                            WideCharToMultiByte(CP_UTF8, 0, s3.c_str(),
                                                (int)s3.size(), &out3[0], n3,
                                                nullptr, nullptr);
                        fwrite(out3.data(), 1, out3.size(), *fp);
                        return Value::Bool_(true); });
                    tb2->map[L"close"] = NF([fp](vector<Value>&) {
                        if (*fp) { fclose(*fp); *fp = nullptr; }
                        return Value(); });
                    tb2->map[L"size"] = NF([fp](vector<Value>&) {
                        if (!*fp) return Value::Num_(0);
                        long long cur = ftell(*fp);
                        fseek(*fp, 0, SEEK_END);
                        long long end = ftell(*fp);
                        fseek(*fp, cur, SEEK_SET);
                        return Value::Num_((double)end); });
                    return Value::Table_(tb2); });
                // io.readFile(path) — convenience wrapper
                // round 43: optional encoding arg — "utf8" (default, with
                // BOM tolerance), "gbk"/"ansi"/numeric codepage, or "raw"
                // (byte string: one char per raw byte)
                tb->map[L"readFile"] = NF([](vector<Value>& a) {
                    wstring s3;
                    wstring enc3 = a.size() > 1 && a[1].t != Value::Null
                        ? BStr(a[1]) : L"utf8";
                    // M6: published payload — "~\res\x" (exe-relative) or a
                    // bare relative key resolves from the embedded FS first
                    // (libEmbed exes carry res/ + the lib closure)
                    bool bHit = false;
                    if (g_interpCurrent && a[0].t == Value::Str)
                    {
                        wstring p = BStr(a[0]);
                        wstring key = p;
                        if (!key.empty() && key[0] == L'~')
                            key = key.substr(1);
                        if (!key.empty() &&
                            (key[0] == L'\\' || key[0] == L'/'))
                            key = key.substr(1);
                        auto eit = g_interpCurrent->embedded.find(key);
                        if (eit == g_interpCurrent->embedded.end() &&
                            key != p)
                            eit = g_interpCurrent->embedded.find(p);
                        if (eit != g_interpCurrent->embedded.end())
                        {
                            bHit = true;
                            if (enc3 == L"raw")
                            {
                                // byte string over the payload's UTF-8
                                int nb = WideCharToMultiByte(CP_UTF8, 0,
                                    eit->second.c_str(),
                                    (int)eit->second.size(), nullptr, 0,
                                    nullptr, nullptr);
                                std::string b8((size_t)(nb > 0 ? nb : 0), 0);
                                if (nb > 0)
                                    WideCharToMultiByte(CP_UTF8, 0,
                                        eit->second.c_str(),
                                        (int)eit->second.size(), &b8[0],
                                        nb, nullptr, nullptr);
                                s3.resize(b8.size());
                                for (size_t k = 0; k < b8.size(); k++)
                                    s3[k] = (wchar_t)
                                        (unsigned char)b8[k];
                            }
                            else
                                s3 = eit->second;   // already wide text
                        }
                    }
                    if (!bHit)
                    {
                        // aardio "~" = the exe's own directory (published
                        // res/ also exist on disk during F5 debugging)
                        wstring strPath = BStr(a[0]);
                        if (!strPath.empty() && strPath[0] == L'~')
                        {
                            strPath = strPath.substr(1);
                            if (!strPath.empty() &&
                                (strPath[0] == L'\\' || strPath[0] == L'/'))
                                strPath = strPath.substr(1);
                            wchar_t szExe[MAX_PATH];
                            GetModuleFileNameW(nullptr, szExe, MAX_PATH);
                            wchar_t* szCut = wcsrchr(szExe, L'\\');
                            if (szCut) *szCut = 0;
                            strPath = wstring(szExe) + L"\\" + strPath;
                        }
                        FILE* pf = nullptr;
                        if (_wfopen_s(&pf, strPath.c_str(), L"rb") != 0 ||
                            !pf)
                            return Value();
                        std::string raw3;
                        char buf3[4096]; size_t rd3;
                        while ((rd3 = fread(buf3, 1, sizeof(buf3), pf)) > 0)
                            raw3.append(buf3, rd3);
                        fclose(pf);
                        if (enc3 == L"raw")
                        {
                            s3.resize(raw3.size());
                            for (size_t k = 0; k < raw3.size(); k++)
                                s3[k] =
                                    (wchar_t)(unsigned char)raw3[k];
                            return Value::Str_(s3);
                        }
                        UINT cp3 = CodepageOf(a.size() > 1 ? a[1] : Value());
                        size_t off3 = 0;
                        if (enc3 == L"utf8" && raw3.size() >= 3 &&
                            (unsigned char)raw3[0] == 0xEF &&
                            (unsigned char)raw3[1] == 0xBB &&
                            (unsigned char)raw3[2] == 0xBF)
                            off3 = 3;    // strip UTF-8 BOM before decoding
                        int w3 = MultiByteToWideChar(cp3, 0,
                            raw3.data() + off3,
                            (int)(raw3.size() - off3), nullptr, 0);
                        s3.resize((size_t)(w3 > 0 ? w3 : 0));
                        if (w3 > 0)
                            MultiByteToWideChar(cp3, 0, raw3.data() + off3,
                                (int)(raw3.size() - off3), &s3[0], w3);
                    }
                    return Value::Str_(s3); });
                // M6: io.exist — embedded payload first, then disk
                // ("~" maps to the exe directory)
                tb->map[L"exist"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty() || a[0].t != Value::Str)
                        return Value::Bool_(false);
                    wstring strPath = BStr(a[0]);
                    if (g_interpCurrent)
                    {
                        wstring p = strPath;
                        wstring key = p;
                        if (!key.empty() && key[0] == L'~')
                            key = key.substr(1);
                        if (!key.empty() &&
                            (key[0] == L'\\' || key[0] == L'/'))
                            key = key.substr(1);
                        if (g_interpCurrent->embedded.count(key) ||
                            (key != p && g_interpCurrent->embedded.count(p)))
                            return Value::Bool_(true);
                    }
                    if (!strPath.empty() && strPath[0] == L'~')
                    {
                        strPath = strPath.substr(1);
                        if (!strPath.empty() &&
                            (strPath[0] == L'\\' || strPath[0] == L'/'))
                            strPath = strPath.substr(1);
                        wchar_t szExe[MAX_PATH];
                        GetModuleFileNameW(nullptr, szExe, MAX_PATH);
                        wchar_t* szCut = wcsrchr(szExe, L'\\');
                        if (szCut) *szCut = 0;
                        strPath = wstring(szExe) + L"\\" + strPath;
                    }
                    return Value::Bool_(
                        _waccess(strPath.c_str(), 0) == 0); });
                tb->map[L"writeFile"] = NF([](vector<Value>& a) {
                    FILE* pf = nullptr;
                    if (_wfopen_s(&pf, BStr(a[0]).c_str(), L"wb") != 0 || !pf)
                        return Value::Bool_(false);
                    wstring s3 = BStr(a[1]);
                    wstring enc3 = a.size() > 2 && a[2].t != Value::Null
                        ? BStr(a[2]) : L"utf8";
                    std::string out3;
                    if (enc3 == L"raw")
                    {
                        out3.resize(s3.size());
                        for (size_t k = 0; k < s3.size(); k++)
                            out3[k] = (char)(unsigned char)s3[k];
                    }
                    else
                    {
                        UINT cp3 = CodepageOf(a.size() > 2 ? a[2]
                                                           : Value());
                        int n3 = WideCharToMultiByte(cp3, 0, s3.c_str(),
                                                     (int)s3.size(), nullptr,
                                                     0, nullptr, nullptr);
                        out3.resize((size_t)(n3 > 0 ? n3 : 0));
                        if (n3 > 0)
                            WideCharToMultiByte(cp3, 0, s3.c_str(),
                                                (int)s3.size(), &out3[0],
                                                n3, nullptr, nullptr);
                    }
                    fwrite(out3.data(), 1, out3.size(), pf);
                    fclose(pf);
                    return Value::Bool_(true); });
            }
            // ---- console ----
            {
                auto tb = GetRoot(L"console").tab;
                auto writer = [](vector<Value>& a, bool nl) -> Value {
                    wstring out;
                    for (size_t i = 0; i < a.size(); i++)
                    {
                        if (i) out += L" ";
                        out += a[i].ToString();
                    }
                    if (nl) out += L"\n";
                    WriteU8(stdout, out);
                    fflush(stdout);
                    return Value();
                };
                tb->map[L"log"] = NF([writer](vector<Value>& a) {
                    return writer(a, true); });
                tb->map[L"println"] = tb->map[L"log"];
                tb->map[L"print"] = NF([writer](vector<Value>& a) {
                    return writer(a, false); });
                tb->map[L"dump"] = NF([writer](vector<Value>& a) {
                    for (auto& v : a)
                    {
                        if (v.t == Value::TableT && v.tab)
                        {
                            wstring out = L"{ ";
                            for (auto& kv : v.tab->map)
                                out += kv.first + L"=" + kv.second.ToString() + L"; ";
                            out += L"}";
                            WriteU8(stdout, out);
                        }
                        else
                            WriteU8(stdout, v.ToString());
                        WriteU8(stdout, L"\n");
                    }
                    fflush(stdout);
                    return Value(); });
                tb->map[L"pause"] = NF([](vector<Value>&) {
                    WriteU8(stdout, L"请按任意键继续 . . .\n");
                    fflush(stdout);
                    if (fgetwc(stdin) == WEOF)
                    {
                        // stdin closed (pipe/headless): nothing to wait on
                    }
                    return Value(); });
                tb->map[L"open"] = NF([](vector<Value>&) { return Value(); });
                tb->map[L"close"] = NF([](vector<Value>&) { return Value(); });
            }
            // ---- time (round 30) ----
            {
                auto tb = GetRoot(L"time").tab;
                tb->map[L"now"] = NF([](vector<Value>&) {
                    return Value::Num_((double)time(nullptr)); });
                tb->map[L"tick"] = NF([](vector<Value>&) {
                    return Value::Num_((double)GetTickCount()); });
                tb->map[L"date"] = NF([](vector<Value>& a) {
                    wstring fmt = a.empty() ? L"%Y-%m-%d %H:%M:%S"
                                            : BStr(a[0]);
                    time_t t = a.size() > 1 ? (time_t)BNum(a[1]) : time(nullptr);
                    tm tmb;
                    localtime_s(&tmb, &t);
                    wchar_t buf[256];
                    wcsftime(buf, 256, fmt.c_str(), &tmb);
                    return Value::Str_(buf); });
            }
            // ---- json (round 30) ----
            {
                auto tb = GetRoot(L"json").tab;
                tb->map[L"parse"] = NF([](vector<Value>& a) {
                    JParser jp{ BStr(a[0]), 0, nullptr };
                    wstring err2;
                    jp.err = &err2;
                    Value v = jp.Parse();
                    if (!err2.empty()) return Value();
                    return v; });
                tb->map[L"stringify"] = NF([](vector<Value>& a) {
                    bool pretty = a.size() > 1 && a[1].Truthy();
                    return Value::Str_(JWrite(a[0], pretty ? 0 : -1, 0)); });
            }
            // ---- win (round 29 minimal): msgbox + clipboard ----
            {
                auto tb = GetRoot(L"win").tab;
                auto clip = std::make_shared<Table>();
                tb->map[L"clip"] = Value::Table_(clip);
                tb->map[L"msgbox"] = NF([](vector<Value>& a) {
                    wstring text = a.empty() ? L"" : a[0].ToString();
                    wstring title = a.size() > 1 ? a[1].ToString() : L"aardio";
                    UINT flag = a.size() > 2 ? (UINT)BNum(a[2]) : 0u;
                    int rc = MessageBoxW(nullptr, text.c_str(), title.c_str(),
                                         MB_OK | flag);
                    return Value::Num_((double)rc); });
                tb->map[L"msgboxTest"] = NF([](vector<Value>& a) {
                    wstring text = a.empty() ? L"" : a[0].ToString();
                    wstring title = a.size() > 1 ? a[1].ToString() : L"aardio";
                    return Value::Bool_(MessageBoxW(nullptr, text.c_str(),
                        title.c_str(), MB_OKCANCEL | MB_ICONQUESTION) == IDOK); });
                clip->map[L"write"] = NF([](vector<Value>& a) {
                    wstring text = a.empty() ? L"" : a[0].ToString();
                    BOOL ok = FALSE;
                    if (OpenClipboard(nullptr))
                    {
                        EmptyClipboard();
                        int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                            (int)text.size(), nullptr, 0, nullptr, nullptr);
                        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE,
                                                (size_t)n + 2);
                        if (h)
                        {
                            char* p = (char*)GlobalLock(h);
                            WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                (int)text.size(), p, n, nullptr, nullptr);
                            p[n] = 0; p[n + 1] = 0;
                            GlobalUnlock(h);
                            // clipboard text is Unicode: put CF_UNICODETEXT
                            int w2 = (int)text.size();
                            HGLOBAL h2 = GlobalAlloc(GMEM_MOVEABLE,
                                (size_t)(w2 + 1) * 2);
                            if (h2)
                            {
                                wchar_t* q = (wchar_t*)GlobalLock(h2);
                                memcpy(q, text.c_str(), (size_t)w2 * 2);
                                q[w2] = 0;
                                GlobalUnlock(h2);
                                ok = SetClipboardData(CF_UNICODETEXT, h2) != nullptr;
                            }
                            GlobalFree(h);
                        }
                        CloseClipboard();
                    }
                    return Value::Bool_(ok != FALSE); });
                clip->map[L"read"] = NF([](vector<Value>&) {
                    wstring out;
                    if (OpenClipboard(nullptr))
                    {
                        HANDLE h = GetClipboardData(CF_UNICODETEXT);
                        if (h)
                        {
                            wchar_t* q = (wchar_t*)GlobalLock(h);
                            if (q) { out = q; GlobalUnlock(h); }
                        }
                        CloseClipboard();
                    }
                    return Value::Str_(out); });
                // round 42: raw window access + common dialogs
                tb->map[L"findWindow"] = NF([](vector<Value>& a) -> Value {
                    wstring s1 = a.size() > 0 ? BStr(a[0]) : L"";
                    wstring s2 = a.size() > 1 ? BStr(a[1]) : L"";
                    HWND h = FindWindowW(
                        a.size() > 0 && a[0].t != Value::Null && !s1.empty()
                            ? s1.c_str() : nullptr,
                        a.size() > 1 && a[1].t != Value::Null && !s2.empty()
                            ? s2.c_str() : nullptr);
                    return h ? Value::Num_((double)(INT_PTR)h) : Value(); });
                tb->map[L"postMessage"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty() || a[0].t != Value::Num)
                        return Value::Bool_(false);
                    HWND h = (HWND)(INT_PTR)(long long)BNum(a[0]);
                    UINT m = a.size() > 1 ? (UINT)BNum(a[1]) : 0;
                    WPARAM w = a.size() > 2
                        ? (WPARAM)(INT_PTR)(long long)BNum(a[2]) : 0;
                    LPARAM l = a.size() > 3
                        ? (LPARAM)(INT_PTR)(long long)BNum(a[3]) : 0;
                    return Value::Bool_(PostMessageW(h, m, w, l) != 0); });
                auto HwndOf = [](const Value& v) -> HWND {
                    if (v.t == Value::Num)
                        return (HWND)(INT_PTR)(long long)v.n;
                    if (v.t == Value::TableT && v.tab)
                    {
                        auto ih = v.tab->map.find(L"hwnd");
                        if (ih != v.tab->map.end() &&
                            ih->second.t == Value::Num)
                            return (HWND)(INT_PTR)(long long)ih->second.n;
                    }
                    return nullptr; };
                auto dlg = std::make_shared<Table>();
                tb->map[L"dlg"] = Value::Table_(dlg);
                // open(formOrHwnd, title, filter, initDir, initFile) -> path
                // filter uses aardio pipe form: "文本|*.txt|所有|*.*"
                auto dlgImpl = [HwndOf](vector<Value>& a, bool bSave)
                    -> Value {
                    HWND hOwner = a.empty() ? nullptr : HwndOf(a[0]);
                    wstring sTitle = a.size() > 1 ? BStr(a[1]) : L"";
                    wstring sFilter = a.size() > 2 ? BStr(a[2]) : L"";
                    wstring sDir = a.size() > 3 ? BStr(a[3]) : L"";
                    wstring sFile = a.size() > 4 ? BStr(a[4]) : L"";
                    for (auto& c : sFilter)
                        if (c == L'|') c = L'\0';
                    if (!sFilter.empty()) sFilter.push_back(L'\0');
                    wchar_t buf[MAX_PATH * 2] = L"";
                    wcsncpy_s(buf, sFile.c_str(), _TRUNCATE);
                    OPENFILENAMEW ofn = { sizeof(ofn) };
                    ofn.hwndOwner = hOwner;
                    ofn.lpstrFilter = sFilter.empty()
                                          ? L"所有文件\0*.*\0"
                                          : sFilter.c_str();
                    ofn.lpstrFile = buf;
                    ofn.nMaxFile = MAX_PATH * 2;
                    ofn.lpstrTitle =
                        sTitle.empty() ? nullptr : sTitle.c_str();
                    ofn.lpstrInitialDir =
                        sDir.empty() ? nullptr : sDir.c_str();
                    ofn.Flags = OFN_EXPLORER | OFN_HIDEREADONLY;
                    if (bSave)
                        ofn.Flags |= OFN_OVERWRITEPROMPT;
                    else
                        ofn.Flags |= OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
                    BOOL ok = bSave ? GetSaveFileNameW(&ofn)
                                    : GetOpenFileNameW(&ofn);
                    if (!ok)
                        return Value();     // cancel -> null
                    return Value::Str_(buf); };
                dlg->map[L"open"] = NF([dlgImpl](vector<Value>& a)
                    -> Value {
                    return dlgImpl(a, false); });
                dlg->map[L"save"] = NF([dlgImpl](vector<Value>& a)
                    -> Value {
                    return dlgImpl(a, true); });
            }
            // ---- round 44: raw — P/Invoke + raw memory ----
            {
                auto tb = GetRoot(L"raw").tab;
                tb->map[L"loadDll"] = NF([this](vector<Value>& a)
                    -> Value {
                    HMODULE hMod = nullptr;
                    bool bIsMainMod = false;
                    if (a.empty() || a[0].t == Value::Null || (a[0].t == Value::Str && a[0].s.empty()))
                    {
                        hMod = GetModuleHandleW(nullptr);
                        bIsMainMod = true;
                    }
                    else
                    {
                        hMod = LoadLibraryW(BStr(a[0]).c_str());
                        if (!hMod)
                        {
                            (*err) = L"运行错误: 无法加载 DLL: " + BStr(a[0]);
                            return Value();
                        }
                    }
                    auto dll = std::make_shared<Table>();
                    dll->map[L"_h"] =
                        Value::Pointer_(hMod);
                    dll->map[L"gethandle"] = NF([hMod](vector<Value>&) {
                        return Value::Pointer_(hMod);
                    });
                    dll->map[L"free"] = NF([hMod, bIsMainMod](vector<Value>&) {
                        if (!bIsMainMod) FreeLibrary(hMod);
                        return Value(); });
                    bool bDllCdecl = false;
                    bool bDllUni = false;
                    if (a.size() > 1)
                    {
                        wstring conv = BStr(a[1]);
                        for (auto& c : conv) c = towlower(c);
                        if (conv.find(L"cdecl") != wstring::npos) bDllCdecl = true;
                        if (conv.find(L"unicode") != wstring::npos) bDllUni = true;
                    }
                    dll->map[L"_cdecl"] = Value::Bool_(bDllCdecl);
                    dll->map[L"_unicode"] = Value::Bool_(bDllUni);
                    // api(name, signature) -> callable function value
                    dll->map[L"api"] = NF([this, hMod](
                        vector<Value>& a2) -> Value {
                        if (a2.empty())
                        {
                            (*err) = L"运行错误: dll.api(name, sig)";
                            return Value();
                        }
                        FARPROC fp = nullptr;
                        wstring strName;
                        if (a2[0].t == Value::Pointer)
                            fp = (FARPROC)a2[0].ptr;
                        else if (a2[0].t == Value::Num)
                            fp = (FARPROC)(INT_PTR)a2[0].n;
                        else
                        {
                            strName = BStr(a2[0]);
                        }
                        ApiSig sig;
                        wstring sigStr = a2.size() > 1 ? BStr(a2[1]) : L"void()";
                        if (!ParseApiSig(sigStr, &sig))
                        {
                            (*err) = L"运行错误: API 签名无效: " + sigStr;
                            return Value();
                        }
                        if (!fp && !strName.empty())
                        {
                            std::string strNameA((size_t)
                                WideCharToMultiByte(CP_ACP, 0,
                                    strName.c_str(), (int)strName.size(),
                                    nullptr, 0, nullptr, nullptr), 0);
                            if (!strNameA.empty())
                                WideCharToMultiByte(CP_ACP, 0,
                                    strName.c_str(), (int)strName.size(),
                                    &strNameA[0], (int)strNameA.size(),
                                    nullptr, nullptr);
                            fp = ::GetProcAddress(hMod,
                                strNameA.c_str());
                            // Win32 dual exports: bare name -> W -> A, and
                            // W-suffixed declarations of ANSI-only exports
                            // (user32 "CopyImageW" exists only as CopyImage —
                            // the original VM strips the trailing W on miss)
                            if (!fp && strNameA.find('@') == std::string::npos)
                            {
                                std::string w = strNameA + "W";
                                fp = ::GetProcAddress(hMod, w.c_str());
                            }
                            if (!fp && strNameA.size() > 1 &&
                                strNameA.back() == L'W' &&
                                strNameA.find('@') == std::string::npos)
                            {
                                std::string bare = strNameA.substr(0,
                                    strNameA.size() - 1);
                                fp = ::GetProcAddress(hMod, bare.c_str());
                            }
                            if (!fp)
                            {
                                std::string an = strNameA + "A";
                                fp = ::GetProcAddress(hMod, an.c_str());
                            }
                        }
                        if (!fp)
                        {
                            if (a2.size() == 1)
                                return Value();
                            (*err) = L"运行错误: 找不到 API: " + strName;
                            return Value();
                        }
                        void* fn = (void*)fp;
#if defined(_WIN64)
                        // one executable call stub per bound signature —
                        // carries float/double args into XMM0..3
                        void* stub = EmitX64CallStub(sig);
#else
                        void* stub = nullptr;
#endif
                        Value retFn = NF([this, fn, sig, stub](vector<Value>& a3)
                            -> Value {
                            // box arguments into integer slots
                            vector<SlotT> slots;
                            std::vector<std::string> tmpA;
                            std::vector<wstring> tmpW;
                            size_t nA = 0, nW = 0;
                            for (size_t i = 0; i < sig.params.size();
                                 i++)
                            {
                                Value v = i < a3.size()
                                    ? a3[i] : Value();
                                if (sig.params[i] == 6 && v.t == Value::Str)
                                    nA++;
                                if (sig.params[i] == 7 && v.t == Value::Str)
                                    nW++;
                            }
                            tmpA.reserve(nA);
                            tmpW.reserve(nW);
                            struct ApiArgRef
                            {
                                size_t paramIdx = 0;
                                int paramType = 0;
                                void* pBuffer = nullptr;
                                bool bStruct = false;
                                Value origVal;
                            };
                            std::vector<ApiArgRef> tmpRefs;
                            for (size_t i = 0; i < sig.params.size();
                                 i++)
                            {
                                Value v = i < a3.size()
                                    ? a3[i] : Value();
                                int pt = sig.params[i] & 0xFF;
                                bool bByRef =
                                    (sig.params[i] & 0x100) != 0;
                                // & out-params: allocate scratch memory, copy initial value if passed
                                if (bByRef && (pt == 0 || pt == 1 || pt == 2 ||
                                               pt == 3 || pt == 4 || pt == 5 ||
                                               pt == 9 || pt == 10))
                                {
                                    void* pRef = HeapAlloc(
                                        GetProcessHeap(),
                                        HEAP_ZERO_MEMORY, 8);
                                    if (pt == 0 || pt == 4) *(int*)pRef = (int)(unsigned int)(long long)BNum(v);
                                    else if (pt == 1) *(__int64*)pRef = (__int64)BNum(v);
                                    else if (pt == 2) *(unsigned short*)pRef = (unsigned short)(unsigned int)(long long)BNum(v);
                                    else if (pt == 3) *(unsigned char*)pRef = (unsigned char)(unsigned int)(long long)BNum(v);
                                    else if (pt == 5) *(INT_PTR*)pRef = (INT_PTR)(long long)BNum(v);
                                    else if (pt == 9) { float f = (float)BNum(v); memcpy(pRef, &f, sizeof(float)); }
                                    else if (pt == 10) { double d = BNum(v); memcpy(pRef, &d, sizeof(double)); }
                                    tmpRefs.push_back({ i, pt, pRef, false, v });
                                    slots.push_back((SlotT)(INT_PTR)pRef);
                                    continue;
                                }
                                if (pt == 8) // struct
                                {
                                    if (v.t == Value::TableT || v.t == Value::Class)
                                    {
                                        int total = 0;
                                        void* pBuf = StructSerialize(v, &total);
                                        if (pBuf)
                                        {
                                            slots.push_back((SlotT)(INT_PTR)pBuf);
                                            tmpRefs.push_back({ i, pt, pBuf, true, v });
                                            continue;
                                        }
                                    }
                                    if (v.t == Value::Null)
                                    {
                                        slots.push_back((SlotT)0);
                                        continue;
                                    }
                                    if (v.t == Value::Pointer)
                                    {
                                        slots.push_back((SlotT)(INT_PTR)v.ptr);
                                        continue;
                                    }
                                    slots.push_back((SlotT)(INT_PTR)(long long)BNum(v));
                                    continue;
                                }
                                switch (pt)
                                {
                                case 0:   // int
                                    // wrap modulo 2^32 (aardio doubles:
                                    // 0xFFFFFFF0 must arrive as -16, not
                                    // the MSVC saturate value)
                                    slots.push_back((SlotT)(
                                        INT_PTR)(int)(unsigned int)
                                            (long long)BNum(v));
                                    break;
                                case 1:   // int64
#if defined(_M_IX86)
                                {
                                    long long v64 =
                                        (long long)BNum(v);
                                    slots.push_back((SlotT)
                                        (v64 & 0xFFFFFFFF));
                                    slots.push_back((SlotT)
                                        ((unsigned long long)v64
                                         >> 32));
                                    break;
                                }
#else
                                    slots.push_back((SlotT)
                                        (unsigned long long)
                                            (long long)BNum(v));
                                    break;
#endif
                                case 2:   // word
                                    slots.push_back((SlotT)
                                        (unsigned short)(unsigned int)
                                            (long long)BNum(v));
                                    break;
                                case 3:   // byte
                                    slots.push_back((SlotT)
                                        (unsigned char)(unsigned int)
                                            (long long)BNum(v));
                                    break;
                                case 4:   // bool
                                    slots.push_back(
                                        BNum(v) != 0 ? 1 : 0);
                                    break;
                                case 5:   // pointer (hwnd / buffer)
                                    if (v.t == Value::Pointer)
                                        slots.push_back((SlotT)(INT_PTR)v.ptr);
                                    else
                                        slots.push_back(v.t == Value::Null
                                            ? (SlotT)0
                                            : (SlotT)(INT_PTR)(long long)
                                                  BNum(v));
                                    break;
                                case 6:   // str (ANSI)
                                    if (v.t == Value::Null)
                                        slots.push_back((SlotT)0);
                                    else if (v.t == Value::Pointer)
                                        slots.push_back((SlotT)(INT_PTR)v.ptr);
                                    else if (v.t == Value::Num)
                                        slots.push_back((SlotT)
                                            (INT_PTR)(long long)
                                                BNum(v));
                                    else
                                    {
                                        wstring w = BStr(v);
                                        std::string sA((size_t)
                                            WideCharToMultiByte(
                                                CP_ACP, 0, w.c_str(),
                                                (int)w.size(), nullptr,
                                                0, nullptr, nullptr), 0);
                                        if (!sA.empty())
                                            WideCharToMultiByte(CP_ACP,
                                                0, w.c_str(),
                                                (int)w.size(), &sA[0],
                                                (int)sA.size(), nullptr,
                                                nullptr);
                                        sA.push_back('\0');
                                        tmpA.push_back(std::move(sA));
                                        slots.push_back((SlotT)
                                            (INT_PTR)
                                                tmpA.back().c_str());
                                    }
                                    break;
                                case 7:   // wstr (UTF-16)
                                    if (v.t == Value::Null)
                                        slots.push_back((SlotT)0);
                                    else if (v.t == Value::Pointer)
                                        slots.push_back((SlotT)(INT_PTR)v.ptr);
                                    else if (v.t == Value::Num)
                                        slots.push_back((SlotT)
                                            (INT_PTR)(long long)
                                                BNum(v));
                                    else
                                    {
                                        tmpW.push_back(BStr(v));
                                        slots.push_back((SlotT)
                                            (INT_PTR)
                                                tmpW.back().c_str());
                                    }
                                    break;
                                case 9:   // float
                                {
                                    float f = (float)BNum(v);
                                    SlotT s = 0;
                                    memcpy(&s, &f, sizeof(float));
                                    slots.push_back(s);
                                    break;
                                }
                                case 10:  // double
                                {
#if defined(_M_IX86)
                                    double d = BNum(v);
                                    unsigned __int64 u64 = 0;
                                    memcpy(&u64, &d, sizeof(double));
                                    slots.push_back((SlotT)(u64 & 0xFFFFFFFF));
                                    slots.push_back((SlotT)(u64 >> 32));
#else
                                    double d = BNum(v);
                                    SlotT s = 0;
                                    memcpy(&s, &d, sizeof(double));
                                    slots.push_back(s);
#endif
                                    break;
                                }
                                default:
                                    slots.push_back(0);
                                    break;
                                }
                            }
                            if (slots.size() > 14)
                            {
                                (*err) =
                                    L"运行错误: API 参数超过 14 个";
                                return Value();
                            }
                            // invoke, then interpret the result.
                            // x64: the per-signature call stub routes
                            // float/double params through XMM0..3 per the
                            // ABI; a float/double return arrives bit-cast
                            // in RAX (stub epilogue) and is re-decoded.
#if defined(_WIN64)
                            bool bStub = stub && !sig.bCdecl;
#else
                            bool bStub = false;
#endif
                            if (bStub && sig.ret == ARVoid)
                            {
#if defined(_WIN64)
                                InvokeX64Stub(stub, fn, slots);
#endif
                                return Value();
                            }
                            if (sig.ret == ARDbl)
                            {
                                double d;
#if defined(_WIN64)
                                if (bStub)
                                {
                                    __int64 rb =
                                        InvokeX64Stub(stub, fn, slots);
                                    memcpy(&d, &rb, 8);
                                }
                                else
#endif
                                    d = sig.bCdecl
                                        ? InvokeCdl<double>(fn, slots)
                                        : InvokeStd<double>(fn, slots);
                                return Value::Num_(d);
                            }
                            __int64 r;
#if defined(_WIN64)
                            if (bStub)
                                r = InvokeX64Stub(stub, fn, slots);
                            else
#endif
                                r = sig.bCdecl
                                    ? InvokeCdl<__int64>(fn, slots)
                                    : InvokeStd<__int64>(fn, slots);
                            // decode the primary return
                            Value vRet;
                            switch (sig.ret)
                            {
                            case ARInt:
                                vRet = Value::Num_((double)(int)r);
                                break;
                            case ARWord:
                                vRet = Value::Num_(
                                    (double)(unsigned short)r);
                                break;
                            case ARByte:
                                vRet = Value::Num_(
                                    (double)(unsigned char)r);
                                break;
                            case ARBool:
                                vRet = Value::Bool_(r != 0);
                                break;
                            case ARPtr:
                                vRet = Value::Num_(
                                    (double)(__int64)(INT_PTR)r);
                                break;
                            case ARStrA:
                                vRet = Value::Str_(
                                    ReadStrA((const char*)(INT_PTR)r));
                                break;
                            case ARStrW:
                                vRet = Value::Str_(
                                    ReadStrW((const wchar_t*)
                                             (INT_PTR)r));
                                break;
                            case ARI64:
                            default:
                                vRet = Value::Num_((double)r);
                                break;
                            }
                            // aardio by-ref out-params come back as extra
                            // return values (var len,str = formatMessage(..))
                            if (!tmpRefs.empty())
                            {
                                auto extras =
                                    std::make_shared<vector<Value>>();
                                for (auto& ref : tmpRefs)
                                {
                                    if (ref.bStruct)
                                    {
                                        bool bRef = (sig.params[ref.paramIdx] & 0x100) != 0;
                                        if (ref.origVal.t == Value::TableT || ref.origVal.t == Value::Class)
                                        {
                                            StructDeserialize(ref.origVal, ref.pBuffer);
                                        }
                                        if (bRef)
                                            extras->push_back(ref.origVal);
                                        HeapFree(GetProcessHeap(), 0, ref.pBuffer);
                                    }
                                    else
                                    {
                                        Value outVal;
                                        switch (ref.paramType)
                                        {
                                        case 0:
                                            outVal = Value::Num_((double)*(int*)ref.pBuffer);
                                            break;
                                        case 1:
                                            outVal = Value::Num_((double)*(__int64*)ref.pBuffer);
                                            break;
                                        case 2:
                                            outVal = Value::Num_((double)*(unsigned short*)ref.pBuffer);
                                            break;
                                        case 3:
                                            outVal = Value::Num_((double)*(unsigned char*)ref.pBuffer);
                                            break;
                                        case 4:
                                            outVal = Value::Bool_(*(int*)ref.pBuffer != 0);
                                            break;
                                        case 5:
                                            outVal = Value::Num_((double)(__int64)*(INT_PTR*)ref.pBuffer);
                                            break;
                                        case 9:
                                        {
                                            float f = 0;
                                            memcpy(&f, ref.pBuffer, sizeof(float));
                                            outVal = Value::Num_(f);
                                            break;
                                        }
                                        case 10:
                                        {
                                            double d = 0;
                                            memcpy(&d, ref.pBuffer, sizeof(double));
                                            outVal = Value::Num_(d);
                                            break;
                                        }
                                        default:
                                            outVal = Value::Num_((double)*(int*)ref.pBuffer);
                                            break;
                                        }
                                        extras->push_back(outVal);
                                        HeapFree(GetProcessHeap(), 0, ref.pBuffer);
                                    }
                                }
                                if (!extras->empty())
                                    vRet.mrets = extras;
                            }
                            return vRet;
                            });
                            retFn.ptr = fn;
                            return retFn;
                        });
                    return Value::Table_(dll); });
                // raw memory: pointer values are plain numbers
                tb->map[L"buffer"] = NF([](vector<Value>& a) -> Value {
                    size_t n = a.empty() ? 0
                        : (size_t)(long long)BNum(a[0]);
                    if (n == 0) return Value::Num_(0);
                    void* p = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                        n);
                    return Value::Num_((double)(INT_PTR)p); });
                tb->map[L"free"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty() || a[0].t != Value::Num)
                        return Value::Bool_(false);
                    void* p = (void*)(INT_PTR)(long long)BNum(a[0]);
                    if (!p) return Value::Bool_(true);
                    return Value::Bool_(
                        HeapFree(GetProcessHeap(), 0, p) != FALSE); });
                // write(ptr, s): byte string -> raw bytes; text -> UTF-8
                tb->map[L"write"] = NF([](vector<Value>& a) -> Value {
                    if (a.size() < 2 || a[0].t != Value::Num)
                        return Value::Num_(0);
                    char* p = (char*)(INT_PTR)(long long)BNum(a[0]);
                    if (!p) return Value::Num_(0);
                    wstring s = BStr(a[1]);
                    bool bText = false;
                    for (wchar_t c : s)
                        if (c > 0xFF) { bText = true; break; }
                    if (!bText)
                    {
                        for (size_t k = 0; k < s.size(); k++)
                            p[k] = (char)(unsigned char)s[k];
                        return Value::Num_((double)s.size());
                    }
                    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(),
                        (int)s.size(), nullptr, 0, nullptr, nullptr);
                    if (n <= 0) return Value::Num_(0);
                    WideCharToMultiByte(CP_UTF8, 0, s.c_str(),
                        (int)s.size(), p, n, nullptr, nullptr);
                    return Value::Num_((double)n); });
                // read(ptr, n) -> byte string (one char per raw byte)
                tb->map[L"read"] = NF([](vector<Value>& a) -> Value {
                    if (a.size() < 2 || a[0].t != Value::Num)
                        return Value::Str_(L"");
                    const char* p =
                        (const char*)(INT_PTR)(long long)BNum(a[0]);
                    long long n = (long long)BNum(a[1]);
                    if (!p || n <= 0) return Value::Str_(L"");
                    wstring s((size_t)n, 0);
                    for (long long k = 0; k < n; k++)
                        s[(size_t)k] =
                            (wchar_t)(unsigned char)p[k];
                    return Value::Str_(s); });
                // readW(ptr, nChars) -> wide text from a UTF-16 buffer
                tb->map[L"readW"] = NF([](vector<Value>& a) -> Value {
                    if (a.size() < 2 || a[0].t != Value::Num)
                        return Value::Str_(L"");
                    const wchar_t* p = (const wchar_t*)(INT_PTR)
                        (long long)BNum(a[0]);
                    long long n = (long long)BNum(a[1]);
                    if (!p || n <= 0) return Value::Str_(L"");
                    return Value::Str_(wstring((size_t)n, L'\0')
                        .assign(p, p + n)); });
                // ---- M3-b: struct layout serialize/deserialize ----
                // serialize: table fields -> heap buffer; returns pointer
                tb->map[L"structPtr"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty()) return Value::Num_(0);
                    void* p = StructSerialize(a[0]);
                    return Value::Num_((double)(INT_PTR)p);
                });
                // deserialize: buffer -> table fields (in place)
                tb->map[L"structFrom"] = NF([](vector<Value>& a) -> Value {
                    if (a.size() < 2 || (a[0].t != Value::TableT && a[0].t != Value::Class))
                        return a.empty() ? Value() : a[0];
                    Value vTab = a[0];
                    void* p = a[1].t == Value::Pointer ? a[1].ptr
                        : (void*)(INT_PTR)(long long)BNum(a[1]);
                    StructDeserialize(vTab, p);
                    return vTab;
                });
                // byte size of a struct table
                tb->map[L"sizeOf"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty()) return Value::Num_(0);
                    if (a[0].t == Value::Str)
                        return Value::Num_((double)GetStructFieldSize(a[0].s, 1));
                    return Value::Num_((double)CalcStructTotalSize(a[0]));
                });
                tb->map[L"sizeof"] = tb->map[L"sizeOf"];
                tb->map[L"toPointer"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty() || a[0].t == Value::Null) return Value::Pointer_(nullptr);
                    if (a[0].t == Value::Pointer) return a[0];
                    if (a[0].t == Value::Num) return Value::Pointer_((void*)(INT_PTR)(long long)a[0].n);
                    if (a[0].t == Value::Str) return Value::Pointer_((void*)a[0].s.c_str());
                    return Value::Pointer_((void*)(INT_PTR)(long long)BNum(a[0])); });
                tb->map[L"convert"] =
                    NF([](vector<Value>& a) -> Value {
                    if (a.size() < 2) return a.empty() ? Value() : a[0];
                    const Value& from = a[0];
                    Value to = a[1];
                    long long off = a.size() > 2 ? (long long)BNum(a[2]) : 0;
                    if (to.t != Value::TableT || !to.tab) return to;

                    if (from.t == Value::TableT || from.t == Value::Class)
                    {
                        void* pSrc = StructSerialize(from);
                        if (pSrc)
                        {
                            StructDeserialize(to, (const char*)pSrc + off);
                            HeapFree(GetProcessHeap(), 0, pSrc);
                        }
                    }
                    else if (from.t == Value::Pointer)
                    {
                        if (from.ptr) StructDeserialize(to, (const char*)from.ptr + off);
                    }
                    else if (from.t == Value::Num)
                    {
                        void* p = (void*)(INT_PTR)(long long)from.n;
                        if (p) StructDeserialize(to, (const char*)p + off);
                    }
                    else if (from.t == Value::Str)
                    {
                        StructDeserialize(to, (const char*)from.s.data() + off);
                    }
                    return to;
                });
                // M5: preloaded system DLLs, mirroring builtin/_.aardio's
                // ::User32/::Kernel32 — api.aardio assumes they exist
                {
                    auto uDll = tb->map[L"loadDll"];
                    vector<Value> ua{ Value::Str_(L"User32.dll") };
                    if (uDll.native)
                    {
                        globals.vars[L"User32"] = uDll.native(ua);
                        roots[L"User32"] = globals.vars[L"User32"];
                    }
                    vector<Value> ka{ Value::Str_(L"Kernel32.dll") };
                    if (uDll.native)
                    {
                        globals.vars[L"Kernel32"] = uDll.native(ka);
                        roots[L"Kernel32"] = globals.vars[L"Kernel32"];
                    }
                    // win/_.aardio also calls ::Shell32 / ::Gdi32 /
                    // ::Comctl32 style roots - preload the common ones
                    const wchar_t* DLLS[] = { L"Shell32.dll", L"Gdi32.dll",
                        L"Ole32.dll", L"OleAut32.dll", L"Advapi32.dll",
                        L"Comctl32.dll", L"Comdlg32.dll", L"Uxtheme.dll",
                        L"Version.dll", L"Winmm.dll", L"Msimg32.dll",
                        L"Rpcrt4.dll", nullptr };
                    for (int di = 0; DLLS[di]; di++)
                    {
                        vector<Value> da{ Value::Str_(DLLS[di]) };
                        Value dv = uDll.native(da);
                        wstring nm(DLLS[di]);
                        nm = nm.substr(0, nm.size() - 4);
                        globals.vars[nm] = dv;
                        roots[nm] = dv;
                    }
                }

                // install the late-bound dispatch used by trampolines
                if (!g_cbDispatch)
                    g_cbDispatch = [](CbCtx* ctx,
                                      vector<Value>& args) -> __int64 {
                        Value r = ctx->interp->CallOwner(
                            ctx->fn, ctx->owner, args);
                        switch (ctx->sig.ret)
                        {
                        case ARDbl: return (__int64)BNum(r);
                        case ARBool: return BNum(r) != 0 ? 1 : 0;
                        case ARStrA:
                        case ARStrW:
                        default: return (__int64)BNum(r);
                        }
                    };
                // M3-c: callback(fn, "ret(p1,p2)", owner) -> function pointer
                // (number) that APIs can call; the VM function receives
                // the caller's arguments as numbers
                auto fnCallback =
                    NF([this](vector<Value>& a) -> Value {
                    if (a.size() < 2 ||
                        !(a[0].t == Value::Func))
                    {
                        (*err) =
                            L"运行错误: raw.callback(fn, sig, owner)";
                        return Value();
                    }
                    ApiSig sig;
                    if (!ParseApiSig(BStr(a[1]), &sig))
                    {
                        (*err) = L"运行错误: 回调签名无效: " +
                                 BStr(a[1]);
                        return Value();
                    }
                    CbCtx* ctx = new CbCtx();
                    ctx->fn = a[0];
                    if (a.size() >= 3)
                        ctx->owner = a[2];
                    ctx->sig = sig;
                    ctx->interp = this;
                    CallbackTrampoline(ctx);
                    g_cbs.push_back(ctx);
                    return Value::Num_(
                        (double)(__int64)(INT_PTR)ctx->code); });
                tb->map[L"callback"] = fnCallback;
                tb->map[L"tostdcall"] = fnCallback;
                tb->map[L"tocdecl"] = fnCallback;
                globals.vars[L"tostdcall"] = fnCallback;
                globals.vars[L"tocdecl"] = fnCallback;
            }
            // ---- table ----
            {
                auto tb = GetRoot(L"table").tab;
                // table.assign(target, src...) - copy key-values from
                // each source table into the target (metaProperty ctor)
                tb->map[L"assign"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty() || a[0].t != Value::TableT || !a[0].tab)
                        return Value();
                    for (size_t si = 1; si < a.size(); si++)
                    {
                        if (a[si].t != Value::TableT || !a[si].tab)
                            continue;
                        for (auto& kv : a[si].tab->map)
                        {
                            if (kv.second.t != Value::Null)
                                a[0].tab->map[kv.first] = kv.second;
                            else
                                a[0].tab->map.erase(kv.first);
                        }
                        for (size_t ai = 0; ai < a[si].tab->arr.size(); ai++)
                            a[0].tab->arr.push_back(a[si].tab->arr[ai]);
                    }
                    return a[0];
                });
                tb->map[L"count"] = NF([](vector<Value>& a) {
                    if (a[0].t != Value::TableT || !a[0].tab) return Value::Num_(0);
                    return Value::Num_((double)(a[0].tab->arr.size() +
                                                a[0].tab->map.size())); });
                auto fnIsArray = NF([](vector<Value>& a) -> Value {
                    if (a.empty() || a[0].t != Value::TableT || !a[0].tab)
                        return Value::Bool_(false);
                    return Value::Bool_(!a[0].tab->arr.empty());
                });
                tb->map[L"isArray"] = fnIsArray;
                tb->map[L"isArrayLike"] = fnIsArray;
                globals.vars[L"isArray"] = fnIsArray;
                globals.vars[L"isArrayLike"] = fnIsArray;
                tb->map[L"append"] = NF([](vector<Value>& a) {
                    if (a[0].t == Value::TableT && a[0].tab)
                        a[0].tab->arr.push_back(a.size() > 1 ? a[1] : Value());
                    return a[0]; });
                tb->map[L"gc"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty()) return Value();
                    if (a.size() > 1 && a[0].t == Value::TableT && a[0].tab)
                        a[0].tab->map[L"_gc"] = a[1];
                    return a[0]; });
                tb->map[L"add_gc"] = tb->map[L"gc"];
                auto fnPush = NF([](vector<Value>& a) -> Value {
                    if (a.empty() || a[0].t != Value::TableT || !a[0].tab)
                        return Value::Num_(0);
                    Table* t = a[0].tab.get();
                    for (size_t i = 1; i < a.size(); i++)
                        t->arr.push_back(a[i]);
                    return Value::Num_((double)t->arr.size());
                });
                tb->map[L"push"] = fnPush;
                globals.vars[L"push"] = fnPush;

                auto fnPop = NF([](vector<Value>& a) -> Value {
                    if (a.empty() || a[0].t != Value::TableT || !a[0].tab || a[0].tab->arr.empty())
                        return Value();
                    Table* t = a[0].tab.get();
                    Value v = t->arr.back();
                    t->arr.pop_back();
                    return v;
                });
                tb->map[L"pop"] = fnPop;
                globals.vars[L"pop"] = fnPop;

                auto fnSplice = NF([](vector<Value>& a) -> Value {
                    auto res = std::make_shared<Table>();
                    if (a.empty() || a[0].t != Value::TableT || !a[0].tab)
                        return Value::Table_(res);
                    Table* t = a[0].tab.get();
                    long long idx = a.size() > 1 ? (long long)BNum(a[1]) : 1;
                    if (idx < 1) idx = (long long)t->arr.size() + idx + 1;
                    if (idx < 1) idx = 1;
                    size_t start = (size_t)idx - 1;
                    if (start > t->arr.size()) start = t->arr.size();
                    long long cnt = a.size() > 2 ? (long long)BNum(a[2]) : ((long long)t->arr.size() - (long long)start);
                    if (cnt < 0) cnt = 0;
                    size_t count = (size_t)cnt;
                    if (start + count > t->arr.size()) count = t->arr.size() - start;
                    for (size_t k = 0; k < count; k++)
                        res->arr.push_back(t->arr[start + k]);
                    t->arr.erase(t->arr.begin() + start, t->arr.begin() + start + count);
                    for (size_t k = 3; k < a.size(); k++)
                        t->arr.insert(t->arr.begin() + start + (k - 3), a[k]);
                    return Value::Table_(res);
                });
                tb->map[L"splice"] = fnSplice;
                globals.vars[L"splice"] = fnSplice;
                // ---- round 43: array ops ----
                tb->map[L"insert"] = NF([](vector<Value>& a) -> Value {
                    if (a.size() < 2 || a[0].t != Value::TableT || !a[0].tab)
                        return a.size() > 0 ? a[0] : Value();
                    Table* t = a[0].tab.get();
                    if (a.size() >= 3)
                    {
                        long long i = (long long)BNum(a[1]);
                        if (i < 1) i = 1;
                        if (i > (long long)t->arr.size() + 1)
                            i = (long long)t->arr.size() + 1;
                        t->arr.insert(t->arr.begin() + (i - 1), a[2]);
                    }
                    else
                        t->arr.push_back(a[1]);
                    return a[0]; });
                tb->map[L"remove"] = NF([](vector<Value>& a) -> Value {
                    if (a.size() < 2 || a[0].t != Value::TableT || !a[0].tab)
                        return Value();
                    Table* t = a[0].tab.get();
                    long long i = (long long)BNum(a[1]);
                    if (i < 1 || i > (long long)t->arr.size())
                        return Value();
                    Value v = t->arr[(size_t)i - 1];
                    t->arr.erase(t->arr.begin() + (i - 1));
                    return v; });
                tb->map[L"reverse"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty() || a[0].t != Value::TableT || !a[0].tab)
                        return a.empty() ? Value() : a[0];
                    std::reverse(a[0].tab->arr.begin(),
                                 a[0].tab->arr.end());
                    return a[0]; });
                tb->map[L"concat"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty() || a[0].t != Value::TableT || !a[0].tab)
                        return Value::Str_(L"");
                    wstring sep = a.size() > 1 ? BStr(a[1]) : L"";
                    wstring out;
                    auto& arr = a[0].tab->arr;
                    for (size_t k = 0; k < arr.size(); k++)
                    {
                        if (k) out += sep;
                        out += arr[k].ToString();
                    }
                    return Value::Str_(out); });
                tb->map[L"find"] = NF([](vector<Value>& a) -> Value {
                    if (a.size() < 2 || a[0].t != Value::TableT || !a[0].tab)
                        return Value();
                    auto& arr = a[0].tab->arr;
                    const Value& want = a[1];
                    for (size_t k = 0; k < arr.size(); k++)
                    {
                        const Value& v = arr[k];
                        bool eq;
                        if (v.t == Value::TableT || want.t == Value::TableT)
                            eq = v.tab == want.tab;   // identity
                        else
                            eq = v.ToString() == want.ToString() &&
                                 v.t == want.t;
                        if (eq) return Value::Num_((double)(k + 1));
                    }
                    return Value(); });
                // sort: default compares numbers numerically, everything
                // else by ToString; a custom comparator (function value)
                // is invoked re-entrantly and must return truthy when the
                // first argument belongs before the second (Lua/aardio)
                tb->map[L"sort"] = NF([this](vector<Value>& a) -> Value {
                    if (a.empty() || a[0].t != Value::TableT || !a[0].tab)
                        return a.empty() ? Value() : a[0];
                    std::vector<Value>* pArr = &a[0].tab->arr;
                    Value cmp = a.size() > 1 ? a[1] : Value();
                    // shell sort — no recursion, stable enough for VM use
                    size_t n = pArr->size();
                    size_t gap = 1;
                    while (gap < n / 3) gap = gap * 3 + 1;
                    auto before = [this, &cmp](const Value& x,
                                               const Value& y) -> bool {
                        if (cmp.t == Value::Func || cmp.native)
                        {
                            vector<Value> args{ x, y };
                            Value r = CallVmFunc(cmp, args);
                            return r.Truthy();
                        }
                        if (x.t == Value::Num && y.t == Value::Num)
                            return x.n < y.n;
                        return x.ToString() < y.ToString(); };
                    while (gap > 0)
                    {
                        for (size_t i = gap; i < n; i++)
                        {
                            Value tmp = (*pArr)[i];
                            size_t j = i;
                            while (j >= gap && before(tmp, (*pArr)[j - gap]))
                            {
                                (*pArr)[j] = (*pArr)[j - gap];
                                j -= gap;
                            }
                            (*pArr)[j] = tmp;
                        }
                        gap /= 3;
                    }
                    return a[0]; });
                tb->map[L"cache"] = NF([](vector<Value>& a) -> Value {
                    auto tbRes = std::make_shared<Table>();
                    auto meta = std::make_shared<Table>();
                    meta->map[L"_weak"] = Value::Str_(L"kv");
                    if (a.size() >= 2 && (a[1].t == Value::Func || a[1].native))
                        meta->map[L"_get"] = a[1];
                    else if (a.size() >= 1 && (a[0].t == Value::Func || a[0].native))
                        meta->map[L"_get"] = a[0];
                    tbRes->map[L"_meta"] = Value::Table_(meta);
                    return Value::Table_(tbRes); });
                tb->map[L"mix"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty()) return Value();
                    Value target = a[0];
                    if (target.t != Value::TableT || !target.tab)
                    {
                        target = Value::Table_(std::make_shared<Table>());
                    }
                    for (size_t i = 1; i < a.size(); i++)
                    {
                        if (a[i].t != Value::TableT || !a[i].tab) continue;
                        for (auto& kv : a[i].tab->map)
                        {
                            if (target.tab->map.find(kv.first) == target.tab->map.end())
                                target.tab->map[kv.first] = kv.second;
                        }
                        for (auto& v : a[i].tab->arr)
                            target.tab->arr.push_back(v);
                    }
                    return target; });
            }
            // ---- round 36: web (WinINet HTTP) ----
            {
                auto tb = GetRoot(L"web").tab;
                tb->map[L"get"] = NF([this](vector<Value>& a) -> Value {
                    wstring strErr2;
                    wstring r = HttpFetch(BStr(a[0]), L"GET", L"", L"",
                                          &strErr2);
                    if (!strErr2.empty())
                    {
                        (*err) = L"运行错误: web.get: " + strErr2;
                        return Value();
                    }
                    return Value::Str_(r); });
                tb->map[L"post"] = NF([this](vector<Value>& a) -> Value {
                    wstring strErr2;
                    wstring strCType = a.size() > 2
                        ? BStr(a[2])
                        : L"application/x-www-form-urlencoded";
                    wstring r = HttpFetch(BStr(a[0]), L"POST",
                                          a.size() > 1 ? BStr(a[1]) : L"",
                                          strCType, &strErr2);
                    if (!strErr2.empty())
                    {
                        (*err) = L"运行错误: web.post: " + strErr2;
                        return Value();
                    }
                    return Value::Str_(r); });
                tb->map[L"download"] = NF([this](vector<Value>& a) -> Value {
                    wstring strErr2;
                    wstring r = HttpFetch(BStr(a[0]), L"GET", L"", L"",
                                          &strErr2);
                    if (!strErr2.empty())
                    {
                        (*err) = L"运行错误: web.download: " + strErr2;
                        return Value::Bool_(false);
                    }
                    wstring strFile = a.size() > 1 ? BStr(a[1]) : L"web.bin";
                    FILE* pf = nullptr;
                    if (_wfopen_s(&pf, strFile.c_str(), L"wb") != 0 || !pf)
                    {
                        (*err) = L"运行错误: web.download: 无法写文件";
                        return Value::Bool_(false);
                    }
                    int nU8 = WideCharToMultiByte(CP_UTF8, 0, r.c_str(),
                                                  (int)r.size(), nullptr, 0,
                                                  nullptr, nullptr);
                    if (nU8 > 0)
                    {
                        std::vector<char> buf8((size_t)nU8);
                        WideCharToMultiByte(CP_UTF8, 0, r.c_str(), (int)r.size(),
                                            buf8.data(), nU8, nullptr,
                                            nullptr);
                        fwrite(buf8.data(), 1, (size_t)nU8, pf);
                    }
                    fclose(pf);
                    return Value::Bool_(true); });
                tb->map[L"encode"] = NF([](vector<Value>& a) -> Value {
                    wstring s = BStr(a[0]);
                    int nU8 = WideCharToMultiByte(CP_UTF8, 0, s.c_str(),
                                                  (int)s.size(), nullptr, 0,
                                                  nullptr, nullptr);
                    std::string str8((size_t)max(nU8, 0), 0);
                    if (nU8 > 0)
                        WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(),
                                            &str8[0], nU8, nullptr, nullptr);
                    wstring out;
                    const wchar_t* hex = L"0123456789ABCDEF";
                    for (unsigned char c : str8)
                    {
                        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                            (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                            c == '.' || c == '~')
                            out += (wchar_t)c;
                        else
                        {
                            out += L'%';
                            out += hex[c >> 4];
                            out += hex[c & 0xF];
                        }
                    }
                    return Value::Str_(out); });
                tb->map[L"decode"] = NF([](vector<Value>& a) -> Value {
                    wstring s = BStr(a[0]);
                    std::string str8;
                    for (size_t i = 0; i < s.size(); i++)
                    {
                        if (s[i] == L'%' && i + 2 < s.size() + 1 &&
                            i + 2 < s.size() + 1)
                        {
                            wchar_t h1 = s[i + 1], h2 = s[i + 2];
                            auto hv = [](wchar_t c) -> int {
                                if (c >= L'0' && c <= L'9') return c - L'0';
                                if (c >= L'A' && c <= L'F') return c - L'A' + 10;
                                if (c >= L'a' && c <= L'f') return c - L'a' + 10;
                                return -1; };
                            int a1 = hv(h1), a2 = hv(h2);
                            if (a1 >= 0 && a2 >= 0)
                            {
                                str8.push_back((char)((a1 << 4) | a2));
                                i += 2;
                                continue;
                            }
                        }
                        if (s[i] == L'+')
                            str8.push_back(' ');   // form-urlencoded space
                        else
                        {
                            int nU8 = WideCharToMultiByte(CP_UTF8, 0, &s[i],
                                                          1, nullptr, 0,
                                                          nullptr, nullptr);
                            std::string one((size_t)max(nU8, 0), 0);
                            if (nU8 > 0)
                                WideCharToMultiByte(CP_UTF8, 0, &s[i], 1,
                                                    &one[0], nU8, nullptr,
                                                    nullptr);
                            str8 += one;
                        }
                    }
                    int w = MultiByteToWideChar(CP_UTF8, 0, str8.data(),
                                                (int)str8.size(), nullptr, 0);
                    if (w <= 0)
                        return Value::Str_(s);
                    wstring out((size_t)w, 0);
                    MultiByteToWideChar(CP_UTF8, 0, str8.data(),
                                        (int)str8.size(), &out[0], w);
                    return Value::Str_(out); });
            }

            // ---- round 36: thread (worker threads, own VM per thread) ----
            {
                auto tb = GetRoot(L"thread").tab;
                // thread.applyCallWnd(form, wParam, lParam) - forward
                // a WM_THREAD_CALLWND payload to the owning window
                // (VM builtin in the original; residual stub here)
                tb->map[L"applyCallWnd"] = NF([](vector<Value>& a) -> Value {
                    return Value();
                });
                tb->map[L"getId"] = NF([](vector<Value>&) -> Value {
                    return Value::Num_((double)::GetCurrentThreadId());
                });
                tb->map[L"get"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty()) return Value();
                    std::lock_guard<std::mutex> lock(g_threadSharedMutex);
                    auto it = g_threadSharedMap.find(BStr(a[0]));
                    if (it != g_threadSharedMap.end()) return it->second;
                    return Value();
                });
                tb->map[L"set"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty()) return Value();
                    std::lock_guard<std::mutex> lock(g_threadSharedMutex);
                    if (a.size() > 1 && a[1].t != Value::Null)
                        g_threadSharedMap[BStr(a[0])] = a[1];
                    else
                        g_threadSharedMap.erase(BStr(a[0]));
                    return Value();
                });
                tb->map[L"getWnd"] = NF([](vector<Value>&) -> Value {
                    return Value();
                });
                tb->map[L"setWnd"] = NF([](vector<Value>&) -> Value {
                    return Value();
                });
                tb->map[L"callWnd"] = NF([](vector<Value>&) -> Value {
                    return Value();
                });
                globals.vars[L"__waitForMultipleObjects"] = NF([](vector<Value>& a) -> Value {
                    return Value::Num_(0);
                });
                globals.vars[L"__messagePeek"] = NF([](vector<Value>& a) -> Value {
                    return Value::Bool_(false);
                });
                tb->map[L"create"] = NF([this](vector<Value>& a) -> Value {
                    if (a.empty() || a[0].t != Value::Func || !a[0].fn)
                    {
                        (*err) = L"运行错误: thread.create 需要函数字面值";
                        return Value();
                    }
                    PNode fnBody = a[0].fn;
                    vector<wstring> params = a[0].params;
                    vector<Value> args;
                    for (size_t i = 1; i < a.size(); i++)
                        args.push_back(DeepCopy(a[i]));
                    auto st = std::make_shared<ThreadState>();
                    st->th = std::thread(
                        [fnBody, params, args, st]() {
                            wstring threadErr;
                            Interp wt{ &threadErr };
                            g_interpCurrent = &wt;
                            wt.InitBuiltins();
                            Env local;
                            local.parent = &wt.globals;
                            for (size_t i = 0; i < params.size(); i++)
                                local.vars[params[i]] =
                                    i < args.size() ? args[i] : Value();
                            wt.nReturn = 0;
                            Value r = wt.ExecBlock(fnBody, &local);
                            if (threadErr.empty())
                                st->result = DeepCopy(r);
                            else
                                st->error = threadErr;
                            st->done = true;
                        });
                    st->th.detach();
                    auto h = std::make_shared<Table>();
                    h->map[L"join"] = NF([st, this](vector<Value>&) -> Value {
                        while (!st->done)
                            Sleep(5);
                        if (!st->error.empty())
                        {
                            (*err) = L"运行错误: 线程内错误: " + st->error;
                            return Value();
                        }
                        return st->result; });
                    h->map[L"done"] = NF([st](vector<Value>&) -> Value {
                        return Value::Bool_(st->done); });
                    return Value::Table_(h); });
            }

            // ---- round 37: fsys (file system) ----
            {
                auto tb = GetRoot(L"fsys").tab;
                tb->map[L"exists"] = NF([](vector<Value>& a) -> Value {
                    return Value::Bool_(
                        GetFileAttributesW(BStr(a[0]).c_str()) !=
                        INVALID_FILE_ATTRIBUTES); });
                tb->map[L"isDir"] = NF([](vector<Value>& a) -> Value {
                    DWORD at = GetFileAttributesW(BStr(a[0]).c_str());
                    return Value::Bool_(at != INVALID_FILE_ATTRIBUTES &&
                                        (at & FILE_ATTRIBUTE_DIRECTORY)); });
                tb->map[L"createDir"] = NF([](vector<Value>& a) -> Value {
                    // recursive: create each path segment in turn
                    wstring strPath = BStr(a[0]);
                    if (strPath.empty())
                        return Value::Bool_(false);
                    DWORD at = GetFileAttributesW(strPath.c_str());
                    if (at != INVALID_FILE_ATTRIBUTES)
                        return Value::Bool_(at & FILE_ATTRIBUTE_DIRECTORY);
                    for (size_t i = 3; i < strPath.size(); i++)
                    {
                        if (strPath[i] != L'\\' && strPath[i] != L'/')
                            continue;
                        wstring part = strPath.substr(0, i);
                        if (!part.empty() && part[part.size() - 1] != L':')
                            CreateDirectoryW(part.c_str(), nullptr);
                    }
                    return Value::Bool_(CreateDirectoryW(strPath.c_str(),
                                                         nullptr) ||
                        GetFileAttributesW(strPath.c_str()) !=
                            INVALID_FILE_ATTRIBUTES); });
                tb->map[L"delete"] = NF([](vector<Value>& a) -> Value {
                    // recursive delete (files then deepest-first directories)
                    std::function<bool(const wstring&)> del =
                        [&](const wstring& strDir) -> bool {
                        WIN32_FIND_DATAW fd;
                        HANDLE h = FindFirstFileW(
                            (strDir + L"\\*").c_str(), &fd);
                        if (h != INVALID_HANDLE_VALUE)
                        {
                            do
                            {
                                if (!wcscmp(fd.cFileName, L".") ||
                                    !wcscmp(fd.cFileName, L".."))
                                    continue;
                                wstring strChild =
                                    strDir + L"\\" + fd.cFileName;
                                if (fd.dwFileAttributes &
                                    FILE_ATTRIBUTE_DIRECTORY)
                                    del(strChild);
                                else
                                    DeleteFileW(strChild.c_str());
                            } while (FindNextFileW(h, &fd));
                            FindClose(h);
                        }
                        return RemoveDirectoryW(strDir.c_str()) ||
                            DeleteFileW(strDir.c_str()) ||
                            GetFileAttributesW(strDir.c_str()) ==
                                INVALID_FILE_ATTRIBUTES;
                    };
                    return Value::Bool_(del(BStr(a[0]))); });
                tb->map[L"copy"] = NF([](vector<Value>& a) -> Value {
                    return Value::Bool_(CopyFileW(BStr(a[0]).c_str(),
                        BStr(a[1]).c_str(), FALSE)); });
                tb->map[L"move"] = NF([](vector<Value>& a) -> Value {
                    return Value::Bool_(MoveFileW(BStr(a[0]).c_str(),
                        BStr(a[1]).c_str())); });
                tb->map[L"size"] = NF([](vector<Value>& a) -> Value {
                    WIN32_FIND_DATAW fd;
                    HANDLE h = FindFirstFileW(BStr(a[0]).c_str(), &fd);
                    if (h == INVALID_HANDLE_VALUE)
                        return Value::Num_(0);
                    FindClose(h);
                    ULONGLONG n = ((ULONGLONG)fd.nFileSizeHigh << 32) |
                                  fd.nFileSizeLow;
                    return Value::Num_((double)n); });
                tb->map[L"list"] = NF([](vector<Value>& a) -> Value {
                    // array of {name, dir, size}; "." and ".." skipped
                    auto out = std::make_shared<Table>();
                    WIN32_FIND_DATAW fd;
                    HANDLE h = FindFirstFileW(
                        (BStr(a[0]) + L"\\*").c_str(), &fd);
                    if (h == INVALID_HANDLE_VALUE)
                        return Value::Table_(out);
                    do
                    {
                        if (!wcscmp(fd.cFileName, L".") ||
                            !wcscmp(fd.cFileName, L".."))
                            continue;
                        auto it = std::make_shared<Table>();
                        it->map[L"name"] = Value::Str_(fd.cFileName);
                        it->map[L"dir"] = Value::Bool_(
                            fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
                        ULONGLONG n = ((ULONGLONG)fd.nFileSizeHigh << 32) |
                                      fd.nFileSizeLow;
                        it->map[L"size"] = Value::Num_((double)n);
                        out->arr.push_back(Value::Table_(it));
                    } while (FindNextFileW(h, &fd));
                    FindClose(h);
                    return Value::Table_(out); });
                tb->map[L"tmpDir"] = NF([](vector<Value>&) -> Value {
                    wchar_t szPath[MAX_PATH + 1] = L"";
                    DWORD n = GetTempPathW(MAX_PATH, szPath);
                    wstring s(n ? szPath : L"");
                    if (!s.empty() && s[s.size() - 1] != L'\\')
                        s += L"\\";
                    return Value::Str_(s); });
            }

            // ---- round 37: process (launch + run with output) ----
            {
                auto tb = GetRoot(L"process").tab;
                tb->map[L"execute"] = NF([](vector<Value>& a) -> Value {
                    // ShellExecute: launch, do not wait
                    wstring strParams = a.size() > 1 ? BStr(a[1]) : L"";
                    HINSTANCE h = ShellExecuteW(nullptr, L"open",
                        BStr(a[0]).c_str(),
                        strParams.empty() ? nullptr : strParams.c_str(),
                        nullptr, SW_SHOWNORMAL);
                    return Value::Bool_((INT_PTR)h > 32); });
                tb->map[L"run"] = NF([](vector<Value>& a) -> Value {
                    // run a command line, wait, capture merged output
                    auto out = std::make_shared<Table>();
                    out->map[L"out"] = Value::Str_(L"");
                    out->map[L"code"] = Value::Num_(-1);
                    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
                    HANDLE hRead = nullptr, hWrite = nullptr;
                    if (!CreatePipe(&hRead, &hWrite, &sa, 0))
                        return Value::Table_(out);
                    STARTUPINFOW si = { sizeof(si) };
                    si.dwFlags = STARTF_USESTDHANDLES;
                    si.hStdOutput = hWrite;
                    si.hStdError = hWrite;
                    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
                    wstring strCmd = BStr(a[0]);
                    vector<wchar_t> cmd(strCmd.begin(), strCmd.end());
                    cmd.push_back(L'\0');
                    PROCESS_INFORMATION pi = {};
                    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr,
                        nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                        &si, &pi);
                    CloseHandle(hWrite);   // child holds the write end now
                    if (!ok)
                    {
                        CloseHandle(hRead);
                        return Value::Table_(out);
                    }
                    std::string str8;
                    char buf[4096];
                    DWORD n = 0;
                    while (ReadFile(hRead, buf, sizeof(buf), &n, nullptr) &&
                           n > 0)
                        str8.append(buf, n);
                    CloseHandle(hRead);
                    WaitForSingleObject(pi.hProcess, 15000);
                    DWORD code = 0;
                    GetExitCodeProcess(pi.hProcess, &code);
                    CloseHandle(pi.hThread);
                    CloseHandle(pi.hProcess);
                    out->map[L"code"] = Value::Num_((double)code);
                    int w = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                        str8.data(), (int)str8.size(), nullptr, 0);
                    if (w <= 0)
                        w = MultiByteToWideChar(CP_ACP, 0, str8.data(),
                                                (int)str8.size(), nullptr, 0);
                    if (w > 0)
                    {
                        wstring s((size_t)w, 0);
                        MultiByteToWideChar(CP_UTF8, 0, str8.data(),
                                            (int)str8.size(), &s[0], w);
                        out->map[L"out"] = Value::Str_(s);
                    }
                    return Value::Table_(out); });
            }

    // ---- round 38: win.form — minimal windowing ----
            {
                auto tb = GetRoot(L"win").tab;
                tb->map[L"form"] = NF([this](vector<Value>& a) -> Value {
                    if (!RegisterFormClass())
                    {
                        (*err) = L"运行错误: win.form 窗口类注册失败";
                        return Value();
                    }
                    wstring strText = a.size() > 0 ? BStr(a[0]) : L"窗体";
                    int nW = a.size() > 1 ? (int)BNum(a[1]) : 600;
                    int nH = a.size() > 2 ? (int)BNum(a[2]) : 400;
                    FormState* fs = new FormState();
                    fs->it = this;
                    HWND hwnd = CreateWindowExW(
                        0, L"AardioRE_form", strText.c_str(),
                        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                        nW, nH, nullptr, nullptr, GetModuleHandle(nullptr),
                        nullptr);
                    if (!hwnd)
                    {
                        delete fs;
                        (*err) = L"运行错误: win.form 窗口创建失败";
                        return Value();
                    }
                    g_forms[hwnd] = fs;

                    auto form = std::make_shared<Table>();

                    HFONT hGui = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
                    SendMessageW(hwnd, WM_SETFONT, (WPARAM)hGui, TRUE);

                    form->map[L"text"] = NF([hwnd](vector<Value>&) -> Value {
                        wchar_t buf[512] = L"";
                        GetWindowTextW(hwnd, buf, 512);
                        return Value::Str_(buf); });
                    form->map[L"setText"] = NF([hwnd](vector<Value>& a2) -> Value {
                        SetWindowTextW(hwnd, BStr(a2[0]).c_str());
                        return Value(); });
                    form->map[L"hwnd"] = Value::Num_((double)(INT_PTR)hwnd);
                    form->map[L"show"] = NF([hwnd](vector<Value>&) -> Value {
                        ShowWindow(hwnd, SW_SHOW);
                        UpdateWindow(hwnd);
                        return Value(); });
                    form->map[L"doModal"] = NF([fs, hwnd](vector<Value>&)
                        -> Value {
                        ShowWindow(hwnd, SW_SHOW);
                        UpdateWindow(hwnd);
                        MSG msg;
                        while (!fs->bModalDone && GetMessageW(&msg, nullptr,
                                                               0, 0) > 0)
                        {
                            TranslateMessage(&msg);
                            DispatchMessageW(&msg);
                        }
                        return Value::Num_((double)fs->nModalResult); });
                    form->map[L"endModal"] = NF([fs, hwnd](vector<Value>& a2)
                        -> Value {
                        fs->nModalResult = a2.empty() ? 0 : (int)BNum(a2[0]);
                        fs->bModalDone = true;
                        PostMessageW(hwnd, WM_QUIT, 0, 0);
                        return Value(); });
                    form->map[L"close"] = NF([hwnd](vector<Value>&) -> Value {
                        PostMessageW(hwnd, WM_CLOSE, 0, 0);
                        return Value(); });
                    // round 40: paint trigger + timers (ontimer(tid) member)
                    fs->formVal = Value::Table_(form);
                    form->map[L"invalidate"] =
                        NF([fs, hwnd](vector<Value>&) -> Value {
                        InvalidateRect(hwnd, nullptr, TRUE);
                        if (!fs->bInPaint)
                            UpdateWindow(hwnd);   // synchronous repaint
                        return Value(); });
                    form->map[L"setInterval"] =
                        NF([fs, hwnd](vector<Value>& a2) -> Value {
                        int nId = fs->nNextTimer++;
                        UINT ms = a2.empty() ? 1000 : (UINT)BNum(a2[0]);
                        SetTimer(hwnd, (UINT_PTR)nId, ms, nullptr);
                        return Value::Num_((double)nId); });
                    form->map[L"killTimer"] =
                        NF([fs, hwnd](vector<Value>& a2) -> Value {
                        if (!a2.empty())
                            KillTimer(hwnd, (UINT_PTR)(int)BNum(a2[0]));
                        return Value(); });
                    // round 41: menu bar — addMenu(text) -> popup table;
                    // popup.add(text) -> item table with .oncommand slot
                    form->map[L"addMenu"] =
                        NF([fs, hwnd, form, this](vector<Value>& a2)
                            -> Value {
                        wstring strText = a2.empty() ? L"菜单" : BStr(a2[0]);
                        if (!fs->hMenuBar)
                        {
                            fs->hMenuBar = CreateMenu();
                            SetMenu(hwnd, fs->hMenuBar);
                        }
                        HMENU hPopup = CreatePopupMenu();
                        AppendMenuW(fs->hMenuBar, MF_POPUP,
                            (UINT_PTR)hPopup, strText.c_str());
                        DrawMenuBar(hwnd);
                        auto popup = std::make_shared<Table>();
                        // popup.add(text) -> item table
                        popup->map[L"add"] = NF([fs, hPopup, hwnd, this](
                            vector<Value>& a3) -> Value {
                            wstring s = a3.empty() ? L"" : BStr(a3[0]);
                            int nMid = fs->nNextMenuId++;
                            AppendMenuW(hPopup, MF_STRING,
                                (UINT_PTR)nMid, s.c_str());
                            auto item = std::make_shared<Table>();
                            item->map[L"text"] = NF([s](
                                vector<Value>&) -> Value {
                                return Value::Str_(s); });
                            item->map[L"oncommand"] = Value();
                            item->map[L"click"] = NF([hwnd, nMid](
                                vector<Value>&) -> Value {
                                PostMessageW(hwnd, WM_COMMAND,
                                    MAKEWPARAM(nMid, 0), 0);
                                return Value(); });
                            item->map[L"enable"] = NF([fs, nMid](
                                vector<Value>& a4) -> Value {
                                EnableMenuItem(fs->hMenuBar, nMid,
                                    MF_BYCOMMAND |
                                        (!a4.empty() && BNum(a4[0]) != 0
                                             ? MF_ENABLED
                                             : MF_GRAYED));
                                return Value(); });
                            item->map[L"check"] = NF([fs, nMid](
                                vector<Value>& a4) -> Value {
                                CheckMenuItem(fs->hMenuBar, nMid,
                                    MF_BYCOMMAND |
                                        (!a4.empty() && BNum(a4[0]) != 0
                                             ? MF_CHECKED
                                             : MF_UNCHECKED));
                                return Value(); });
                            fs->menuItems[nMid] = Value::Table_(item);
                            fs->menus[nMid] = NF([fs, nMid, this](
                                vector<Value>&) -> Value {
                                auto ii = fs->menuItems.find(nMid);
                                if (ii == fs->menuItems.end() ||
                                    !ii->second.tab)
                                    return Value();
                                auto ib = ii->second.tab->map.find(
                                    L"oncommand");
                                if (ib == ii->second.tab->map.end())
                                    return Value();
                                vector<Value> noArgs;
                                return this->CallVmFunc(ib->second,
                                                        noArgs); });
                            return Value::Table_(item); });
                        // popup.sep() -> separator
                        popup->map[L"sep"] = NF([hPopup](
                            vector<Value>&) -> Value {
                            AppendMenuW(hPopup, MF_SEPARATOR, 0, nullptr);
                            return Value(); });
                        // popup.addMenu(text) -> nested popup
                        popup->map[L"addMenu"] =
                            NF([fs, hPopup, hwnd, this](
                                vector<Value>& a3) -> Value {
                            wstring s = a3.empty() ? L"" : BStr(a3[0]);
                            HMENU hSub = CreatePopupMenu();
                            AppendMenuW(hPopup, MF_POPUP,
                                (UINT_PTR)hSub, s.c_str());
                            // reuse the same item-building closure shape:
                            // return a table with add/sep bound to hSub
                            auto sub = std::make_shared<Table>();
                            sub->map[L"add"] = NF([fs, hSub, hwnd, this](
                                vector<Value>& a4) -> Value {
                                wstring s2 = a4.empty() ? L"" : BStr(a4[0]);
                                int nMid = fs->nNextMenuId++;
                                AppendMenuW(hSub, MF_STRING,
                                    (UINT_PTR)nMid, s2.c_str());
                                auto item2 = std::make_shared<Table>();
                                item2->map[L"text"] = NF([s2](
                                    vector<Value>&) -> Value {
                                    return Value::Str_(s2); });
                                item2->map[L"oncommand"] = Value();
                                item2->map[L"click"] = NF([hwnd, nMid](
                                    vector<Value>&) -> Value {
                                    PostMessageW(hwnd, WM_COMMAND,
                                        MAKEWPARAM(nMid, 0), 0);
                                    return Value(); });
                                fs->menuItems[nMid] = Value::Table_(item2);
                                fs->menus[nMid] = NF([fs, nMid, this](
                                    vector<Value>&) -> Value {
                                    auto ii = fs->menuItems.find(nMid);
                                    if (ii == fs->menuItems.end() ||
                                        !ii->second.tab)
                                        return Value();
                                    auto ib = ii->second.tab->map.find(
                                        L"oncommand");
                                    if (ib == ii->second.tab->map.end())
                                        return Value();
                                    vector<Value> noArgs;
                                    return this->CallVmFunc(ib->second,
                                                            noArgs); });
                                return Value::Table_(item2); });
                            sub->map[L"sep"] = NF([hSub](
                                vector<Value>&) -> Value {
                                AppendMenuW(hSub, MF_SEPARATOR, 0, nullptr);
                                return Value(); });
                            return Value::Table_(sub); });
                        return Value::Table_(popup); });
                    form->map[L"add"] = NF([form, fs, hwnd, this, hGui](
                        vector<Value>& a2) -> Value {
                        if (a2.size() < 7)
                        {
                            (*err) = L"运行错误: form.add(name, cls, text, "
                                     L"x, y, w, h)";
                            return Value();
                        }
                        wstring strName = BStr(a2[0]);
                        wstring strCls = BStr(a2[1]);
                        const wchar_t* szCls = nullptr;
                        DWORD dwCtlStyle = 0;
                        if (strCls == L"button") szCls = L"BUTTON";
                        else if (strCls == L"edit") szCls = L"EDIT";
                        else if (strCls == L"static") szCls = L"STATIC";
                        else if (strCls == L"checkbox")
                        {
                            szCls = L"BUTTON";
                            dwCtlStyle = BS_AUTOCHECKBOX;
                        }
                        else if (strCls == L"radio")
                        {
                            szCls = L"BUTTON";
                            dwCtlStyle = BS_AUTORADIOBUTTON;
                        }
                        else if (strCls == L"groupbox")
                        {
                            szCls = L"BUTTON";
                            dwCtlStyle = BS_GROUPBOX;
                        }
                        else if (strCls == L"listbox")
                        {
                            szCls = L"LISTBOX";
                            dwCtlStyle = LBS_NOTIFY |
                                LBS_NOINTEGRALHEIGHT |
                                WS_VSCROLL | WS_BORDER;
                        }
                        else if (strCls == L"combobox")
                        {
                            szCls = L"COMBOBOX";
                            dwCtlStyle = CBS_DROPDOWNLIST |
                                WS_VSCROLL;
                        }
                        if (!szCls)
                        {
                            (*err) = L"运行错误: 未知控件类型: " + strCls;
                            return Value();
                        }
                        int nId = fs->nNextId++;
                        HWND hC = CreateWindowExW(0, szCls,
                            BStr(a2[2]).c_str(),
                            WS_CHILD | WS_VISIBLE | dwCtlStyle |
                                (strCls == L"button" ? BS_PUSHBUTTON :
                                 strCls == L"edit" ?
                                     ES_LEFT | WS_BORDER : 0),
                            (int)BNum(a2[3]), (int)BNum(a2[4]),
                            (int)BNum(a2[5]), (int)BNum(a2[6]),
                            hwnd, (HMENU)(INT_PTR)nId,
                            GetModuleHandle(nullptr), nullptr);
                        if (!hC)
                        {
                            (*err) = L"运行错误: 控件创建失败";
                            return Value();
                        }
                        SendMessageW(hC, WM_SETFONT, (WPARAM)hGui, TRUE);
                        fs->hCtrl[nId] = hC;
                        auto ctrl = std::make_shared<Table>();
                        if (strCls == L"checkbox" || strCls == L"radio")
                        {
                            ctrl->map[L"checked"] = NF([hC](vector<Value>&)
                                -> Value {
                                return Value::Bool_(
                                    SendMessageW(hC, BM_GETCHECK, 0, 0) ==
                                    BST_CHECKED); });
                            ctrl->map[L"setChecked"] =
                                NF([hC](vector<Value>& a3) -> Value {
                                SendMessageW(hC, BM_SETCHECK,
                                    a3.empty() || BNum(a3[0]) == 0
                                        ? BST_UNCHECKED
                                        : BST_CHECKED, 0);
                                return Value(); });
                        }
                        // round 41: listbox/combobox — same method surface,
                        // different message numbers (LB_* vs CB_*)
                        if (strCls == L"listbox" || strCls == L"combobox")
                        {
                            bool bCombo = strCls == L"combobox";
                            UINT mAdd = bCombo ? CB_ADDSTRING : LB_ADDSTRING;
                            UINT mCnt = bCombo ? CB_GETCOUNT : LB_GETCOUNT;
                            UINT mCur = bCombo ? CB_GETCURSEL : LB_GETCURSEL;
                            UINT mSet = bCombo ? CB_SETCURSEL : LB_SETCURSEL;
                            UINT mTxt = bCombo ? CB_GETLBTEXT : LB_GETTEXT;
                            UINT mLen = bCombo ? CB_GETLBTEXTLEN
                                               : LB_GETTEXTLEN;
                            UINT mRst = bCombo ? CB_RESETCONTENT
                                               : LB_RESETCONTENT;
                            UINT mDel = bCombo ? CB_DELETESTRING
                                               : LB_DELETESTRING;
                            auto getitem = [hC, mTxt, mLen](int i1)
                                -> wstring {
                                int n = SendMessageW(hC, mLen, i1 - 1, 0);
                                if (n < 0) return L"";
                                std::vector<wchar_t> buf((size_t)n + 1, 0);
                                SendMessageW(hC, mTxt, i1 - 1,
                                             (LPARAM)buf.data());
                                return buf.data(); };
                            ctrl->map[L"add"] = NF([hC, mAdd, mCnt](
                                vector<Value>& a3) -> Value {
                                wstring s = a3.empty() ? L"" : BStr(a3[0]);
                                SendMessageW(hC, mAdd, 0,
                                             (LPARAM)s.c_str());
                                int c = (int)SendMessageW(hC, mCnt, 0, 0);
                                return Value::Num_((double)c); });
                            ctrl->map[L"count"] = NF([hC, mCnt](
                                vector<Value>&) -> Value {
                                return Value::Num_(
                                    (double)SendMessageW(hC, mCnt, 0, 0)); });
                            ctrl->map[L"getText"] = NF([getitem](
                                vector<Value>& a3) -> Value {
                                return Value::Str_(
                                    (int)BNum(a3[0]) >= 1
                                        ? getitem((int)BNum(a3[0]))
                                        : L""); });
                            ctrl->map[L"sel"] = NF([hC, mCur](
                                vector<Value>&) -> Value {
                                int i = (int)SendMessageW(hC, mCur, 0, 0);
                                return Value::Num_((double)(i < 0 ? 0
                                                                  : i + 1)); });
                            ctrl->map[L"selText"] = NF([hC, mCur, getitem](
                                vector<Value>&) -> Value {
                                int i = (int)SendMessageW(hC, mCur, 0, 0);
                                return Value::Str_(i < 0 ? L""
                                                         : getitem(i + 1)); });
                            ctrl->map[L"setSel"] = NF([hC, mSet](
                                vector<Value>& a3) -> Value {
                                SendMessageW(hC, mSet,
                                    a3.empty() ? -1
                                               : (WPARAM)(int)BNum(a3[0]) - 1,
                                    0);
                                return Value(); });
                            ctrl->map[L"clear"] = NF([hC, mRst](
                                vector<Value>&) -> Value {
                                SendMessageW(hC, mRst, 0, 0);
                                return Value(); });
                            ctrl->map[L"del"] = NF([hC, mDel](
                                vector<Value>& a3) -> Value {
                                SendMessageW(hC, mDel,
                                    (WPARAM)(int)BNum(a3[0]) - 1, 0);
                                return Value(); });
                            // force the selection-change notification the
                            // control would send on real user interaction
                            // (LB/CB_SETCURSEL deliberately do not)
                            ctrl->map[L"notifySel"] =
                                NF([fs, nId, hwnd, hC](
                                    vector<Value>&) -> Value {
                                PostMessageW(hwnd, WM_COMMAND,
                                    MAKEWPARAM(nId, LBN_SELCHANGE),
                                    (LPARAM)hC);
                                return Value(); });
                        }
                        ctrl->map[L"text"] = NF([hC](vector<Value>&)
                            -> Value {
                            wchar_t buf[512] = L"";
                            GetWindowTextW(hC, buf, 512);
                            return Value::Str_(buf); });
                        ctrl->map[L"setText"] = NF([hC](vector<Value>& a3)
                            -> Value {
                            SetWindowTextW(hC, BStr(a3[0]).c_str());
                            return Value(); });
                        ctrl->map[L"click"] = NF([nId, hwnd, hC](
                            vector<Value>&) -> Value {
                            PostMessageW(hwnd, WM_COMMAND,
                                MAKEWPARAM(nId, BN_CLICKED),
                                (LPARAM)hC);
                            return Value(); });
                        // callback slot: assign a function to .oncommand
                        ctrl->map[L"oncommand"] = Value();   // set by script
                        // the dispatch reads ctrl->map["oncommand"] live
                        fs->ctrls[nId] = Value::Table_(ctrl);
                        fs->callbacks[nId] = Value();       // filled lazily
                        // hook: keep the id in the value so dispatch can
                        // resolve; store id on the control table
                        ctrl->map[L"_id"] = Value::Num_((double)nId);
                        fs->callbacks[nId] = NF([fs, nId, this](
                            vector<Value>&) -> Value {
                            auto ic = fs->ctrls.find(nId);
                            if (ic == fs->ctrls.end() || !ic->second.tab)
                                return Value();
                            auto ib = ic->second.tab->map.find(L"oncommand");
                            if (ib == ic->second.tab->map.end())
                                return Value();
                            vector<Value> noArgs;
                            return this->CallVmFunc(ib->second, noArgs); });
                        fs->changes[nId] = NF([fs, nId, this](
                            vector<Value>&) -> Value {
                            auto ic = fs->ctrls.find(nId);
                            if (ic == fs->ctrls.end() || !ic->second.tab)
                                return Value();
                            auto ib = ic->second.tab->map.find(L"onchange");
                            if (ib == ic->second.tab->map.end())
                                return Value();
                            vector<Value> noArgs;
                            return this->CallVmFunc(ib->second, noArgs); });
                        fs->selchgs[nId] = NF([fs, nId, this](
                            vector<Value>&) -> Value {
                            auto ic = fs->ctrls.find(nId);
                            if (ic == fs->ctrls.end() || !ic->second.tab)
                                return Value();
                            auto ib = ic->second.tab->map.find(L"onselchange");
                            if (ib == ic->second.tab->map.end())
                                return Value();
                            vector<Value> noArgs;
                            return this->CallVmFunc(ib->second, noArgs); });
                        form->map[strName] = Value::Table_(ctrl);
                        return Value::Table_(ctrl); });
                    return Value::Table_(form); });
            }

            // first-class scalar builtins (also reachable via fast paths)
            globals.vars[L"tostring"] = NF([](vector<Value>& a) {
                return Value::Str_(a.empty() ? L"null" : a[0].ToString()); });
            Value vType = NF([](vector<Value>& a) -> Value {
                if (a.empty()) return Value::Str_(L"null");
                switch (a[0].t)
                {
                case Value::Null: return Value::Str_(L"null");
                case Value::Bool: return Value::Str_(L"boolean");
                case Value::Num: return Value::Str_(L"number");
                case Value::Pointer: return Value::Str_(L"pointer");
                case Value::Str: return Value::Str_(L"string");
                case Value::TableT: return Value::Str_(L"table");
                case Value::Class: return Value::Str_(L"class");
                case Value::Func:
                    return Value::Str_(a[0].native ? L"native function"
                                                   : L"function");
                }
                return Value::Str_(L"null"); });
            vType.tab = std::make_shared<Table>();
            vType.tab->map[L"null"] = Value::Str_(L"null");
            vType.tab->map[L"boolean"] = Value::Str_(L"boolean");
            vType.tab->map[L"number"] = Value::Str_(L"number");
            vType.tab->map[L"string"] = Value::Str_(L"string");
            vType.tab->map[L"table"] = Value::Str_(L"table");
            vType.tab->map[L"function"] = Value::Str_(L"function");
            vType.tab->map[L"cdata"] = Value::Str_(L"cdata");
            vType.tab->map[L"pointer"] = Value::Str_(L"pointer");
            vType.tab->map[L"isString"] = NF([](vector<Value>& a) -> Value {
                return Value::Bool_(!a.empty() && a[0].t == Value::Str);
            });
            vType.tab->map[L"isNumber"] = NF([](vector<Value>& a) -> Value {
                return Value::Bool_(!a.empty() && a[0].t == Value::Num);
            });
            vType.tab->map[L"isTable"] = NF([](vector<Value>& a) -> Value {
                return Value::Bool_(!a.empty() && a[0].t == Value::TableT);
            });
            vType.tab->map[L"isFunction"] = NF([](vector<Value>& a) -> Value {
                return Value::Bool_(!a.empty() && (a[0].t == Value::Func || a[0].native != nullptr));
            });
            vType.tab->map[L"isPointer"] = NF([](vector<Value>& a) -> Value {
                return Value::Bool_(!a.empty() && a[0].t == Value::Pointer);
            });
            vType.tab->map[L"isClass"] = NF([](vector<Value>& a) -> Value {
                return Value::Bool_(!a.empty() && a[0].t == Value::Class);
            });
            globals.vars[L"type"] = vType;
            roots[L"type"] = vType;
            globals.vars[L"topointer"] = NF([](vector<Value>& a) -> Value {
                if (a.empty() || a[0].t == Value::Null) return Value::Pointer_(nullptr);
                if (a[0].t == Value::Pointer) return a[0];
                if (a[0].ptr != nullptr) return Value::Pointer_(a[0].ptr);
                if (a[0].t == Value::Num) return Value::Pointer_((void*)(INT_PTR)(long long)a[0].n);
                if (a[0].t == Value::Str) return Value::Pointer_((void*)a[0].s.c_str());
                return Value::Pointer_((void*)(INT_PTR)(long long)BNum(a[0])); });
            globals.vars[L"tonumber"] = NF([](vector<Value>& a) {
                return a.empty() ? Value() : Value::Num_(BNum(a[0])); });
            globals.vars[L"sleep"] = NF([](vector<Value>& a) {
                if (!a.empty()) Sleep((DWORD)BNum(a[0]));
                return Value(); });
            globals.vars[L"_HINSTANCE"] = Value::Pointer_(GetModuleHandleW(nullptr));
            globals.vars[L"_HAPPICON"] = Value::Pointer_(LoadIconW(nullptr, IDI_APPLICATION));
            globals.vars[L"_INVALID_HANDLE_VALUE"] = Value::Pointer_(INVALID_HANDLE_VALUE);
            // Preload builtin (official aardio VM preloads builtin on startup)
            {
                PNode nImp = std::make_shared<Node>();
                nImp->k = Node::Import;
                nImp->names = { L"builtin" };
                DoImport(nImp, &globals);
                if (err && !err->empty())
                {
                    err->clear();
                }
            }
        }
    };

    // ---- round 40: gdi — per-paint graphics object -----------------------
    //
    // WM_PAINT wraps the HDC in a table of native drawing methods; state
    // (pen/brush/text colors) lives in a shared GdiCtx so the methods act
    // like one device context. Colors are COLORREF numbers (0x00BBGGRR).
    struct GdiCtx
    {
        HDC hdc = nullptr;
        COLORREF clrPen = RGB(0, 0, 0);
        int nPenW = 1;
        COLORREF clrBrush = RGB(255, 255, 255);
        COLORREF clrText = RGB(0, 0, 0);
        int nBkMode = TRANSPARENT;
        // round 43: font created lazily by setFont(); null = DEFAULT_GUI_FONT
        HFONT hFont = nullptr;
        int nFontH = 0;
        wstring sFace;
        bool bBold = false;
        bool bItalic = false;
    };

    static Value MakeGdiTable(const std::shared_ptr<GdiCtx>& g)
    {
        auto tb = std::make_shared<Table>();
        tb->map[L"setPen"] = NF([g](vector<Value>& a) -> Value {
            g->clrPen = a.empty() ? g->clrPen : (COLORREF)(INT_PTR)BNum(a[0]);
            g->nPenW = a.size() > 1 ? (int)BNum(a[1]) : 1;
            if (g->nPenW < 1) g->nPenW = 1;
            return Value(); });
        tb->map[L"setBrush"] = NF([g](vector<Value>& a) -> Value {
            if (!a.empty()) g->clrBrush = (COLORREF)(INT_PTR)BNum(a[0]);
            return Value(); });
        tb->map[L"setTextColor"] = NF([g](vector<Value>& a) -> Value {
            if (!a.empty()) g->clrText = (COLORREF)(INT_PTR)BNum(a[0]);
            return Value(); });
        // round 43: setFont(height, face, bold, italic) — subsequent
        // text() uses the font; height is positive pixel height
        tb->map[L"setFont"] = NF([g](vector<Value>& a) -> Value {
            g->nFontH = a.empty() ? 0 : (int)BNum(a[0]);
            g->sFace = a.size() > 1 ? BStr(a[1]) : L"";
            g->bBold = a.size() > 2 && BNum(a[2]) != 0;
            g->bItalic = a.size() > 3 && BNum(a[3]) != 0;
            if (g->hFont) { DeleteObject(g->hFont); g->hFont = nullptr; }
            if (g->nFontH > 0)
            {
                g->hFont = CreateFontW(
                    -g->nFontH, 0, 0, 0,
                    g->bBold ? FW_BOLD : FW_NORMAL,
                    g->bItalic ? TRUE : FALSE, 0, 0, DEFAULT_CHARSET,
                    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                    CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                    g->sFace.empty() ? L"Microsoft YaHei"
                                     : g->sFace.c_str());
            }
            return Value(); });
        tb->map[L"setBkMode"] = NF([g](vector<Value>& a) -> Value {
            // 1 = transparent, 2 = opaque
            g->nBkMode = (!a.empty() && BNum(a[0]) == 2) ? OPAQUE
                                                         : TRANSPARENT;
            return Value(); });
        tb->map[L"line"] = NF([g](vector<Value>& a) -> Value {
            if (a.size() < 4) return Value();
            HPEN hPen = CreatePen(PS_SOLID, g->nPenW, g->clrPen);
            HGDIOBJ hOld = SelectObject(g->hdc, hPen);
            MoveToEx(g->hdc, (int)BNum(a[0]), (int)BNum(a[1]), nullptr);
            LineTo(g->hdc, (int)BNum(a[2]), (int)BNum(a[3]));
            SelectObject(g->hdc, hOld);
            DeleteObject(hPen);
            return Value(); });
        tb->map[L"rect"] = NF([g](vector<Value>& a) -> Value {
            if (a.size() < 4) return Value();
            HPEN hPen = CreatePen(PS_SOLID, g->nPenW, g->clrPen);
            HBRUSH hBr = CreateSolidBrush(g->clrBrush);
            HGDIOBJ hOldP = SelectObject(g->hdc, hPen);
            HGDIOBJ hOldB = SelectObject(g->hdc, hBr);
            Rectangle(g->hdc, (int)BNum(a[0]), (int)BNum(a[1]),
                      (int)BNum(a[2]), (int)BNum(a[3]));
            SelectObject(g->hdc, hOldP);
            SelectObject(g->hdc, hOldB);
            DeleteObject(hPen);
            DeleteObject(hBr);
            return Value(); });
        tb->map[L"ellipse"] = NF([g](vector<Value>& a) -> Value {
            if (a.size() < 4) return Value();
            HPEN hPen = CreatePen(PS_SOLID, g->nPenW, g->clrPen);
            HBRUSH hBr = CreateSolidBrush(g->clrBrush);
            HGDIOBJ hOldP = SelectObject(g->hdc, hPen);
            HGDIOBJ hOldB = SelectObject(g->hdc, hBr);
            Ellipse(g->hdc, (int)BNum(a[0]), (int)BNum(a[1]),
                    (int)BNum(a[2]), (int)BNum(a[3]));
            SelectObject(g->hdc, hOldP);
            SelectObject(g->hdc, hOldB);
            DeleteObject(hPen);
            DeleteObject(hBr);
            return Value(); });
        tb->map[L"fillRect"] = NF([g](vector<Value>& a) -> Value {
            if (a.size() < 4) return Value();
            RECT rc = { (int)BNum(a[0]), (int)BNum(a[1]),
                        (int)BNum(a[2]), (int)BNum(a[3]) };
            HBRUSH hBr = CreateSolidBrush(
                a.size() > 4 ? (COLORREF)(INT_PTR)BNum(a[4]) : g->clrBrush);
            FillRect(g->hdc, &rc, hBr);
            DeleteObject(hBr);
            return Value(); });
        tb->map[L"clear"] = NF([g](vector<Value>& a) -> Value {
            RECT rc;
            GetClientRect(WindowFromDC(g->hdc), &rc);
            HBRUSH hBr = CreateSolidBrush(
                a.empty() ? g->clrBrush : (COLORREF)(INT_PTR)BNum(a[0]));
            FillRect(g->hdc, &rc, hBr);
            DeleteObject(hBr);
            return Value(); });
        tb->map[L"text"] = NF([g](vector<Value>& a) -> Value {
            if (a.size() < 3) return Value();
            SetTextColor(g->hdc, g->clrText);
            SetBkMode(g->hdc, g->nBkMode);
            HFONT hF = g->hFont
                ? g->hFont
                : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
            HGDIOBJ hOld = SelectObject(g->hdc, hF);
            wstring s = BStr(a[2]);
            TextOutW(g->hdc, (int)BNum(a[0]), (int)BNum(a[1]),
                     s.c_str(), (int)s.size());
            SelectObject(g->hdc, hOld);
            return Value(); });
        tb->map[L"pixel"] = NF([g](vector<Value>& a) -> Value {
            if (a.size() < 2) return Value();
            COLORREF c = GetPixel(g->hdc, (int)BNum(a[0]), (int)BNum(a[1]));
            return Value::Num_((double)c); });
        tb->map[L"width"] = NF([g](vector<Value>&) -> Value {
            HWND hW = WindowFromDC(g->hdc);
            RECT rc;
            GetClientRect(hW, &rc);
            return Value::Num_((double)(rc.right - rc.left)); });
        tb->map[L"height"] = NF([g](vector<Value>&) -> Value {
            HWND hW = WindowFromDC(g->hdc);
            RECT rc;
            GetClientRect(hW, &rc);
            return Value::Num_((double)(rc.bottom - rc.top)); });
        return Value::Table_(tb);
    }

            static LRESULT CALLBACK FormWndProc(HWND hwnd, UINT msg, WPARAM wp,
                                        LPARAM lp)
    {
        auto it = g_forms.find(hwnd);
        FormState* fs = it != g_forms.end() ? it->second : nullptr;
        switch (msg)
        {
        case WM_PAINT:
            if (fs && fs->it)
            {
                PAINTSTRUCT psP;
                HDC hdcP = BeginPaint(hwnd, &psP);
                auto if2 = fs->formVal.tab
                    ? fs->formVal.tab->map.find(L"onpaint")
                    : fs->formVal.tab->map.end();
                if (fs->formVal.tab && if2 != fs->formVal.tab->map.end() &&
                    (if2->second.t == Value::Func || if2->second.native))
                {
                    auto g = std::make_shared<GdiCtx>();
                    g->hdc = hdcP;
                    vector<Value> args{ MakeGdiTable(g) };
                    fs->bInPaint = true;
                    fs->it->CallVmFunc(if2->second, args);
                    fs->bInPaint = false;
                }
                EndPaint(hwnd, &psP);
                return 0;
            }
            break;
        case WM_TIMER:
            if (fs && fs->it && fs->formVal.tab)
            {
                auto it2 = fs->formVal.tab->map.find(L"ontimer");
                if (it2 != fs->formVal.tab->map.end() &&
                    (it2->second.t == Value::Func || it2->second.native))
                {
                    vector<Value> args{ Value::Num_((double)(int)wp) };
                    fs->it->CallVmFunc(it2->second, args);
                }
                return 0;
            }
            break;
        case WM_COMMAND:
            if (fs)
            {
                WORD id = LOWORD(wp), code = HIWORD(wp);
                // round 41: menu commands carry lParam==0 and live in the
                // 5000+ id range — check them before control dispatch
                auto im = fs->menus.find(id);
                if (im != fs->menus.end())
                {
                    vector<Value> noArgs;
                    fs->it->CallVmFunc(im->second, noArgs);
                }
                else
                {
                bool bChange = code == EN_CHANGE &&
                               fs->changes.count(id);
                bool bClick = code == BN_CLICKED &&
                              fs->callbacks.count(id);
                bool bSel = code == LBN_SELCHANGE &&
                            fs->selchgs.count(id);
                if (bChange)
                {
                    Value f = fs->changes[id];
                    vector<Value> noArgs;
                    fs->it->CallVmFunc(f, noArgs);
                }
                else if (bClick)
                {
                    Value f = fs->callbacks[id];
                    vector<Value> noArgs;
                    fs->it->CallVmFunc(f, noArgs);
                }
                else if (bSel)
                {
                    Value f = fs->selchgs[id];
                    vector<Value> noArgs;
                    fs->it->CallVmFunc(f, noArgs);
                }
                }
            }
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (fs)
            {
                fs->bModalDone = true;
                PostQuitMessage(0);
            }
            return 0;
        case WM_NCDESTROY:
            if (fs)
            {
                g_forms.erase(hwnd);
                delete fs;
            }
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    static bool RegisterFormClass()
    {
        static bool bDone = false;
        if (bDone)
            return true;
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = FormWndProc;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"AardioRE_form";
        bDone = RegisterClassExW(&wc) != 0;
        return bDone;
    }

    int CheckSyntax(const wstring& src, wstring* strError)
    {
        // parse only — no execution (F8 compile check)
        wstring localErr;
        vector<Tok> toks = Lex(src, &localErr);
        if (localErr.empty())
        {
            Parser ps{ toks, 0, &localErr };
            ps.ParseBlock();
        }
        if (!localErr.empty())
        {
            if (strError) *strError = localErr;
            return -1;
        }
        return 0;
    }

    static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep)
    {
        wchar_t szMod[MAX_PATH] = L"unknown";
        HMODULE hMod = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR)ep->ExceptionRecord->ExceptionAddress, &hMod))
        {
            GetModuleFileNameW(hMod, szMod, MAX_PATH);
        }
        fwprintf(stderr, L"\n[FATAL CRASH] code=0x%08X at addr=%p (%s) file=%s line=%d\n",
                 ep->ExceptionRecord->ExceptionCode,
                 ep->ExceptionRecord->ExceptionAddress,
                 szMod,
                 g_curModuleFile.c_str(),
                 g_interpCurrent ? g_interpCurrent->nErrLine : 0);
        fflush(stderr);
        ExitProcess((UINT)ep->ExceptionRecord->ExceptionCode);
        return EXCEPTION_EXECUTE_HANDLER;
    }

    int RunSource(const wstring& src, wstring* strError,
                  const wstring* pBaseDir,
                  const std::unordered_map<wstring, wstring>* pEmbedded,
                  bool bDebug, const wstring* pBreakLines)
    {
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
        SetUnhandledExceptionFilter(CrashFilter);
        setvbuf(stderr, nullptr, _IONBF, 0);
        wstring localErr;
        vector<Tok> toks = Lex(src, &localErr);
        Parser ps{ toks, 0, &localErr };
        PNode prog = ps.ParseBlock();   // whole program as a block
        if (!localErr.empty())
        {
            if (strError) *strError = localErr;
            return -1;
        }
        Interp it{ &localErr };
        g_interpCurrent = &it;
        if (pBaseDir)
            it.baseDir = *pBaseDir;
        if (pEmbedded)
            it.embedded = *pEmbedded;
        it.bDebug = bDebug;
        if (pBreakLines)
        {
            // "3,7,12" -> breakpoint set
            wstring str = *pBreakLines;
            size_t pos = 0;
            while (pos < str.size())
            {
                size_t comma = str.find(L',', pos);
                if (comma == wstring::npos) comma = str.size();
                int nLine = _wtoi(str.substr(pos, comma - pos).c_str());
                if (nLine > 0)
                    it.breakpoints.insert(nLine);
                pos = comma + 1;
            }
        }
        it.InitBuiltins();
        try
        {
            it.ExecBlock(prog, &it.globals);
        }
        catch (const std::exception& e)
        {
            // an STL/CRT escape (bad_function_call, out_of_range, ...) —
            // surface the reason instead of dying as 0xE06D7363
            int nW = MultiByteToWideChar(CP_UTF8, 0, e.what(), -1,
                                         nullptr, 0);
            wstring whatW((size_t)max(nW, 1), 0);
            if (nW > 1)
                MultiByteToWideChar(CP_UTF8, 0, e.what(), -1, &whatW[0], nW);
            localErr = L"运行错误: C++ 异常 " + whatW + L" @ 行 " +
                       std::to_wstring(it.nErrLine);
        }
        catch (...)
        {
            localErr = L"运行错误: 未知 C++ 异常 @ 行 " +
                       std::to_wstring(it.nErrLine);
        }
        if (!localErr.empty())
        {
            // runtime errors carry the innermost statement line; parse errors
            // already embed one via the parser
            if (localErr.find(L"运行错误") != wstring::npos &&
                it.nErrLine > 0)
                localErr += (L" @ 行 " + std::to_wstring(it.nErrLine));
            if (strError) *strError = localErr;
            return -1;
        }
        return it.ret.t == Value::Num ? (int)it.ret.n : 0;
    }
}
