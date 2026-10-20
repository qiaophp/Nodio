// apps/nodio-stub/main.cpp — minimal standalone runtime stub for published applications.
// Contains zero publishing logic, zero dev/debug CLI tooling, zero syntax check scanners.
#include <windows.h>
#include <commctrl.h>
#pragma comment(lib, "comctl32.lib")
#include "engine/common.h"
#include "engine/NodioVM.h"
#include "publish/payload.h"
#include <string>
#include <vector>
#include <map>
#include <unordered_map>

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

int wmain(int argc, wchar_t** argv)
{
    INITCOMMONCONTROLSEX icc = { sizeof(INITCOMMONCONTROLSEX), ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES | ICC_COOL_CLASSES };
    InitCommonControlsEx(&icc);

    wchar_t szSelf[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, szSelf, MAX_PATH);

    std::map<std::wstring, std::string> raw;
    if (!NodioPayload::ReadPayload(szSelf, raw) || raw.empty())
    {
        MessageBoxW(nullptr, L"Nodio Runtime Stub: No embedded payload found.", L"Nodio", MB_OK | MB_ICONERROR);
        return 1;
    }

    std::unordered_map<std::wstring, std::wstring> wide;
    NodioPayload::ToWideMap(raw, wide);

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
        if (strEntry.empty() && NodioVM::HasScriptExtension(lower))
            strEntry = k;
    }

    if (strEntry.empty())
    {
        WriteStderrUtf8(L"运行错误: 载荷中没有可执行的启动脚本");
        return 1;
    }

    std::wstring strErr;
    std::wstring strBase = L"";
    // S-2: the entry script's name inside the package is the file a
    // runtime error belongs to.
    NodioVM::RunSource(wide[strEntry], &strErr, &strBase, &wide, false, nullptr,
                       &strEntry);
    if (!strErr.empty())
    {
        WriteStderrUtf8(strErr);
    }

    wchar_t szPause[8];
    if (GetEnvironmentVariableW(L"NODIO_PAUSE", szPause, 8) > 0 &&
        szPause[0] == L'1')
        _wsystem(L"pause");

    return strErr.empty() ? 0 : 1;
}
