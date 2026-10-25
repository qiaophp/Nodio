// util.cpp — deep copy, HTTP fetch, thread-shared map, UTF-8 output.
#include "engine/common.h"

namespace NodioVM
{
    static Value DeepCopyInternal(const Value& v, std::unordered_map<Table*, shared_ptr<Table>>& visited)
    {
        // 第四十二轮：拷贝迭代化。thread 序列化一个 5 万层嵌套表会把
        // 递归版烧穿线程栈段错误（实测 exit 139 静默死亡）。显式工作栈，
        // 语义逐路保持：visited 命中壳（Class 重建侧表）、新表先注册、
        // "self" 回指新壳、Func 字面量早退、标量/native 尾路（fnData
        // 深拷贝后清 nsClosure/fn）。同一张新表的全部目标槽位先落地再
        // 填充，处理顺序 DFS→LIFO 不影响结果；新建对象受当前语句纪元
        // 保护，拷贝途中 GC 不会扫走半成品。
        struct DeepCopyItem
        {
            const Value* src;
            Value* dst;
        };
        Value root;
        std::vector<DeepCopyItem> stack;
        stack.push_back(DeepCopyItem{ &v, &root });
        while (!stack.empty())
        {
            DeepCopyItem it = stack.back();
            stack.pop_back();
            const Value& s = *it.src;
            if ((s.t == Value::TableT || s.t == Value::Class) && s.tab)
            {
                auto hit = visited.find(s.tab.get());
                if (hit != visited.end())
                {
                    Value res = Value::Table_(hit->second);
                    if (s.t == Value::Class)
                    {
                        res.t = Value::Class;
                        res.fnData = std::make_shared<FuncData>();
                        res.fnData->fn = s.fnData->fn;
                    }
                    *it.dst = std::move(res);
                    continue;
                }

                auto nt = std::make_shared<Table>();
                visited[s.tab.get()] = nt;

                for (auto& kv : s.tab->map)
                {
                    if (kv.first == L"self") continue;
                    stack.push_back(DeepCopyItem{ &kv.second, &nt->map[kv.first] });
                }
                nt->arr.resize(s.tab->arr.size());
                for (size_t i = 0; i < s.tab->arr.size(); i++)
                    stack.push_back(DeepCopyItem{ &s.tab->arr[i], &nt->arr[i] });

                Value res = Value::Table_(nt);
                if (s.t == Value::Class)
                {
                    res.t = Value::Class;
                    res.fnData = std::make_shared<FuncData>();
                    res.fnData->fn = s.fnData->fn;
                }
                if (s.tab->map.count(L"self"))
                    nt->map[L"self"] = res;
                *it.dst = std::move(res);
                continue;
            }
            // functions cross threads as a re-bindable literal: the AST
            // (PNode) is immutable and shared, the worker VM re-resolves
            // bare names against its own globals (multi-threading: thread
            // functions get an independent global environment)
            if (s.t == Value::Func && s.fnData->fn)
            {
                Value x = s;
                x.tab.reset();
                x.native = nullptr;
                *it.dst = std::move(x);
                continue;
            }
            Value x = s;
            x.tab.reset();
            // 标量（Null/Num/Str）与 native 函数也走这条尾路：fnData 为空时
            // 没有 函数数据可清（旧代码等价于对 null shared_ptr reset）
            if (s.fnData)
            {
                // 深拷贝函数数据再清除 nsClosure：fnData 现在是共享侧表，
                // 原地 reset 会把原函数的命名空间闭包一起清掉
                x.fnData = std::make_shared<FuncData>(*s.fnData);
                x.fnData->nsClosure.reset();
                x.fnData->fn.reset();
            }
            x.native = nullptr;
            *it.dst = std::move(x);
        }
        return root;
    }

    Value DeepCopy(const Value& v)
    {
        std::unordered_map<Table*, shared_ptr<Table>> visited;
        return DeepCopyInternal(v, visited);
    }

    // thread.create worker state (round 36)

    std::map<wstring, Value> g_threadSharedMap;
    std::recursive_mutex g_threadSharedMutex;

    wstring HttpFetch(const wstring& strUrl, const wchar_t* szMethod,
                             const wstring& strBody,
                             const wstring& strCType, wstring* pErr)
    {
        wstring result;
        HINTERNET hOpen = InternetOpenW(GetNodioConfig().userAgent.c_str(),
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

    static std::mutex s_bufMutex;
    static std::unordered_map<const void*, size_t> s_bufSizes;

    void RegisterBuffer(void* ptr, size_t size)
    {
        if (!ptr) return;
        std::lock_guard<std::mutex> lock(s_bufMutex);
        s_bufSizes[ptr] = size;
    }

    void UnregisterBuffer(void* ptr)
    {
        if (!ptr) return;
        std::lock_guard<std::mutex> lock(s_bufMutex);
        s_bufSizes.erase(ptr);
    }

    bool IsBuffer(const void* ptr)
    {
        if (!ptr) return false;
        std::lock_guard<std::mutex> lock(s_bufMutex);
        return s_bufSizes.find(ptr) != s_bufSizes.end();
    }

    size_t GetBufferSize(const void* ptr)
    {
        if (!ptr) return 0;
        std::lock_guard<std::mutex> lock(s_bufMutex);
        auto it = s_bufSizes.find(ptr);
        return it != s_bufSizes.end() ? it->second : 0;
    }

    static NodioConfig s_config;
    static std::once_flag s_configOnce;

    static std::string UnescapeJsonString(const std::string& in)
    {
        std::string out;
        out.reserve(in.size());
        for (size_t i = 0; i < in.size(); ++i)
        {
            if (in[i] == '\\' && i + 1 < in.size())
            {
                char c = in[++i];
                switch (c)
                {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                default:
                    out.push_back('\\');
                    out.push_back(c);
                    break;
                }
            }
            else
            {
                out.push_back(in[i]);
            }
        }
        return out;
    }

    static size_t FindJsonStringEnd(const std::string& s, size_t startQuote)
    {
        for (size_t i = startQuote + 1; i < s.size(); ++i)
        {
            if (s[i] == '\\' && i + 1 < s.size())
            {
                i++;
            }
            else if (s[i] == '"')
            {
                return i;
            }
        }
        return std::string::npos;
    }

    static void LoadConfigInternal()
    {
        std::wstring configPath;
        wchar_t szEnvConfig[MAX_PATH] = L"";
        if (GetEnvironmentVariableW(L"NODIO_CONFIG", szEnvConfig, MAX_PATH) > 0 &&
            _waccess(szEnvConfig, 0) == 0)
        {
            configPath = szEnvConfig;
        }
        else if (_waccess(L"nodio.json", 0) == 0)
        {
            configPath = L"nodio.json";
        }
        else
        {
            wchar_t szExe[MAX_PATH] = L"";
            if (GetModuleFileNameW(nullptr, szExe, MAX_PATH) > 0)
            {
                std::wstring exeDir = szExe;
                size_t nSlash = exeDir.find_last_of(L"\\/");
                if (nSlash != std::wstring::npos)
                {
                    std::wstring cand = exeDir.substr(0, nSlash + 1) + L"nodio.json";
                    if (_waccess(cand.c_str(), 0) == 0)
                        configPath = cand;
                    else
                    {
                        std::wstring candParent = exeDir.substr(0, nSlash) + L"\\..\\nodio.json";
                        if (_waccess(candParent.c_str(), 0) == 0)
                            configPath = candParent;
                        else
                        {
                            std::wstring candGrandParent = exeDir.substr(0, nSlash) + L"\\..\\..\\nodio.json";
                            if (_waccess(candGrandParent.c_str(), 0) == 0)
                                configPath = candGrandParent;
                        }
                    }
                }
            }
        }

        std::string rawJson;
        if (!configPath.empty())
        {
            FILE* pf = nullptr;
            if (_wfopen_s(&pf, configPath.c_str(), L"rb") == 0 && pf)
            {
                char buf[4096];
                size_t rd = 0;
                while ((rd = fread(buf, 1, sizeof(buf), pf)) > 0)
                    rawJson.append(buf, rd);
                fclose(pf);
            }
        }

        if (!rawJson.empty())
        {
            size_t pos = rawJson.find("\"extensions\"");
            if (pos != std::string::npos)
            {
                size_t bStart = rawJson.find('[', pos);
                size_t bEnd = (bStart != std::string::npos) ? rawJson.find(']', bStart) : std::string::npos;
                if (bStart != std::string::npos && bEnd != std::string::npos)
                {
                    size_t cur = bStart + 1;
                    while (cur < bEnd)
                    {
                        size_t q1 = rawJson.find('"', cur);
                        if (q1 == std::string::npos || q1 >= bEnd) break;
                        size_t q2 = FindJsonStringEnd(rawJson, q1);
                        if (q2 == std::string::npos || q2 > bEnd) break;
                        std::string item = UnescapeJsonString(rawJson.substr(q1 + 1, q2 - q1 - 1));
                        if (!item.empty())
                        {
                            if (item[0] != '.') item = "." + item;
                            int w = MultiByteToWideChar(CP_UTF8, 0, item.c_str(), (int)item.size(), nullptr, 0);
                            if (w > 0)
                            {
                                std::wstring wItem((size_t)w, 0);
                                MultiByteToWideChar(CP_UTF8, 0, item.c_str(), (int)item.size(), &wItem[0], w);
                                s_config.extensions.push_back(wItem);
                            }
                        }
                        cur = q2 + 1;
                    }
                }
            }
            size_t pMagic = rawJson.find("\"magicHeaders\"");
            if (pMagic != std::string::npos)
            {
                size_t bStart = rawJson.find('[', pMagic);
                size_t bEnd = (bStart != std::string::npos) ? rawJson.find(']', bStart) : std::string::npos;
                if (bStart != std::string::npos && bEnd != std::string::npos)
                {
                    size_t cur = bStart + 1;
                    while (cur < bEnd)
                    {
                        size_t q1 = rawJson.find('"', cur);
                        if (q1 == std::string::npos || q1 >= bEnd) break;
                        size_t q2 = FindJsonStringEnd(rawJson, q1);
                        if (q2 == std::string::npos || q2 > bEnd) break;
                        std::string item = UnescapeJsonString(rawJson.substr(q1 + 1, q2 - q1 - 1));
                        if (!item.empty())
                        {
                            int w = MultiByteToWideChar(CP_UTF8, 0, item.c_str(), (int)item.size(), nullptr, 0);
                            if (w > 0)
                            {
                                std::wstring wItem((size_t)w, 0);
                                MultiByteToWideChar(CP_UTF8, 0, item.c_str(), (int)item.size(), &wItem[0], w);
                                s_config.magicHeaders.push_back(wItem);
                            }
                        }
                        cur = q2 + 1;
                    }
                }
            }
            auto ParseStringField = [&](const char* fieldName, std::wstring& out) {
                size_t p = rawJson.find(fieldName);
                if (p != std::string::npos)
                {
                    size_t colon = rawJson.find(':', p);
                    if (colon != std::string::npos)
                    {
                        size_t q1 = rawJson.find('"', colon);
                        if (q1 != std::string::npos)
                        {
                            size_t q2 = FindJsonStringEnd(rawJson, q1);
                            if (q2 != std::string::npos)
                            {
                                std::string val = UnescapeJsonString(rawJson.substr(q1 + 1, q2 - q1 - 1));
                                int w = MultiByteToWideChar(CP_UTF8, 0, val.c_str(), (int)val.size(), nullptr, 0);
                                if (w > 0)
                                {
                                    out.resize((size_t)w);
                                    MultiByteToWideChar(CP_UTF8, 0, val.c_str(), (int)val.size(), &out[0], w);
                                }
                            }
                        }
                    }
                }
            };
            ParseStringField("\"defaultTitle\"", s_config.defaultTitle);
            ParseStringField("\"windowClass\"", s_config.windowClass);
            ParseStringField("\"userAgent\"", s_config.userAgent);
            ParseStringField("\"libDir\"", s_config.libDir);
        }

        if (s_config.extensions.empty())
        {
            wchar_t szEnvExt[512] = L"";
            if (GetEnvironmentVariableW(L"NODIO_EXTENSIONS", szEnvExt, 512) > 0)
            {
                std::wstring envExt = szEnvExt;
                size_t st = 0;
                while (st < envExt.size())
                {
                    size_t sep = envExt.find_first_of(L";,", st);
                    if (sep == std::wstring::npos) sep = envExt.size();
                    std::wstring token = envExt.substr(st, sep - st);
                    while (!token.empty() && (token.front() == L' ' || token.front() == L'\t')) token.erase(token.begin());
                    while (!token.empty() && (token.back() == L' ' || token.back() == L'\t')) token.pop_back();
                    if (!token.empty())
                    {
                        if (token[0] != L'.') token = L"." + token;
                        s_config.extensions.push_back(token);
                    }
                    st = sep + 1;
                }
            }
        }

        // 官方契约的扩展名与魔数是**引擎内置**的，不依赖 nodio.json：
        //   .aardio  —— 官方标准库的唯一源码扩展名（lib\ 下 989 个文件）
        //   =aardio  —— 官方预编译块（=aardio）魔数
        // nodio.json 只是**追加**扩展名，不是替换：配置文件找不到
        // （发布产物、自定义部署目录、CI 的临时工作目录）时，官方库
        // 仍必须可用，否则整个标准库会静默失效。
        // 查找顺序上 .nodio 优先于 .aardio：同名文件时 Nodio 自己的
        // 源码优先。
        {
            static const std::vector<std::wstring> kBuiltinExt = {
                L".nodio", L".aardio"
            };
            static const std::vector<std::wstring> kBuiltinMagic = {
                L"=nodio", L"=bytecode", L"=aardio"
            };
            std::vector<std::wstring> exts = kBuiltinExt;
            for (const auto& e : s_config.extensions)
            {
                bool bDup = false;
                for (const auto& b : exts)
                    if (_wcsicmp(b.c_str(), e.c_str()) == 0) { bDup = true; break; }
                if (!bDup) exts.push_back(e);
            }
            s_config.extensions = exts;

            std::vector<std::wstring> magic = kBuiltinMagic;
            for (const auto& e : s_config.magicHeaders)
            {
                bool bDup = false;
                for (const auto& b : magic)
                    if (b == e) { bDup = true; break; }
                if (!bDup) magic.push_back(e);
            }
            s_config.magicHeaders = magic;
        }
        if (s_config.defaultTitle.empty())
        {
            s_config.defaultTitle = L"Nodio";
        }
        if (s_config.windowClass.empty())
        {
            s_config.windowClass = L"Nodio_Form";
        }
        if (s_config.userAgent.empty())
        {
            s_config.userAgent = L"Nodio/1.0";
        }
    }

    const NodioConfig& GetNodioConfig()
    {
        std::call_once(s_configOnce, LoadConfigInternal);
        return s_config;
    }

    const std::vector<std::wstring>& GetConfiguredExtensions()
    {
        return GetNodioConfig().extensions;
    }

    const std::vector<std::wstring>& GetConfiguredMagicHeaders()
    {
        return GetNodioConfig().magicHeaders;
    }

    bool HasScriptExtension(const std::wstring& filename)
    {
        for (const auto& ext : GetConfiguredExtensions())
        {
            if (filename.size() >= ext.size() &&
                _wcsicmp(filename.c_str() + filename.size() - ext.size(), ext.c_str()) == 0)
            {
                return true;
            }
        }
        return false;
    }

    std::wstring StripScriptExtension(const std::wstring& filename)
    {
        for (const auto& ext : GetConfiguredExtensions())
        {
            if (filename.size() >= ext.size() &&
                _wcsicmp(filename.c_str() + filename.size() - ext.size(), ext.c_str()) == 0)
            {
                return filename.substr(0, filename.size() - ext.size());
            }
        }
        return filename;
    }

    bool IsCompiledModule(const wstring& src)
    {
        for (const auto& hdr : GetConfiguredMagicHeaders())
        {
            if (src.size() >= hdr.size() && src.compare(0, hdr.size(), hdr) == 0)
                return true;
        }
        return false;
    }
}

