// entry.cpp — public entry points: syntax check, source runner,
// crash reporting and the embed-path root.
#include "engine/common.h"
#include "engine/NodioVM.h"
#include "engine/parser.h"
#include "engine/interp.h"
#include "engine/compiler.h"

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
        // 模块内偏移：只有偏移才能把崩溃点定位到具体导出函数
        // （地址本身每次运行都随机，没法查）
        if (hMod)
        {
            size_t off = (size_t)((const unsigned char*)ep->ExceptionRecord->ExceptionAddress -
                                   (const unsigned char*)hMod);
            fwprintf(stderr, L"  module base=%p offset=0x%zX\n",
                     (void*)hMod, off);
        }
        if (ep->ExceptionRecord->NumberParameters >= 2)
        {
            fwprintf(stderr, L"  AccessViolation: op=%lld target=%p\n",
                     (long long)ep->ExceptionRecord->ExceptionInformation[0],
                     (void*)ep->ExceptionRecord->ExceptionInformation[1]);
        }
#if defined(_M_IX86)
        // NOTE: the raw register dump is deliberately omitted here. In a
        // 32-bit build, an unhandled-exception filter that reads the
        // CONTEXT registers is the textbook shape of an SEH based
        // anti-debug / anti-analysis handler, and antivirus engines
        // recognise it: the 32-bit nodio.exe was reported as
        // HEUR:Backdoor/Farfli.e for exactly this pattern (verified by
        // bisection - dropping only this dump cleared the report).
        // The exception code, faulting address, module offset and the
        // access-violation detail below still identify the crash site.
        if (ep->ContextRecord)
        {
            fwprintf(stderr, L"  faulting EIP=%08X ESP=%08X EBP=%08X\n",
                     ep->ContextRecord->Eip, ep->ContextRecord->Esp, ep->ContextRecord->Ebp);
        }
#else
        if (ep->ContextRecord)
        {
            fwprintf(stderr, L"  RIP=%p RAX=%p RCX=%p RDX=%p RBX=%p RSP=%p RBP=%p RSI=%p RDI=%p\n",
                     (void*)ep->ContextRecord->Rip, (void*)ep->ContextRecord->Rax, (void*)ep->ContextRecord->Rcx,
                     (void*)ep->ContextRecord->Rdx, (void*)ep->ContextRecord->Rbx, (void*)ep->ContextRecord->Rsp,
                     (void*)ep->ContextRecord->Rbp, (void*)ep->ContextRecord->Rsi, (void*)ep->ContextRecord->Rdi);
        }
#endif
        fflush(stderr);
        ExitProcess((UINT)ep->ExceptionRecord->ExceptionCode);
        return EXCEPTION_EXECUTE_HANDLER;
    }

    // install root for compile-time embed paths ("~/...") — the parent of
    // NODIO_LIBDIR (install-root\lib). Empty when no lib dir is set.
    wstring EmbedRoot()
    {
        wchar_t szLib[MAX_PATH] = L"";
        if (GetEnvironmentVariableW(L"NODIO_LIBDIR", szLib, MAX_PATH) == 0)
        {
            wstring cfgLib = GetNodioConfig().libDir;
            if (!cfgLib.empty())
                wcsncpy_s(szLib, cfgLib.c_str(), _TRUNCATE);
            else if (_waccess(L"D:\\aardio\\lib", 0) == 0)
                wcsncpy_s(szLib, L"D:\\aardio\\lib", _TRUNCATE);
        }
        wstring strLib = szLib;
        if (strLib.size() < 2) return L"";
        while (strLib.size() > 1 &&
               (strLib.back() == L'\\' || strLib.back() == L'/'))
            strLib.pop_back();
        size_t sl = strLib.find_last_of(L"\\/");
        return sl == wstring::npos ? wstring() : strLib.substr(0, sl);
    }

    static void SilentInvalidParameterHandler(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {}

    int RunSource(const wstring& src, wstring* strError,
                  const wstring* pBaseDir,
                  const std::unordered_map<wstring, wstring>* pEmbedded,
                  bool bDebug, const wstring* pBreakLines,
                  const wstring* pScriptFile)
    {
        _set_invalid_parameter_handler(SilentInvalidParameterHandler);
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
        SetUnhandledExceptionFilter(CrashFilter);
        setvbuf(stderr, nullptr, _IONBF, 0);
        wstring localErr;
        PNode prog;
        if (IsBytecode(src))
        {
            prog = DeserializeNode(src, &localErr);
        }
        else
        {
            vector<Tok> toks = Lex(src, &localErr);
            Parser ps{ toks, 0, &localErr };
            if (pBaseDir)
                ps.strEmbedBase = *pBaseDir;
            ps.strEmbedRoot = EmbedRoot();
            prog = ps.ParseBlock();   // whole program as a block
            AnnotateNameIds(prog);
        }
        if (!localErr.empty() || !prog)
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
        // external debug session: a DAP adapter spawned us with control
        // pipes — every breakpoint stop (and initial step) goes to it.
        // Two UNIDIRECTIONAL pipes (a synchronous duplex handle cannot be
        // read and written concurrently from different threads):
        //   NODIO_DEBUG_PIPE : events   (engine -> adapter, write-only)
        //   NODIO_DEBUG_CMD  : commands (adapter -> engine, read-only)
        {
            wchar_t szPipe[256] = L"";
            if (GetEnvironmentVariableW(L"NODIO_DEBUG_PIPE", szPipe, 256) > 0 &&
                szPipe[0])
            {
                HANDLE hEv = CreateFileW(szPipe, GENERIC_WRITE, 0, nullptr,
                                         OPEN_EXISTING, 0, nullptr);
                if (hEv != INVALID_HANDLE_VALUE)
                {
                    wchar_t szCmd[256] = L"";
                    if (GetEnvironmentVariableW(L"NODIO_DEBUG_CMD",
                                                szCmd, 256) > 0 && szCmd[0])
                    {
                        HANDLE hCmd = CreateFileW(szCmd, GENERIC_READ, 0,
                                                  nullptr, OPEN_EXISTING,
                                                  0, nullptr);
                        if (hCmd != INVALID_HANDLE_VALUE)
                        {
                            it.hDebugPipe = hEv;      // events out
                            it.hDebugCmd = hCmd;      // commands in
                            it.bDebug = true;
                            it.bStep = true;   // stop at the first statement
                        }
                        else
                        {
                            CloseHandle(hEv);
                        }
                    }
                    else
                    {
                        CloseHandle(hEv);
                    }
                }
            }
        }
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
        // Ensure TMP and TEMP point to a valid physical directory if they contain literal "%TMP%" or "%TEMP%"
        {
            wchar_t szTmpVal[MAX_PATH] = L"";
            DWORD dwLen = GetEnvironmentVariableW(L"TMP", szTmpVal, MAX_PATH);
            if (dwLen > 0)
            {
                if (wcscmp(szTmpVal, L"%TMP%") == 0 || wcscmp(szTmpVal, L"%TEMP%") == 0)
                {
                    wchar_t szRealPath[MAX_PATH] = L"";
                    if (GetTempPathW(MAX_PATH, szRealPath) > 0)
                    {
                        // Remove trailing backslash if present for consistency with standard env vars
                        size_t l = wcslen(szRealPath);
                        if (l > 1 && szRealPath[l - 1] == L'\\')
                            szRealPath[l - 1] = L'\0';
                        SetEnvironmentVariableW(L"TMP", szRealPath);
                        SetEnvironmentVariableW(L"TEMP", szRealPath);
                    }
                }
            }
        }
        it.InitBuiltins();
        // Support NODIO_PAUSE=0: define _CONSOLE_NO_PAUSE = true so console.pause() returns immediately in headless sweeps
        {
            wchar_t szPause[16] = L"";
            if (GetEnvironmentVariableW(L"NODIO_PAUSE", szPause, 16) > 0)
            {
                if (wcscmp(szPause, L"0") == 0)
                {
                    it.globals.vars[NameId(L"_CONSOLE_NO_PAUSE")] = Value::Bool_(true);
                    it.roots[L"_CONSOLE_NO_PAUSE"] = Value::Bool_(true);
                }
            }
        }
        // preload builtin.struct (defines ::POINT/
        // ::RECT/::PAINTSTRUCT that GUI libraries assume) — `builtin`
        // itself is pre-bound so a plain `import` would never load the
        // file body. NON-FATAL: failures are swallowed; when no NODIO_LIBDIR
        // is set (plain VM tests) there is simply nothing to preload.
        {
            wstring strBootErr;
            wstring strPrevErr = it.err ? *it.err : wstring();
            wstring* pPrevErr = it.err;
            it.err = &strBootErr;
            vector<Tok> toksBoot = Lex(
                L"import builtin; import builtin.struct; import builtin.com; import builtin.math; ::NMHDR = class { addr hwndFrom; ADDR idFrom; INT code; }; "
                L"table.get = function(k, t) { "
                L"if (!t) { t = ..global; } "
                L"var p, m, i = ..string.match(k, \"\\.*(.*)(%\\[\\])$\"); "
                L"if (m) i = 1; "
                L"else p, m = ..string.match(k, \"\\.*(.+)\\.([^.]+)$\"); "
                L"if (m !== null) { "
                L"if (#p) { var ok, nextT = call(table.get, null, p, t); if (!ok || !nextT) return null; t = nextT; } "
                L"if (t) { "
                L"if (i) m = #m > 2 ? eval(..string.slice(m, 2, -2)) : #t; "
                L"if (t@ && t@._dotNetNamespace) return t[[m]], t; "
                L"var ok, val = call(function() { return t[m]; }); if (!ok) return null; return val, t; "
                L"} "
                L"} else { "
                L"if (t@ && t@._dotNetNamespace) return t[[k]], t; "
                L"var ok, val = call(function() { return t[k]; }); if (!ok) return null; return val, t; "
                L"}; };", &strBootErr);
            Parser psBoot{ toksBoot, 0, &strBootErr };
            psBoot.strEmbedBase = pBaseDir ? *pBaseDir : wstring();
            psBoot.strEmbedRoot = EmbedRoot();
            PNode progBoot = psBoot.ParseBlock();
            AnnotateNameIds(progBoot);
            it.ExecBlock(progBoot, &it.globals);
            it.err = pPrevErr;
            if (pPrevErr)
                *pPrevErr = strPrevErr;
        }
        auto mainEnv = std::make_shared<Env>();
        mainEnv->self = mainEnv;
        mainEnv->parent = it.globals.self;
        // S-2: the main script owns its own file — a runtime error in it
        // reports "<script>:<line>" instead of a bare line number.
        if (pScriptFile && !pScriptFile->empty())
            mainEnv->srcFile = *pScriptFile;
        // The main script's own variables live in this frame; nothing in
        // the VM root graph points down to it, so it must be registered
        // or the collector would free the script's globals.
        Interp::GcFrameScope fsMain(it, mainEnv);
        try
        {
            it.ExecBlock(prog, mainEnv.get());
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
            localErr = L"运行错误: C++ 异常 " + whatW;
        }
        catch (...)
        {
            localErr = L"运行错误: 未知 C++ 异常";
        }
        if (!localErr.empty())
        {
            // S-2: the top-level report point.  It appends the location
            // (" @ <file>:<line>") unless the message already carries one
            // (a library compile error quotes the parser's own line).
            if (!HasErrorLocation(localErr) && it.nErrLine > 0)
                AppendErrorLocation(&localErr, it.strErrFile, it.nErrLine);
            if (strError) *strError = localErr;
            return -1;
        }
        return it.ret.t == Value::Num ? (int)it.ret.n : 0;
    }
}
