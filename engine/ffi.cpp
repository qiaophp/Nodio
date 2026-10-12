#include "engine/interp.h"

namespace NodioVM
{
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

    void* EmitX64CallStub(const ApiSig& sig)
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
        const unsigned char sPro[] = {
            0x53,
            0x48, 0x89, 0xCB,
            0x48, 0x81, 0xEC, 0xC0, 0x00, 0x00, 0x00 };
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
                const unsigned char cvt[] = {
                    0xF3, 0x0F, 0x5A, 0xC0 };   // cvtss2sd xmm0,xmm0
                put(cvt, 4);
            }
            const unsigned char movq[] = {
                0x66, 0x48, 0x0F, 0x7E, 0xC0 }; // movq rax,xmm0
            put(movq, 5);
        }
        const unsigned char sEp[] = {
            0x48, 0x81, 0xC4, 0xC0, 0x00, 0x00, 0x00, // add rsp,0xC0
            0x5B,                                     // pop rbx
            0xC3 };                                   // ret
        put(sEp, (int)sizeof(sEp));
        FlushInstructionCache(GetCurrentProcess(), c, n);
        return c;
    }

    int GetStructFieldSize(const wstring& type, int arrCount, bool bIsArray)
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

    int CalcStructTotalSize(const Value& vTab)
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


    std::vector<CbCtx*> g_cbs;
    CbDispatchFn g_cbDispatch = nullptr;
    std::wstring g_curModuleFile;

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
}
