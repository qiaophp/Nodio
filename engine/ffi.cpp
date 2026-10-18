#include "engine/interp.h"
#include <deque>

namespace NodioVM
{
    // raw-buffer wrappers store bytes one-per-wchar in a wstring; unpack
    // back to raw bytes without any codepage translation
    static std::string BytesOf(const wstring& ws)
    {
        std::string out;
        out.reserve(ws.size());
        for (wchar_t c : ws) out.push_back((char)(unsigned char)c);
        return out;
    }

    int ApiTypeOf(const wstring& tRaw, bool* pByRef)
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
        if (w == L"int64" || w == L"longlong" || w == L"i64" ||
            w == L"longlong" || w == L"qword")
            return 1;
        if (w == L"word" || w == L"ushort" || w == L"short" || w == L"wchar")
            return 2;
        if (w == L"byte" || w == L"uchar" || w == L"char" || w == L"int8")
            return 3;
        if (w == L"bool" || w == L"boolean")
            return 4;
        if (w == L"ptr" || w == L"pointer" || w == L"lpvoid")
            return 5;
        if (w == L"addr" || w == L"handle" || w == L"hwnd" || w == L"hkey")
            return 12;
        if (w == L"str")
            return 6;
        if (w == L"string" || w == L"strb" || w == L"lpcstr")
            return 11;
        if (w == L"wstr" || w == L"ustring" || w == L"ustr" ||
            w == L"lpwstr" || w == L"bstr")
            return 7;
        if (w == L"struct")
            return 8;
        if (w == L"float")
            return 9;
        if (w == L"double")
            return 10;
        return -1;
    }

#if defined(_M_IX86)
    __int64 __fastcall CallbackShim(CbCtx* ctx, unsigned char* stack)
#else
    __int64 CallbackShim(CbCtx* ctx, unsigned char* stack,
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
            else if (pt == 5)
            {
                void* p = *(void**)pArg;
                args.push_back(p ? Value::Pointer_(p) : Value());
                pArg += 4;
            }
            else if (pt == 12)
            {
                unsigned int v = *(unsigned int*)pArg;
                args.push_back(Value::Num_((double)v));
                pArg += 4;
            }
            else if (pt == 4)
            {
                int v = *(int*)pArg;
                args.push_back(Value::Bool_(v != 0));
                pArg += 4;
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
            else if (pt == 5)     // pointer
            {
                unsigned __int64 v = (i < 4) ? regs[i] : *(unsigned __int64*)pArg;
                if (i >= 4) pArg += 8;
                args.push_back(v ? Value::Pointer_((void*)(INT_PTR)v) : Value());
            }
            else if (pt == 4)     // bool
            {
                unsigned __int64 v = (i < 4) ? regs[i] : *(unsigned __int64*)pArg;
                if (i >= 4) pArg += 8;
                args.push_back(Value::Bool_(v != 0));
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
    __int64 __fastcall CallbackShimX86(CbCtx* ctx,
                                              unsigned char* stack)
    {
        return CallbackShim(ctx, stack);
    }
#endif

    void CallbackTrampoline(CbCtx* ctx)
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
        int popBytes = (ctx->sig.bCdecl) ? 0 : (slots * 4); // stdcall callee cleanup, cdecl caller cleans
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
        const unsigned char sFrame[] = {
            0x48, 0x83, 0xEC, 0x78 };              // sub rsp,0x78
        const unsigned char sXmmSave[] = {
            0x66, 0x0F, 0xD6, 0x04, 0x24,          // movq [rsp],xmm0
            0x66, 0x0F, 0xD6, 0x44, 0x24, 0x08,    // movq [rsp+8],xmm1
            0x66, 0x0F, 0xD6, 0x44, 0x24, 0x10,    // movq [rsp+16],xmm2
            0x66, 0x0F, 0xD6, 0x44, 0x24, 0x18 };  // movq [rsp+24],xmm3
        const unsigned char sIntSave[] = {
            0x48, 0x89, 0x8C, 0x24, 0x80, 0x00, 0x00, 0x00, // mov [rsp+0x78+8],rcx
            0x48, 0x89, 0x94, 0x24, 0x88, 0x00, 0x00, 0x00, // mov [rsp+0x78+16],rdx
            0x4C, 0x89, 0x84, 0x24, 0x90, 0x00, 0x00, 0x00, // mov [rsp+0x78+24],r8
            0x4C, 0x89, 0x8C, 0x24, 0x98, 0x00, 0x00, 0x00 };// mov [rsp+0x78+32],r9
        const unsigned char sGo[] = {
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
    bool ParseApiSig(const wstring& sig, ApiSig* out)
    {
        size_t pl = sig.find(L'(');
        size_t pr = sig.rfind(L')');
        if (pl == wstring::npos)
            return false;
        wstring strRet = sig.substr(0, pl);
        wstring strArgs, tail;
        if (pr != wstring::npos && pr > pl)
        {
            strArgs = sig.substr(pl + 1, pr - pl - 1);
            tail = sig.substr(pr + 1);
        }
        else
        {
            // official lib quirk: truncated signatures ship without the
            // closing paren ("INT(POINTER hProcess") — tolerate them
            strArgs = sig.substr(pl + 1);
        }
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
            // `addr` returns are plain NUMBERS;
            // `pointer` returns are the pointer datatype (type()=="pointer")
            else if (rt == L"addr" || rt == L"handle")
                out->ret = ARAddr;
            else if (rt == L"ptr" || rt == L"pointer" ||
                     rt == L"struct") out->ret = ARPtr;
            else if (rt == L"int" || rt == L"long" || rt == L"dword" ||
                     rt == L"uint") out->ret = ARInt;
            // a custom type name in the return slot (py2's "pyObject(...)")
            // is an opaque machine word — treat it as a pointer
            else out->ret = ARPtr;
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
                // api signatures may name custom types (a struct or
                // opaque handle, e.g. py2's "PyObject o"). They are passed
                // as a machine word, so treat an unknown type as a pointer
                // rather than rejecting the whole signature.
                if (p < 0) p = 5;   // ARPtr
                out->params.push_back(p | (bRef ? 0x100 : 0));
            }
            pos = comma + 1;
        }
        return true;
    }

    void* EmitX64CallStub(const ApiSig& sig)
    {
        unsigned char* c = (unsigned char*)VirtualAlloc(
            nullptr, 512, MEM_COMMIT | MEM_RESERVE,
            PAGE_EXECUTE_READWRITE);
        int n = 0;
        auto put = [&c, &n](const unsigned char* p, int len)
        { memcpy(c + n, p, (size_t)len); n += len; };
        auto put32 = [&c, &n](unsigned int v)
        { memcpy(c + n, &v, 4); n += 4; };
        // push rbx ; mov rbx, rcx ; sub rsp,0x120 (16-aligned: entry ≡8,
        // push →0, 0x120 ≡0 → call-site rsp ≡0 mod 16)
        const unsigned char sPro[] = {
            0x53,
            0x48, 0x89, 0xCB,
            0x48, 0x81, 0xEC, 0x20, 0x01, 0x00, 0x00 };
        put(sPro, (int)sizeof(sPro));
        // mov REG, [rbx+disp32] — mod 10, reg=dest, rm=011 (rbx)
        //   rcx: 48 8B 8B dd..  rdx: 48 8B 93 dd..
        //   r8 : 4C 8B 83 dd..  r9 : 4C 8B 8B dd..
        //   rax: 48 8B 83 dd..
        const unsigned char kMovRbx[][3] = {
            { 0x48, 0x8B, 0x8B },   // rcx
            { 0x48, 0x8B, 0x93 },   // rdx
            { 0x4C, 0x8B, 0x83 },   // r8
            { 0x4C, 0x8B, 0x8B },   // r9
            { 0x48, 0x8B, 0x83 } }; // rax
        // movsd/movss XMMN, [rbx+disp32] — F2/F3 0F 10 mod(0x83|reg<<3)
        const unsigned char kMsdX[][3] = {
            { 0xF2, 0x0F, 0x10 }, { 0xF2, 0x0F, 0x10 },
            { 0xF2, 0x0F, 0x10 }, { 0xF2, 0x0F, 0x10 } };
        (void)kMsdX;
        // x64 ABI: register slots are tied to PARAMETER POSITION —
        // position 1..4 use rcx/rdx/r8/r9 for integers and xmm0..3 for
        // floats *at that same position*. A float at position 2 takes
        // xmm1 while position 3's integer still takes r8
        // (GdipCreateFont's (ptr,float,int,int,ptr&) was the proof:
        // independent numbering put emSize in xmm0 and style in rdx,
        // and GDI+ read garbage → InvalidParameter)
        int nIntReg = 0, nStack = 0;
        for (size_t i = 0; i < sig.params.size() && i < 32; i++)
        {
            int pt = sig.params[i] & 0xFF;
            bool bByRef = (sig.params[i] & 0x100) != 0;
            bool bFloat = !bByRef && (pt == 9 || pt == 10);
            unsigned int disp = (unsigned int)(8 + 8 * i);
            if (i < 4)
            {
                if (bFloat)
                {
                    unsigned char o[3] = { pt == 10 ? 0xF2u : 0xF3u,
                                           0x0F, 0x10 };
                    c[n++] = o[0]; c[n++] = o[1]; c[n++] = o[2];
                    c[n++] = (unsigned char)(0x83 | (i << 3));
                    put32(disp);
                }
                else
                {
                    put(kMovRbx[i], 3);
                    put32(disp);
                }
                continue;
            }
            // position 5+: stack
            put(kMovRbx[4], 3);               // mov rax,[rbx+disp]
            put32(disp);
            unsigned int dispStack = (unsigned int)(0x20 + 8 * nStack);
            if (dispStack < 0x80)
            {
                unsigned char st[4] = { 0x48, 0x89, 0x44, 0x24 }; // mov [rsp+disp8], rax
                put(st, 4);
                c[n++] = (unsigned char)dispStack;
            }
            else
            {
                unsigned char st[4] = { 0x48, 0x89, 0x84, 0x24 }; // mov [rsp+disp32], rax
                put(st, 4);
                put32(dispStack);
            }
            nStack++;
        }
        c[n++] = 0xFF; c[n++] = 0x13;   // call qword [rbx]
        if (sig.ret == ARDbl)
        {
            if (sig.bRetFloat)
            {
                const unsigned char cvt[] = {
                    0xF3, 0x0F, 0x5A, 0xC0 };   // cvtss2sd xmm0,xmm0
                put(cvt, 4);
            }
            const unsigned char movq[] = {
                0x66, 0x48, 0x0F, 0x7E, 0xC0 }; // movq rax,xmm0
            put(movq, 5);
        }
        const unsigned char sEp[] = {
            0x48, 0x81, 0xC4, 0x20, 0x01, 0x00, 0x00, // add rsp,0x120
            0x5B,                                     // pop rbx
            0xC3 };                                   // ret
        put(sEp, (int)sizeof(sEp));
        FlushInstructionCache(GetCurrentProcess(), c, n);
        return c;
    }

    int GetStructFieldSize(const wstring& type, int arrCount, bool bIsArray, bool bPtrArray)
    {
        wstring t = type;
        for (auto& c : t) c = towlower(c);
        if (arrCount < 0) arrCount = 0;
        if (!bIsArray) arrCount = 1;
        if (bPtrArray) return (int)sizeof(void*) * arrCount;
        if (t == L"byte" || t == L"uchar" || t == L"char" || t == L"int8") return 1 * arrCount;
        if (t == L"word" || t == L"ushort" || t == L"short" || t == L"wchar") return 2 * arrCount;
        if (t == L"int" || t == L"uint" || t == L"dword" || t == L"bool") return 4 * arrCount;
        if (t == L"int64" || t == L"uint64" || t == L"long" || t == L"ulong" || t == L"long64" || t == L"ulong64" || t == L"i64" || t == L"qword") return 8 * arrCount;
        if (t == L"float") return 4 * arrCount;
        if (t == L"double") return 8 * arrCount;
        if (t == L"ustring" || t == L"ustr" || t == L"wstr" || t == L"bstr")
            return bIsArray ? (2 * arrCount) : (int)sizeof(void*);
        if (t == L"string" || t == L"str" || t == L"strb" || t == L"lpcstr")
            return bIsArray ? (1 * arrCount) : (int)sizeof(void*);
        if (t == L"pointer" || t == L"ptr" || t == L"addr" || t == L"handle")
            return (int)sizeof(void*) * arrCount;
        return 4 * arrCount;
    }

    static Value GetTableElement(const Value& vTab, size_t idx)
    {
        if (vTab.t != Value::TableT && vTab.t != Value::Class) return Value();
        if (!vTab.tab) return Value();
        if (idx < vTab.tab->arr.size() && vTab.tab->arr[idx].t != Value::Null)
            return vTab.tab->arr[idx];
        wstring k = std::to_wstring(idx + 1);
        auto it = vTab.tab->map.find(k);
        if (it != vTab.tab->map.end())
            return it->second;
        return Value();
    }

    static size_t GetTableLength(const Value& vTab)
    {
        if (vTab.t != Value::TableT && vTab.t != Value::Class) return 0;
        if (!vTab.tab) return 0;
        size_t sz = vTab.tab->arr.size();
        if (sz > 0) return sz;
        auto ilen = vTab.tab->map.find(L"length");
        if (ilen != vTab.tab->map.end()) return (size_t)BNum(ilen->second);
        size_t count = 0;
        while (vTab.tab->map.find(std::to_wstring(count + 1)) != vTab.tab->map.end())
            count++;
        return count;
    }

    std::vector<StructFieldDef> ParseStructDef(const Value& vTab)
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
                bool bPtrArr = false;
                if (lb != wstring::npos)
                {
                    bArr = true;
                    sname = srest.substr(0, lb);
                    size_t rb = srest.find(L']', lb);
                    wstring snum = (rb != wstring::npos) ? srest.substr(lb + 1, rb - lb - 1) : L"";
                    nArr = snum.empty() ? 0 : _wtoi(snum.c_str());
                    while (!sname.empty() && (sname.back() == L' ' || sname.back() == L'\t'))
                        sname.pop_back();
                    if (nArr <= 0)
                    {
                        if (vTab.t == Value::TableT && vTab.tab)
                        {
                            auto iv = vTab.tab->map.find(sname);
                            if (iv != vTab.tab->map.end())
                            {
                                if (iv->second.t == Value::TableT && iv->second.tab)
                                {
                                    nArr = (int)GetTableLength(iv->second);
                                }
                                else if (iv->second.t == Value::Str)
                                {
                                    // official contract: aardio strings are
                                    // UTF-8 BYTE strings — #s is the byte
                                    // length, so a BYTE[]/string[] array
                                    // sized from a string value must use
                                    // the UTF-8 byte count, not the wchar
                                    // count ("你的密钥&" = 13 bytes, not 5)
                                    if (iv->second.bRawBytes)
                                        nArr = (int)iv->second.s.size();
                                    else
                                    {
                                        int cb = WideCharToMultiByte(CP_UTF8, 0,
                                            iv->second.s.c_str(),
                                            (int)iv->second.s.size(),
                                            nullptr, 0, nullptr, nullptr);
                                        nArr = cb > 0 ? cb : 0;
                                    }
                                }
                                else if (iv->second.t == Value::Pointer && iv->second.ptr)
                                {
                                    size_t bsz = GetBufferSize(iv->second.ptr);
                                    if (bsz > 0) nArr = (int)bsz;
                                }
                            }
                        }
                    }
                    wstring lowerType = stype;
                    for (auto& c : lowerType) c = towlower(c);
                    if (lowerType == L"string" || lowerType == L"str" || lowerType == L"strb" ||
                        lowerType == L"lpcstr" || lowerType == L"ustring" || lowerType == L"ustr" ||
                        lowerType == L"wstr" || lowerType == L"bstr" || lowerType == L"pointer" ||
                        lowerType == L"ptr")
                    {
                        if (snum.empty())
                            bPtrArr = true;
                        else if (vTab.t == Value::TableT && vTab.tab)
                        {
                            auto iv = vTab.tab->map.find(sname);
                            if (iv != vTab.tab->map.end() && iv->second.t == Value::TableT)
                                bPtrArr = true;
                        }
                    }
                }
                while (!sname.empty() && (sname.back() == L' ' || sname.back() == L'\t'))
                    sname.pop_back();
                fields.push_back({ sname, stype, nArr, bArr, bPtrArr });
            }
        }
        else if (is->second.t == Value::TableT && is->second.tab)
        {
            for (auto& kv : is->second.tab->map)
            {
                wstring t = kv.second.ToString();
                int nArr = 1;
                bool bArr = false;
                bool bPtrArr = false;
                size_t lb = t.find(L'[');
                wstring stype = t;
                if (lb != wstring::npos)
                {
                    bArr = true;
                    stype = t.substr(0, lb);
                    size_t rb = t.find(L']', lb);
                    wstring snum = (rb != wstring::npos) ? t.substr(lb + 1, rb - lb - 1) : L"";
                    nArr = snum.empty() ? 0 : _wtoi(snum.c_str());
                    if (nArr <= 0)
                    {
                        if (vTab.t == Value::TableT && vTab.tab)
                        {
                            auto iv = vTab.tab->map.find(kv.first);
                            if (iv != vTab.tab->map.end() && iv->second.t == Value::TableT && iv->second.tab)
                            {
                                nArr = (int)GetTableLength(iv->second);
                            }
                        }
                    }
                    wstring lowerType = stype;
                    for (auto& c : lowerType) c = towlower(c);
                    if (lowerType == L"string" || lowerType == L"str" || lowerType == L"strb" ||
                        lowerType == L"lpcstr" || lowerType == L"ustring" || lowerType == L"ustr" ||
                        lowerType == L"wstr" || lowerType == L"bstr" || lowerType == L"pointer" ||
                        lowerType == L"ptr")
                    {
                        if (snum.empty())
                            bPtrArr = true;
                        else if (vTab.t == Value::TableT && vTab.tab)
                        {
                            auto iv = vTab.tab->map.find(kv.first);
                            if (iv != vTab.tab->map.end() && iv->second.t == Value::TableT)
                                bPtrArr = true;
                        }
                    }
                }
                fields.push_back({ kv.first, stype, nArr, bArr, bPtrArr });
            }
        }
        return fields;
    }

    // natural member alignment per API type (MSVC default packing) —
    // without this a pointer after a 4-byte member lands at offset 4 on
    // x64 and Win32 reads a garbage pointer (SECURITY_ATTRIBUTES broke
    // CreatePipe exactly so)
    int GetStructFieldAlign(const wstring& type)
    {
        wstring t = type;
        for (auto& c : t) c = towlower(c);
        if (t == L"pointer" || t == L"ptr" || t == L"addr" ||
            t == L"handle" || t == L"ustring" || t == L"ustr" ||
            t == L"wstr" || t == L"bstr" || t == L"string" || t == L"str" ||
            t == L"strb" || t == L"lpcstr")
            return (int)sizeof(void*);
        if (t == L"int64" || t == L"uint64" || t == L"long" || t == L"ulong" ||
            t == L"long64" || t == L"ulong64" || t == L"i64" || t == L"qword" || t == L"double")
            return 8;
        if (t == L"word" || t == L"ushort" || t == L"short" || t == L"wchar")
            return 2;
        if (t == L"byte" || t == L"uchar" || t == L"char" || t == L"int8")
            return 1;
        return 4;
    }

    // a nested struct aligns to its widest member (recursive, MSVC rule)
    int CalcStructAlign(const Value& vTab)
    {
        int maxA = 1;
        for (auto& f : ParseStructDef(vTab))
        {
            wstring t = f.type;
            for (auto& c : t) c = towlower(c);
            int a = 1;
            if (f.bPtrArray)
            {
                a = (int)sizeof(void*);
            }
            else if ((t == L"struct" || t == L"union") && vTab.tab)
            {
                auto itM = vTab.tab->map.find(f.name);
                if (itM != vTab.tab->map.end() &&
                    (itM->second.t == Value::TableT ||
                     itM->second.t == Value::Class) &&
                    itM->second.tab)
                {
                    if (t == L"union")
                    {
                        int uMaxA = 1;
                        auto uFields = ParseStructDef(itM->second);
                        for (auto& uf : uFields)
                        {
                            wstring ut = uf.type;
                            for (auto& c : ut) c = towlower(c);
                            int a = 1;
                            if ((ut == L"struct" || ut == L"union") && itM->second.tab)
                            {
                                auto itU = itM->second.tab->map.find(uf.name);
                                if (itU != itM->second.tab->map.end() &&
                                    (itU->second.t == Value::TableT || itU->second.t == Value::Class) &&
                                    itU->second.tab)
                                    a = CalcStructAlign(itU->second);
                            }
                            else
                                a = GetStructFieldAlign(uf.type);
                            if (a > uMaxA) uMaxA = a;
                        }
                        a = uMaxA;
                    }
                    else if (f.bIsArray)
                    {
                        Value elem = GetTableElement(itM->second, 0);
                        if (elem.t == Value::TableT || elem.t == Value::Class)
                            a = CalcStructAlign(elem);
                    }
                    else
                    {
                        a = CalcStructAlign(itM->second);
                    }
                }
            }
            else
                a = GetStructFieldAlign(f.type);
            if (a > maxA) maxA = a;
        }
        return maxA;
    }

    static int AlignOffset(int off, int al)
    {
        return (al > 1) ? ((off + al - 1) / al) * al : off;
    }

    int CalcStructTotalSize(const Value& vTab)
    {
        auto fields = ParseStructDef(vTab);
        int total = 0;
        int maxA = 1;
        for (auto& f : fields)
        {
            wstring t = f.type;
            for (auto& c : t) c = towlower(c);
            if ((t == L"struct" || t == L"union") && vTab.tab)
            {
                auto itM = vTab.tab->map.find(f.name);
                if (itM != vTab.tab->map.end() && (itM->second.t == Value::TableT || itM->second.t == Value::Class))
                {
                    if (t == L"union")
                    {
                        int uMaxSz = 0;
                        int uMaxA = 1;
                        auto uFields = ParseStructDef(itM->second);
                        for (auto& uf : uFields)
                        {
                            wstring ut = uf.type;
                            for (auto& c : ut) c = towlower(c);
                            int subSz = 0;
                            int subA = 1;
                            if ((ut == L"struct" || ut == L"union") && itM->second.tab)
                            {
                                auto itU = itM->second.tab->map.find(uf.name);
                                if (itU != itM->second.tab->map.end() && (itU->second.t == Value::TableT || itU->second.t == Value::Class))
                                {
                                    subSz = CalcStructTotalSize(itU->second);
                                    subA = CalcStructAlign(itU->second);
                                }
                            }
                            else
                            {
                                subSz = GetStructFieldSize(uf.type, uf.arrCount, uf.bIsArray, uf.bPtrArray);
                                subA = GetStructFieldAlign(uf.type);
                            }
                            if (subSz > uMaxSz) uMaxSz = subSz;
                            if (subA > uMaxA) uMaxA = subA;
                        }
                        total = AlignOffset(total, uMaxA);
                        uMaxSz = AlignOffset(uMaxSz, uMaxA);
                        total += uMaxSz;
                        if (uMaxA > maxA) maxA = uMaxA;
                        continue;
                    }
                    if (f.bIsArray)
                    {
                        int count = f.arrCount > 0 ? f.arrCount : (int)GetTableLength(itM->second);
                        Value elem = GetTableElement(itM->second, 0);
                        int subSz = (elem.t == Value::TableT || elem.t == Value::Class) ? CalcStructTotalSize(elem) : 0;
                        int subA = (elem.t == Value::TableT || elem.t == Value::Class) ? CalcStructAlign(elem) : 1;
                        total = AlignOffset(total, subA);
                        total += (subSz > 0 ? subSz : 4) * (count > 0 ? count : 1);
                        if (subA > maxA) maxA = subA;
                    }
                    else
                    {
                        int subSz = CalcStructTotalSize(itM->second);
                        int subA = CalcStructAlign(itM->second);
                        total = AlignOffset(total, subA);
                        total += (subSz > 0 ? subSz : 4);
                        if (subA > maxA) maxA = subA;
                    }
                    continue;
                }
            }
            int al = f.bPtrArray ? (int)sizeof(void*) : GetStructFieldAlign(f.type);
            total = AlignOffset(total, al);
            total += GetStructFieldSize(f.type, f.arrCount, f.bIsArray, f.bPtrArray);
            if (al > maxA) maxA = al;
        }
        return AlignOffset(total, maxA);
    }

    void* StructSerialize(const Value& vTab, int* pOutTotal)
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
        Value s_nullVal;
        for (auto& f : fields)
        {
            auto iv = vTab.tab->map.find(f.name);
            const Value& fv = (iv != vTab.tab->map.end()) ? iv->second : s_nullVal;
            wstring t = f.type;
            for (auto& c : t) c = towlower(c);
            int sz = GetStructFieldSize(f.type, f.arrCount, f.bIsArray, f.bPtrArray);
            wstring lowerName = f.name;
            for (auto& c : lowerName) c = towlower(c);
            {
                int al;
                if (f.bPtrArray)
                    al = (int)sizeof(void*);
                else if (t == L"struct" || t == L"union")
                {
                    auto itM2 = vTab.tab->map.find(f.name);
                    if (itM2 != vTab.tab->map.end() &&
                        (itM2->second.t == Value::TableT ||
                         itM2->second.t == Value::Class) &&
                        itM2->second.tab)
                        al = CalcStructAlign(itM2->second);
                    else
                        al = 1;
                }
                else
                    al = GetStructFieldAlign(f.type);
                off = AlignOffset(off, al);
            }
            if (f.bPtrArray)
            {
                int count = f.arrCount > 0 ? f.arrCount : (int)GetTableLength(fv);
                thread_local std::deque<std::string> s_bytesKeep;
                thread_local std::deque<std::wstring> s_wstrKeep;
                for (int k = 0; k < count; k++)
                {
                    Value elem = GetTableElement(fv, k);
                    void* ptr = nullptr;
                    if (elem.t == Value::Str)
                    {
                        bool bIsBytes = true;
                        for (wchar_t c : elem.s)
                        {
                            if ((unsigned short)c > 0xFF)
                            {
                                bIsBytes = false;
                                break;
                            }
                        }
                        if (bIsBytes)
                        {
                            s_bytesKeep.push_back(BytesOf(elem.s));
                            ptr = (void*)s_bytesKeep.back().data();
                        }
                        else if (t == L"ustring" || t == L"ustr" || t == L"wstr" || t == L"bstr")
                        {
                            s_wstrKeep.push_back(elem.s);
                            ptr = (void*)s_wstrKeep.back().c_str();
                        }
                        else
                        {
                            std::string sA((size_t)WideCharToMultiByte(CP_ACP, 0, elem.s.c_str(),
                                (int)elem.s.size(), nullptr, 0, nullptr, nullptr), 0);
                            if (!sA.empty())
                                WideCharToMultiByte(CP_ACP, 0, elem.s.c_str(), (int)elem.s.size(),
                                                   &sA[0], (int)sA.size(), nullptr, nullptr);
                            s_bytesKeep.push_back(std::move(sA));
                            ptr = (void*)s_bytesKeep.back().c_str();
                        }
                    }
                    else if (elem.t == Value::Pointer)
                    {
                        ptr = elem.ptr;
                    }
                    else if (elem.t == Value::Num)
                    {
                        ptr = (void*)(INT_PTR)(long long)elem.n;
                    }
                    *(void**)(buf + off + k * sizeof(void*)) = ptr;
                }
            }
            else if ((lowerName == L"cbsize" || lowerName == L"dwsize") && (t == L"int" || t == L"uint" || t == L"dword"))
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
                    if (f.bIsArray)
                    {
                        int count = f.arrCount > 0 ? f.arrCount : (int)GetTableLength(fv);
                        Value elem0 = GetTableElement(fv, 0);
                        int elemSz = (elem0.t == Value::TableT || elem0.t == Value::Class) ? CalcStructTotalSize(elem0) : 0;
                        if (elemSz <= 0) elemSz = 4;
                        for (int k = 0; k < count; k++)
                        {
                            Value elem = GetTableElement(fv, k);
                            if ((elem.t == Value::TableT || elem.t == Value::Class) && elem.tab)
                            {
                                int subTotal = 0;
                                void* pSub = StructSerialize(elem, &subTotal);
                                if (pSub)
                                {
                                    memcpy(buf + off + k * elemSz, pSub, subTotal < elemSz ? subTotal : elemSz);
                                    HeapFree(GetProcessHeap(), 0, pSub);
                                }
                            }
                        }
                        sz = elemSz * (count > 0 ? count : 1);
                    }
                    else
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
            }
            else if (t == L"union")
            {
                if ((fv.t == Value::TableT || fv.t == Value::Class) && fv.tab)
                {
                    auto uFields = ParseStructDef(fv);
                    int uMaxSz = 0;
                    int uMaxA = 1;
                    for (auto& uf : uFields)
                    {
                        wstring ut = uf.type;
                        for (auto& c : ut) c = towlower(c);
                        auto itU = fv.tab->map.find(uf.name);
                        if (itU != fv.tab->map.end())
                        {
                            if ((ut == L"struct" || ut == L"union") && (itU->second.t == Value::TableT || itU->second.t == Value::Class) && itU->second.tab)
                            {
                                int subTotal = 0;
                                void* pSub = StructSerialize(itU->second, &subTotal);
                                if (pSub)
                                {
                                    memcpy(buf + off, pSub, subTotal);
                                    HeapFree(GetProcessHeap(), 0, pSub);
                                    if (subTotal > uMaxSz) uMaxSz = subTotal;
                                }
                                int subA = CalcStructAlign(itU->second);
                                if (subA > uMaxA) uMaxA = subA;
                            }
                            else
                            {
                                int subSz = GetStructFieldSize(uf.type, uf.arrCount, uf.bIsArray, uf.bPtrArray);
                                int subA = GetStructFieldAlign(uf.type);
                                long long v64 = 0;
                                if (subSz == 8 && IsSize64(itU->second))
                                    v64 = (__int64)GetSize64Val(itU->second);
                                else if (itU->second.t == Value::Pointer)
                                    v64 = (long long)(INT_PTR)itU->second.ptr;
                                else
                                    v64 = (long long)BNum(itU->second);
                                if (subSz == 8) *(__int64*)(buf + off) = (__int64)v64;
                                else if (subSz == 2) *(unsigned short*)(buf + off) = (unsigned short)v64;
                                else if (subSz == 1) *(unsigned char*)(buf + off) = (unsigned char)v64;
                                else *(int*)(buf + off) = (int)v64;
                                if (subSz > uMaxSz) uMaxSz = subSz;
                                if (subA > uMaxA) uMaxA = subA;
                            }
                        }
                    }
                    sz = AlignOffset(uMaxSz, uMaxA);
                }
            }
            else if (t == L"ustring" || t == L"ustr" || t == L"wstr" || t == L"bstr")
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
                    // struct ustring fields receive the RAW pointer — a
                    // byte-string (string.toUtf16 product, one byte per
                    // wchar) must be re-packed into a real UTF-16 buffer
                    // or every high byte becomes a NUL and Windows reads
                    // only the first character ("CLSH[..." -> "C")
                    thread_local std::deque<std::wstring> sKeep;
                    wstring u16;
                    const wstring* src = nullptr;
                    if (iv != vTab.tab->map.end() && iv->second.t == Value::Str)
                        src = &iv->second.s;
                    else if (fv.t == Value::Str)
                        src = &fv.s;
                    if (src)
                    {
                        bool bAllByte = true;
                        bool bHasNul = false;
                        for (wchar_t c : *src)
                        {
                            if ((unsigned short)c > 0xFF)
                            {
                                bAllByte = false;
                                break;
                            }
                            if (c == 0) bHasNul = true;
                        }
                        if (bAllByte && bHasNul && ((*src).size() % 2 == 0))
                        {
                            // re-pack UTF-16 LE bytes (e.g. from string.toUtf16) into real wchars
                            u16.reserve((*src).size() / 2 + 1);
                            for (size_t k = 0; k + 1 < (*src).size(); k += 2)
                                u16.push_back((wchar_t)(
                                    (unsigned char)(*src)[k] |
                                    ((unsigned short)(unsigned char)
                                         (*src)[k + 1]
                                     << 8)));
                            sKeep.push_back(std::move(u16));
                            *(const wchar_t**)(buf + off) =
                                sKeep.back().c_str();
                        }
                        else
                            *(const wchar_t**)(buf + off) = src->c_str();
                    }
                    else if (fv.ptr)
                        // api function values carry their entry point in
                        // .ptr — `wc.lpfnWndProc = ::DefWindowProc` relies
                        // on this to land a real WNDPROC in the struct
                        *(void**)(buf + off) = fv.ptr;
                    else if (fv.t == Value::Pointer)
                        *(void**)(buf + off) = fv.ptr;
                    else if (fv.t == Value::Num)
                        *(void**)(buf + off) = (void*)(INT_PTR)(long long)fv.n;
                    else
                        *(void**)(buf + off) = nullptr;
                }
            }
            else if (t == L"string" || t == L"str" || t == L"strb" || t == L"lpcstr")
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
                    {
                        thread_local std::deque<std::string> s_ansiKeep;
                        std::string sA;
                        bool bIsBytes = true;
                        for (wchar_t c : fv.s)
                        {
                            if ((unsigned short)c > 0xFF)
                            {
                                bIsBytes = false;
                                break;
                            }
                        }
                        if (bIsBytes)
                        {
                            sA = BytesOf(fv.s);
                        }
                        else
                        {
                            sA.resize((size_t)WideCharToMultiByte(CP_ACP, 0, fv.s.c_str(), (int)fv.s.size(), nullptr, 0, nullptr, nullptr));
                            if (!sA.empty())
                                WideCharToMultiByte(CP_ACP, 0, fv.s.c_str(), (int)fv.s.size(), &sA[0], (int)sA.size(), nullptr, nullptr);
                        }
                        s_ansiKeep.push_back(std::move(sA));
                        *(const char**)(buf + off) = s_ansiKeep.back().c_str();
                    }
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
                else if (fv.t == Value::Str)
                {
                    thread_local std::deque<std::string> s_ptrBytesKeep;
                    thread_local std::deque<std::wstring> s_ptrWstrKeep;
                    bool bIsBytes = true;
                    for (wchar_t c : fv.s)
                    {
                        if ((unsigned short)c > 0xFF)
                        {
                            bIsBytes = false;
                            break;
                        }
                    }
                    if (bIsBytes)
                    {
                        s_ptrBytesKeep.push_back(BytesOf(fv.s));
                        *(void**)(buf + off) = (void*)s_ptrBytesKeep.back().data();
                    }
                    else
                    {
                        s_ptrWstrKeep.push_back(fv.s);
                        *(void**)(buf + off) = (void*)s_ptrWstrKeep.back().c_str();
                    }
                }
                else if (fv.t == Value::TableT && fv.tab)
                {
                    auto iby = fv.tab->map.find(L"_bytes");
                    if (iby != fv.tab->map.end() && iby->second.t == Value::Str)
                    {
                        thread_local std::deque<std::string> s_ptrBytesKeep;
                        s_ptrBytesKeep.push_back(BytesOf(iby->second.s));
                        *(void**)(buf + off) = (void*)s_ptrBytesKeep.back().data();
                    }
                    else
                    {
                        auto itp = fv.tab->map.find(L"_topointer");
                        if (itp != fv.tab->map.end() && itp->second.t == Value::Pointer)
                            *(void**)(buf + off) = itp->second.ptr;
                        else
                            *(void**)(buf + off) = (void*)(INT_PTR)(long long)BNum(fv);
                    }
                }
                else if (fv.t == Value::Null)
                {
                    *(void**)(buf + off) = nullptr;
                }
                else
                    *(void**)(buf + off) = (void*)(INT_PTR)(long long)BNum(fv);
            }
            else if (t == L"float")
            {
                if (f.bIsArray && (fv.t == Value::TableT) && fv.tab)
                {
                    auto& arr = fv.tab->arr;
                    for (int k = 0; k < f.arrCount; k++)
                    {
                        float flt = 0.0f;
                        if (k < (int)arr.size()) flt = (float)BNum(arr[k]);
                        memcpy(buf + off + k * sizeof(float), &flt, sizeof(float));
                    }
                }
                else
                {
                    float flt = (float)BNum(fv);
                    memcpy(buf + off, &flt, sizeof(float));
                }
            }
            else if (t == L"double")
            {
                if (f.bIsArray && (fv.t == Value::TableT) && fv.tab)
                {
                    auto& arr = fv.tab->arr;
                    for (int k = 0; k < f.arrCount; k++)
                    {
                        double dbl = 0.0;
                        if (k < (int)arr.size()) dbl = BNum(arr[k]);
                        memcpy(buf + off + k * sizeof(double), &dbl, sizeof(double));
                    }
                }
                else
                {
                    double dbl = BNum(fv);
                    memcpy(buf + off, &dbl, sizeof(double));
                }
            }
            else if (f.bIsArray && t == L"word" && fv.t == Value::Str)
            {
                wchar_t* pw = (wchar_t*)(buf + off);
                int maxC = f.arrCount;
                for (int k = 0; k < maxC; k++)
                    pw[k] = (k < (int)fv.s.size()) ? fv.s[k] : 0;
            }
            else if (f.bIsArray && t == L"byte" && fv.t == Value::Str)
            {
                char* pc = buf + off;
                std::string sA;
                if (fv.bRawBytes)
                {
                    for (wchar_t c : fv.s) sA.push_back((char)(unsigned char)c);
                }
                else
                {
                    sA.resize((size_t)WideCharToMultiByte(CP_UTF8, 0, fv.s.c_str(), (int)fv.s.size(), nullptr, 0, nullptr, nullptr));
                    if (!sA.empty())
                        WideCharToMultiByte(CP_UTF8, 0, fv.s.c_str(), (int)fv.s.size(), &sA[0], (int)sA.size(), nullptr, nullptr);
                }
                for (int k = 0; k < f.arrCount; k++)
                    pc[k] = (k < (int)sA.size()) ? sA[k] : 0;
            }
            else if (f.bIsArray && t == L"byte" && fv.t == Value::Pointer && fv.ptr)
            {
                char* pc = buf + off;
                size_t srcSz = GetBufferSize(fv.ptr);
                size_t cpy = (srcSz > 0 && (int)srcSz < f.arrCount) ? srcSz : (size_t)f.arrCount;
                memcpy(pc, fv.ptr, cpy);
            }
            else if (f.bIsArray && (fv.t == Value::TableT) && fv.tab)
            {
                // numeric / pointer element arrays (BYTE Data4[8], ptr p[1], ...):
                // an array table serializes element-wise; esz is
                // the PER-ELEMENT size, not the whole-field size
                auto& arr = fv.tab->arr;
                int esz = f.arrCount > 0 ? sz / f.arrCount : sz;
                for (int k = 0; k < f.arrCount; k++)
                {
                    long long ev = 0;
                    if (k < (int)arr.size())
                    {
                        if (arr[k].t == Value::Pointer)
                            ev = (long long)(INT_PTR)arr[k].ptr;
                        else
                            ev = (long long)BNum(arr[k]);
                    }
                    if (esz == 8) *(__int64*)(buf + off + k * 8) = (__int64)ev;
                    else if (esz == 2) *(unsigned short*)(buf + off + k * 2) = (unsigned short)ev;
                    else if (esz == 1) *(unsigned char*)(buf + off + k) = (unsigned char)ev;
                    else *(int*)(buf + off + k * 4) = (int)ev;
                }
            }
            else
            {
                long long v64 = 0;
                if (sz == 8 && IsSize64(fv))
                    v64 = (__int64)GetSize64Val(fv);
                else
                    v64 = (long long)BNum(fv);
                if (sz == 8) *(__int64*)(buf + off) = (__int64)v64;
                else if (sz == 2) *(unsigned short*)(buf + off) = (unsigned short)v64;
                else if (sz == 1) *(unsigned char*)(buf + off) = (unsigned char)v64;
                else *(int*)(buf + off) = (int)v64;
            }
            off += sz;
        }
        return buf;
    }

    void StructDeserialize(Value& vTab, const void* pBuf)
    {
        if ((vTab.t != Value::TableT && vTab.t != Value::Class) || !vTab.tab || !pBuf)
            return;
        auto fields = ParseStructDef(vTab);
        const char* buf = (const char*)pBuf;
        int off = 0;
        for (auto& f : fields)
        {
            auto iv = vTab.tab->map.find(f.name);
            int sz = GetStructFieldSize(f.type, f.arrCount, f.bIsArray, f.bPtrArray);
            wstring t = f.type;
            for (auto& c : t) c = towlower(c);
            {
                int al;
                if (f.bPtrArray)
                    al = (int)sizeof(void*);
                else if (t == L"struct" || t == L"union")
                {
                    if (iv != vTab.tab->map.end() &&
                        (iv->second.t == Value::TableT ||
                         iv->second.t == Value::Class) &&
                        iv->second.tab)
                        al = CalcStructAlign(iv->second);
                    else
                        al = 1;
                }
                else
                    al = GetStructFieldAlign(f.type);
                off = AlignOffset(off, al);
            }
            if (f.bPtrArray)
            {
                int count = f.arrCount > 0 ? f.arrCount : 0;
                auto outArr = std::make_shared<Table>();
                for (int k = 0; k < count; k++)
                {
                    void* ptr = *(void**)(buf + off + k * sizeof(void*));
                    if (!ptr)
                    {
                        outArr->arr.push_back(Value());
                    }
                    else if (t == L"ustring" || t == L"ustr" || t == L"wstr" || t == L"bstr")
                    {
                        const wchar_t* pw = (const wchar_t*)ptr;
                        outArr->arr.push_back(Value::Str_(wstring(pw)));
                    }
                    else if (t == L"pointer" || t == L"ptr")
                    {
                        outArr->arr.push_back(Value::Pointer_(ptr));
                    }
                    else
                    {
                        const char* pc = (const char*)ptr;
                        outArr->arr.push_back(Value::Str_(ReadStrA(pc)));
                    }
                }
                vTab.tab->map[f.name] = Value::Table_(outArr);
            }
            else if (t == L"struct")
            {
                if (iv != vTab.tab->map.end() && (iv->second.t == Value::TableT || iv->second.t == Value::Class) && iv->second.tab)
                {
                    if (f.bIsArray)
                    {
                        int count = f.arrCount > 0 ? f.arrCount : (int)GetTableLength(iv->second);
                        if (f.arrCount <= 0 && (iv->second.t == Value::TableT || iv->second.t == Value::Class) && iv->second.tab)
                        {
                            auto itLen = iv->second.tab->map.find(L"length");
                            if (itLen != iv->second.tab->map.end() && itLen->second.t == Value::Num)
                            {
                                int dynCount = (int)itLen->second.n;
                                if (dynCount > count) count = dynCount;
                            }
                        }
                        Value elem0 = GetTableElement(iv->second, 0);
                        int elemSz = (elem0.t == Value::TableT || elem0.t == Value::Class) ? CalcStructTotalSize(elem0) : 0;
                        if (elemSz <= 0) elemSz = 4;
                        if (iv->second.tab && (int)iv->second.tab->arr.size() < count && (elem0.t == Value::TableT || elem0.t == Value::Class))
                        {
                            Value tmpl = elem0;
                            while ((int)iv->second.tab->arr.size() < count)
                                iv->second.tab->arr.push_back(DeepCopy(tmpl));
                        }
                        for (int k = 0; k < count; k++)
                        {
                            Value elem = GetTableElement(iv->second, k);
                            if ((elem.t == Value::TableT || elem.t == Value::Class) && elem.tab)
                            {
                                StructDeserialize(elem, buf + off + k * elemSz);
                                if (iv->second.tab && k < (int)iv->second.tab->arr.size())
                                    iv->second.tab->arr[k] = elem;
                            }
                        }
                        sz = elemSz * (count > 0 ? count : 1);
                    }
                    else
                    {
                        sz = CalcStructTotalSize(iv->second);
                        StructDeserialize(iv->second, buf + off);
                    }
                }
            }
            else if (t == L"union")
            {
                if (iv != vTab.tab->map.end() && (iv->second.t == Value::TableT || iv->second.t == Value::Class) && iv->second.tab)
                {
                    auto uFields = ParseStructDef(iv->second);
                    int uMaxSz = 0;
                    int uMaxA = 1;
                    for (auto& uf : uFields)
                    {
                        wstring ut = uf.type;
                        for (auto& c : ut) c = towlower(c);
                        auto itU = iv->second.tab->map.find(uf.name);
                        if (itU != iv->second.tab->map.end())
                        {
                            if ((ut == L"struct" || ut == L"union") && (itU->second.t == Value::TableT || itU->second.t == Value::Class) && itU->second.tab)
                            {
                                StructDeserialize(itU->second, buf + off);
                                int subSz = CalcStructTotalSize(itU->second);
                                int subA = CalcStructAlign(itU->second);
                                if (subSz > uMaxSz) uMaxSz = subSz;
                                if (subA > uMaxA) uMaxA = subA;
                            }
                            else
                            {
                                int subSz = GetStructFieldSize(uf.type, uf.arrCount, uf.bIsArray, uf.bPtrArray);
                                int subA = GetStructFieldAlign(uf.type);
                                long long ev = 0;
                                if (subSz == 8) ev = *(__int64*)(buf + off);
                                else if (subSz == 2) ev = *(unsigned short*)(buf + off);
                                else if (subSz == 1) ev = *(unsigned char*)(buf + off);
                                else ev = *(int*)(buf + off);
                                if (subSz == 8) itU->second = CreateSize64((uint64_t)ev);
                                else if (uf.type == L"pointer" || uf.type == L"ptr") itU->second = Value::Pointer_((void*)(INT_PTR)ev);
                                else itU->second = Value::Num_((double)ev);
                                if (subSz > uMaxSz) uMaxSz = subSz;
                                if (subA > uMaxA) uMaxA = subA;
                            }
                        }
                    }
                    sz = AlignOffset(uMaxSz, uMaxA);
                }
            }
            else if (t == L"ustring" || t == L"ustr" || t == L"wstr" || t == L"bstr")
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
            else if (t == L"string" || t == L"str" || t == L"strb" || t == L"lpcstr")
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
            else if (f.bIsArray)
            {
                // WORD[] and BYTE[] arrays deserialize to string
                // if the existing field was not initialized/assigned as an array table
                bool bExistingTable = (iv != vTab.tab->map.end() && iv->second.t == Value::TableT);
                if (!bExistingTable && t == L"word")
                {
                    const wchar_t* pw = (const wchar_t*)(buf + off);
                    int len = 0;
                    while (len < f.arrCount && pw[len]) len++;
                    vTab.tab->map[f.name] = Value::Str_(wstring(pw, len));
                }
                else if (!bExistingTable && t == L"byte" && (iv != vTab.tab->map.end() && iv->second.t == Value::Str))
                {
                    const char* pc = (const char*)(buf + off);
                    int len = 0;
                    while (len < f.arrCount && pc[len]) len++;
                    vTab.tab->map[f.name] = Value::Str_(ReadStrA(std::string(pc, len).c_str()));
                }
                else
                {
                    // numeric / pointer / float element arrays read back as an array;
                    // esz is the PER-ELEMENT size (guard arrCount<=0)
                    auto at = std::make_shared<Table>();
                    int esz = f.arrCount > 0 ? sz / f.arrCount : sz;
                    bool bPointerElem = (t == L"pointer" || t == L"ptr" || t == L"addr");
                    bool bFloatElem = (t == L"float");
                    bool bDoubleElem = (t == L"double");
                    for (int k = 0; k < f.arrCount; k++)
                    {
                        if (bFloatElem)
                        {
                            float flt = 0;
                            memcpy(&flt, buf + off + k * sizeof(float), sizeof(float));
                            at->arr.push_back(Value::Num_(flt));
                        }
                        else if (bDoubleElem)
                        {
                            double dbl = 0;
                            memcpy(&dbl, buf + off + k * sizeof(double), sizeof(double));
                            at->arr.push_back(Value::Num_(dbl));
                        }
                        else
                        {
                            long long v64 = 0;
                            if (esz == 8) v64 = *(__int64*)(buf + off + k * 8);
                            else if (esz == 2) v64 = *(unsigned short*)(buf + off + k * 2);
                            else if (esz == 1) v64 = *(unsigned char*)(buf + off + k);
                            else v64 = *(int*)(buf + off + k * 4);
                            if (bPointerElem)
                                at->arr.push_back(v64 ? Value::Pointer_((void*)(INT_PTR)v64) : Value());
                            else
                                at->arr.push_back(Value::Num_((double)v64));
                        }
                    }
                    vTab.tab->map[f.name] = Value::Table_(at);
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
                if (sz == 8 && iv != vTab.tab->map.end() && IsSize64(iv->second))
                {
                    vTab.tab->map[f.name] = CreateSize64((uint64_t)v64);
                }
                else
                {
                    vTab.tab->map[f.name] = Value::Num_((double)v64);
                }
            }
            off += sz;
        }
        if (vTab.tab && !vTab.tab->arr.empty() && vTab.tab->map.count(L"length"))
        {
            auto itLen = vTab.tab->map.find(L"length");
            int count = (itLen != vTab.tab->map.end() && itLen->second.t == Value::Num)
                ? (int)itLen->second.n : 0;
            if (count > 0 && vTab.tab->arr.size() == 1)
            {
                Value elemTemplate = vTab.tab->arr[0];
                int elemSz = (elemTemplate.t == Value::TableT || elemTemplate.t == Value::Class)
                    ? CalcStructTotalSize(elemTemplate) : 0;
                if (elemSz > 0)
                {
                    vTab.tab->arr.clear();
                    for (int k = 0; k < count; k++)
                    {
                        Value elem = DeepCopy(elemTemplate);
                        StructDeserialize(elem, buf + off + k * elemSz);
                        vTab.tab->arr.push_back(elem);
                    }
                    off += count * elemSz;
                }
            }
        }
    }


    std::vector<CbCtx*> g_cbs;
    CbDispatchFn g_cbDispatch = nullptr;
    std::wstring g_curModuleFile;

    static BOOL WINAPI NodioWriteConsoleW(HANDLE hConsoleOutput, const VOID *lpBuffer, DWORD nNumberOfCharsToWrite, LPDWORD lpNumberOfCharsWritten, LPVOID lpReserved)
    {
        if (::WriteConsoleW(hConsoleOutput, lpBuffer, nNumberOfCharsToWrite, lpNumberOfCharsWritten, lpReserved))
            return TRUE;
        DWORD err = ::GetLastError();
        if (err == ERROR_INVALID_FUNCTION || err == ERROR_INVALID_HANDLE || err == ERROR_NOT_ENOUGH_MEMORY)
        {
            if (lpBuffer && nNumberOfCharsToWrite > 0)
            {
                const wchar_t* pw = (const wchar_t*)lpBuffer;
                int nBytes = ::WideCharToMultiByte(CP_UTF8, 0, pw, (int)nNumberOfCharsToWrite, nullptr, 0, nullptr, nullptr);
                if (nBytes > 0)
                {
                    std::string u8(nBytes, 0);
                    ::WideCharToMultiByte(CP_UTF8, 0, pw, (int)nNumberOfCharsToWrite, &u8[0], nBytes, nullptr, nullptr);
                    DWORD written = 0;
                    BOOL ok = ::WriteFile(hConsoleOutput, u8.data(), (DWORD)u8.size(), &written, nullptr);
                    if (ok)
                    {
                        if (lpNumberOfCharsWritten)
                            *lpNumberOfCharsWritten = nNumberOfCharsToWrite;
                        return TRUE;
                    }
                }
            }
            else if (nNumberOfCharsToWrite == 0)
            {
                if (lpNumberOfCharsWritten)
                    *lpNumberOfCharsWritten = 0;
                return TRUE;
            }
        }
        return FALSE;
    }

    static BOOL WINAPI NodioWriteConsoleA(HANDLE hConsoleOutput, const VOID *lpBuffer, DWORD nNumberOfCharsToWrite, LPDWORD lpNumberOfCharsWritten, LPVOID lpReserved)
    {
        if (::WriteConsoleA(hConsoleOutput, lpBuffer, nNumberOfCharsToWrite, lpNumberOfCharsWritten, lpReserved))
            return TRUE;
        DWORD err = ::GetLastError();
        if (err == ERROR_INVALID_FUNCTION || err == ERROR_INVALID_HANDLE || err == ERROR_NOT_ENOUGH_MEMORY)
        {
            if (lpBuffer && nNumberOfCharsToWrite > 0)
            {
                DWORD written = 0;
                BOOL ok = ::WriteFile(hConsoleOutput, lpBuffer, nNumberOfCharsToWrite, &written, nullptr);
                if (ok)
                {
                    if (lpNumberOfCharsWritten)
                        *lpNumberOfCharsWritten = nNumberOfCharsToWrite;
                    return TRUE;
                }
            }
            else if (nNumberOfCharsToWrite == 0)
            {
                if (lpNumberOfCharsWritten)
                    *lpNumberOfCharsWritten = 0;
                return TRUE;
            }
        }
        return FALSE;
    }

// ---- CreateDirectApiWrapper ----
Value Interp::CreateDirectApiWrapper(HMODULE hMod, const wstring& funcName,
                                     bool bDllCdecl, bool bDllUnicode)
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

            // Step 1: try exact name
            fp = ::GetProcAddress(hMod, strNameA.c_str());
            bool bFoundExact = (fp != nullptr);

            // Step 2: if not found and suffix exists, try bareName
            if (!fp && suffix)
            {
                fp = ::GetProcAddress(hMod, bareName.c_str());
            }

            if (fp)
            {
                if (suffix == 'W') { bUnicode = true; }
                else if (suffix == 'A') { bUnicode = false; }
                else if (!bFoundExact)
                {
                    if (suffix == 'L') { retType = ARI64; }
                    else if (suffix == 'P') { retType = ARPtr; }
                    else if (suffix == 'D' || suffix == 'F') { retType = ARDbl; }
                    else if (suffix == 'B') { retType = ARBool; }
                }
            }

            // Step 3: if not found, try appending 'W'
            if (!fp)
            {
                std::string tryW = (suffix ? bareName : strNameA) + "W";
                fp = ::GetProcAddress(hMod, tryW.c_str());
                if (fp)
                {
                    bUnicode = true;
                    if (suffix == 'L') { retType = ARI64; }
                    else if (suffix == 'P') { retType = ARPtr; }
                    else if (suffix == 'D' || suffix == 'F') { retType = ARDbl; }
                    else if (suffix == 'B') { retType = ARBool; }
                }
            }

            // Step 4: if not found, try appending 'A'
            if (!fp)
            {
                std::string tryA = (suffix ? bareName : strNameA) + "A";
                fp = ::GetProcAddress(hMod, tryA.c_str());
                if (fp)
                {
                    bUnicode = false;
                    if (suffix == 'L') { retType = ARI64; }
                    else if (suffix == 'P') { retType = ARPtr; }
                    else if (suffix == 'D' || suffix == 'F') { retType = ARDbl; }
                    else if (suffix == 'B') { retType = ARBool; }
                }
            }

            if (!fp)
                return Value(); // not found

            std::string checkName = suffix ? bareName : strNameA;
            if (checkName == "WriteConsole")
            {
                if (suffix == 'A')
                {
                    fp = (FARPROC)NodioWriteConsoleA;
                    bUnicode = false;
                }
                else
                {
                    fp = (FARPROC)NodioWriteConsoleW;
                    bUnicode = true;
                }
                if (suffix == 'B') retType = ARBool;
            }
            if (retType == ARInt && suffix == 0)
            {
                if (checkName == "LoadCursor" || checkName == "LoadIcon" ||
                    checkName == "LoadImage" || checkName == "LoadBitmap" ||
                    checkName == "GetCursor" || checkName == "CreateCursor" ||
                    checkName == "CreateIcon" || checkName == "CreateIconIndirect" ||
                    checkName == "GetDC" || checkName == "GetWindowDC" ||
                    checkName == "CreateCompatibleDC")
                {
                    retType = ARPtr;
                }
            }

            fp = ResolveLongPtrAlias(hMod, fp, strNameA);

            void* fn = (void*)fp;
            bool bCdecl = bDllCdecl;

            Value fWrap = NF([this, fn, bUnicode, bCdecl, retType](vector<Value>& a) -> Value {
                vector<SlotT> slots;
                thread_local std::vector<std::string> tl_activeA[16];
                thread_local std::vector<wstring> tl_activeW[16];
                thread_local size_t tl_slotIdx = 0;
                size_t mySlot = (tl_slotIdx++) % 16;
                auto& tmpA = tl_activeA[mySlot];
                auto& tmpW = tl_activeW[mySlot];
                tmpA.clear();
                tmpW.clear();
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
#if defined(_WIN64)
                        slots.push_back((SlotT)(INT_PTR)(long long)v.n);
#else
                        slots.push_back((SlotT)(INT_PTR)(int)(unsigned int)(long long)v.n);
#endif
                    }
                    else if (v.t == Value::Pointer)
                    {
                        slots.push_back((SlotT)(INT_PTR)v.ptr);
                    }
                    else if (v.t == Value::Str)
                    {
                        if (v.bRawBytes)
                        {
                            if (bUnicode)
                            {
                                // Re-pack raw UTF-16 LE bytes into real wchars
                                wstring u16;
                                u16.reserve(v.s.size() / 2 + 1);
                                for (size_t k = 0; k + 1 < v.s.size(); k += 2)
                                {
                                    u16.push_back((wchar_t)(
                                        (unsigned char)v.s[k] |
                                        ((unsigned short)(unsigned char)v.s[k + 1] << 8)));
                                }
                                tmpW.push_back(std::move(u16));
                                slots.push_back((SlotT)(INT_PTR)tmpW.back().c_str());
                            }
                            else
                            {
                                tmpA.push_back(BytesOf(v.s));
                                slots.push_back((SlotT)(INT_PTR)tmpA.back().data());
                            }
                        }
                        else
                        {
                            bool bByteString = false;
                            for (wchar_t c : v.s)
                            {
                                if (c == 0) { bByteString = true; break; }
                            }
                            if (bByteString)
                            {
                                tmpA.push_back(BytesOf(v.s));
                                slots.push_back((SlotT)(INT_PTR)tmpA.back().data());
                            }
                            else if (bUnicode)
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
                    }
                    else if (v.t == Value::TableT || v.t == Value::Class)
                    {
                        // raw.long(0,true)-style out-parameter wrapper:
                        // { _bytes = <raw buffer>, value = n } — pass the
                        // buffer's address (StructSerialize would mangle
                        // the embedded string member and QPF-style APIs
                        // would write through garbage)
                        auto iby = v.t == Value::TableT && v.tab
                            ? v.tab->map.find(L"_bytes") : v.tab->map.end();
                        if (iby != v.tab->map.end() && iby->second.t == Value::Str)
                        {
                            tmpA.push_back(BytesOf(iby->second.s));
                            slots.push_back(
                                (SlotT)(INT_PTR)tmpA.back().data());
                            outStructs.push_back({ i, (void*)tmpA.back().data(), v });
                        }
                        else
                        {
                            // If object provides _topointer meta-method (e.g. gdip objects),
                            // convert to native pointer
                            Value vp = GetMember(v, L"_topointer");
                            if (vp.t == Value::Func || vp.native)
                            {
                                vector<Value> noArgs;
                                Value r = CallOwner(vp, v, noArgs);
                                if (r.t == Value::Pointer)
                                    slots.push_back((SlotT)(INT_PTR)r.ptr);
                                else
                                    slots.push_back((SlotT)(INT_PTR)(long long)BNum(r));
                            }
                            else
                            {
                                int total = 0;
                                void* pBuf = StructSerialize(v, &total);
                                if (pBuf)
                                {
                                    // auto-maintains cbSize: when a struct's
                                    // first member is named cbSize, every API call
                                    // stamps it with the real size (WNDCLASSEX default
                                    // 48 would otherwise fail RegisterClassEx's size check on x64)
                                    if (total >= 4 && v.tab &&
                                        v.tab->map.find(L"cbSize") != v.tab->map.end())
                                        *(int*)pBuf = total;
                                    slots.push_back((SlotT)(INT_PTR)pBuf);
                                    outStructs.push_back({ i, pBuf, v });
                                }
                                else
                                {
                                    slots.push_back((SlotT)(INT_PTR)(long long)BNum(v));
                                }
                            }
                        }
                    }
                    else if (v.t == Value::Func)
                    {
                        // Bare script function passed to dynamic FFI: auto-wrap as stdcall callback
                        CbCtx* ctx = new CbCtx();
                        ctx->fn = v;
                        ctx->sig.bCdecl = false;
                        ctx->sig.ret = ARI64;
                        for (int p = 0; p < 8; p++) ctx->sig.params.push_back(ARI64);
                        ctx->interp = this;
                        CallbackTrampoline(ctx);
                        g_cbs.push_back(ctx);
                        slots.push_back((SlotT)(INT_PTR)ctx->code);
                    }
                    else
                    {
                        slots.push_back((SlotT)(INT_PTR)(long long)BNum(v));
                    }
                }

                if (slots.size() > 32)
                {
                    if (err->empty()) (*err) = L"运行错误: API 参数超过 32 个";
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
                        vRet = Value::Bool_((int)r != 0);
                    else if (retType == ARPtr)
                        vRet = (INT_PTR)r ? Value::Pointer_((void*)(INT_PTR)r)
                                          : Value();
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
                        // raw.long-style _bytes wrapper: copy the buffer
                        // back and refresh .value (first 8 bytes little-
                        // endian) instead of struct-deserializing
                        auto ivb = ref.origVal.tab
                            ? ref.origVal.tab->map.find(L"_bytes")
                            : ref.origVal.tab->map.end();
                        if (ivb != ref.origVal.tab->map.end() &&
                            ivb->second.t == Value::Str)
                        {
                            size_t cb = ivb->second.s.size();
                            const char* p = (const char*)ref.pBuf;
                            unsigned long long vv = 0;
                            for (size_t k = 0; k < cb && k < 8; k++)
                                vv |= ((unsigned long long)
                                       (unsigned char)p[k]) << (8 * k);
                            wstring ws((size_t)cb, L'\0');
                            for (size_t k = 0; k < cb; k++)
                                ws[k] = (wchar_t)(unsigned char)p[k];
                            ivb->second = Value::Str_(ws);
                            auto ivv = ref.origVal.tab->map.find(L"value");
                            if (ivv != ref.origVal.tab->map.end())
                                ivv->second = Value::Num_((double)vv);
                            extras->push_back(ref.origVal);
                            continue;
                        }
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
}
