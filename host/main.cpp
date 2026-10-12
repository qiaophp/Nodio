// host/main.cpp — Nodio standalone console host.
//
// Role parity with the retired MFC suitcase (CAardioApp InitInstance):
//   AARDIO_RUN=<file>              run a script headlessly and exit
//                                  (F5 runtime + regression harness)
//   AARDIO_LIBDIR=<dir>            official-library root for `import`
//   AARDIO_DEBUG=1                 mini-debugger on
//   AARDIO_BREAK="3,7"             seed breakpoints (implies debug)
//   AARDIO_CONSOLE=1               force-allocate a console (io.print)
//   AARDIO_PAUSE=1                 keep the console open afterwards
//   AARDIO_PUBLISH=...             (Phase 2) publisher pipeline — stub
//
// The publisher is intentionally absent here; it is rebuilt against the
// clean engine in Phase 2 (binary-safe resource embedding, PE stamper).

#include <windows.h>
#include "engine/AardioVM.h"
#include <cstdio>
#include <cstdlib>
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
    // AARDIO_RUN regression / F5 harness
    wchar_t szRunFile[MAX_PATH];
    if (GetEnvironmentVariableW(L"AARDIO_RUN", szRunFile, MAX_PATH) > 0)
    {
        wchar_t szCon[8];
        bool bForceConsole =
            GetEnvironmentVariableW(L"AARDIO_CONSOLE", szCon, 8) > 0 &&
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
        if (GetEnvironmentVariableW(L"AARDIO_DEBUG", szDbg, 16) > 0 &&
            szDbg[0] == L'1')
            bDbg = true;

        wchar_t szBrk[1024];
        std::wstring strBreaks;
        if (GetEnvironmentVariableW(L"AARDIO_BREAK", szBrk, 1024) > 0)
        {
            strBreaks = szBrk;
            bDbg = true;
        }

        std::wstring strErr;
        AardioVM::RunSource(src, &strErr, &strDir, nullptr, bDbg,
                            strBreaks.empty() ? nullptr : &strBreaks);
        if (!strErr.empty())
            WriteStderrUtf8(strErr);
        fflush(stdout);

        wchar_t szPause[8];
        if (GetEnvironmentVariableW(L"AARDIO_PAUSE", szPause, 8) > 0 &&
            szPause[0] == L'1')
            _wsystem(L"pause");
        return strErr.empty() ? 0 : 1;
    }

    // Phase 2 placeholder: publisher pipeline
    wchar_t szPubFile[MAX_PATH];
    if (GetEnvironmentVariableW(L"AARDIO_PUBLISH", szPubFile, MAX_PATH) > 0)
    {
        EnsureConsole(false);
        WriteStderrUtf8(L"错误位置:发布\n错误信息:Nodio 发布管线尚未重建 (Phase 2 计划项)");
        return 1;
    }

    EnsureConsole(false);
    wprintf(L"Nodio aardio engine host — set AARDIO_RUN=<script> to run a "
            L"script, or see docs/roadmap.md\n");
    return 0;
}
