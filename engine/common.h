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
#include <atomic>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <deque>
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
        bool bLeadingSpace = false;
        bool bUtf16 = false;
        bool bRawBytes = false;
        // Absolute offset of the token's FIRST character in the source
        // string (wchar units == UTF-16 code units, same unit LSP's
        // `character` uses). The lexer only tracked the line number before,
        // which is enough for diagnostics but not for "go to definition" —
        // a jump needs the exact column, and the column of a token can only
        // be derived from its absolute offset (a line's start offset is not
        // known until the whole file has been scanned).
        //
        // Filled by Lex() for every token; 0 for tokens synthesized by hand
        // (the parser never synthesizes any).
        size_t pos = 0;
    };

    // Compiled (bytecode) modules begin with a configured magic header
    // followed by a binary body. Such a file is not source text, so a
    // source-syntax check does not apply; loading it as source must
    // report a format error rather than a misleading syntax error.
    bool IsCompiledModule(const wstring& src);

    inline const wchar_t* const KEYWORDS[] = {
        L"var", L"function", L"if", L"else", L"elseif", L"while", L"for",
        L"in", L"break", L"continue", L"return", L"null", L"true", L"false",
        L"and", L"or", L"not", L"do", L"import", L"namespace",
        L"try", L"catch", L"class", L"lambda", L"λ", L"select", L"case", L"with",
        nullptr
    };

    struct Env;
    struct Value;

    // gc.cpp — pooled slot buffers (used by Env::VarAt's overflow fallback;
    // declarations early because Env's accessors reference them)
    void GcReleaseSlotBuf(std::vector<Value>&& v);
    void GcReleaseBoundBuf(std::vector<uint8_t>&& v);

    // VarAt 槽越界诊断计数（runtime.cpp 定义，BootDump 打印）——必须放在
    // 文件作用域：块作用域 extern 会被 MSVC 绑到全局命名空间（LNK2019）
    extern long g_nSlotConv;
    extern long g_nKeepResize;

    struct Node;
    using PNode = shared_ptr<Node>;


    // ---- PNode 节点池（gc.cpp 实现链表）----
    // import 普查（allocstat.cpp）：一次 import win.ui 家族 make_shared<Node>
    // 约 13 万次（256B 桶 27.8MB）。allocate_shared 定制分配器把节点块
    // （控制块+Node 一体的整块）按尺寸归还 thread_local 空闲链表，命中时
    // 零堆调用。块级回收——块只在引用归零后入链，被复用时旧 AST 必已死，
    // 无 arena 的生命周期问题；进程内的 AST 终生存活特性让池命中率先行爬升。
    void* NodePoolTake(size_t nBytes);
    void NodePoolGive(void* p, size_t nBytes);

    template <class T>
    struct NodePoolAlloc
    {
        using value_type = T;
        NodePoolAlloc() = default;
        template <class U> NodePoolAlloc(const NodePoolAlloc<U>&) {}
        template <class U> struct rebind { using other = NodePoolAlloc<U>; };
        T* allocate(size_t n) { return (T*)NodePoolTake(n * sizeof(T)); }
        void deallocate(T* p, size_t n) { NodePoolGive(p, n * sizeof(T)); }
        bool operator==(const NodePoolAlloc&) const { return true; }
        bool operator!=(const NodePoolAlloc&) const { return false; }
    };

    // 所有 AST 节点的统一创建入口（parser / compiler / builtins / import）
    inline PNode NewPNode()
    {
        return std::allocate_shared<Node>(NodePoolAlloc<Node>{});
    }

    struct Env;
    struct Value;
    struct ComCacheEntry;

    // Identity key for non-string Value keys (tables/functions/classes).
    // ToString() flattens every table to "table", so the official-library
    // cycle guard (objreferences[t]) and any t[t]=... map collapsed distinct
    // objects into one slot. Object identity keys keep them distinct.
    wstring ObjectKeyOf(const Value& v);

    struct Table : public std::enable_shared_from_this<Table>
    {
        std::unordered_map<wstring, Value> map;
        vector<Value> arr;
        std::unordered_map<wstring, Value> scopeVars; // private namespace/module scope variables (var x)
        // 官方表对象 +0x08 处的指针：既是父命名空间（成员查找的回退目标），
        // 也可被 setmetatable 覆盖（官方 0x10 SET_METATABLE 写的就是这一格）。
        std::shared_ptr<Table> parent;
        bool bExplicitMeta = false;
        std::shared_ptr<std::unordered_map<wstring, ComCacheEntry>> comCache;
        // statement epoch this object was born in (gc.cpp): the collector
        // never touches objects that belong to the statement currently
        // being evaluated — a value may still live in a C++ temporary
        // across the collection point.
        long nEpoch = 0;
        // GC registry: every live table is tracked (gc.cpp) so the
        // collector can find reference cycles. The VM's shared_ptr graph
        // IS cyclic by construction — a closure captures its defining
        // frame and the frame holds the closure — so plain refcounting
        // can never reclaim those; only a reachability sweep can.
        Table();
        Table(const Table& rhs);
        ~Table();
    };

    // ---- 函数/类专属数据（Value 的侧表，第 40 轮）----
    // 旧 Value 把这 9 个字段内联：sizeof(Value)=328B 里 ~200B 只有 Func/Class
    // 用得上，每次 Value 拷贝还白付 7 次原子引用计数。收进一个 shared_ptr：
    // Value ~136B，拷贝 3 次原子操作；函数值创建时分配一次，之后随值共享。
    struct FuncData
    {
        PNode fn;                             // function literal body
        PNode classBody;                      // class body statement block
        vector<wstring> params;
        wstring fname;                        // debug
        shared_ptr<Table> nsClosure;          // defining namespace (lexical upvalue)
        shared_ptr<Env> envUp;                // defining frame chain (closure upvalues)
        shared_ptr<vector<PNode>> pdefaults;  // FuncLit param defaults
        shared_ptr<Value> thisVal;            // lexical this captured at function definition
        shared_ptr<Value> ownerVal;           // lexical owner captured at function definition
    };

    // native builtin（round 19 → 第 40 轮）：64B 内联的 std::function 换成
    // 裸函数指针 + 可选上下文，调用点签名 (ctx, args)。经 NF/Native_ 注册
    // 的无捕获 lambda 零上下文零分配（ctx 指向类型级静态空对象）。
    template <class F>
    Value NativeTrampoline(void* ctx, std::vector<Value>& args)
    {
        return (*reinterpret_cast<F*>(ctx))(args);
    }

    struct Value
    {
        enum T { Null, Bool, Num, Str, Func, TableT, Class, Pointer } t = Null;
        bool b = false;
        double n = 0;
        wstring s;
        void* ptr = nullptr;
        shared_ptr<Table> tab;
        shared_ptr<FuncData> fnData;          // Func/Class 专属（见 FuncData）
        shared_ptr<vector<Value>> mrets;      // extra return values
        bool bDots = false;             // `...` packed value: mrets holds ALL varargs
        bool bUtf16 = false;            // UTF-16 (ustring) flag
        bool bRawBytes = false;         // raw binary bytes (string& / raw buffer)
        // native builtin: 裸指针 + 可选上下文（见 NativeTrampoline）
        Value (*native)(void* ctx, std::vector<Value>&) = nullptr;
        shared_ptr<void> nativeCtx;
        template <class F>
        void SetNative(F f)
        {
            using Fx = std::decay_t<F>;
            native = &NativeTrampoline<Fx>;
            if constexpr (std::is_empty_v<Fx>)
            {
                static Fx inst(f);   // 拷贝构造：C++17 下无捕获 lambda 无默认构造   // 无捕获 lambda：静态空对象，别名指针不拥有
                nativeCtx = std::shared_ptr<void>(std::shared_ptr<void>(),
                                                  (void*)&inst);
            }
            else
                nativeCtx = std::make_shared<Fx>(std::move(f));
        }
        template <class F>
        static Value Native_(F f)
        {
            Value x; x.t = Func;
            x.fnData = std::make_shared<FuncData>();
            x.SetNative(std::move(f));
            return x;
        }
        static Value Num_(double v) { Value x; x.t = Num; x.n = v; return x; }
        static Value Bool_(bool v) { Value x; x.t = Bool; x.b = v; return x; }
        static Value Str_(const wstring& v, bool isUtf16 = false) { Value x; x.t = Str; x.s = v; x.bUtf16 = isUtf16; return x; }
        static Value Table_(shared_ptr<Table> tb) { Value x; x.t = TableT; x.tab = tb; return x; }
        static Value Pointer_(void* p) { Value x; x.t = Pointer; x.ptr = p; return x; }
        bool Truthy() const;
        wstring ToString() const;
    };

    struct ComCacheEntry
    {
        long dispid = -1;
        Value boundMethod;
    };

    // builtin-writing helpers (round 19)
    inline wstring BStr(const Value& v) { return v.ToString(); }
    // zero-copy string arg access for hot natives: returns the Value's own
    // wstring when it already holds text (no per-call full-string copy);
    // only non-string args pay one conversion copy into `tmp`. Callers
    // keep one tmp per arg slot alive for the whole call.
    inline const wstring& BStrRef(const Value& v, wstring& tmp)
    {
        if (v.t == Value::Str) return v.s;
        tmp = v.ToString();
        return tmp;
    }
    // tight UTF-8 byte-length walk over wstring storage; only astral high
    // surrogates pay the next-wchar lookahead. Hot path of #s and every
    // position builtin — replaces per-use kernel conversion calls
    // (A/B benchmarked: a shared c2-lookahead helper measured ~2.6x
    // slower per wchar on this walk).
    inline size_t Utf8ByteLenOf(const wstring& w)
    {
        size_t acc = 0;
        for (size_t i = 0; i < w.size(); i++)
        {
            wchar_t c = w[i];
            if (c < 0x80) { acc += 1; continue; }
            if (c < 0x800) { acc += 2; continue; }
            if (c >= 0xD800 && c <= 0xDBFF && i + 1 < w.size() &&
                w[i + 1] >= 0xDC00 && w[i + 1] <= 0xDFFF)
            { acc += 4; i++; continue; }
            acc += 3;
        }
        return acc;
    }
    // UTF-8 byte view of a string value (for byte-level primitives such
    // as string.crc32)
    inline std::string BUtf8(const Value& v)
    {
        if (v.bRawBytes)
        {
            std::string out;
            out.reserve(v.s.size());
            for (wchar_t ch : v.s)
                out.push_back((char)(unsigned char)ch);
            return out;
        }
        wstring w = v.ToString();
        if (w.empty()) return std::string();
        int cb = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                     nullptr, 0, nullptr, nullptr);
        std::string out((size_t)max(cb, 0), 0);
        if (cb > 0)
            WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0],
                                cb, nullptr, nullptr);
        return out;
    }
    // UTF-8 encoding of a wide string.
    inline std::string Utf8Bytes(const wstring& w)
    {
        if (w.empty()) return std::string();
        int cb = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                     nullptr, 0, nullptr, nullptr);
        std::string out((size_t)max(cb, 0), 0);
        if (cb > 0)
            WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0],
                                cb, nullptr, nullptr);
        return out;
    }
    // Canonical content form of a string value. Official aardio strings ARE
    // byte arrays, so every string has exactly one content:
    //   - raw byte strings (bRawBytes): one byte per wchar, as stored
    //   - UTF-16 strings (bUtf16):     the same text, encoded as UTF-8
    //   - text strings:                UTF-8 encoding
    // Comparison / concatenation / console output all go through this
    // form, so a source literal and the same bytes read back from a file
    // are the same string.
    inline std::string StrByteForm(const Value& v)
    {
        if (v.bRawBytes)
        {
            std::string out;
            out.reserve(v.s.size());
            for (wchar_t ch : v.s)
                out.push_back((char)(unsigned char)ch);
            return out;
        }
        // A UTF-16 (ustring) value holds UTF-16 code units; encoding it as
        // UTF-8 yields the same text a plain string literal would hold, so
        // the two compare equal (official code always converts ustrings with
        // string.fromUtf16 before comparing them).
        return Utf8Bytes(v.s);
    }
    // Byte form of any value, for console output.
    inline std::string ValueByteForm(const Value& v)
    {
        if (v.t == Value::Str) return StrByteForm(v);
        return Utf8Bytes(v.ToString());
    }
    bool IsSize64(const Value& v);
    uint64_t GetSize64Val(const Value& v);
    void SetSize64Val(Value& v, uint64_t val);
    uint64_t ToSize64Uint(const Value& v);
    Value CreateSize64(uint64_t val);

    const uint32_t DYN_BUF_MAGIC = 0xAA55AA55;
    struct DynBufHeader
    {
        uint32_t magic;
        uint32_t reserved;
        uint32_t capacity;
        uint32_t length;
    };
    static_assert(sizeof(DynBufHeader) == 16, "DynBufHeader must be 16 bytes");

    // AUDIT-11: a dynbuf's data pointer is preceded by its header, but the
    // header is NOT part of the pointer the script holds — the script gets
    // the data area (header + 16). Probing the magic at (p - 16) therefore
    // reads 16 bytes BEFORE the allocation for every pointer that is not a
    // dynbuf (a plain raw.buffer, an FFI pointer, a stack address): a
    // 4-byte out-of-bounds read that ASAN reports as
    // heap-buffer-overflow (probe p6, reproduced on the pre-R2 build).
    // The identity is therefore tracked in a registry (util.cpp), and the
    // magic probe only runs on a pointer the registry vouches for.
    void RegisterDynBuf(void* pData);
    void UnregisterDynBuf(void* pData);
    bool IsRegisteredDynBuf(const void* ptr);

    inline bool IsDynBuffer(const void* p)
    {
        if (!p) return false;
        // registry check first: a registered plain buffer has no header at
        // all, and an unregistered pointer must never be dereferenced.
        if (!IsRegisteredDynBuf(p)) return false;
        __try
        {
            const DynBufHeader* h = (const DynBufHeader*)((const char*)p - 16);
            return h->magic == DYN_BUF_MAGIC;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    inline DynBufHeader* GetDynBufHeader(void* p)
    {
        return (DynBufHeader*)((char*)p - 16);
    }

    inline const DynBufHeader* GetDynBufHeader(const void* p)
    {
        return (const DynBufHeader*)((const char*)p - 16);
    }

    void* DynAlloc(size_t n, void* pOld = nullptr);

    inline double BNum(const Value& v)
    {
        if (v.t == Value::Num) return v.n;
        if (v.t == Value::Pointer) return (double)(__int64)(INT_PTR)v.ptr;
        if (v.t == Value::Str) { wchar_t* e = nullptr; return wcstod(v.s.c_str(), &e); }
        if (v.t == Value::Bool) return v.b ? 1 : 0;
        if (v.t == Value::TableT && v.tab)
        {
            auto ilo = v.tab->map.find(L"_size64_lo");
            auto ihi = v.tab->map.find(L"_size64_hi");
            if (ilo != v.tab->map.end() && ihi != v.tab->map.end())
            {
                uint32_t lo = (uint32_t)(unsigned long long)ilo->second.n;
                uint32_t hi = (uint32_t)(unsigned long long)ihi->second.n;
                return (double)(((uint64_t)hi << 32) | lo);
            }
        }
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
    template <class F>
    inline Value NF(F f) { return Value::Native_(std::move(f)); }

    // Environment flag helper. getenv() does not see every variable the
    // desktop runtime injects into the process, so the diagnostics read
    // the process environment directly. The buffer form is used on
    // purpose: GetEnvironmentVariableW(name, NULL, 0) reports 0 here.
    inline bool EnvFlag(const wchar_t* name)
    {
        wchar_t sz[8] = L"";
        return GetEnvironmentVariableW(name, sz, 8) > 0;
    }

    // Cached environment flags: the value is read ONCE per process.
    // GetEnvironmentVariableW() scans the process environment block, so a
    // call from a per-statement path costs ~1-2 microseconds. The GC's
    // safe point runs on every statement, and the trace flag was checked
    // there — that single call was the interpreter's dominant per-statement
    // cost (measured: ~1.75us per statement, dropping to ~0.1us once
    // the flag is cached).
    inline bool GcTraceEnabled()
    {
        static const bool b = EnvFlag(L"NODIO_GC_TRACE");
        return b;
    }
    inline bool GcNoFinalEnabled()
    {
        static const bool b = EnvFlag(L"NODIO_GC_NOFINAL");
        return b;
    }
    inline bool ComTraceEnabled()
    {
        static const bool b = EnvFlag(L"NODIO_COM_TRACE");
        return b;
    }

    // x64 pointer-width API families: the runtime maps SetWindowLong /
    // GetWindowLong / SetClassLong / GetClassLong to the *PtrW exports in
    // 64-bit builds — the 32-bit exports cannot carry a 64-bit window
    // proc. Forms subclass through SetWindowPointer
    // (declared as SetWindowLong with a ptr param); without this alias the
    // subclass silently fails and NO window message ever reaches the host.
    inline bool IsLongPtrFamily(const std::string& n)
    {
        return n.rfind("SetWindowLong", 0) == 0 ||
               n.rfind("GetWindowLong", 0) == 0 ||
               n.rfind("SetClassLong", 0) == 0 ||
               n.rfind("GetClassLong", 0) == 0;
    }
    inline FARPROC ResolveLongPtrAlias(HMODULE hMod, FARPROC fp,
                                       const std::string& strNameA)
    {
        if (sizeof(void*) == 8 && fp && IsLongPtrFamily(strNameA))
        {
            std::string base = strNameA;
            if (!base.empty() && (base.back() == 'W' || base.back() == 'A'))
                base.pop_back();
            if (FARPROC f2 =
                    ::GetProcAddress(hMod, (base + "PtrW").c_str()))
                return f2;
        }
        return fp;
    }

    // round M2: struct field type names recognized in class/table member
    // declarations (int x; WORD name[32];)
    inline bool IsStructType(const wstring& w)
    {
        static const wchar_t* const kTypes[] = {
            L"int", L"INT", L"uint", L"UINT", L"byte", L"BYTE", L"word", L"WORD",
            L"dword", L"DWORD", L"long", L"LONG", L"int64", L"INT64", L"uint64", L"UINT64", L"long64",
            L"int16", L"INT16", L"uint16", L"UINT16", L"int32", L"INT32", L"uint32", L"UINT32",
            L"pointer", L"PTR", L"ptr", L"addr", L"ADDR", L"ustring",
            L"string", L"bool", L"BOOL", L"float", L"FLOAT", L"double", L"DOUBLE",
            L"struct", L"ustr", L"bstr", L"wstr", L"lpcstr", L"lpwstr",
            L"short", L"SHORT", L"ushort", L"USHORT", L"wchar", L"char", L"CHAR",
            L"uchar", L"UCHAR", L"int8", L"INT8", L"uint8", L"UINT8", L"i64", L"qword", L"handle",
            L"hwnd", L"hkey", L"lpvoid", L"strb",
            // 官方源码里的拼写错误：lib/string/markdown/_.aardio 的 html_renderopt
            // 类写的是 `stuct toc_data = { ... };`（struct 少了一个 r）。
            // aardio 容忍这个写法；Nodio 若不认，这个嵌套结构体字段就不会进
            // `_struct` 定义，序列化出来的内存布局随之错位 ——
            // 实测：官方 sundown 的 html_renderopt.flags 因此读到越界垃圾，
            // 同一个输入的渲染结果在 <hr> / <hr/> 之间随机跳变，
            // 含 "://" 的链接还会直接崩进程（0xC0000005）。
            L"stuct",
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
        ARStrA, ARStrW, ARPtr, ARAddr
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
    // root for compile-time embed paths ("~/...") — parent of
    // NODIO_LIBDIR; defined near RunSource, used by DoImport
    struct Interp;   // (fwd — real struct defined later)
    struct CbCtx
    {
        Value fn;               // function to invoke
        Value owner;            // owner/this receiver for callback invocation
        ApiSig sig;             // declared signature
        Interp* interp = nullptr;
        void* code = nullptr;   // executable trampoline
        size_t codeSize = 0;
        // Official contract (docs/library-guide/builtin/raw/callback.md VI):
        // a native callback is a GC managed "cdata" object - once the last
        // script reference to it is gone the collector releases the native
        // trampoline and the script closure behind it. "rooted" marks the
        // callbacks that have no script visible owner yet (a bare function
        // handed to a dynamic FFI call): those stay GC roots.
        bool rooted = true;
        bool released = false;  // script side already detached
        int popBytes = 0;       // stdcall stack cleanup of the trampoline
    };
    extern std::vector<std::weak_ptr<CbCtx>> g_cbs;
    // Release a native callback: detach the script closure, neutralize the
    // trampoline and give its slot back to the trampoline pool.
    void ReleaseCallback(CbCtx* ctx);

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
        case 15: { typedef R(__stdcall *FP)(SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT); return ((FP)fn)(s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[9], s[10], s[11], s[12], s[13], s[14]); }
        case 16: { typedef R(__stdcall *FP)(SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT); return ((FP)fn)(s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[9], s[10], s[11], s[12], s[13], s[14], s[15]); }
        case 17: { typedef R(__stdcall *FP)(SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT); return ((FP)fn)(s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[9], s[10], s[11], s[12], s[13], s[14], s[15], s[16]); }
        case 18: { typedef R(__stdcall *FP)(SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT); return ((FP)fn)(s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[9], s[10], s[11], s[12], s[13], s[14], s[15], s[16], s[17]); }
        case 19: { typedef R(__stdcall *FP)(SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT); return ((FP)fn)(s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[9], s[10], s[11], s[12], s[13], s[14], s[15], s[16], s[17], s[18]); }
        case 20: { typedef R(__stdcall *FP)(SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT); return ((FP)fn)(s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[9], s[10], s[11], s[12], s[13], s[14], s[15], s[16], s[17], s[18], s[19]); }
        case 21: { typedef R(__stdcall *FP)(SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT); return ((FP)fn)(s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[9], s[10], s[11], s[12], s[13], s[14], s[15], s[16], s[17], s[18], s[19], s[20]); }
        case 22: { typedef R(__stdcall *FP)(SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT); return ((FP)fn)(s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[9], s[10], s[11], s[12], s[13], s[14], s[15], s[16], s[17], s[18], s[19], s[20], s[21]); }
        case 23: { typedef R(__stdcall *FP)(SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT); return ((FP)fn)(s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[9], s[10], s[11], s[12], s[13], s[14], s[15], s[16], s[17], s[18], s[19], s[20], s[21], s[22]); }
        case 24: { typedef R(__stdcall *FP)(SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT); return ((FP)fn)(s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[9], s[10], s[11], s[12], s[13], s[14], s[15], s[16], s[17], s[18], s[19], s[20], s[21], s[22], s[23]); }
        case 25: { typedef R(__stdcall *FP)(SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT); return ((FP)fn)(s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[9], s[10], s[11], s[12], s[13], s[14], s[15], s[16], s[17], s[18], s[19], s[20], s[21], s[22], s[23], s[24]); }
        case 26: { typedef R(__stdcall *FP)(SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT); return ((FP)fn)(s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[9], s[10], s[11], s[12], s[13], s[14], s[15], s[16], s[17], s[18], s[19], s[20], s[21], s[22], s[23], s[24], s[25]); }
        case 27: { typedef R(__stdcall *FP)(SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT); return ((FP)fn)(s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[9], s[10], s[11], s[12], s[13], s[14], s[15], s[16], s[17], s[18], s[19], s[20], s[21], s[22], s[23], s[24], s[25], s[26]); }
        case 28: { typedef R(__stdcall *FP)(SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT); return ((FP)fn)(s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[9], s[10], s[11], s[12], s[13], s[14], s[15], s[16], s[17], s[18], s[19], s[20], s[21], s[22], s[23], s[24], s[25], s[26], s[27]); }
        case 29: { typedef R(__stdcall *FP)(SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT); return ((FP)fn)(s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[9], s[10], s[11], s[12], s[13], s[14], s[15], s[16], s[17], s[18], s[19], s[20], s[21], s[22], s[23], s[24], s[25], s[26], s[27], s[28]); }
        case 30: { typedef R(__stdcall *FP)(SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT); return ((FP)fn)(s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[9], s[10], s[11], s[12], s[13], s[14], s[15], s[16], s[17], s[18], s[19], s[20], s[21], s[22], s[23], s[24], s[25], s[26], s[27], s[28], s[29]); }
        case 31: { typedef R(__stdcall *FP)(SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT); return ((FP)fn)(s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[9], s[10], s[11], s[12], s[13], s[14], s[15], s[16], s[17], s[18], s[19], s[20], s[21], s[22], s[23], s[24], s[25], s[26], s[27], s[28], s[29], s[30]); }
        case 32: { typedef R(__stdcall *FP)(SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT); return ((FP)fn)(s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[9], s[10], s[11], s[12], s[13], s[14], s[15], s[16], s[17], s[18], s[19], s[20], s[21], s[22], s[23], s[24], s[25], s[26], s[27], s[28], s[29], s[30], s[31]); }
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
        case 15: { typedef R(__cdecl *FP)(SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT); return ((FP)fn)(s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[9], s[10], s[11], s[12], s[13], s[14]); }
        case 16: { typedef R(__cdecl *FP)(SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT); return ((FP)fn)(s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[9], s[10], s[11], s[12], s[13], s[14], s[15]); }
        case 17: { typedef R(__cdecl *FP)(SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT); return ((FP)fn)(s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[9], s[10], s[11], s[12], s[13], s[14], s[15], s[16]); }
        case 18: { typedef R(__cdecl *FP)(SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT); return ((FP)fn)(s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[9], s[10], s[11], s[12], s[13], s[14], s[15], s[16], s[17]); }
        case 19: { typedef R(__cdecl *FP)(SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT); return ((FP)fn)(s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[9], s[10], s[11], s[12], s[13], s[14], s[15], s[16], s[17], s[18]); }
        case 20: { typedef R(__cdecl *FP)(SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT); return ((FP)fn)(s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[9], s[10], s[11], s[12], s[13], s[14], s[15], s[16], s[17], s[18], s[19]); }
        case 21: { typedef R(__cdecl *FP)(SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT); return ((FP)fn)(s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[9], s[10], s[11], s[12], s[13], s[14], s[15], s[16], s[17], s[18], s[19], s[20]); }
        case 22: { typedef R(__cdecl *FP)(SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT); return ((FP)fn)(s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[9], s[10], s[11], s[12], s[13], s[14], s[15], s[16], s[17], s[18], s[19], s[20], s[21]); }
        case 23: { typedef R(__cdecl *FP)(SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT); return ((FP)fn)(s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[9], s[10], s[11], s[12], s[13], s[14], s[15], s[16], s[17], s[18], s[19], s[20], s[21], s[22]); }
        case 24: { typedef R(__cdecl *FP)(SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT); return ((FP)fn)(s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[9], s[10], s[11], s[12], s[13], s[14], s[15], s[16], s[17], s[18], s[19], s[20], s[21], s[22], s[23]); }
        case 25: { typedef R(__cdecl *FP)(SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT); return ((FP)fn)(s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[9], s[10], s[11], s[12], s[13], s[14], s[15], s[16], s[17], s[18], s[19], s[20], s[21], s[22], s[23], s[24]); }
        case 26: { typedef R(__cdecl *FP)(SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT); return ((FP)fn)(s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[9], s[10], s[11], s[12], s[13], s[14], s[15], s[16], s[17], s[18], s[19], s[20], s[21], s[22], s[23], s[24], s[25]); }
        case 27: { typedef R(__cdecl *FP)(SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT); return ((FP)fn)(s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[9], s[10], s[11], s[12], s[13], s[14], s[15], s[16], s[17], s[18], s[19], s[20], s[21], s[22], s[23], s[24], s[25], s[26]); }
        case 28: { typedef R(__cdecl *FP)(SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT); return ((FP)fn)(s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[9], s[10], s[11], s[12], s[13], s[14], s[15], s[16], s[17], s[18], s[19], s[20], s[21], s[22], s[23], s[24], s[25], s[26], s[27]); }
        case 29: { typedef R(__cdecl *FP)(SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT); return ((FP)fn)(s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[9], s[10], s[11], s[12], s[13], s[14], s[15], s[16], s[17], s[18], s[19], s[20], s[21], s[22], s[23], s[24], s[25], s[26], s[27], s[28]); }
        case 30: { typedef R(__cdecl *FP)(SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT); return ((FP)fn)(s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[9], s[10], s[11], s[12], s[13], s[14], s[15], s[16], s[17], s[18], s[19], s[20], s[21], s[22], s[23], s[24], s[25], s[26], s[27], s[28], s[29]); }
        case 31: { typedef R(__cdecl *FP)(SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT); return ((FP)fn)(s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[9], s[10], s[11], s[12], s[13], s[14], s[15], s[16], s[17], s[18], s[19], s[20], s[21], s[22], s[23], s[24], s[25], s[26], s[27], s[28], s[29], s[30]); }
        case 32: { typedef R(__cdecl *FP)(SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT, SlotT); return ((FP)fn)(s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[9], s[10], s[11], s[12], s[13], s[14], s[15], s[16], s[17], s[18], s[19], s[20], s[21], s[22], s[23], s[24], s[25], s[26], s[27], s[28], s[29], s[30], s[31]); }
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
    // frame->fnData->fn, and funnels a float/double return from XMM0 back into
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
    // the worker and the main VM never share mutable state (threads
    // receive value copies); functions/natives do not cross threads
    struct ThreadState
    {
        std::thread th;
        Value result;
        wstring error;
        bool done = false;
        // The worker thread's interpreter. It must outlive the THREAD
        // (the thread's stack is freed on exit) because the GC can still
        // reach envs it owns - see ThreadWorkerProc in builtins.cpp.
        std::shared_ptr<Interp> interp;
    };
    // heap job packet handed to a raw Win32 thread (thread.create):
    // keeps the VM nodes/args alive for the worker lifetime
    struct ThreadJob;
    extern std::map<HANDLE, std::shared_ptr<ThreadState>> g_threadStates;
    extern std::mutex g_threadStatesMutex;
    extern std::map<wstring, Value> g_threadSharedMap;
    extern std::recursive_mutex g_threadSharedMutex;
    struct RE
    {
        int k;                 // 0 char, 1 class, 2 dot, 3 group, 4 $ end, 7 balanced, 8 alt
        wchar_t ch = 0;
        wchar_t chClose = 0;
        unsigned cls = 0;      // 1=\d 2=\D 4=\w 8=\W 16=\s 32=\S
        vector<std::pair<wchar_t, wchar_t>> ranges;
        bool neg = false;
        int gi = -1;           // capture index (group)
        vector<RE> sub;
        vector<vector<RE>> alt;
        bool nocase = false;
        int min = 1, max = 1;  // max -1 = unbounded
        bool lazy = false;
    };

    inline constexpr size_t RENPOS = (size_t)-1;

    long long REMatchRep(const vector<RE>& els, size_t idx,
                         const wstring& s, size_t pos, int count,
                         vector<wstring>& caps, vector<size_t>& groupStarts,
                         bool anchored);

    // regex.cpp — pattern engine surface used by string.match/replace etc.
    bool RECompile(const wstring& pat, vector<vector<RE>>& branches,
                   bool* pAnchored, int* pGroups, wstring* err);
    bool RETry(const vector<vector<RE>>& branches, const wstring& s,
               size_t pos, bool anchored, vector<wstring>& caps,
               size_t* pEnd);
    // true when pat parses to a plain literal-char sequence (no
    // metacharacters) — lets find/match/count use memchr-speed
    // wstring::find instead of the per-position matcher
    bool REIsPlainLiteral(const wstring& pat);

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
        bool bPtrArray = false;
    };

    struct Env;

    // ---- variable-name interning (round 12) ----
    // Variable names are interned to uint32 ids so Env::vars uses plain
    // int keys: hot-path lookups (loop bodies, call frames) do zero
    // wstring hashing. The registry only grows — ids are valid for the
    // whole process lifetime, so a cached id (Node::nid,
    // Node::nameSeqOff) never goes stale. Id 0 is the "not interned" sentinel and is never
    // returned. The mutex guards REGISTRATION only: worker threads parse
    // scripts too (thread.create), but readers that already hold an id
    // never touch the registry, so hot paths run lock-free.
    inline std::mutex& NameIntMutex()
    {
        static std::mutex m;
        return m;
    }
    inline std::unordered_map<wstring, uint32_t>& NameIdMap()
    {
        static std::unordered_map<wstring, uint32_t> m;
        return m;
    }
    // id -> name; deque so element references stay stable while the
    // parser (possibly on a worker thread) registers more names.
    inline std::deque<wstring>& NameTable()
    {
        static std::deque<wstring> t;
        return t;
    }
    // Inner registration — caller MUST hold NameIntMutex. Round 14: the
    // name-seq append interns ids and appends the slice in ONE critical
    // section, and std::mutex is not recursive, so NameId delegates here.
    inline uint32_t NameIdLocked(const wstring& name)
    {
        auto& ids = NameIdMap();
        auto it = ids.find(name);
        if (it != ids.end())
            return it->second;
        uint32_t id = (uint32_t)NameTable().size() + 1;
        NameTable().emplace_back(name);
        ids.emplace(NameTable().back(), id);
        return id;
    }
    inline uint32_t NameId(const wstring& name)
    {
        std::lock_guard<std::mutex> lk(NameIntMutex());
        return NameIdLocked(name);
    }
    inline const wstring& NameOf(uint32_t id)
    {
        std::lock_guard<std::mutex> lk(NameIntMutex());
        return NameTable()[(size_t)(id - 1)];
    }
    // Prefer a cached id (AST node); fall back to live interning for
    // nodes the parser did not annotate (defensive — should be rare).
    inline uint32_t NameIdOf(const wstring& name, uint32_t cached)
    {
        return cached ? cached : NameId(name);
    }
    // Pre-interned ids for the runtime-synthesized bindings. Magic
    // statics make first use thread-safe; every later use is a plain
    // int read (no registry lock).
    inline uint32_t IdThis() { static uint32_t id = NameId(L"this"); return id; }
    inline uint32_t IdOwner() { static uint32_t id = NameId(L"owner"); return id; }
    inline uint32_t IdArg() { static uint32_t id = NameId(L"arg"); return id; }
    inline uint32_t IdDots() { static uint32_t id = NameId(L"..."); return id; }
    inline uint32_t IdSelf() { static uint32_t id = NameId(L"self"); return id; }

    // ---- round 14: name-id side table (per-node id slices) ----
    // Node::nameIds was a full vector<uint32_t> on EVERY AST node — 24
    // bytes regardless of use. The ids now live in process-wide paged
    // storage and a node stores just the +1 offset of its slice
    // (Node::nameSeqOff, 0 = not annotated).
    // Layout: 4096 static directory slots (atomic page pointers, never
    // moved) x 256 ids per heap page = 1,048,576 distinct identifiers.
    // Pages are allocated once and never freed or moved; appends only
    // write slots that were never published, so the published region is
    // immutable. Writers (the parser, possibly on a worker thread) run
    // under NameIntMutex; readers index with two plain loads:
    //   page = dirSlot[g >> 8].load(acquire); id = page[g & 255];
    // A node can only execute after its parse finished, so the slice a
    // reader indexes was fully appended before the read — the release
    // store of the page pointer and the acquire load above make the page
    // itself safe, and the parse→execute ordering makes the contents
    // visible (x86 TSO publishes stores immediately).
    inline std::atomic<uint32_t*>& NameSeqSlot(size_t page)
    {
        static std::atomic<uint32_t*> s[4096];
        return s[page];
    }
    // Total ids appended so far (writer side, guarded by NameIntMutex).
    inline uint32_t& NameSeqSize()
    {
        static uint32_t n = 0;
        return n;
    }
    // Read one id from a slice. offPlus1 is the Node's nameSeqOff (+1
    // sentinel, 0 = absent). Lock-free; see layout notes above.
    inline uint32_t NameSeqAt(uint32_t offPlus1, size_t i)
    {
        size_t g = (size_t)offPlus1 - 1 + i;
        uint32_t* pg = NameSeqSlot(g >> 8).load(std::memory_order_acquire);
        return pg[g & 255];
    }
    // Inner append — caller MUST hold NameIntMutex. Returns the 0-based
    // global offset of the appended slice.
    inline uint32_t NameSeqAppendLocked(const vector<wstring>& names)
    {
        uint32_t base = NameSeqSize();
        for (auto& nm : names)
        {
            uint32_t id = NameIdLocked(nm);
            uint32_t n = NameSeqSize();
            uint32_t* pg = NameSeqSlot(n >> 8).load(std::memory_order_relaxed);
            if (!pg)
            {
                pg = new uint32_t[256]();
                NameSeqSlot(n >> 8).store(pg, std::memory_order_release);
            }
            pg[n & 255] = id;
            NameSeqSize() = n + 1;
        }
        return base;
    }
    // Parser-facing append (takes the intern lock).
    inline uint32_t NameSeqAppend(const vector<wstring>& names)
    {
        std::lock_guard<std::mutex> lk(NameIntMutex());
        return NameSeqAppendLocked(names);
    }

    struct EnvWeakRef
    {
        std::weak_ptr<Env> wp;
        EnvWeakRef() = default;
        inline EnvWeakRef(const std::shared_ptr<Env>& sp);
        inline EnvWeakRef& operator=(const std::shared_ptr<Env>& sp);
        inline operator std::shared_ptr<Env>() const;
        inline explicit operator bool() const;
        inline std::shared_ptr<Env> lock() const;
        inline Env* get() const;
    };

    struct Env
    {
        // Owning frame chain: call/block frames are heap objects so a
        // closure can keep its defining frames (and their locals —
        // lexical upvalues) alive after the call returns. `self` is a
        // weak handle to break the self-referencing cycle: closures and
        // child frames promote it to shared_ptr on capture.
        // Machine-lifetime envs (globals) use a no-op deleter.
        std::shared_ptr<Env> parent;
        EnvWeakRef self;
        // Round 12: keys are interned name ids (see NameId above) —
        // loop/call hot paths hash nothing. Use NameOf(id) when a
        // human-readable name is needed (dumps, mirrors back to
        // wstring-keyed Table maps).
        std::unordered_map<uint32_t, Value> vars;
        // Round 13: RESIDENT envs (the Interp::globals root and the
        // main-script frame) store their vars in a slot vector indexed by
        // (intern id - 1). The intern table only ever grows, so an id's
        // slot is stable for the env's lifetime; a parallel bound-flag
        // byte separates "no entry" from "entry holding Null", exactly
        // matching the map's find() semantics. Call frames keep the map —
        // their transient ids (loop counters, temporaries) would blow the
        // slot vectors' memory. ALL vars access must go through the
        // accessors below (FindInSelf/VarAt/EraseVar/CountVar/ForEachVar/
        // clearVars): they dispatch on bSlots, so a single code path serves
        // both storages and a write can never land in the wrong one.
        bool bSlots = false;
        // 池化槽帧的槽空间上限（见 VarAt 越界退化）；驻留帧不受限。
        // 512 是第三十九轮的取值（当时普查的是 import 帧大 id 膨胀）；
        // 第五十轮普查反向实锤：常规脚本调用帧的 this/owner/参数 id 落在
        // 512-2048，每帧触发整帧退化 vars map → 每调用 5-6 个节点分配。
        // 2048 槽 ≈ 278KB/缓冲，池化复用下常驻个位数缓冲，24MB 预算充裕；
        // id ≥2048 的帧仍整帧退化。
        static constexpr size_t kSlotMax = 512;
        std::vector<Value> slots;
        std::vector<uint8_t> slotBound;
        // 第五十轮：稠密局部槽（帧内紧凑编号）。id 索引槽对晚定义 id
        // 根本性失效——参数 id 随 import 无界增长（普查：常规脚本调用帧
        // 的 id 已过 2048），kSlotMax 调到多大都会整帧退化 vars map →
        // 每调用 5-6 个节点堆分配。调用/块帧改走稠密模式：denseIds 内联
        // 记 8 个 intern id，**值存池化 8 槽缓冲 slots**（复用既有池，
        // 稳态零堆分配；内联 Value 数组的方案实测把 make_shared<Env>
        // 顶进 2K 桶、每调用 +618B 字节，已撤）。第 9 个不同 id 整帧退化。
        static constexpr size_t kDenseMax = 16;
        uint32_t denseIds[kDenseMax] = {};
        uint16_t nDense = 0;
        bool bDense = false;
        shared_ptr<Table> nsTable;   // namespace scope: bare names land here
        shared_ptr<Table> modTable;  // file/module scope table: scopeVars
        // S-2: the source file that owns the code running in this frame.
        // Module frames carry their own path; a call frame inherits it from
        // the DEFINING frame chain (a function of module A called from
        // module B still belongs to A).  Runtime errors use this to report
        // the file the line number actually comes from — a bare line number
        // is ambiguous once a library has been imported.
        wstring srcFile;
        // GC registry (gc.cpp): live frames are tracked so the collector
        // can reclaim frame cycles (a frame holding a closure that
        // captured that same frame). Round 34: membership is granted
        // lazily - only when a closure captures this frame
        // (GcRegisterEnv from the envUp promotion sites); frames that are
        // never captured cannot be cycle members and die by refcounting.
        long nEpoch = 0;
        bool bGcRegistered = false;
        Env();
        ~Env();
        // true while a namespace or module BODY executes directly in this
        // env: a `var` declared here is a namespace member, so
        // VarDecl mirrors it into the namespace table's map. Function-call
        // envs leave it false — their locals must stay local.
        bool bNsBody = false;
        // true when nsTable is an INSTANCE scope (method/ctor call frame,
        // `local.nsTable = inst`): one instance's members must never be
        // captured as a function's defining namespace. nsClosure capture
        // skips these frames so method-defined callbacks (win.form's
        // wndproc) still resolve namespace members like `_defaultProc`.
        bool bNsInst = false;
        // 槽帧保护开关：驻留帧（globals/mainEnv，entry.cpp 与 Interp()
        // 显式 bSlots=true）槽空间随 intern id 增长是一次性成本，保持槽化；
        // 池化槽帧（EnvUsePooledSlots）遇到越界 id 时整帧退化回 vars map
        // （kSlotMax 见 VarAt），避免晚定义函数的调用帧为几个局部变量
        // 撑起 max-id × sizeof(Value) 的槽向量。
        bool bSlotsKeep = false;
        // true while executing classBody member initializers
        bool bClassBody = false;
        // true in the ctor body frame: `this.x = v` there is member
        // initialization and must bypass the metatable's _set (the official
        // semantic — gdip.family's ctor assigns `this.name` while the
        // metaProperty declares name as a read-only _get property)
        bool bInstInit = false;
        // the instance table being constructed by this ctor frame (identity
        // only, no ownership — the instance stays alive through the frame's
        // `this` variable). A ctor's `this.x = v` is member initialization
        // ONLY when the write lands on THIS instance: after `this = other`
        // (official instance replacement, as win.ui.tracker does) the write
        // targets `other` and must go through the metatable's _set — that is
        // how the official tracker installs its wndproc message table.
        Table* instTable = nullptr;

        // ---- Round 13 unified var accessors (slot table or map) ----
        // Lookup in THIS env only (no chain walk). Returns nullptr when
        // the id has no entry here.
        Value* FindInSelf(uint32_t id)
        {
            if (bDense)
            {
                for (uint8_t i = 0; i < nDense; i++)
                    if (denseIds[i] == id) return &slots[i];
                return nullptr;
            }
            if (bSlots)
            {
                size_t s = (size_t)id - 1;
                return (s < slots.size() && slotBound[s]) ? &slots[s] : nullptr;
            }
            auto it = vars.find(id);
            return it != vars.end() ? &it->second : nullptr;
        }
        // map operator[] semantics: create a default (Null) entry when
        // absent, return a reference to it. NOTE: like unordered_map, a
        // growth (resize) invalidates references previously handed out by
        // this call — never nest two VarAt calls on the same env in one
        // expression, and never hold the reference across another VarAt.
        Value& VarAt(uint32_t id)
        {
            if (bDense)
            {
                for (uint8_t i = 0; i < nDense; i++)
                    if (denseIds[i] == id) return slots[i];
                if (nDense < kDenseMax)
                {
                    denseIds[nDense] = id;
                    return slots[nDense++];
                }
                // 第 9 个不同 id：整帧退化 vars map（一次性 O(本帧变量数)；
                // g_nSlotConv 延续诊断计数）。稠密缓冲归还池。
                g_nSlotConv++;
                for (uint8_t i = 0; i < nDense; i++)
                    vars[denseIds[i]] = std::move(slots[i]);
                nDense = 0;
                bDense = false;
                GcReleaseSlotBuf(std::move(slots));
                return vars[id];
            }
            if (bSlots)
            {
                size_t s = (size_t)id - 1;
                if (s >= slots.size())
                {
                    // 池化槽帧越界保护：槽下标 = 进程级 intern id - 1，id
                    // 随导入的模块线性增长。晚定义函数的调用帧只写几个
                    // 局部，却要把槽向量撑到 max-id × sizeof(Value)
                    //（win.ui 家族 ~5 万 id ≈ 4.4MB/帧），池预算只够缓存
                    // 个位数大缓冲，之后每帧重新 malloc+resize+清零——
                    // 普查 175 次 × ~400KB 全源于此。非驻留帧遇到越界
                    // id 时把已有槽位搬回 vars map、整帧退化为 map 模式
                    //（一次性 O(本帧变量数)），槽向量保持小而可池化。
                    if (!bSlotsKeep && s >= kSlotMax)
                    {
                        g_nSlotConv++;   // 诊断计数（BootDump 打印）
                        // 整帧退化为 map：已写入的槽位必须搬走（否则
                        // FindInSelf 落到 parent 链，局部变量凭空丢失），
                        // 一次性 O(本帧变量数)；槽缓冲归还池。
                        for (size_t i = 0; i < slots.size(); i++)
                            if (slotBound[i])
                                vars[(uint32_t)(i + 1)] = std::move(slots[i]);
                        GcReleaseBoundBuf(std::move(slotBound));
                        GcReleaseSlotBuf(std::move(slots));
                        bSlots = false;
                        return vars[id];
                    }
                    // 几何增长：intern id 进程级递增，每导入一个新模块
                    // max-id 就抬升；若逐次 +1 扩张，每次都要 realloc 拷贝
                    // 整段旧槽 + 清零尾部（普查：import 一次 175 次 ×
                    // ~400KB）。几何步长让容量内的后续 +1 写退化为 O(1)。
                    if (bSlotsKeep) g_nKeepResize++;
                    size_t ns = slots.size() ? slots.size() * 2 : 64;
                    if (ns < s + 1) ns = s + 1;
                    slots.resize(ns);
                    slotBound.resize(ns, 0);
                }
                slotBound[s] = 1;
                return slots[s];
            }
            return vars[id];
        }
        void EraseVar(uint32_t id)
        {
            if (bDense)
            {
                // 紧凑移除：erase 的语义是「FindInSelf 此后 miss」（落回
                // 父链），所以 id 必须从稠密表里消失——尾槽补位
                for (uint8_t i = 0; i < nDense; i++)
                {
                    if (denseIds[i] != id) continue;
                    slots[i] = std::move(slots[nDense - 1]);
                    denseIds[i] = denseIds[nDense - 1];
                    slots[nDense - 1] = Value();
                    nDense--;
                    return;
                }
                return;
            }
            if (bSlots)
            {
                size_t s = (size_t)id - 1;
                if (s < slots.size()) { slotBound[s] = 0; slots[s] = Value(); }
                return;
            }
            vars.erase(id);
        }
        size_t CountVar(uint32_t id)
        {
            return FindInSelf(id) ? 1 : 0;
        }
        // Iterate every bound entry: f(id, Value&). Read-only use (GC
        // marking, dumps). Do not add entries from inside the callback.
        template <class F> void ForEachVar(F&& f)
        {
            if (bDense)
            {
                for (uint8_t i = 0; i < nDense; i++)
                    f(denseIds[i], slots[i]);
                return;
            }
            if (bSlots)
            {
                for (size_t s = 0; s < slots.size(); s++)
                    if (slotBound[s]) f((uint32_t)(s + 1), slots[s]);
                return;
            }
            for (auto& kv : vars) f(kv.first, kv.second);
        }
        void clearVars()
        {
            if (bDense)
            {
                for (uint8_t i = 0; i < nDense; i++) slots[i] = Value();
                nDense = 0;
                return;
            }
            if (bSlots) { slots.clear(); slotBound.clear(); return; }
            vars.clear();
        }

        Value* Find(const wstring& k)
        {
            return FindById(NameId(k), k);
        }
        // Hot path: the parser cached the interned id on the AST node
        // (Node::nid), so loop bodies and call frames resolve names with
        // plain int keys — zero wstring hashing per lookup.
        Value* Find(uint32_t id, const wstring& k)
        {
            return FindById(id, k);
        }
        Value* FindById(uint32_t id, const wstring& k)
        {
            // 1. Local frames (frames that have a parent)
            for (Env* e = this; e && e->parent; e = e->parent.get())
            {
                if (Value* pv = e->FindInSelf(id))
                    return pv;
            }
            // 2. Built-in primitives like `type`, `call`, `loadcode` etc. are never shadowed by namespace members
            static auto isBuiltinPrimitive = [](const wstring& name) {
                switch (name.size())
                {
                case 4: return name == L"type" || name == L"call" || name == L"eval" || name == L"rget";
                case 5: return name == L"error";
                case 6: return name == L"assert" || name == L"invoke" || name == L"callex";
                case 8: return name == L"loadcode";
                case 9: return name == L"loadcodex";
                default: return false;
                }
            };
            bool bIsBuiltin = isBuiltinPrimitive(k);
            if (bIsBuiltin)
            {
                Env* root = this;
                while (root->parent) root = root->parent.get();
                if (Value* pv = root->FindInSelf(id))
                    return pv;
            }
            // 3. Namespace members in ambient namespace scopes
            for (Env* e = this; e; e = e->parent.get())
            {
                if (e->nsTable && !e->bNsInst && !bIsBuiltin)
                {
                    auto itS = e->nsTable->scopeVars.find(k);
                    if (itS != e->nsTable->scopeVars.end())
                        return &itS->second;
                    auto it2 = e->nsTable->map.find(k);
                    if (it2 != e->nsTable->map.end())
                        return &it2->second;
                    // In libraries where sub-namespaces share core declarations from sibling
                    // `object` class (e.g. py3.dict calling PyObject_GetAttrString declared in py3.object),
                    // fall back to looking up in parent namespace's `object` member table.
                    auto itObj = e->nsTable->map.find(L"object");
                    if (itObj != e->nsTable->map.end() && (itObj->second.t == Value::TableT || itObj->second.t == Value::Class) && itObj->second.tab)
                    {
                        auto itObjMem = itObj->second.tab->map.find(k);
                        if (itObjMem != itObj->second.tab->map.end())
                            return &itObjMem->second;
                    }
                }
                if (e->modTable && !isBuiltinPrimitive(k))
                {
                    auto itS = e->modTable->scopeVars.find(k);
                    if (itS != e->modTable->scopeVars.end())
                        return &itS->second;
                }
            }
            // 4. Global variables (root env where parent == nullptr)
            Env* root = this;
            while (root->parent) root = root->parent.get();
            if (Value* pv = root->FindInSelf(id))
                return pv;

            return nullptr;
        }
    };

    inline EnvWeakRef::EnvWeakRef(const std::shared_ptr<Env>& sp) : wp(sp) {}
    inline EnvWeakRef& EnvWeakRef::operator=(const std::shared_ptr<Env>& sp) { wp = sp; return *this; }
    inline EnvWeakRef::operator std::shared_ptr<Env>() const { return wp.lock(); }
    inline EnvWeakRef::operator bool() const { return !wp.expired(); }
    inline std::shared_ptr<Env> EnvWeakRef::lock() const { return wp.lock(); }
    inline Env* EnvWeakRef::get() const { auto sp = wp.lock(); return sp.get(); }

    inline void CleanDeadEnv(std::shared_ptr<Env>& env)
    {
        return;
    }

    // ---- Round 34 (gc.cpp): pooled slot buffers + lazy Env registry ----
    std::vector<Value> GcAcquireSlotBuf();
    std::vector<uint8_t> GcAcquireBoundBuf();
    void GcReleaseSlotBuf(std::vector<Value>&& v);
    void GcReleaseBoundBuf(std::vector<uint8_t>&& v);
    void GcRegisterEnv(Env* e);

    // Switch a fresh call/block frame onto pooled slot storage. The
    // reused buffer may be longer than this frame needs: stale slots stay
    // invisible because the bound flags are zeroed here, and FindInSelf /
    // ForEachVar / the accessors all gate on slotBound. The invariant
    // slotBound.size() >= slots.size() is kept so ForEachVar can index
    // the flags for every slot.
    inline void EnvStartDense(Env* e)
    {
        // 第五十轮：调用/块帧的稠密局部槽启动——denseIds 内联记 8 个
        // intern id，值存池化 8 槽缓冲（复用既有池，稳态零堆分配）。
        // id 索引槽对晚定义 id 根本性失效（参数 id 随 import 无界增长，
        // kSlotMax 调到多大都会整帧退化 vars map —— 普查实测每调用
        // 5-6 个节点分配）。新池首次取缓冲时分配 1KB，随释放进池循环。
        // ⚠ 必须在建帧点显式调用：第 46 轮回退后 EnvUsePooledSlots 已无
        // 调用点，只设 bDense 不取缓冲 = 写空向量（实测 0xC0000005）。
        e->slots = GcAcquireSlotBuf();
        if (e->slots.size() < Env::kDenseMax)
            e->slots.resize(Env::kDenseMax);
        e->nDense = 0;
        e->bDense = true;
    }

    inline void EnvUsePooledSlots(Env* e)
    {
        if (e->bSlotsKeep)
        {
            // 驻留帧（globals/mainEnv）保持 id 索引槽：槽空间随 intern
            // id 增长是一次性成本，池化缓冲几何增长后复用。
            e->slots = GcAcquireSlotBuf();
            e->slotBound = GcAcquireBoundBuf();
            e->bSlots = true;
            if (e->slotBound.size() < e->slots.size())
                e->slotBound.resize(e->slots.size(), 0);
            std::fill(e->slotBound.begin(), e->slotBound.end(), 0);
            return;
        }
        EnvStartDense(e);
    }

    // Invariant guard for the GC frame stack. The stack is popped with
    // pop_back(), so a missing push or an extra pop silently removes
    // ANOTHER frame's env from the root set - that env then looks
    // unreachable while its C++ frame is still running, and the collector
    // frees it under the VM's feet. Every push/pop site pairs its own
    // frame, so a mismatch here is always a bug.
    inline void GcFrameCheck(std::vector<std::shared_ptr<Env>>& frames,
                              Env* self, const char* where)
    {
        if (frames.empty() || frames.back().get() != self)
        {
            // %hs: `where` is a narrow string, and fwprintf's %s would
            // reinterpret those bytes as UTF-16 (garbage output).
            fwprintf(stderr, L"[GCFRAME] %hs: popping %p but top is %p (size=%zu)\n",
                     where, (void*)self,
                     frames.empty() ? nullptr : (void*)frames.back().get(),
                     frames.size());
            fflush(stderr);
        }
    }

    struct Interp;
    extern thread_local Interp* g_interpCurrent;   // M2: active interp for meta hooks
    // >0 while a metaProperty _get/_set FUNCTION is dispatching: inner
    // this[k] lookups must see RAW definition tables (never auto-call
    // getters, never re-enter the _get function)
    extern thread_local int g_metaDispatchDepth;
    extern thread_local std::unordered_set<Table*> g_metaDispatchActive;
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
        bool bUtf16 = false;         // Str literal: '...'u UTF-16 string
        bool bRawBytes = false;      // Str literal: raw binary bytes
        int line = 0;                // source line (round 30 error reporting)
        // Round 12: interned variable-name id for Ident/left-value nodes
        // (0 = not cached — NameIdOf falls back to live interning).
        uint32_t nid = 0;
        // Round 14: offset+1 of this node's names' id slice in the
        // process-wide name-seq pages (see NameSeqSlot above; 0 = not
        // annotated). Replaces the per-node vector<uint32_t> nameIds —
        // 24 bytes per node down to 4, and bind sites read ids by
        // NodeNameIdAt/ParamIdAt without touching a heap vector.
        uint32_t nameSeqOff = 0;
    };

    // Interned id of parameter i of a function value. The FuncLit node
    // slice in the name-seq pages is parser-filled, parallel to names /
    // Value::params; fall back to live interning when sizes disagree
    // (native-constructed function values).
    inline uint32_t ParamIdAt(const Value& f, size_t i)
    {
        if (f.fnData->fn && f.fnData->fn->nameSeqOff && i < f.fnData->params.size())
            return NameSeqAt(f.fnData->fn->nameSeqOff, i);
        return NameId(f.fnData->params[i]);
    }

    // Interned id of names[i] on a node (VarDecl / ForIn bind sites).
    inline uint32_t NodeNameIdAt(const PNode& n, size_t i)
    {
        if (n->nameSeqOff && i < n->names.size())
            return NameSeqAt(n->nameSeqOff, i);
        return NameId(n->names[i]);
    }

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
        std::map<HWND, int> hCtrlRev;       // handle -> control id (O(1) reverse lookup for WM_COMMAND/WM_NOTIFY dispatch)
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
    extern std::map<HWND, std::shared_ptr<FormState>> g_forms;
    LRESULT CALLBACK FormWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    // ---------------- GC (gc.cpp) ----------------
    // Live heap-object counters (every Table/Env registers itself).
    // Atomics: a worker thread allocates objects concurrently with the
    // main thread, so these counters are touched from several threads.
    extern std::atomic<long> g_liveTables;
    extern std::atomic<long> g_liveEnvs;
    // Total objects ever created — used to pace automatic collection.
    extern std::atomic<long> g_gcAllocTick;
    void GcDumpLive();
    // permanent GC roots: metatables kept alive by static members
    void GcAddRootTable(const std::shared_ptr<Table>& tb);
    // The live-object registry is shared by EVERY thread. Two threads
    // inserting into the same unordered_set without a lock corrupts the
    // heap (observed as STATUS_HEAP_CORRUPTION / 0xC0000374 with a
    // drifting access-violation site). Recursive: a collector that
    // holds the lock may run a finalizer, which allocates objects.
    extern std::recursive_mutex g_gcRegistryMutex;
    // Serializes collectors: the mark phase uses a shared scratch set.
    extern std::mutex g_gcCollectMutex;
    // Number of interpreters currently running script code. The
    // collector's mark phase only sees the roots of the interpreter that
    // runs it, so collecting while another interpreter runs on another
    // thread would free objects that thread still holds.
    extern std::atomic<int> g_activeVmThreads;
    // Statement epoch: bumped at every statement boundary. Heap objects
    // remember the epoch they were born in, so the collector can spare
    // the statement currently being evaluated.
    extern std::atomic<long> g_gcEpoch;
    // Epoch / root stacks are per-thread: their push/pop discipline is
    // stack-structured, so one thread's restore must never truncate
    // another thread's entries.
    extern thread_local std::vector<long> g_gcEpochStack;
    inline long GcProtectEpoch()
    {
        return g_gcEpochStack.empty() ? g_gcEpoch.load() : g_gcEpochStack.back();
    }
    // Explicit root stack: values that live in native C++ frames across a
    // possible collection point (call arguments, evaluation temporaries).
    extern thread_local std::vector<Value> g_gcRoots;
    struct GcRootScope
    {
        size_t base;
        GcRootScope() : base(g_gcRoots.size()) {}
        ~GcRootScope() { g_gcRoots.resize(base); }
        void add(const Value& v) { g_gcRoots.push_back(v); }
        void addAll(const std::vector<Value>& vs)
        {
            for (const auto& v : vs) g_gcRoots.push_back(v);
        }
    };

    // ---------------- fiber execution contexts ----------------
    // A fiber runs on its OWN C++ stack (Win32 fiber), so the VM's
    // stack-structured bookkeeping must travel with the fiber that pushed
    // it: the GC frame stack, the explicit root stack, the epoch stack and
    // the control-flow flags (return / break / continue) plus the pending
    // return value.
    //
    // Without this, a fiber that yields in the middle of a call leaves its
    // frame on top of the resumer's frame stack. The resumer's pop_back()
    // then removes the WRONG frame, so the collector loses live frames
    // (the same failure mode as "a running frame got collected"), and the
    // two execution streams' control-flow flags get mixed - a `return`
    // inside a fiber can abort the resumer's loop, and vice versa.
    struct VMFiber;
    struct FiberCtx
    {
        std::vector<std::shared_ptr<Env>> gcFrames;
        std::vector<Value> roots;
        std::vector<long> epochStack;
        int nReturn = 0, nBreak = 0, nContinue = 0;
        Value ret;
        Value fn;                     // the fiber's entry function (a GC root)
        void* osFiber = nullptr;      // the OS fiber this context runs on
        Value owner;                  // the meta-dispatch owner
        int metaDepth = 0;            // metaProperty dispatch depth
        std::unordered_set<Table*> metaActive;
        // The fiber's script value. WEAK on purpose: a strong reference here
        // would keep the table alive forever, and the table's reachability
        // is exactly what tells the collector that the fiber was abandoned.
        std::weak_ptr<Table> fiberVal;
        void* fiber = nullptr;             // the owning VMFiber
        VMFiber* runningFiber = nullptr;   // the fiber this context runs
    };

    // A fiber (纤程) runs on its own C++ stack. The state below belongs to
    // the fiber, not to the thread: the script value is the only handle the
    // VM has on it, and the execution context travels with the fiber.
    struct VMFiber
    {
        void* hFiber = nullptr;
        std::shared_ptr<FiberCtx> ctx;
        wstring status = L"suspended";   // suspended | running | dead
        std::vector<Value> inArgs;      // arguments for the next resume
        std::vector<Value> outVals;     // values yielded or returned
        void* resumeFiber = nullptr;    // OS fiber to switch back to
        FiberCtx* resumeCtx = nullptr;  // execution context to switch back to
    };


    // ---------------- module surface ----------------
    // sysapi.cpp — bare `::` system API resolution (::SetWindowPointer ...)
    struct Interp;
    Value ResolveSysApi(Interp* it, const wstring& name);

    // lexer.cpp
    bool IsIdentChar(wchar_t c);
    vector<Tok> Lex(const wstring& src, wstring* strError);

    // util.cpp
    struct NodioConfig
    {
        std::vector<std::wstring> extensions;
        std::vector<std::wstring> magicHeaders;
        std::wstring defaultTitle;
        std::wstring windowClass;
        std::wstring userAgent;
        std::wstring libDir;
    };
    const NodioConfig& GetNodioConfig();
    const std::vector<std::wstring>& GetConfiguredExtensions();
    const std::vector<std::wstring>& GetConfiguredMagicHeaders();
    bool HasScriptExtension(const std::wstring& filename);
    std::wstring StripScriptExtension(const std::wstring& filename);

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
                           bool bIsArray = false, bool bPtrArray = false);
    std::vector<StructFieldDef> ParseStructDef(const Value& vTab);
    int CalcStructTotalSize(const Value& vTab);
    // cbSlack: extra zeroed bytes appended to the marshalling buffer.
    // The declared layout stays authoritative (pOutTotal reports it); the
    // slack only guarantees that a callee reading a wider platform layout
    // stays inside the allocation.
    void* StructSerialize(const Value& vTab, int* pOutTotal = nullptr,
                          size_t cbSlack = 0);
    // Zeroed slack a struct marshalling buffer must carry so a callee that
    // uses a wider layout than the declared one (x64: ULONG_PTR = 8 bytes
    // while the official library declares `int` = 4 bytes) cannot read past
    // the allocation and hit an unmapped page.
    size_t StructOverreadSlack(const Value& vTab);
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

    // buffer tracking
    void RegisterBuffer(void* ptr, size_t size);
    void UnregisterBuffer(void* ptr);
    bool IsBuffer(const void* ptr);
    size_t GetBufferSize(const void* ptr);
    // AUDIT-11: dynbuf identity registry (see IsDynBuffer above)
    // (declared next to IsDynBuffer as well — this is the buffer-tracking
    // block's view of the same API)

    // time.cpp / builtins.cpp
    Value MakeTimeObject(const SYSTEMTIME& st, const wstring& fmt = L"%Y/%m/%d %H:%M:%S");

    // entry.cpp
    wstring EmbedRoot();
    int CheckSyntax(const wstring& src, wstring* strError);
    // S-2: the source file that owns the code running in `env` — walks the
    // defining-frame chain and returns the first frame that carries one
    // (a module frame carries its own path; a call frame inherits it from
    // the frame its function was DEFINED in, so a function of module A
    // called from module B is still attributed to A).
    const wstring* SourceFilePtrOfEnv(Env* env);
    // S-2: does the message already carry a location marker? The two
    // report points (the try/catch handler and the top-level report) must
    // not append a second one, so this is checked before appending.
    bool HasErrorLocation(const wstring& msg);
    // S-2: append the location of a runtime error to a message:
    //   "@ <file>:<line>"  when the owning file is known,
    //   "@ 行 <line>"      when it is not (a bare line number).
    // Official aardio keeps 文件 / 错误行号 in a structured error report
    // (see the field table in aardio.exe) and drops them entirely when the
    // project is published with debugging disabled; Nodio keeps them in the
    // message itself, so console output, the LSP and CI logs can all
    // attribute a failure to a file.
    void AppendErrorLocation(wstring* msg, const wstring& file, int line);
    // RunSource is declared in NodioVM.h (the public entry point) — it
    // must not be declared twice: two overloads with different arities
    // would make every 6-argument call ambiguous.

    // Resolve '~'-rooted path to a real disk path. '~' is the
    // application install root: the parent of NODIO_LIBDIR when set (dev/F5),
    // otherwise the running exe's own directory (a published app). The
    // library loads bundled DLLs as raw.loadDll("~\lib\...") — resolving
    // '~' to the exe dir alone misses them.
    inline wstring ResolveTildeDisk(const wstring& strPath)
    {
        if (strPath.empty() || strPath[0] != L'~') return strPath;
        wstring rel = strPath.substr(1);
        while (!rel.empty() && (rel[0] == L'\\' || rel[0] == L'/'))
            rel = rel.substr(1);

        // Check 64-bit overlay first (e.g. D:\Nodio\lib\...)
        if (rel.rfind(L"lib\\", 0) == 0 || rel.rfind(L"lib/", 0) == 0)
        {
            wstring sub = rel.substr(4);
            wchar_t szOverlay[MAX_PATH] = L"";
            if (GetEnvironmentVariableW(L"NODIO_OVERLAY_LIBDIR", szOverlay, MAX_PATH) > 0)
            {
                wstring cand = wstring(szOverlay) + L"\\" + sub;
                if (_waccess(cand.c_str(), 0) == 0) return cand;
            }
#if defined(_WIN64) || defined(__x86_64__)
            if (_waccess((L"D:\\Nodio\\lib\\" + sub).c_str(), 0) == 0)
                return L"D:\\Nodio\\lib\\" + sub;
#endif
            if (_waccess((L"D:\\aardio\\lib\\" + sub).c_str(), 0) == 0)
                return L"D:\\aardio\\lib\\" + sub;
        }

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
