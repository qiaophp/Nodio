// util.cpp — deep copy, HTTP fetch, thread-shared map, UTF-8 output.
#include "engine/common.h"

namespace NodioVM
{
    Value DeepCopy(const Value& v)
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

    std::map<wstring, Value> g_threadSharedMap;
    std::mutex g_threadSharedMutex;

    wstring HttpFetch(const wstring& strUrl, const wchar_t* szMethod,
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

    void WriteU8(FILE* f, const wstring& s)
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
}
