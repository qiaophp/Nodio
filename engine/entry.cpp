// entry.cpp — public entry points: syntax check, source runner,
// crash reporting and the embed-path root.
#include "engine/common.h"
#include "engine/parser.h"
#include "engine/interp.h"

namespace NodioVM
{
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

    // aardio root for compile-time embed paths ("~/...") — the parent of
    // NODIO_LIBDIR (install-root\lib). Empty when no lib dir is set.
    wstring EmbedRoot()
    {
        wchar_t szLib[MAX_PATH] = L"";
        GetEnvironmentVariableW(L"NODIO_LIBDIR", szLib, MAX_PATH);
        wstring strLib = szLib;
        if (strLib.size() < 2) return L"";
        while (strLib.size() > 1 &&
               (strLib.back() == L'\\' || strLib.back() == L'/'))
            strLib.pop_back();
        size_t sl = strLib.find_last_of(L"\\/");
        return sl == wstring::npos ? wstring() : strLib.substr(0, sl);
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
        if (pBaseDir)
            ps.strEmbedBase = *pBaseDir;
        ps.strEmbedRoot = EmbedRoot();
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
