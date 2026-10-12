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
//   NODIO_PUBLISH=<main.aardio>   build a standalone exe
//   NODIO_PUBOUT=<out.exe>        output path (default <dir>\dist\<name>.exe)
//   NODIO_ICON=<icon.ico>         optional icon stamped into the output

#include <windows.h>
#include "engine/NodioVM.h"
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

static void EnsureConsole(bool bForce)
{
    if (!GetConsoleWindow() &&
        (bForce || !GetStdHandle(STD_OUTPUT_HANDLE)))
    {
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

        std::wstring strDir = szRunFile;
        size_t nSlash = strDir.find_last_of(L'\\');
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
        NodioVM::RunSource(src, &strErr, &strDir, nullptr, bDbg,
                            strBreaks.empty() ? nullptr : &strBreaks);
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

            // entry = top-level main.aardio / main.nodio, else the first
            // top-level .aardio / .nodio file
            std::wstring strEntry;
            for (auto& kv : wide)
            {
                const std::wstring& k = kv.first;
                if (k.find(L'\\') != std::wstring::npos) continue;
                std::wstring lower = k;
                for (auto& c : lower) c = towlower(c);
                if (lower == L"main.aardio" || lower == L"main.nodio")
                {
                    strEntry = k;
                    break;
                }
                bool bScript = (lower.size() > 7 &&
                                lower.substr(lower.size() - 7) == L".aardio") ||
                               (lower.size() > 6 &&
                                lower.substr(lower.size() - 6) == L".nodio");
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
            NodioVM::RunSource(wide[strEntry], &strErr, &strBase, &wide,
                               false, nullptr);
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
    wprintf(L"Nodio aardio engine host — set NODIO_RUN=<script> to run a "
            L"script, or see docs/roadmap.md\n");
    return 0;
}
