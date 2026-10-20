// host/main.cpp — Nodio standalone host.
//
// Roles:
//   published exe                 self-detects an appended payload and runs
//                                 its embedded entry script
//   NODIO_RUN=<file>              run a script headlessly and exit
//                                 (F5 runtime + regression harness)
//   NODIO_LIBDIR=<dir>            official-library root for `import`
//   NODIO_DEBUG=1                 mini-debugger on
//   NODIO_BREAK="3,7"             seed breakpoints (implies debug)
//   NODIO_CONSOLE=1               force-allocate a console (io.print)
//   NODIO_PAUSE=1                 keep the console open afterwards
//   NODIO_PUBLISH=<entry_script> build a standalone exe
//   NODIO_PUBOUT=<out.exe>        output path (default <dir>\dist\<name>.exe)
//   NODIO_ICON=<icon.ico>         optional icon stamped into the output
//   NODIO_PUBVER="1.2.3.4"        PE version resource (needs NODIO_PUBMANIFEST=1)
//   NODIO_PUBCOMPANY=...          version strings
//   NODIO_PUBPRODUCT=...
//   NODIO_PUBDESC=...
//   NODIO_PUBCOPYRIGHT=...
//   NODIO_PUBMANIFEST=1           rewrite the manifest resource in place to
//                                 apply the values below. Off by default:
//                                 the loader rejects some rewritten
//                                 manifests, so the host's own manifest
//                                 (already DPI-aware) is kept unless asked.
//   NODIO_PUBUAC=<level>          asInvoker | requireAdministrator |
//                                 highestAvailable (with NODIO_PUBMANIFEST=1)
//   NODIO_PUBNODPI=1              informational only — the host manifest is
//                                 always DPI-aware; there is no per-publish
//                                 opt-out in this build.

#include <windows.h>
#include <commctrl.h>
#pragma comment(lib, "comctl32.lib")
#include "engine/common.h"
#include "engine/NodioVM.h"
#include "engine/aardio_chunk.h"
#include "engine/aardio_vm.h"
#include "publish/payload.h"
#include "publish/publish.h"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <unordered_map>
#include <string>
#include <vector>

static std::wstring ReadFileUtf8ToWide(const wchar_t* szPath, bool* pOk)
{
    *pOk = false;
    FILE* pf = nullptr;
    if (_wfopen_s(&pf, szPath, L"rb") != 0 || !pf)
        return L"";
    fseek(pf, 0, SEEK_END);
    long cb = ftell(pf);
    fseek(pf, 0, SEEK_SET);
    std::vector<char> raw((size_t)cb + 1, 0);
    size_t rd = cb > 0 ? fread(raw.data(), 1, (size_t)cb, pf) : 0;
    fclose(pf);
    long off = (rd >= 3 && (unsigned char)raw[0] == 0xEF &&
                (unsigned char)raw[1] == 0xBB &&
                (unsigned char)raw[2] == 0xBF)
                   ? 3 : 0;
    int w = MultiByteToWideChar(CP_UTF8, 0, raw.data() + off,
                                (int)(rd - off), nullptr, 0);
    std::vector<wchar_t> wide((size_t)w + 1, 0);
    if (w > 0)
        MultiByteToWideChar(CP_UTF8, 0, raw.data() + off, (int)(rd - off),
                            wide.data(), w);
    *pOk = true;
    return std::wstring(wide.data(), (size_t)w);
}

// binary read (needed for the official "=aardio" precompiled chunks, which
// are not UTF-8 text)
static std::string ReadFileBytes(const wchar_t* szPath, bool* pOk)
{
    *pOk = false;
    FILE* pf = nullptr;
    if (_wfopen_s(&pf, szPath, L"rb") != 0 || !pf)
        return std::string();
    fseek(pf, 0, SEEK_END);
    long cb = ftell(pf);
    fseek(pf, 0, SEEK_SET);
    if (cb < 0) { fclose(pf); return std::string(); }
    std::string raw;
    raw.resize((size_t)cb);
    size_t rd = cb > 0 ? fread(&raw[0], 1, (size_t)cb, pf) : 0;
    fclose(pf);
    raw.resize(rd);
    *pOk = true;
    return raw;
}

// stderr plumbing takes UTF-8 bytes (pipes + the test harness decode UTF-8)
static void WriteStderrUtf8(const std::wstring& s)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(),
                                nullptr, 0, nullptr, nullptr);
    if (n <= 0) return;
    std::vector<char> buf((size_t)n, 0);
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(),
                        buf.data(), n, nullptr, nullptr);
    fwrite(buf.data(), 1, (size_t)n, stderr);
    fputc('\n', stderr);
}

// stdout plumbing, also UTF-8 (used by the syntax-check sweep)
static void WriteStdoutUtf8(const std::wstring& s)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(),
                                nullptr, 0, nullptr, nullptr);
    if (n <= 0) return;
    std::vector<char> buf((size_t)n, 0);
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(),
                        buf.data(), n, nullptr, nullptr);
    fwrite(buf.data(), 1, (size_t)n, stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

static void EnsureConsole(bool bForce)
{
    if (!GetConsoleWindow() &&
        (bForce || !GetStdHandle(STD_OUTPUT_HANDLE)))
    {
        // A valid stdout that is NOT a console (pipe/file: the regression
        // harness, process.popen from the IDE) must be kept — allocating a
        // console and freopen-ing CONOUT$ would detach the pipe and every
        // io.print line would vanish into an invisible window.
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        if (hOut)
        {
            DWORD mode = 0;
            // a valid non-console stdout (pipe/file: the regression
            // harness, process.popen from the IDE) must be kept —
            // GetConsoleMode succeeds only on a real console device
            if (!GetConsoleMode(hOut, &mode))
                return;
        }
        AllocConsole();
        freopen("CONIN$", "r", stdin);
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    }
    if (GetConsoleWindow())
        SetConsoleOutputCP(65001);   // VM output is UTF-8
}

int wmain(int argc, wchar_t** argv)
{
    INITCOMMONCONTROLSEX icc = { sizeof(INITCOMMONCONTROLSEX), ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES | ICC_COOL_CLASSES };
    InitCommonControlsEx(&icc);

    // NODIO_CHECK=<listfile>: parse-only sweep for coverage measurement.
    // The list file holds one script path per line; each is lexed and
    // parsed (no execution) and reported as "OK <path>" / "FAIL <path> <err>".
    wchar_t szCheckList[MAX_PATH];
    if (GetEnvironmentVariableW(L"NODIO_CHECK", szCheckList, MAX_PATH) > 0)
    {
        bool bOk = false;
        std::wstring lst = ReadFileUtf8ToWide(szCheckList, &bOk);
        if (!bOk)
        {
            WriteStderrUtf8(L"无法读取清单: " + std::wstring(szCheckList));
            return 2;
        }
        size_t nOk = 0, nFail = 0, nCompiled = 0;
        size_t pos = 0;
        while (pos <= lst.size())
        {
            size_t nl = lst.find(L'\n', pos);
            if (nl == std::wstring::npos) nl = lst.size();
            std::wstring line = lst.substr(pos, nl - pos);
            pos = nl + 1;
            while (!line.empty() &&
                   (line.back() == L'\r' || line.back() == L' '))
                line.pop_back();
            if (line.empty()) continue;
            bool bF = false;
            std::wstring src = ReadFileUtf8ToWide(line.c_str(), &bF);
            if (!bF)
            {
                nFail++;
                WriteStdoutUtf8(L"FAIL " + line + L" <unreadable>");
                continue;
            }
            std::wstring se;
            if (NodioVM::IsCompiledModule(src))
            {
                // Compiled bytecode, not source text. Source syntax does
                // not apply, so it is counted separately from parse failures.
                nCompiled++;
                WriteStdoutUtf8(L"COMPILED " + line);
            }
            else if (NodioVM::CheckSyntax(src, &se) != 0)
            {
                nFail++;
                WriteStdoutUtf8(L"FAIL " + line + L" " + se);
            }
            else
            {
                nOk++;
                WriteStdoutUtf8(L"OK " + line);
            }
        }
        WriteStdoutUtf8(L"== check " + std::to_wstring(nOk) + L" ok, " +
                        std::to_wstring(nFail) + L" fail, " +
                        std::to_wstring(nCompiled) + L" compiled ==");
        return nFail ? 1 : 0;
    }

    // `-disasm <file>` — disassemble an official "=aardio" precompiled chunk.
    // Official aardio can *execute* its own precompiled chunks but cannot
    // inspect them; Nodio decodes the record stream with the official
    // runtime layout (interpreter FUN_004C4BC0) and prints a disassembly.
    if (argc > 2 && wcscmp(argv[1], L"-disasm") == 0)
    {
        bool bOk = false;
        std::string data = ReadFileBytes(argv[2], &bOk);
        if (!bOk)
        {
            WriteStderrUtf8(L"无法读取文件: " + std::wstring(argv[2]));
            return 2;
        }
        NodioVM::AardioChunkNode root;
        NodioVM::AardioChunkStats st;
        std::wstring err;
        if (!NodioVM::ParseAardioChunkTree(data, &root, &st, &err))
        {
            WriteStderrUtf8(L"解析失败: " + err);
            return 1;
        }
        WriteStdoutUtf8(L"== 官方预编译块 =aardio 反汇编 ==");
        WriteStdoutUtf8(L"文件: " + std::wstring(argv[2]) +
            L"  字节: " + std::to_wstring((int)data.size()));
        WriteStdoutUtf8(L"对象: " + std::to_wstring(st.objects) +
            L"  记录: " + std::to_wstring(st.records) +
            L"  常量: " + std::to_wstring(st.consts) +
            L"（字符串 " + std::to_wstring(st.strings) +
            L" / 数字 " + std::to_wstring(st.numbers) + L"）" +
            L"  最大嵌套: " + std::to_wstring(st.maxDepth));
        WriteStdoutUtf8(L"记录种类: 用到 " + std::to_wstring(st.kindsUsed) +
            L"/61  非法种类: " + std::to_wstring(st.badKinds) +
            L"  跳转: " + std::to_wstring(st.jumps) +
            L"（可解引用 " + std::to_wstring(st.jumpsResolved) + L"）");
        std::wstring hist;
        for (int k = 0; k < 61; k++)
        {
            if (!st.kindCount[k]) continue;
            if (!hist.empty()) hist += L"  ";
            hist += std::wstring(NodioVM::AardioOpcodeName(k)) + L"=" +
                std::to_wstring(st.kindCount[k]);
        }
        WriteStdoutUtf8(L"种类分布: " + hist);
        // S-1b：官方校验器 FUN_004D4450 的等价实现 —— 官方只能执行、不能诊断，
        // 这一节给出可定位到「对象 + 记录号」的结构化结论。
        {
            NodioVM::AardioValidateResult vres;
            std::wstring vreport;
            NodioVM::ValidateAardioChunkTree(root, &vres, &vreport);
            std::wstring sec = NodioVM::AardioValidateSection(root, vres);
            if (!vreport.empty())
            {
                sec += L"\n违规明细:";
                sec += L"\n" + vreport;
            }
            size_t pos = 0;
            while (pos < sec.size())
            {
                size_t nl = sec.find(L'\n', pos);
                if (nl == std::wstring::npos) nl = sec.size();
                WriteStdoutUtf8(sec.substr(pos, nl - pos));
                pos = nl + 1;
            }
        }
        // S-6：把「Nodio 还缺什么」量化到记录种类 —— 官方能执行自己的预编译块，
        // Nodio 目前只能解释它，这一节直接列出还不能翻译/执行的记录种类。
        {
            std::wstring sec = NodioVM::AardioUnimplementedSection();
            size_t pos = 0;
            while (pos < sec.size())
            {
                size_t nl = sec.find(L'\n', pos);
                if (nl == std::wstring::npos) nl = sec.size();
                WriteStdoutUtf8(sec.substr(pos, nl - pos));
                pos = nl + 1;
            }
        }
        WriteStdoutUtf8(L"---");
        std::wstring dis = NodioVM::DisassembleAardioTree(root, 200);
        // stdout 一次写完整段（WriteStdoutUtf8 会补换行，这里拆行输出）
        size_t pos = 0;
        while (pos < dis.size())
        {
            size_t nl = dis.find(L'\n', pos);
            if (nl == std::wstring::npos) nl = dis.size();
            WriteStdoutUtf8(dis.substr(pos, nl - pos));
            pos = nl + 1;
        }
        return 0;
    }

    // `-runchunk <file>` — 执行官方 "=aardio" 预编译块（记录 VM，S-1b 第二阶段）。
    // 官方 aardio 只能执行自己编译出来的块，从不提供这种「执行一个预编译块并
    // 打印结果」的入口；Nodio 的记录 VM 直接跑官方记录流。
    if (argc > 2 && wcscmp(argv[1], L"-runchunk") == 0)
    {
        bool bOk = false;
        std::string data = ReadFileBytes(argv[2], &bOk);
        if (!bOk)
        {
            WriteStderrUtf8(L"无法读取文件: " + std::wstring(argv[2]));
            return 2;
        }
        NodioVM::AardioChunkNode root;
        NodioVM::AardioChunkStats st;
        std::wstring err;
        if (!NodioVM::ParseAardioChunkTree(data, &root, &st, &err))
        {
            WriteStderrUtf8(L"解析失败: " + err);
            return 1;
        }
        WriteStdoutUtf8(L"== 记录 VM 执行 ==");
        WriteStdoutUtf8(L"文件: " + std::wstring(argv[2]) +
            L"  对象: " + std::to_wstring(st.objects) +
            L"  记录: " + std::to_wstring(st.records));
        NodioVM::AardioRunResult r = NodioVM::RunAardioChunk(root);
        if (!r.ok)
        {
            WriteStdoutUtf8(L"执行失败: " + r.err);
            if (r.pc >= 0)
            {
                std::wstring where = L"位置: 记录号 " + std::to_wstring(r.pc);
                if (r.kind >= 0)
                {
                    wchar_t hex[8];
                    swprintf_s(hex, L"%02X", r.kind);
                    where += L"  种类 0x" + std::wstring(hex);
                }
                WriteStdoutUtf8(where);
            }
            return 1;
        }
        WriteStdoutUtf8(L"结果: " + r.value.ToString());
        WriteStdoutUtf8(L"执行记录: " + std::to_wstring(r.steps));
        return 0;
    }

    // support `-e "code"` one-liner execution
    if (argc > 2 && wcscmp(argv[1], L"-e") == 0)
    {
        wchar_t szCon[8];
        bool bForceConsole =
            GetEnvironmentVariableW(L"NODIO_CONSOLE", szCon, 8) > 0 &&
            szCon[0] == L'1';
        EnsureConsole(bForceConsole);

        std::wstring src = argv[2];
        std::wstring strDir = L"";
        std::wstring strErr;
        NodioVM::RunSource(src, &strErr, &strDir, nullptr, false, nullptr);
        if (!strErr.empty())
            WriteStderrUtf8(strErr);
        fflush(stdout);
        return strErr.empty() ? 0 : 1;
    }

    // a script path as the first command-line argument runs it directly:
    // forward it into the NODIO_RUN harness below.
    // The self-hosted IDE's F5 spawns `nodio.exe <script>` and streams the
    // child's output through process.popen.
    if (argc > 1 && argv[1][0])
        SetEnvironmentVariableW(L"NODIO_RUN", argv[1]);

    // NODIO_RUN regression / F5 harness
    wchar_t szRunFile[MAX_PATH];
    if (GetEnvironmentVariableW(L"NODIO_RUN", szRunFile, MAX_PATH) > 0)
    {
        wchar_t szCon[8];
        bool bForceConsole =
            GetEnvironmentVariableW(L"NODIO_CONSOLE", szCon, 8) > 0 &&
            szCon[0] == L'1';
        EnsureConsole(bForceConsole);

        bool bOk = false;
        std::wstring src = ReadFileUtf8ToWide(szRunFile, &bOk);
        if (!bOk)
        {
            WriteStderrUtf8(L"运行错误: 无法打开脚本文件 " + std::wstring(szRunFile));
            return 2;
        }

        // the base dir drives $"path" embedding and import resolution, so
        // it must be derived from whichever separator the path uses
        std::wstring strDir = szRunFile;
        size_t nSlash = strDir.find_last_of(L"\\/");
        strDir = nSlash != std::wstring::npos && nSlash > 0
                     ? strDir.substr(0, nSlash) : L"";

        bool bDbg = false;
        wchar_t szDbg[16];
        if (GetEnvironmentVariableW(L"NODIO_DEBUG", szDbg, 16) > 0 &&
            szDbg[0] == L'1')
            bDbg = true;

        wchar_t szBrk[1024];
        std::wstring strBreaks;
        if (GetEnvironmentVariableW(L"NODIO_BREAK", szBrk, 1024) > 0)
        {
            strBreaks = szBrk;
            bDbg = true;
        }

        std::wstring strErr;
        // S-2: the script's own path — a runtime error reports
        // "<script>:<line>" instead of a bare line number.
        std::wstring strScriptFile = szRunFile;
        NodioVM::RunSource(src, &strErr, &strDir, nullptr, bDbg,
                            strBreaks.empty() ? nullptr : &strBreaks,
                            &strScriptFile);
        if (!strErr.empty())
            WriteStderrUtf8(strErr);
        fflush(stdout);

        wchar_t szPause[8];
        if (GetEnvironmentVariableW(L"NODIO_PAUSE", szPause, 8) > 0 &&
            szPause[0] == L'1')
            _wsystem(L"pause");
        return strErr.empty() ? 0 : 1;
    }

    // Phase 2: publisher pipeline
    wchar_t szPubFile[MAX_PATH];
    if (GetEnvironmentVariableW(L"NODIO_PUBLISH", szPubFile, MAX_PATH) > 0)
    {
        EnsureConsole(false);
        wchar_t szLib[MAX_PATH] = L"";
        GetEnvironmentVariableW(L"NODIO_LIBDIR", szLib, MAX_PATH);
        wchar_t szOut[MAX_PATH] = L"";
        GetEnvironmentVariableW(L"NODIO_PUBOUT", szOut, MAX_PATH);
        wchar_t szIcon[MAX_PATH] = L"";
        GetEnvironmentVariableW(L"NODIO_ICON", szIcon, MAX_PATH);

        NodioPublish::Options opt;
        opt.strEntry = szPubFile;
        opt.strLibDir = szLib;
        opt.strIcon = szIcon;
        opt.strOut = szOut;
        {
            wchar_t szGui[8];
            // default console (regression harness reads stdout); GUI
            // publishes opt in with NODIO_PUBGUI=1
            opt.bConsole = !(GetEnvironmentVariableW(L"NODIO_PUBGUI", szGui, 8) > 0 &&
                             szGui[0] == L'1');
        }
        {
            auto Rd = [](const wchar_t* k, wchar_t* buf, DWORD n) {
                return GetEnvironmentVariableW(k, buf, n) > 0 ? buf : L"";
            };
            wchar_t b[MAX_PATH];
            opt.strVersion     = Rd(L"NODIO_PUBVER", b, MAX_PATH);
            opt.strCompany     = Rd(L"NODIO_PUBCOMPANY", b, MAX_PATH);
            opt.strProduct     = Rd(L"NODIO_PUBPRODUCT", b, MAX_PATH);
            opt.strDescription = Rd(L"NODIO_PUBDESC", b, MAX_PATH);
            opt.strCopyright   = Rd(L"NODIO_PUBCOPYRIGHT", b, MAX_PATH);
            opt.strUacLevel    = Rd(L"NODIO_PUBUAC", b, MAX_PATH);
            wchar_t szNoDpi[8];
            opt.bDpiAware = !(GetEnvironmentVariableW(L"NODIO_PUBNODPI",
                                                      szNoDpi, 8) > 0 &&
                              szNoDpi[0] == L'1');
        }
        if (opt.strOut.empty())
        {
            // default: <project dir>\dist\<entry base>.exe
            std::wstring entry = szPubFile;
            size_t sl = entry.find_last_of(L"\\/");
            std::wstring dir = sl == std::wstring::npos
                                   ? std::wstring() : entry.substr(0, sl);
            std::wstring base = sl == std::wstring::npos
                                    ? entry : entry.substr(sl + 1);
            size_t dot = base.find_last_of(L'.');
            if (dot != std::wstring::npos) base = base.substr(0, dot);
            opt.strOut = (dir.empty() ? std::wstring() : dir + L"\\dist");
            CreateDirectoryW(opt.strOut.c_str(), nullptr);
            opt.strOut += L"\\" + base + L".exe";
        }

        std::wstring strErr;
        if (!NodioPublish::Publish(opt, strErr))
        {
            WriteStderrUtf8(L"错误位置:发布\n错误信息:" + strErr);
            return 1;
        }
        WriteStderrUtf8(L"发布完成: " + opt.strOut);
        return 0;
    }

    // Published-exe mode: a payload appended to this very executable runs
    // its embedded entry script with embedded file resolution (no external
    // library tree required).
    {
        wchar_t szSelf[MAX_PATH] = L"";
        GetModuleFileNameW(nullptr, szSelf, MAX_PATH);
        std::map<std::wstring, std::string> raw;
        if (NodioPayload::ReadPayload(szSelf, raw) && !raw.empty())
        {
            EnsureConsole(false);
            std::unordered_map<std::wstring, std::wstring> wide;
            NodioPayload::ToWideMap(raw, wide);

            // entry = top-level main script, else the first
            // top-level script file matching configured extensions
            std::wstring strEntry;
            for (auto& kv : wide)
            {
                const std::wstring& k = kv.first;
                if (k.find(L'\\') != std::wstring::npos) continue;
                std::wstring lower = k;
                for (auto& c : lower) c = towlower(c);
                if (lower.rfind(L"main.", 0) == 0 && NodioVM::HasScriptExtension(lower))
                {
                    strEntry = k;
                    break;
                }
                bool bScript = NodioVM::HasScriptExtension(lower);
                if (strEntry.empty() && bScript)
                    strEntry = k;
            }
            if (strEntry.empty())
            {
                WriteStderrUtf8(L"运行错误: 载荷中没有可执行的启动脚本");
                return 1;
            }
            std::wstring strErr;
            std::wstring strBase = L"";   // no disk base: embedded only
            // S-2: the entry script's name inside the package is the file
            // a runtime error belongs to.
            NodioVM::RunSource(wide[strEntry], &strErr, &strBase, &wide,
                               false, nullptr, &strEntry);
            if (!strErr.empty())
                WriteStderrUtf8(strErr);
            wchar_t szPause[8];
            if (GetEnvironmentVariableW(L"NODIO_PAUSE", szPause, 8) > 0 &&
                szPause[0] == L'1')
                _wsystem(L"pause");
            return strErr.empty() ? 0 : 1;
        }
    }

    EnsureConsole(false);
    wprintf(L"Nodio engine host — set NODIO_RUN=<script> to run a "
            L"script, or see docs/roadmap.md\n");
    return 0;
}
