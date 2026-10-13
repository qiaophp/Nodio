#include "engine/interp.h"
#include "engine/parser.h"
#include <clocale>

namespace NodioVM
{
    // ---- runtime compilation (eval / loadcode / loadcodex) ----
    // Compiles 'src' into an anonymous function: the code becomes the body
    // and '...' collects the arguments, because in aardio a loaded code
    // string or file behaves exactly like an anonymous function.
    static PNode CompileToFunc(const wstring& src, wstring* err)
    {
        wstring e;
        vector<Tok> toks = Lex(src, &e);
        if (!e.empty())
        {
            if (err) *err = e;
            return nullptr;
        }
        Parser ps{ toks, 0, &e };
        if (g_interpCurrent && !g_interpCurrent->baseDir.empty())
            ps.strEmbedBase = g_interpCurrent->baseDir;
        ps.strEmbedRoot = EmbedRoot();
        PNode blk = ps.ParseBlock();
        if (!e.empty())
        {
            if (err) *err = e;
            return nullptr;
        }
        PNode fn = std::make_shared<Node>();
        fn->k = Node::FuncLit;
        fn->a = blk;
        fn->names.push_back(L"...");
        fn->defaults.push_back(nullptr);
        return fn;
    }

    // aardio loadcode/loadcodex accept either a code string or a file path.
    // Root-relative ('/x') resolves against the app root, '~' against the
    // installation root; a multi-line argument is always code.
    static bool ReadCodeFile(const wstring& s, wstring* out)
    {
        if (s.empty() || s.find(L'\n') != wstring::npos ||
            s.find(L'\r') != wstring::npos)
            return false;
        wstring p = s;
        if (p[0] == L'/' || p[0] == L'\\')
        {
            wstring root;
            if (g_interpCurrent && !g_interpCurrent->baseDir.empty())
                root = g_interpCurrent->baseDir;
            if (root.empty())
            {
                wchar_t szExe[MAX_PATH] = L"";
                GetModuleFileNameW(nullptr, szExe, MAX_PATH);
                wchar_t* q = wcsrchr(szExe, L'\\');
                if (q) *q = 0;
                root = szExe;
            }
            p = root + L"\\" + p.substr(1);
        }
        else if (p[0] == L'~')
            p = ResolveTildeDisk(p);
        FILE* pf = nullptr;
        if (_wfopen_s(&pf, p.c_str(), L"rb") != 0 || !pf)
            return false;
        fseek(pf, 0, SEEK_END);
        long sz = ftell(pf);
        fseek(pf, 0, SEEK_SET);
        std::string raw;
        if (sz > 0)
        {
            raw.resize((size_t)sz);
            fread(&raw[0], 1, (size_t)sz, pf);
        }
        fclose(pf);
        size_t off = (raw.size() >= 3 && (unsigned char)raw[0] == 0xEF &&
                      (unsigned char)raw[1] == 0xBB &&
                      (unsigned char)raw[2] == 0xBF) ? 3 : 0;
        int w = MultiByteToWideChar(CP_UTF8, 0, raw.data() + off,
                                    (int)(raw.size() - off), nullptr, 0);
        wstring r((size_t)(w > 0 ? w : 0), 0);
        if (w > 0)
            MultiByteToWideChar(CP_UTF8, 0, raw.data() + off,
                                (int)(raw.size() - off), &r[0], w);
        *out = r;
        return true;
    }
    // wide string -> ANSI bytes (the raw.* memory helpers work on bytes)
    static std::string ToA(const wstring& w)
    {
        if (w.empty()) return std::string();
        int n = WideCharToMultiByte(CP_ACP, 0, w.c_str(), (int)w.size(),
                                    nullptr, 0, nullptr, nullptr);
        if (n <= 0) return std::string();
        std::string s((size_t)n, 0);
        WideCharToMultiByte(CP_ACP, 0, w.c_str(), (int)w.size(),
                            &s[0], n, nullptr, nullptr);
        return s;
    }

// ---- InitBuiltins ----
void Interp::InitBuiltins()
        {
            // ---- global call() builtin: call(fn, owner, ...)
            // returns ok, ret (metaProperty machinery depends on it)
            globals.vars[L"call"] =
                NF([this](vector<Value>& a) -> Value {
                auto finish = [this](bool ok, Value r) -> Value {
                    Value vOk = Value::Bool_(ok);
                    auto extras = std::make_shared<vector<Value>>();
                    extras->push_back(r);
                    if (r.mrets)
                    {
                        for (auto& item : *r.mrets)
                            extras->push_back(item);
                    }
                    vOk.mrets = extras;
                    return vOk;
                };
                if (a.empty() || (a[0].t != Value::Func &&
                                  a[0].t != Value::Class &&
                                  a[0].t != Value::TableT))
                    return finish(false, Value());
                try
                {
                    vector<Value> args;
                    for (size_t k = 2; k < a.size(); k++)
                        args.push_back(a[k]);
                    Value r = CallOwner(a[0],
                        a.size() > 1 ? a[1] : Value(), args);
                    if (err && !err->empty())
                    {
                        wstring e = *err;
                        err->clear();
                        return finish(false, Value::Str_(e));
                    }
                    return finish(true, r);
                }
                catch (...)
                {
                    return finish(false, Value());
                }
            });

            // ---- string (1-based, aardio/Lua style) ----
            {
                auto tb = GetRoot(L"string").tab;
                // string.repeat(s,n) — repeat the string n times
                tb->map[L"repeat"] = NF([](vector<Value>& a) -> Value {
                    wstring s = BStr(a[0]);
                    long long n = a.size() > 1 ? (long long)BNum(a[1]) : 1;
                    wstring out;
                    for (long long i = 0; i < n; i++)
                        out += s;
                    return Value::Str_(out); });
                // string.sub(s,i,j) — 1-based substring, negative = from end
                tb->map[L"sub"] = NF([](vector<Value>& a) -> Value {
                    wstring s = BStr(a[0]);
                    long long n = (long long)s.size();
                    long long st = a.size() > 1 ? (long long)BNum(a[1]) : 1;
                    long long en = a.size() > 2 ? (long long)BNum(a[2]) : n;
                    if (st < 0) st = n + st + 1;
                    if (en < 0) en = n + en + 1;
                    if (st < 1) st = 1;
                    if (en > n) en = n;
                    if (en < st) return Value::Str_(L"");
                    return Value::Str_(s.substr((size_t)(st - 1),
                                                (size_t)(en - st + 1))); });
                // string.reverse(s)
                tb->map[L"reverse"] = NF([](vector<Value>& a) -> Value {
                    wstring s = BStr(a[0]);
                    std::reverse(s.begin(), s.end());
                    return Value::Str_(s); });
                tb->map[L"len"] = NF([](vector<Value>& a) {
                    return Value::Num_((double)BStr(a[0]).size()); });
                tb->map[L"left"] = NF([](vector<Value>& a) {
                    wstring s = BStr(a[0]); size_t k = (size_t)BNum(a[1]);
                    return Value::Str_(s.substr(0, min(k, s.size()))); });
                tb->map[L"right"] = NF([](vector<Value>& a) {
                    wstring s = BStr(a[0]); size_t k = (size_t)BNum(a[1]);
                    return Value::Str_(k >= s.size() ? s : s.substr(s.size() - k)); });
                tb->map[L"mid"] = NF([](vector<Value>& a) {
                    wstring s = BStr(a[0]);
                    long long st = (long long)BNum(a[1]); if (st < 1) st = 1;
                    long long ct = a.size() > 2 ? (long long)BNum(a[2]) : -1;
                    if (ct < 0 || (size_t)(st - 1 + ct) > s.size())
                        ct = (long long)s.size() - (st - 1);
                    if (ct < 0) ct = 0;
                    return Value::Str_(s.substr((size_t)(st - 1), (size_t)ct)); });
                tb->map[L"upper"] = NF([](vector<Value>& a) {
                    wstring s = BStr(a[0]);
                    for (auto& c : s) c = (wchar_t)towupper(c);
                    return Value::Str_(s); });
                tb->map[L"lower"] = NF([](vector<Value>& a) {
                    wstring s = BStr(a[0]);
                    for (auto& c : s) c = (wchar_t)towlower(c);
                    return Value::Str_(s); });
                tb->map[L"trim"] = NF([](vector<Value>& a) {
                    wstring s = BStr(a[0]);
                    size_t b = s.find_first_not_of(L" \t\r\n");
                    if (b == wstring::npos) return Value::Str_(L"");
                    size_t e = s.find_last_not_of(L" \t\r\n");
                    return Value::Str_(s.substr(b, e - b + 1)); });
                tb->map[L"index"] = NF([](vector<Value>& a) {
                    wstring s = BStr(a[0]), sub = BStr(a[1]);
                    size_t from = a.size() > 2 ? (size_t)BNum(a[2]) : 1;
                    if (from < 1) from = 1;
                    size_t pos = s.find(sub, from - 1);
                    if (pos == wstring::npos) return Value();
                    return Value::Num_((double)(pos + 1)); });
                tb->map[L"find"] = NF([](vector<Value>& a) {
                    // pattern search (aardio string.find): {start,end} table
                    wstring s = BStr(a[0]), pat = BStr(a[1]);
                    size_t start = a.size() > 2 ? (size_t)BNum(a[2]) : 1;
                    if (start < 1) start = 1;
                    vector<vector<RE>> br; bool anchored = false;
                    int nGroups = 0; wstring perr;
                    if (!RECompile(pat, br, &anchored, &nGroups, &perr))
                        return Value();
                    vector<wstring> caps; size_t end = 0;
                    for (size_t p0 = start - 1; p0 <= s.size(); p0++)
                    {
                        if (RETry(br, s, p0, false, caps, &end))
                        {
                            auto tb2 = std::make_shared<Table>();
                            tb2->arr.push_back(Value::Num_((double)(p0 + 1)));
                            tb2->arr.push_back(Value::Num_((double)end));
                            tb2->map[L"start"] = Value::Num_((double)(p0 + 1));
                            tb2->map[L"end"] = Value::Num_((double)end);
                            return Value::Table_(tb2);
                        }
                        if (anchored) break;
                    }
                    return Value(); });
                tb->map[L"match"] = NF([](vector<Value>& a) {
                    // no groups -> whole match; 1 group -> it; more -> table
                    wstring s = BStr(a[0]), pat = BStr(a[1]);
                    size_t start = a.size() > 2 ? (size_t)BNum(a[2]) : 1;
                    if (start < 1) start = 1;
                    vector<vector<RE>> br; bool anchored = false;
                    int nGroups = 0; wstring perr;
                    if (!RECompile(pat, br, &anchored, &nGroups, &perr))
                        return Value();
                    vector<wstring> caps; size_t end = 0;
                    for (size_t p0 = start - 1; p0 <= s.size(); p0++)
                    {
                        if (RETry(br, s, p0, false, caps, &end))
                        {
                            if (nGroups == 0)
                                return Value::Str_(s.substr(p0, end - p0));
                            if (nGroups == 1)
                                return (size_t)1 < caps.size()
                                    ? Value::Str_(caps[1]) : Value::Str_(L"");
                            auto tb2 = std::make_shared<Table>();
                            for (size_t g = 1; g <= (size_t)nGroups &&
                                               g < caps.size(); g++)
                                tb2->arr.push_back(Value::Str_(caps[g]));
                            return Value::Table_(tb2);
                        }
                        if (anchored) break;
                    }
                    return Value(); });
                tb->map[L"gsub"] = NF([](vector<Value>& a) {
                    wstring s = BStr(a[0]), pat = BStr(a[1]);
                    wstring repl = a.size() > 2 ? BStr(a[2]) : L"";
                    int limit = a.size() > 3 ? (int)BNum(a[3]) : -1;
                    vector<vector<RE>> br; bool anchored = false;
                    int nGroups = 0; wstring perr;
                    if (!RECompile(pat, br, &anchored, &nGroups, &perr))
                        return Value::Str_(s);
                    wstring out;
                    size_t cp = 0, p0 = 0;
                    int done = 0;
                    while (p0 <= s.size() && (limit < 0 || done < limit))
                    {
                        vector<wstring> caps; size_t end = 0;
                        if (RETry(br, s, p0, false, caps, &end))
                        {
                            out += s.substr(cp, p0 - cp);
                            for (size_t r = 0; r < repl.size(); r++)
                            {
                                if ((repl[r] == L'$' || repl[r] == L'\\') &&
                                    r + 1 < repl.size() && iswdigit(repl[r + 1]))
                                {
                                    int g = repl[r + 1] - L'0';
                                    if (g == 0)
                                        out += s.substr(p0, end - p0);
                                    else if ((size_t)g < caps.size())
                                        out += caps[(size_t)g];
                                    r++;
                                }
                                else
                                    out += repl[r];
                            }
                            done++;
                            if (end == p0)
                            {
                                if (p0 < s.size()) out += s[p0];
                                p0++;
                            }
                            else
                                p0 = end;
                            cp = p0;
                            continue;
                        }
                        if (p0 >= s.size()) break;
                        p0++;
                    }
                    out += s.substr(cp);
                    return Value::Str_(out); });
                tb->map[L"replace"] = NF([](vector<Value>& a) {
                    wstring s = BStr(a[0]), f = BStr(a[1]), r = BStr(a[2]);
                    if (f.empty()) return Value::Str_(s);
                    size_t pos = 0;
                    while ((pos = s.find(f, pos)) != wstring::npos)
                    {
                        s.replace(pos, f.size(), r);
                        pos += r.size();
                    }
                    return Value::Str_(s); });
                tb->map[L"split"] = NF([](vector<Value>& a) {
                    wstring s = BStr(a[0]), d = a.size() > 1 ? BStr(a[1]) : L",";
                    auto tb2 = std::make_shared<Table>();
                    if (d.empty())
                    {
                        for (auto& c : s) tb2->arr.push_back(Value::Str_(wstring(1, c)));
                    }
                    else
                    {
                        size_t pos = 0;
                        while (true)
                        {
                            size_t nx = s.find(d, pos);
                            if (nx == wstring::npos)
                            {
                                tb2->arr.push_back(Value::Str_(s.substr(pos)));
                                break;
                            }
                            tb2->arr.push_back(Value::Str_(s.substr(pos, nx - pos)));
                            pos = nx + d.size();
                        }
                    }
                    return Value::Table_(tb2); });
                tb->map[L"count"] = NF([](vector<Value>& a) {
                    wstring s = BStr(a[0]), f = BStr(a[1]);
                    if (f.empty()) return Value::Num_(0);
                    vector<vector<RE>> br; bool anchored = false;
                    int nGroups = 0; wstring perr;
                    if (!RECompile(f, br, &anchored, &nGroups, &perr))
                        return Value::Num_(0);
                    long long c = 0;
                    size_t p0 = 0;
                    while (p0 <= s.size())
                    {
                        vector<wstring> caps; size_t end = 0;
                        if (RETry(br, s, p0, false, caps, &end))
                        {
                            c++;
                            p0 = end > p0 ? end : p0 + 1;
                        }
                        else
                        {
                            if (p0 >= s.size()) break;
                            p0++;
                        }
                    }
                    return Value::Num_((double)c); });
                tb->map[L"rep"] = NF([](vector<Value>& a) {
                    wstring s = BStr(a[0]); long long k = (long long)BNum(a[1]);
                    wstring r;
                    for (long long i = 0; i < k; i++) r += s;
                    return Value::Str_(r); });
                tb->map[L"toUtf16"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty()) return Value::Str_(L"");
                    return Value::Str_(BStr(a[0])); });
                tb->map[L"fromUtf16"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty()) return Value();
                    if (a[0].t == Value::Str) return a[0];
                    if (a[0].t == Value::Pointer || a[0].t == Value::Num)
                    {
                        const wchar_t* p = (const wchar_t*)(INT_PTR)(long long)BNum(a[0]);
                        if (!p) return Value();
                        if (a.size() > 1 && a[1].t == Value::Num)
                        {
                            size_t len = (size_t)BNum(a[1]);
                            return Value::Str_(wstring(p, len));
                        }
                        return Value::Str_(wstring(p));
                    }
                    return Value(); });
                tb->map[L"pack"] = NF([](vector<Value>& a) -> Value {
                    wstring out;
                    std::function<void(const Value&)> addVal = [&](const Value& v) {
                        if (v.t == Value::Num)
                        {
                            out += (wchar_t)(unsigned char)(unsigned int)(long long)v.n;
                        }
                        else if (v.t == Value::Str)
                        {
                            out += v.s;
                        }
                        else if (v.t == Value::TableT && v.tab)
                        {
                            for (auto& item : v.tab->arr)
                                addVal(item);
                        }
                    };
                    for (auto& arg : a)
                        addVal(arg);
                    return Value::Str_(out); });
                tb->map[L"unpack"] = NF([](vector<Value>& a) -> Value {
                    auto t = std::make_shared<Table>();
                    if (a.empty()) return Value::Table_(t);
                    wstring s = BStr(a[0]);
                    int len = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
                    std::string u8(len, 0);
                    if (len > 0)
                        WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &u8[0], len, nullptr, nullptr);
                    for (unsigned char c : u8)
                        t->arr.push_back(Value::Num_((double)c));
                    return Value::Table_(t); });
                tb->map[L"format"] = NF([](vector<Value>& a) {
                    wstring fmt = BStr(a[0]);
                    wstring out;
                    size_t ai = 1;
                    for (size_t i = 0; i < fmt.size(); i++)
                    {
                        if (fmt[i] != L'%' || i + 1 >= fmt.size())
                        {
                            out += fmt[i];
                            continue;
                        }
                        wchar_t c = fmt[++i];
                        if (c == L'%') { out += L'%'; continue; }
                        wstring spec = L"%";
                        while (i < fmt.size() && (wcschr(L".0123456789", fmt[i]) ||
                               fmt[i] == L'l'))
                            spec += fmt[i++];
                        if (i >= fmt.size()) break;
                        spec += fmt[i];
                        Value arg = ai < a.size() ? a[ai++] : Value();
                        wchar_t buf[128];
                        switch (fmt[i])
                        {
                        case L's': out += BStr(arg); break;
                        case L'd': case L'i': case L'u':
                            swprintf(buf, 128, spec.c_str(), (long long)BNum(arg));
                            out += buf; break;
                        case L'f':
                            swprintf(buf, 128, spec.c_str(), BNum(arg));
                            out += buf; break;
                        case L'x': case L'X':
                            swprintf(buf, 128, spec.c_str(), (unsigned)BNum(arg));
                            out += buf; break;
                        case L'c': out += (wchar_t)BNum(arg); break;
                        default: out += arg.ToString(); break;
                        }
                    }
                    return Value::Str_(out); });
                tb->map[L"load"] = NF([](vector<Value>& a) {
                    FILE* pf = nullptr;
                    if (_wfopen_s(&pf, BStr(a[0]).c_str(), L"rb") != 0 || !pf)
                        return Value();
                    std::string raw2;
                    char buf2[4096]; size_t rd;
                    while ((rd = fread(buf2, 1, sizeof(buf2), pf)) > 0)
                        raw2.append(buf2, rd);
                    fclose(pf);
                    int off2 = (raw2.size() >= 3 && (BYTE)raw2[0] == 0xEF &&
                                (BYTE)raw2[1] == 0xBB && (BYTE)raw2[2] == 0xBF) ? 3 : 0;
                    int w2 = MultiByteToWideChar(CP_UTF8, 0, raw2.data() + off2,
                                                 (int)(raw2.size() - off2), nullptr, 0);
                    wstring s2((size_t)w2, 0);
                    if (w2 > 0)
                        MultiByteToWideChar(CP_UTF8, 0, raw2.data() + off2,
                                            (int)(raw2.size() - off2), &s2[0], w2);
                    return Value::Str_(s2); });
                tb->map[L"hex"] = NF([](vector<Value>& a) {
                    wstring str = BStr(a[0]);
                    int n = WideCharToMultiByte(CP_UTF8, 0, str.c_str(),
                        (int)str.size(), nullptr, 0, nullptr, nullptr);
                    std::string raw((size_t)n, 0);
                    WideCharToMultiByte(CP_UTF8, 0, str.c_str(),
                        (int)str.size(), &raw[0], n, nullptr, nullptr);
                    static const wchar_t* kHex = L"0123456789ABCDEF";
                    wstring out;
                    for (unsigned char c : raw)
                    {
                        out += kHex[c >> 4];
                        out += kHex[c & 0xF];
                    }
                    return Value::Str_(out); });
                tb->map[L"unhex"] = NF([](vector<Value>& a) {
                    wstring h = BStr(a[0]);
                    std::string raw;
                    for (size_t j = 0; j + 1 < h.size(); j += 2)
                        raw.push_back((char)wcstoul(h.substr(j, 2).c_str(),
                                                    nullptr, 16));
                    int w = MultiByteToWideChar(CP_UTF8, 0, raw.data(),
                                                (int)raw.size(), nullptr, 0);
                    wstring out((size_t)w, 0);
                    if (w > 0)
                        MultiByteToWideChar(CP_UTF8, 0, raw.data(),
                                            (int)raw.size(), &out[0], w);
                    return Value::Str_(out); });
                tb->map[L"base64"] = NF([](vector<Value>& a) {
                    wstring str = BStr(a[0]);
                    int n = WideCharToMultiByte(CP_UTF8, 0, str.c_str(),
                        (int)str.size(), nullptr, 0, nullptr, nullptr);
                    std::string raw((size_t)n, 0);
                    WideCharToMultiByte(CP_UTF8, 0, str.c_str(),
                        (int)str.size(), &raw[0], n, nullptr, nullptr);
                    static const wchar_t* kB64 =
                        L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                    wstring out;
                    size_t j = 0;
                    while (j < raw.size())
                    {
                        unsigned b0 = (unsigned char)raw[j];
                        unsigned b1 = j + 1 < raw.size() ? (unsigned char)raw[j + 1] : 0;
                        unsigned b2 = j + 2 < raw.size() ? (unsigned char)raw[j + 2] : 0;
                        out += kB64[b0 >> 2];
                        out += kB64[((b0 & 3) << 4) | (b1 >> 4)];
                        out += j + 1 < raw.size() ? kB64[((b1 & 15) << 2) | (b2 >> 6)] : L'=';
                        out += j + 2 < raw.size() ? kB64[b2 & 63] : L'=';
                        j += 3;
                    }
                    return Value::Str_(out); });
                tb->map[L"save"] = NF([](vector<Value>& a) {
                    FILE* pf = nullptr;
                    if (_wfopen_s(&pf, BStr(a[0]).c_str(), L"wb") != 0 || !pf)
                        return Value::Bool_(false);
                    wstring s2 = BStr(a[1]);
                    int n2 = WideCharToMultiByte(CP_UTF8, 0, s2.c_str(),
                                                 (int)s2.size(), nullptr, 0,
                                                 nullptr, nullptr);
                    std::string out2((size_t)n2, 0);
                    if (n2 > 0)
                        WideCharToMultiByte(CP_UTF8, 0, s2.c_str(),
                                            (int)s2.size(), &out2[0], n2,
                                            nullptr, nullptr);
                    fwrite(out2.data(), 1, out2.size(), pf);
                    fclose(pf);
                    return Value::Bool_(true); });
                // round 43: fromto(s, from, to) — transcoding. Strings in
                // the VM are wide text; a "byte string" is a wide string
                // with every char <= 0xFF (one char = one raw byte).
                // Input with any char > 0xFF is TEXT (encoded with `from`
                // to get its bytes); otherwise input is a byte string.
                // Output: `to` empty/omitted -> decoded TEXT; otherwise
                // a byte string. from/to accept names or numeric
                // codepages (see CodepageOf); "" = UTF-8.
                tb->map[L"fromto"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty()) return Value();
                    wstring s = BStr(a[0]);
                    UINT cpFrom = CodepageOf(a.size() > 1 ? a[1] : Value());
                    bool bToText = a.size() < 3 ||
                                   a[2].t == Value::Null ||
                                   BStr(a[2]).empty();
                    UINT cpTo = bToText ? CP_UTF8
                        : CodepageOf(a[2]);
                    bool bTextIn = false;
                    for (wchar_t c : s)
                        if (c > 0xFF) { bTextIn = true; break; }
                    std::string raw;
                    if (bTextIn)
                    {
                        int nF = WideCharToMultiByte(cpFrom, 0, s.c_str(),
                            (int)s.size(), nullptr, 0, nullptr, nullptr);
                        if (nF < 0) return Value();
                        raw.resize((size_t)nF);
                    }
                    else
                    {
                        raw.resize(s.size());
                        for (size_t k = 0; k < s.size(); k++)
                            raw[k] = (char)(unsigned char)s[k];
                    }
                    if (bTextIn)
                        WideCharToMultiByte(cpFrom, 0, s.c_str(),
                            (int)s.size(), &raw[0], (int)raw.size(),
                            nullptr, nullptr);
                    int nWide = MultiByteToWideChar(cpFrom, 0, raw.data(),
                        (int)raw.size(), nullptr, 0);
                    if (nWide < 0) return Value();
                    wstring mid((size_t)nWide, 0);
                    if (nWide > 0)
                        MultiByteToWideChar(cpFrom, 0, raw.data(),
                            (int)raw.size(), &mid[0], nWide);
                    if (bToText)
                        return Value::Str_(mid);   // decoded text
                    int nTo = WideCharToMultiByte(cpTo, 0, mid.c_str(),
                        (int)mid.size(), nullptr, 0, nullptr, nullptr);
                    if (nTo < 0) return Value();
                    std::string out((size_t)nTo, 0);
                    if (nTo > 0)
                        WideCharToMultiByte(cpTo, 0, mid.c_str(),
                            (int)mid.size(), &out[0], nTo, nullptr,
                            nullptr);
                    wstring res((size_t)nTo, 0);
                    for (int k = 0; k < nTo; k++)
                        res[(size_t)k] =
                            (wchar_t)(unsigned char)out[(size_t)k];
                    return Value::Str_(res); });
            }
            // ---- math ----
            {
                auto tb = GetRoot(L"math").tab;
                tb->map[L"PI"] = Value::Num_(3.14159265358979323846);
                tb->map[L"E"] = Value::Num_(2.71828182845904523536);
                tb->map[L"abs"] = NF([](vector<Value>& a) {
                    return Value::Num_(fabs(BNum(a[0]))); });
                tb->map[L"ceil"] = NF([](vector<Value>& a) {
                    return Value::Num_(ceil(BNum(a[0]))); });
                tb->map[L"floor"] = NF([](vector<Value>& a) {
                    return Value::Num_(floor(BNum(a[0]))); });
                tb->map[L"sqrt"] = NF([](vector<Value>& a) {
                    return Value::Num_(sqrt(BNum(a[0]))); });
                tb->map[L"pow"] = NF([](vector<Value>& a) {
                    return Value::Num_(pow(BNum(a[0]), BNum(a[1]))); });
                tb->map[L"log"] = NF([](vector<Value>& a) {
                    return Value::Num_(log(BNum(a[0]))); });
                tb->map[L"exp"] = NF([](vector<Value>& a) {
                    return Value::Num_(exp(BNum(a[0]))); });
                tb->map[L"sin"] = NF([](vector<Value>& a) {
                    return Value::Num_(sin(BNum(a[0]))); });
                tb->map[L"cos"] = NF([](vector<Value>& a) {
                    return Value::Num_(cos(BNum(a[0]))); });
                tb->map[L"tan"] = NF([](vector<Value>& a) {
                    return Value::Num_(tan(BNum(a[0]))); });
                tb->map[L"max"] = NF([](vector<Value>& a) {
                    double m = a.empty() ? 0 : BNum(a[0]);
                    for (size_t i = 1; i < a.size(); i++)
                        m = max(m, BNum(a[i]));
                    return Value::Num_(m); });
                tb->map[L"min"] = NF([](vector<Value>& a) {
                    double m = a.empty() ? 0 : BNum(a[0]);
                    for (size_t i = 1; i < a.size(); i++)
                        m = min(m, BNum(a[i]));
                    return Value::Num_(m); });
                tb->map[L"random"] = NF([](vector<Value>& a) {
                    static bool seeded = false;
                    if (!seeded) { srand((unsigned)time(nullptr)); seeded = true; }
                    if (a.empty())
                        return Value::Num_((double)rand() / RAND_MAX);
                    long long lo = (long long)BNum(a[0]);
                    if (a.size() == 1)
                        return Value::Num_((double)(1 + rand() % lo));
                    long long hi = (long long)BNum(a[1]);
                    if (hi < lo) return Value::Num_((double)lo);
                    return Value::Num_((double)(lo + rand() % (hi - lo + 1))); });
            }
            // ---- io ----
            {
                auto tb = GetRoot(L"io").tab;
                // io.joinpath(...) — join path parts with '\'
                tb->map[L"joinpath"] = NF([](vector<Value>& a) -> Value {
                    wstring out;
                    for (size_t i = 0; i < a.size(); i++)
                    {
                        if (a[i].t != Value::Str)
                            continue;
                        wstring p = BStr(a[i]);
                        if (p.empty())
                            continue;
                        if (!out.empty() && out.back() != L'\\' &&
                            out.back() != L'/')
                            out += L'\\';
                        else if (out.empty())
                        {
                            out = p;
                            continue;
                        }
                        if (!p.empty() && (p[0] == L'\\' || p[0] == L'/'))
                            p = p.substr(1);
                        out += p;
                    }
                    return Value::Str_(out); });
                // io.splitpath(path) — returns a table of path parts
                tb->map[L"splitpath"] = NF([](vector<Value>& a) -> Value {
                    wstring p = BStr(a[0]);
                    auto out = std::make_shared<Table>();
                    size_t slash = p.find_last_of(L"\\/");
                    wstring name = (slash == wstring::npos) ? p
                                                            : p.substr(slash + 1);
                    size_t dot = name.find_last_of(L'.');
                    out->map[L"name"] = Value::Str_(
                        dot == wstring::npos ? name : name.substr(0, dot));
                    out->map[L"ext"] = Value::Str_(
                        dot == wstring::npos ? L"" : name.substr(dot + 1));
                    out->map[L"file"] = Value::Str_(name);
                    out->map[L"dir"] = Value::Str_(
                        slash == wstring::npos ? L"" : p.substr(0, slash + 1));
                    out->map[L"path"] = Value::Str_(p);
                    return Value::Table_(out); });
                auto writer = [](vector<Value>& a, const wchar_t* sep,
                                 bool nl) -> Value {
                    wstring out;
                    for (size_t i = 0; i < a.size(); i++)
                    {
                        if (i) out += sep;
                        out += a[i].ToString();
                    }
                    if (nl) out += L"\n";
                    WriteU8(stdout, out);
                    fflush(stdout);
                    return Value();
                };
                tb->map[L"print"] = NF([writer](vector<Value>& a) {
                    return writer(a, L"", false); });
                tb->map[L"println"] = NF([writer](vector<Value>& a) {
                    return writer(a, L"", true); });
                tb->map[L"open"] = NF([](vector<Value>& a) {
                    wstring path = BStr(a[0]);
                    wstring mode = a.size() > 1 ? BStr(a[1]) : L"r";
                    const wchar_t* m = wcschr(mode.c_str(), L'w') ? L"wb" :
                        wcschr(mode.c_str(), L'a') ? L"ab" : L"rb";
                    FILE* pf = nullptr;
                    if (_wfopen_s(&pf, path.c_str(), m) != 0 || !pf)
                        return Value();
                    auto fp = std::make_shared<FILE*>(pf);
                    auto tb2 = std::make_shared<Table>();
                    tb2->map[L"read"] = NF([fp](vector<Value>&) {
                        if (!*fp) return Value();
                        std::string raw3;
                        char buf3[4096]; size_t rd3;
                        while ((rd3 = fread(buf3, 1, sizeof(buf3), *fp)) > 0)
                            raw3.append(buf3, rd3);
                        int w3 = MultiByteToWideChar(CP_UTF8, 0, raw3.data(),
                                                     (int)raw3.size(), nullptr, 0);
                        wstring s3((size_t)w3, 0);
                        if (w3 > 0)
                            MultiByteToWideChar(CP_UTF8, 0, raw3.data(),
                                                (int)raw3.size(), &s3[0], w3);
                        return Value::Str_(s3); });
                    tb2->map[L"write"] = NF([fp](vector<Value>& a2) {
                        if (!*fp) return Value::Bool_(false);
                        wstring s3 = BStr(a2[0]);
                        int n3 = WideCharToMultiByte(CP_UTF8, 0, s3.c_str(),
                                                     (int)s3.size(), nullptr, 0,
                                                     nullptr, nullptr);
                        std::string out3((size_t)n3, 0);
                        if (n3 > 0)
                            WideCharToMultiByte(CP_UTF8, 0, s3.c_str(),
                                                (int)s3.size(), &out3[0], n3,
                                                nullptr, nullptr);
                        fwrite(out3.data(), 1, out3.size(), *fp);
                        return Value::Bool_(true); });
                    tb2->map[L"close"] = NF([fp](vector<Value>&) {
                        if (*fp) { fclose(*fp); *fp = nullptr; }
                        return Value(); });
                    tb2->map[L"size"] = NF([fp](vector<Value>&) {
                        if (!*fp) return Value::Num_(0);
                        long long cur = ftell(*fp);
                        fseek(*fp, 0, SEEK_END);
                        long long end = ftell(*fp);
                        fseek(*fp, cur, SEEK_SET);
                        return Value::Num_((double)end); });
                    return Value::Table_(tb2); });
                // io.readFile(path) — convenience wrapper
                // round 43: optional encoding arg — "utf8" (default, with
                // BOM tolerance), "gbk"/"ansi"/numeric codepage, or "raw"
                // (byte string: one char per raw byte)
                tb->map[L"readFile"] = NF([](vector<Value>& a) {
                    wstring s3;
                    wstring enc3 = a.size() > 1 && a[1].t != Value::Null
                        ? BStr(a[1]) : L"utf8";
                    // M6: published payload — "~\res\x" (exe-relative) or a
                    // bare relative key resolves from the embedded FS first
                    // (libEmbed exes carry res/ + the lib closure)
                    bool bHit = false;
                    if (g_interpCurrent && a[0].t == Value::Str)
                    {
                        wstring p = BStr(a[0]);
                        wstring key = p;
                        if (!key.empty() && key[0] == L'~')
                            key = key.substr(1);
                        if (!key.empty() &&
                            (key[0] == L'\\' || key[0] == L'/'))
                            key = key.substr(1);
                        auto eit = g_interpCurrent->embedded.find(key);
                        if (eit == g_interpCurrent->embedded.end() &&
                            key != p)
                            eit = g_interpCurrent->embedded.find(p);
                        if (eit != g_interpCurrent->embedded.end())
                        {
                            bHit = true;
                            // The embedded map already stores payload bytes
                            // one-per-wchar (the engine's byte-string
                            // convention), so "raw" is a straight copy —
                            // re-encoding through UTF-8 would inflate
                            // multi-byte sequences and corrupt binaries.
                            s3 = eit->second;
                        }
                    }
                    if (!bHit)
                    {
                        // aardio "~" = the aardio install root (parent of
                        // NODIO_LIBDIR when set, else the exe's directory)
                        wstring strPath = ResolveTildeDisk(BStr(a[0]));
                        FILE* pf = nullptr;
                        if (_wfopen_s(&pf, strPath.c_str(), L"rb") != 0 ||
                            !pf)
                            return Value();
                        std::string raw3;
                        char buf3[4096]; size_t rd3;
                        while ((rd3 = fread(buf3, 1, sizeof(buf3), pf)) > 0)
                            raw3.append(buf3, rd3);
                        fclose(pf);
                        if (enc3 == L"raw")
                        {
                            s3.resize(raw3.size());
                            for (size_t k = 0; k < raw3.size(); k++)
                                s3[k] =
                                    (wchar_t)(unsigned char)raw3[k];
                            return Value::Str_(s3);
                        }
                        UINT cp3 = CodepageOf(a.size() > 1 ? a[1] : Value());
                        size_t off3 = 0;
                        if (enc3 == L"utf8" && raw3.size() >= 3 &&
                            (unsigned char)raw3[0] == 0xEF &&
                            (unsigned char)raw3[1] == 0xBB &&
                            (unsigned char)raw3[2] == 0xBF)
                            off3 = 3;    // strip UTF-8 BOM before decoding
                        int w3 = MultiByteToWideChar(cp3, 0,
                            raw3.data() + off3,
                            (int)(raw3.size() - off3), nullptr, 0);
                        s3.resize((size_t)(w3 > 0 ? w3 : 0));
                        if (w3 > 0)
                            MultiByteToWideChar(cp3, 0, raw3.data() + off3,
                                (int)(raw3.size() - off3), &s3[0], w3);
                    }
                    return Value::Str_(s3); });
                // io.fullpath(path) — convert an aardio-style path to an
                // absolute system path (docs/library-guide/builtin/io/path.md):
                //   '~…'      → EXE/installation root
                //   '/…' '\…' → 应用程序根目录 (the running project's dir)
                //   '//…'     → the first slash is dropped (drive root)
                //   '\\…'     → UNC, returned as null
                //   otherwise → resolved against the current directory
                tb->map[L"fullpath"] = NF([](vector<Value>& a) -> Value {
                    wstring rel = a.empty() ? L"" : BStr(a[0]);
                    if (!rel.empty() && rel[0] == L'~')
                        return Value::Str_(ResolveTildeDisk(rel));
                    // app root: the running script's directory (a published
                    // exe falls back to its own directory)
                    wstring appRoot;
                    if (g_interpCurrent && !g_interpCurrent->baseDir.empty())
                        appRoot = g_interpCurrent->baseDir;
                    if (appRoot.empty())
                    {
                        wchar_t szExe[MAX_PATH] = L"";
                        GetModuleFileNameW(nullptr, szExe, MAX_PATH);
                        wchar_t* pSep = wcsrchr(szExe, L'\\');
                        if (pSep) *pSep = 0;
                        appRoot = szExe;
                    }
                    if (rel.size() >= 2 &&
                        (rel[0] == L'/' || rel[0] == L'\\') &&
                        (rel[1] == L'/' || rel[1] == L'\\'))
                    {
                        // '\\' (UNC) is not converted; '//' drops one slash
                        if (rel[0] == L'\\')
                            return Value();
                        rel = rel.substr(1);
                    }
                    else if (!rel.empty() &&
                             (rel[0] == L'/' || rel[0] == L'\\'))
                    {
                        wstring rest = rel.substr(1);
                        while (!rest.empty() &&
                               (rest[0] == L'/' || rest[0] == L'\\'))
                            rest = rest.substr(1);
                        if (rest.empty())
                            return Value::Str_(appRoot + L"\\");
                        return Value::Str_(appRoot + L"\\" + rest);
                    }
                    wchar_t szFull[MAX_PATH * 2] = L"";
                    DWORD nFull = GetFullPathNameW(rel.c_str(), MAX_PATH * 2,
                                                   szFull, nullptr);
                    if (nFull > 0 && nFull < MAX_PATH * 2)
                        return Value::Str_(szFull);
                    return Value::Str_(rel); });
                // io.remove(path) — delete a file (an empty directory also
                // works, which makes it forgiving of fsys-level callers)
                tb->map[L"remove"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty() || a[0].t != Value::Str)
                        return Value::Bool_(false);
                    wstring p = ResolveTildeDisk(BStr(a[0]));
                    if (p.empty())
                        return Value::Bool_(false);
                    // a trailing separator would defeat DeleteFileW
                    while (p.size() > 3 &&
                           (p.back() == L'\\' || p.back() == L'/') &&
                           p[p.size() - 2] != L':')
                        p.pop_back();
                    if (DeleteFileW(p.c_str()))
                        return Value::Bool_(true);
                    if (RemoveDirectoryW(p.c_str()))
                        return Value::Bool_(true);
                    return Value::Bool_(false); });
                // io.exist(path,mode) — docs/builtin/io/path.md: returns the
                // ABSOLUTE path when the file exists, otherwise null. The
                // return value is a path (not a boolean), which is what the
                // library idiom `io.exist(p) : fallback` relies on
                // (zlib/nodeJs/golang/py2/py3 resolve their bundled DLL this
                // way). mode: 2 = writable, 4 = readable, 6 = both; a
                // directory only satisfies the default mode.
                tb->map[L"exist"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty() || a[0].t != Value::Str)
                        return Value();      // null
                    wstring strPath = BStr(a[0]);
                    if (strPath.empty())
                        return Value();
                    int mode = (a.size() > 1 && a[1].t == Value::Num)
                                   ? (int)a[1].n : 0;
                    // embedded payload of a published exe: it ships as data
                    if (g_interpCurrent)
                    {
                        wstring key = strPath;
                        if (!key.empty() && key[0] == L'~')
                            key = key.substr(1);
                        if (!key.empty() &&
                            (key[0] == L'\\' || key[0] == L'/'))
                            key = key.substr(1);
                        if (mode == 0 &&
                            (g_interpCurrent->embedded.count(key) ||
                             (key != strPath &&
                              g_interpCurrent->embedded.count(strPath))))
                            return Value::Str_(ResolveTildeDisk(strPath));
                    }
                    wstring full = ResolveTildeDisk(strPath);
                    // make it absolute (a relative path resolves against cwd)
                    wchar_t szFull[MAX_PATH * 2] = L"";
                    DWORD nFull = GetFullPathNameW(full.c_str(),
                                                  MAX_PATH * 2, szFull,
                                                  nullptr);
                    if (nFull > 0 && nFull < MAX_PATH * 2)
                        full = szFull;
                    int acc = 0;
                    if (mode == 2 || mode == 6)
                        acc |= 2;
                    if (mode == 4 || mode == 6)
                        acc |= 4;
                    if (_waccess(full.c_str(), acc) != 0)
                        return Value();      // null
                    DWORD attr = GetFileAttributesW(full.c_str());
                    if (attr != INVALID_FILE_ATTRIBUTES &&
                        (attr & FILE_ATTRIBUTE_DIRECTORY) && mode != 0)
                        return Value();      // a directory is not a file
                    return Value::Str_(full); });
                tb->map[L"writeFile"] = NF([](vector<Value>& a) {
                    FILE* pf = nullptr;
                    if (_wfopen_s(&pf, BStr(a[0]).c_str(), L"wb") != 0 || !pf)
                        return Value::Bool_(false);
                    wstring s3 = BStr(a[1]);
                    wstring enc3 = a.size() > 2 && a[2].t != Value::Null
                        ? BStr(a[2]) : L"utf8";
                    std::string out3;
                    if (enc3 == L"raw")
                    {
                        out3.resize(s3.size());
                        for (size_t k = 0; k < s3.size(); k++)
                            out3[k] = (char)(unsigned char)s3[k];
                    }
                    else
                    {
                        UINT cp3 = CodepageOf(a.size() > 2 ? a[2]
                                                           : Value());
                        int n3 = WideCharToMultiByte(cp3, 0, s3.c_str(),
                                                     (int)s3.size(), nullptr,
                                                     0, nullptr, nullptr);
                        out3.resize((size_t)(n3 > 0 ? n3 : 0));
                        if (n3 > 0)
                            WideCharToMultiByte(cp3, 0, s3.c_str(),
                                                (int)s3.size(), &out3[0],
                                                n3, nullptr, nullptr);
                    }
                    fwrite(out3.data(), 1, out3.size(), pf);
                    fclose(pf);
                    return Value::Bool_(true); });
            }
            // ---- console ----
            {
                auto tb = GetRoot(L"console").tab;
                auto writer = [](vector<Value>& a, bool nl) -> Value {
                    wstring out;
                    for (size_t i = 0; i < a.size(); i++)
                    {
                        if (i) out += L" ";
                        out += a[i].ToString();
                    }
                    if (nl) out += L"\n";
                    WriteU8(stdout, out);
                    fflush(stdout);
                    return Value();
                };
                tb->map[L"log"] = NF([writer](vector<Value>& a) {
                    return writer(a, true); });
                tb->map[L"println"] = tb->map[L"log"];
                tb->map[L"print"] = NF([writer](vector<Value>& a) {
                    return writer(a, false); });
                tb->map[L"dump"] = NF([writer](vector<Value>& a) {
                    for (auto& v : a)
                    {
                        if (v.t == Value::TableT && v.tab)
                        {
                            wstring out = L"{ ";
                            for (auto& kv : v.tab->map)
                                out += kv.first + L"=" + kv.second.ToString() + L"; ";
                            out += L"}";
                            WriteU8(stdout, out);
                        }
                        else
                            WriteU8(stdout, v.ToString());
                        WriteU8(stdout, L"\n");
                    }
                    fflush(stdout);
                    return Value(); });
                tb->map[L"pause"] = NF([](vector<Value>&) {
                    WriteU8(stdout, L"请按任意键继续 . . .\n");
                    fflush(stdout);
                    if (fgetwc(stdin) == WEOF)
                    {
                        // stdin closed (pipe/headless): nothing to wait on
                    }
                    return Value(); });
                tb->map[L"open"] = NF([](vector<Value>&) { return Value(); });
                tb->map[L"close"] = NF([](vector<Value>&) { return Value(); });
            }
            // ---- time (round 30) ----
            {
                auto tb = GetRoot(L"time").tab;
                tb->map[L"now"] = NF([](vector<Value>&) {
                    return Value::Num_((double)time(nullptr)); });
                tb->map[L"tick"] = NF([](vector<Value>&) {
                    return Value::Num_((double)GetTickCount()); });
                tb->map[L"date"] = NF([](vector<Value>& a) {
                    wstring fmt = a.empty() ? L"%Y-%m-%d %H:%M:%S"
                                            : BStr(a[0]);
                    time_t t = a.size() > 1 ? (time_t)BNum(a[1]) : time(nullptr);
                    tm tmb;
                    localtime_s(&tmb, &t);
                    wchar_t buf[256];
                    wcsftime(buf, 256, fmt.c_str(), &tmb);
                    return Value::Str_(buf); });
                // time.stamp() — seconds since the epoch, as a number
                tb->map[L"stamp"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty())
                        return Value::Num_((double)time(nullptr));
                    wstring fmt = BStr(a[0]);
                    time_t t = a.size() > 1 ? (time_t)BNum(a[1]) : time(nullptr);
                    tm tmb;
                    localtime_s(&tmb, &t);
                    wchar_t buf[256];
                    wcsftime(buf, 256, fmt.c_str(), &tmb);
                    return Value::Str_(buf); });
                // time.ole() — OLE automation date (days since 1899-12-30)
                tb->map[L"ole"] = NF([](vector<Value>& a) -> Value {
                    SYSTEMTIME st;
                    if (a.empty())
                        GetLocalTime(&st);
                    else
                    {
                        FILETIME ft;
                        SYSTEMTIME stIn;
                        stIn.wYear = (WORD)BNum(a[0]);
                        stIn.wMonth = (WORD)(a.size() > 1 ? BNum(a[1]) : 1);
                        stIn.wDay = (WORD)(a.size() > 2 ? BNum(a[2]) : 1);
                        stIn.wHour = (WORD)(a.size() > 3 ? BNum(a[3]) : 0);
                        stIn.wMinute = (WORD)(a.size() > 4 ? BNum(a[4]) : 0);
                        stIn.wSecond = (WORD)(a.size() > 5 ? BNum(a[5]) : 0);
                        stIn.wMilliseconds = 0;
                        if (a.empty())
                            st = stIn;
                        else if (!SystemTimeToTzSpecificLocalTime(nullptr,
                                                                 &stIn, &st))
                            st = stIn;
                        (void)ft;
                    }
                    // days from 1899-12-30 plus the fraction of the day
                    double dSerial = 0;
                    {
                        FILETIME ft;
                        SYSTEMTIME base;
                        memset(&base, 0, sizeof(base));
                        base.wYear = 1899; base.wMonth = 12; base.wDay = 30;
                        ULARGE_INTEGER ul, ulBase;
                        SystemTimeToFileTime(&st, &ft);
                        ul.HighPart = ft.dwHighDateTime;
                        ul.LowPart = ft.dwLowDateTime;
                        SystemTimeToFileTime(&base, &ft);
                        ulBase.HighPart = ft.dwHighDateTime;
                        ulBase.LowPart = ft.dwLowDateTime;
                        dSerial = (double)(long long)
                            ((ul.QuadPart - ulBase.QuadPart) / 10000000) /
                            86400.0;
                    }
                    return Value::Num_(dSerial); });
            }
            // ---- json (round 30) ----
            {
                auto tb = GetRoot(L"json").tab;
                tb->map[L"parse"] = NF([](vector<Value>& a) {
                    JParser jp{ BStr(a[0]), 0, nullptr };
                    wstring err2;
                    jp.err = &err2;
                    Value v = jp.Parse();
                    if (!err2.empty()) return Value();
                    return v; });
                tb->map[L"stringify"] = NF([](vector<Value>& a) {
                    bool pretty = a.size() > 1 && a[1].Truthy();
                    return Value::Str_(JWrite(a[0], pretty ? 0 : -1, 0)); });
            }
            // ---- win (round 29 minimal): msgbox + clipboard ----
            {
                auto tb = GetRoot(L"win").tab;
                auto clip = std::make_shared<Table>();
                tb->map[L"clip"] = Value::Table_(clip);
                // win.getMessagePos(lParam) — the mouse position packed into
                // an lParam (WM_MOUSEMOVE & friends), returned as x,y
                tb->map[L"getMessagePos"] = NF([](vector<Value>& a) -> Value {
                    long long lp = a.empty() ? 0 : (long long)BNum(a[0]);
                    short x = (short)(lp & 0xFFFF);
                    short y = (short)((lp >> 16) & 0xFFFF);
                    Value vx = Value::Num_((double)x);
                    auto ex = std::make_shared<vector<Value>>();
                    ex->push_back(Value::Num_((double)y));
                    vx.mrets = ex;
                    return vx; });
                tb->map[L"msgbox"] = NF([](vector<Value>& a) {
                    wstring text = a.empty() ? L"" : a[0].ToString();
                    wstring title = a.size() > 1 ? a[1].ToString() : L"aardio";
                    UINT flag = a.size() > 2 ? (UINT)BNum(a[2]) : 0u;
                    int rc = MessageBoxW(nullptr, text.c_str(), title.c_str(),
                                         MB_OK | flag);
                    return Value::Num_((double)rc); });
                tb->map[L"msgboxTest"] = NF([](vector<Value>& a) {
                    wstring text = a.empty() ? L"" : a[0].ToString();
                    wstring title = a.size() > 1 ? a[1].ToString() : L"aardio";
                    return Value::Bool_(MessageBoxW(nullptr, text.c_str(),
                        title.c_str(), MB_OKCANCEL | MB_ICONQUESTION) == IDOK); });
                clip->map[L"write"] = NF([](vector<Value>& a) {
                    wstring text = a.empty() ? L"" : a[0].ToString();
                    BOOL ok = FALSE;
                    if (OpenClipboard(nullptr))
                    {
                        EmptyClipboard();
                        int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                            (int)text.size(), nullptr, 0, nullptr, nullptr);
                        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE,
                                                (size_t)n + 2);
                        if (h)
                        {
                            char* p = (char*)GlobalLock(h);
                            WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                (int)text.size(), p, n, nullptr, nullptr);
                            p[n] = 0; p[n + 1] = 0;
                            GlobalUnlock(h);
                            // clipboard text is Unicode: put CF_UNICODETEXT
                            int w2 = (int)text.size();
                            HGLOBAL h2 = GlobalAlloc(GMEM_MOVEABLE,
                                (size_t)(w2 + 1) * 2);
                            if (h2)
                            {
                                wchar_t* q = (wchar_t*)GlobalLock(h2);
                                memcpy(q, text.c_str(), (size_t)w2 * 2);
                                q[w2] = 0;
                                GlobalUnlock(h2);
                                ok = SetClipboardData(CF_UNICODETEXT, h2) != nullptr;
                            }
                            GlobalFree(h);
                        }
                        CloseClipboard();
                    }
                    return Value::Bool_(ok != FALSE); });
                clip->map[L"read"] = NF([](vector<Value>&) {
                    wstring out;
                    if (OpenClipboard(nullptr))
                    {
                        HANDLE h = GetClipboardData(CF_UNICODETEXT);
                        if (h)
                        {
                            wchar_t* q = (wchar_t*)GlobalLock(h);
                            if (q) { out = q; GlobalUnlock(h); }
                        }
                        CloseClipboard();
                    }
                    return Value::Str_(out); });
                // round 42: raw window access + common dialogs
                tb->map[L"findWindow"] = NF([](vector<Value>& a) -> Value {
                    wstring s1 = a.size() > 0 ? BStr(a[0]) : L"";
                    wstring s2 = a.size() > 1 ? BStr(a[1]) : L"";
                    HWND h = FindWindowW(
                        a.size() > 0 && a[0].t != Value::Null && !s1.empty()
                            ? s1.c_str() : nullptr,
                        a.size() > 1 && a[1].t != Value::Null && !s2.empty()
                            ? s2.c_str() : nullptr);
                    return h ? Value::Num_((double)(INT_PTR)h) : Value(); });
                tb->map[L"postMessage"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty() || a[0].t != Value::Num)
                        return Value::Bool_(false);
                    HWND h = (HWND)(INT_PTR)(long long)BNum(a[0]);
                    UINT m = a.size() > 1 ? (UINT)BNum(a[1]) : 0;
                    WPARAM w = a.size() > 2
                        ? (WPARAM)(INT_PTR)(long long)BNum(a[2]) : 0;
                    LPARAM l = a.size() > 3
                        ? (LPARAM)(INT_PTR)(long long)BNum(a[3]) : 0;
                    return Value::Bool_(PostMessageW(h, m, w, l) != 0); });
                auto HwndOf = [](const Value& v) -> HWND {
                    if (v.t == Value::Num)
                        return (HWND)(INT_PTR)(long long)v.n;
                    if (v.t == Value::TableT && v.tab)
                    {
                        auto ih = v.tab->map.find(L"hwnd");
                        if (ih != v.tab->map.end() &&
                            ih->second.t == Value::Num)
                            return (HWND)(INT_PTR)(long long)ih->second.n;
                    }
                    return nullptr; };
                auto dlg = std::make_shared<Table>();
                tb->map[L"dlg"] = Value::Table_(dlg);
                // open(formOrHwnd, title, filter, initDir, initFile) -> path
                // filter uses aardio pipe form: "文本|*.txt|所有|*.*"
                auto dlgImpl = [HwndOf](vector<Value>& a, bool bSave)
                    -> Value {
                    HWND hOwner = a.empty() ? nullptr : HwndOf(a[0]);
                    wstring sTitle = a.size() > 1 ? BStr(a[1]) : L"";
                    wstring sFilter = a.size() > 2 ? BStr(a[2]) : L"";
                    wstring sDir = a.size() > 3 ? BStr(a[3]) : L"";
                    wstring sFile = a.size() > 4 ? BStr(a[4]) : L"";
                    for (auto& c : sFilter)
                        if (c == L'|') c = L'\0';
                    if (!sFilter.empty()) sFilter.push_back(L'\0');
                    wchar_t buf[MAX_PATH * 2] = L"";
                    wcsncpy_s(buf, sFile.c_str(), _TRUNCATE);
                    OPENFILENAMEW ofn = { sizeof(ofn) };
                    ofn.hwndOwner = hOwner;
                    ofn.lpstrFilter = sFilter.empty()
                                          ? L"所有文件\0*.*\0"
                                          : sFilter.c_str();
                    ofn.lpstrFile = buf;
                    ofn.nMaxFile = MAX_PATH * 2;
                    ofn.lpstrTitle =
                        sTitle.empty() ? nullptr : sTitle.c_str();
                    ofn.lpstrInitialDir =
                        sDir.empty() ? nullptr : sDir.c_str();
                    ofn.Flags = OFN_EXPLORER | OFN_HIDEREADONLY;
                    if (bSave)
                        ofn.Flags |= OFN_OVERWRITEPROMPT;
                    else
                        ofn.Flags |= OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
                    BOOL ok = bSave ? GetSaveFileNameW(&ofn)
                                    : GetOpenFileNameW(&ofn);
                    if (!ok)
                        return Value();     // cancel -> null
                    return Value::Str_(buf); };
                dlg->map[L"open"] = NF([dlgImpl](vector<Value>& a)
                    -> Value {
                    return dlgImpl(a, false); });
                dlg->map[L"save"] = NF([dlgImpl](vector<Value>& a)
                    -> Value {
                    return dlgImpl(a, true); });
            }
            // ---- round 44: raw — P/Invoke + raw memory ----
            {
                auto tb = GetRoot(L"raw").tab;
                tb->map[L"loadDll"] = NF([this](vector<Value>& a)
                    -> Value {
                    HMODULE hMod = nullptr;
                    bool bIsMainMod = false;
                    // A memory DLL keeps its mapped image alive for the
                    // lifetime of the dll object; free() releases it.
                    auto memDll = std::make_shared<MemDll>();
                    if (a.empty() || a[0].t == Value::Null ||
                        (a[0].t == Value::Str && a[0].s.empty()))
                    {
                        hMod = GetModuleHandleW(nullptr);
                        bIsMainMod = true;
                    }
                    else if (a[0].t == Value::Str)
                    {
                        wstring strArg = BStr(a[0]);
                        // A payload-embedded DLL: the published exe carries
                        // res\ under its embedded FS, and $"~\path" only
                        // resolves at publish time (EmbedRoot needs
                        // NODIO_LIBDIR). Resolve it here from the embedded
                        // map, the same way io.readFile does, so a DLL
                        // shipped in a payload loads without a disk file.
                        if (!strArg.empty() && strArg[0] == L'~' &&
                            g_interpCurrent)
                        {
                            wstring key = strArg.substr(1);
                            if (!key.empty() &&
                                (key[0] == L'\\' || key[0] == L'/'))
                                key = key.substr(1);
                            auto eit = g_interpCurrent->embedded.find(key);
                            if (eit == g_interpCurrent->embedded.end() &&
                                key != strArg)
                                eit = g_interpCurrent->embedded.find(strArg);
                            if (eit != g_interpCurrent->embedded.end())
                                strArg = eit->second;
                        }
                        // Order matters: LoadLibraryW first, because a bare
                        // name like "user32.dll" must go through the normal
                        // DLL search path (GetFileAttributesW would reject
                        // it when the CWD is elsewhere). Only when that
                        // fails — or the argument is clearly not a path —
                        // treat the string as mapped image bytes.
                        if (!strArg.empty())
                        {
                            // A mapped image starts with 'MZ' and is full of
                            // NULs in its headers; a path is short and
                            // NUL-free. Try the disk path first so bare
                            // names ("user32.dll") go through the normal
                            // DLL search path, then fall back to mapping.
                            bool bLooksMapped =
                                strArg.size() >= 2 && strArg[0] == L'M' &&
                                strArg[1] == L'Z' &&
                                strArg.find(L'\0') != wstring::npos;
                            if (!bLooksMapped)
                                hMod = LoadLibraryW(
                                    ResolveTildeDisk(strArg).c_str());
                            if (!hMod && bLooksMapped)
                            {
                                void* pMemMod = nullptr;
                                if (MemDllLoad(strArg, &pMemMod,
                                               memDll.get()))
                                    hMod = (HMODULE)pMemMod;
                            }
                        }
                        if (!hMod)
                        {
                            (*err) = L"运行错误: 无法加载 DLL: " + strArg;
                            return Value();
                        }
                    }
                    else
                    {
                        (*err) = L"运行错误: raw.loadDll(路径或内存数据)";
                        return Value();
                    }
                    auto dll = std::make_shared<Table>();
                    dll->map[L"_h"] =
                        Value::Pointer_(hMod);
                    dll->map[L"_memdll"] =
                        Value::Pointer_(memDll->bOk ? memDll.get() : nullptr);
                    dll->map[L"gethandle"] = NF([hMod](vector<Value>&) {
                        return Value::Pointer_(hMod);
                    });
                    dll->map[L"free"] = NF([hMod, bIsMainMod, memDll](
                        vector<Value>&) -> Value {
                        if (bIsMainMod) return Value();
                        if (memDll->bOk) MemDllUnload(memDll.get());
                        else FreeLibrary(hMod);
                        return Value(); });
                    bool bDllCdecl = false;
                    bool bDllUni = false;
                    if (a.size() > 1)
                    {
                        wstring conv = BStr(a[1]);
                        for (auto& c : conv) c = towlower(c);
                        if (conv.find(L"cdecl") != wstring::npos) bDllCdecl = true;
                        if (conv.find(L"unicode") != wstring::npos) bDllUni = true;
                    }
                    dll->map[L"_cdecl"] = Value::Bool_(bDllCdecl);
                    dll->map[L"_unicode"] = Value::Bool_(bDllUni);
                    // api(name, signature) -> callable function value
                    dll->map[L"api"] = NF([this, hMod, memDll](
                        vector<Value>& a2) -> Value {
                        if (a2.empty())
                        {
                            (*err) = L"运行错误: dll.api(name, sig)";
                            return Value();
                        }
                        FARPROC fp = nullptr;
                        wstring strName;
                        if (a2[0].t == Value::Pointer)
                            fp = (FARPROC)a2[0].ptr;
                        else if (a2[0].t == Value::Num)
                            fp = (FARPROC)(INT_PTR)a2[0].n;
                        else
                        {
                            strName = BStr(a2[0]);
                        }
                        ApiSig sig;
                        wstring sigStr = a2.size() > 1 ? BStr(a2[1]) : L"void()";
                        if (!ParseApiSig(sigStr, &sig))
                        {
                            (*err) = L"运行错误: API 签名无效: " + sigStr;
                            return Value();
                        }
                        if (!fp && !strName.empty())
                        {
                            std::string strNameA((size_t)
                                WideCharToMultiByte(CP_ACP, 0,
                                    strName.c_str(), (int)strName.size(),
                                    nullptr, 0, nullptr, nullptr), 0);
                            if (!strNameA.empty())
                                WideCharToMultiByte(CP_ACP, 0,
                                    strName.c_str(), (int)strName.size(),
                                    &strNameA[0], (int)strNameA.size(),
                                    nullptr, nullptr);
                            if (memDll->bOk)
                            {
                                // memory DLL: GetProcAddress does not work
                                // on a manually mapped image
                                fp = MemDllGetProc(memDll->pImage,
                                                   strNameA.c_str());
                            }
                            else
                            {
                            fp = ::GetProcAddress(hMod,
                                strNameA.c_str());
                            // Win32 dual exports: bare name -> W -> A, and
                            // W-suffixed declarations of ANSI-only exports
                            // (user32 "CopyImageW" exists only as CopyImage —
                            // the original VM strips the trailing W on miss)
                            if (!fp && strNameA.find('@') == std::string::npos)
                            {
                                std::string w = strNameA + "W";
                                fp = ::GetProcAddress(hMod, w.c_str());
                            }
                            if (!fp && strNameA.size() > 1 &&
                                strNameA.back() == L'W' &&
                                strNameA.find('@') == std::string::npos)
                            {
                                std::string bare = strNameA.substr(0,
                                    strNameA.size() - 1);
                                fp = ::GetProcAddress(hMod, bare.c_str());
                            }
                            }
                            if (!fp)
                            {
                                std::string an = strNameA + "A";
                                fp = ::GetProcAddress(hMod, an.c_str());
                            }
                        }
                        if (!fp)
                        {
                            // official aardio quirk: api() on a missing
                            // export returns null (callers guard with
                            // try/catch — process/_.aardio WoW64 block)
                            if (a2.size() == 1)
                                return Value();
                            (*err) = L"运行错误: 找不到 API: " + strName;
                            return Value();
                        }
                        void* fn = (void*)fp;
#if defined(_WIN64)
                        // one executable call stub per bound signature —
                        // carries float/double args into XMM0..3
                        void* stub = EmitX64CallStub(sig);
#else
                        void* stub = nullptr;
#endif
                        Value retFn = NF([this, fn, sig, stub](vector<Value>& a3)
                            -> Value {
                            // box arguments into integer slots
                            vector<SlotT> slots;
                            std::vector<std::string> tmpA;
                            std::vector<wstring> tmpW;
                            size_t nA = 0, nW = 0;
                            for (size_t i = 0; i < sig.params.size();
                                 i++)
                            {
                                Value v = i < a3.size()
                                    ? a3[i] : Value();
                                if (sig.params[i] == 6 && v.t == Value::Str)
                                    nA++;
                                if (sig.params[i] == 7 && v.t == Value::Str)
                                    nW++;
                            }
                            tmpA.reserve(nA);
                            tmpW.reserve(nW);
                            struct ApiArgRef
                            {
                                size_t paramIdx = 0;
                                int paramType = 0;
                                void* pBuffer = nullptr;
                                bool bStruct = false;
                                Value origVal;
                            };
                            std::vector<ApiArgRef> tmpRefs;
                            for (size_t i = 0; i < sig.params.size();
                                 i++)
                            {
                                Value v = i < a3.size()
                                    ? a3[i] : Value();
                                int pt = sig.params[i] & 0xFF;
                                bool bByRef =
                                    (sig.params[i] & 0x100) != 0;
                                // & out-params: allocate scratch memory, copy initial value if passed
                                if (bByRef && (pt == 0 || pt == 1 || pt == 2 ||
                                               pt == 3 || pt == 4 || pt == 5 ||
                                               pt == 9 || pt == 10))
                                {
                                    void* pRef = HeapAlloc(
                                        GetProcessHeap(),
                                        HEAP_ZERO_MEMORY, 8);
                                    if (pt == 0 || pt == 4) *(int*)pRef = (int)(unsigned int)(long long)BNum(v);
                                    else if (pt == 1) *(__int64*)pRef = (__int64)BNum(v);
                                    else if (pt == 2) *(unsigned short*)pRef = (unsigned short)(unsigned int)(long long)BNum(v);
                                    else if (pt == 3) *(unsigned char*)pRef = (unsigned char)(unsigned int)(long long)BNum(v);
                                    else if (pt == 5) *(INT_PTR*)pRef = (INT_PTR)(long long)BNum(v);
                                    else if (pt == 9) { float f = (float)BNum(v); memcpy(pRef, &f, sizeof(float)); }
                                    else if (pt == 10) { double d = BNum(v); memcpy(pRef, &d, sizeof(double)); }
                                    tmpRefs.push_back({ i, pt, pRef, false, v });
                                    slots.push_back((SlotT)(INT_PTR)pRef);
                                    continue;
                                }
                                if (pt == 8) // struct
                                {
                                    if (v.t == Value::TableT || v.t == Value::Class)
                                    {
                                        int total = 0;
                                        void* pBuf = StructSerialize(v, &total);
                                        if (pBuf)
                                        {
                                            slots.push_back((SlotT)(INT_PTR)pBuf);
                                            tmpRefs.push_back({ i, pt, pBuf, true, v });
                                            continue;
                                        }
                                    }
                                    if (v.t == Value::Null)
                                    {
                                        slots.push_back((SlotT)0);
                                        continue;
                                    }
                                    if (v.t == Value::Pointer)
                                    {
                                        slots.push_back((SlotT)(INT_PTR)v.ptr);
                                        continue;
                                    }
                                    slots.push_back((SlotT)(INT_PTR)(long long)BNum(v));
                                    continue;
                                }
                                switch (pt)
                                {
                                case 0:   // int
                                    // wrap modulo 2^32 (aardio doubles:
                                    // 0xFFFFFFF0 must arrive as -16, not
                                    // the MSVC saturate value)
                                    slots.push_back((SlotT)(
                                        INT_PTR)(int)(unsigned int)
                                            (long long)BNum(v));
                                    break;
                                case 1:   // int64
#if defined(_M_IX86)
                                {
                                    long long v64 =
                                        (long long)BNum(v);
                                    slots.push_back((SlotT)
                                        (v64 & 0xFFFFFFFF));
                                    slots.push_back((SlotT)
                                        ((unsigned long long)v64
                                         >> 32));
                                    break;
                                }
#else
                                    slots.push_back((SlotT)
                                        (unsigned long long)
                                            (long long)BNum(v));
                                    break;
#endif
                                case 2:   // word
                                    slots.push_back((SlotT)
                                        (unsigned short)(unsigned int)
                                            (long long)BNum(v));
                                    break;
                                case 3:   // byte
                                    slots.push_back((SlotT)
                                        (unsigned char)(unsigned int)
                                            (long long)BNum(v));
                                    break;
                                case 4:   // bool
                                    slots.push_back(
                                        BNum(v) != 0 ? 1 : 0);
                                    break;
                                case 5:   // pointer (hwnd / buffer)
                                    if (v.t == Value::Pointer)
                                        slots.push_back((SlotT)(INT_PTR)v.ptr);
                                    else
                                        slots.push_back(v.t == Value::Null
                                            ? (SlotT)0
                                            : (SlotT)(INT_PTR)(long long)
                                                  BNum(v));
                                    break;
                                case 6:   // str (ANSI)
                                    if (v.t == Value::Null)
                                        slots.push_back((SlotT)0);
                                    else if (v.t == Value::Pointer)
                                        slots.push_back((SlotT)(INT_PTR)v.ptr);
                                    else if (v.t == Value::Num)
                                        slots.push_back((SlotT)
                                            (INT_PTR)(long long)
                                                BNum(v));
                                    else
                                    {
                                        wstring w = BStr(v);
                                        std::string sA((size_t)
                                            WideCharToMultiByte(
                                                CP_ACP, 0, w.c_str(),
                                                (int)w.size(), nullptr,
                                                0, nullptr, nullptr), 0);
                                        if (!sA.empty())
                                            WideCharToMultiByte(CP_ACP,
                                                0, w.c_str(),
                                                (int)w.size(), &sA[0],
                                                (int)sA.size(), nullptr,
                                                nullptr);
                                        sA.push_back('\0');
                                        tmpA.push_back(std::move(sA));
                                        slots.push_back((SlotT)
                                            (INT_PTR)
                                                tmpA.back().c_str());
                                    }
                                    break;
                                case 7:   // wstr (UTF-16)
                                    if (v.t == Value::Null)
                                        slots.push_back((SlotT)0);
                                    else if (v.t == Value::Pointer)
                                        slots.push_back((SlotT)(INT_PTR)v.ptr);
                                    else if (v.t == Value::Num)
                                        slots.push_back((SlotT)
                                            (INT_PTR)(long long)
                                                BNum(v));
                                    else
                                    {
                                        tmpW.push_back(BStr(v));
                                        slots.push_back((SlotT)
                                            (INT_PTR)
                                                tmpW.back().c_str());
                                    }
                                    break;
                                case 9:   // float
                                {
                                    float f = (float)BNum(v);
                                    SlotT s = 0;
                                    memcpy(&s, &f, sizeof(float));
                                    slots.push_back(s);
                                    break;
                                }
                                case 10:  // double
                                {
#if defined(_M_IX86)
                                    double d = BNum(v);
                                    unsigned __int64 u64 = 0;
                                    memcpy(&u64, &d, sizeof(double));
                                    slots.push_back((SlotT)(u64 & 0xFFFFFFFF));
                                    slots.push_back((SlotT)(u64 >> 32));
#else
                                    double d = BNum(v);
                                    SlotT s = 0;
                                    memcpy(&s, &d, sizeof(double));
                                    slots.push_back(s);
#endif
                                    break;
                                }
                                default:
                                    slots.push_back(0);
                                    break;
                                }
                            }
                            if (slots.size() > 14)
                            {
                                (*err) =
                                    L"运行错误: API 参数超过 14 个";
                                return Value();
                            }
                            // invoke, then interpret the result.
                            // x64: the per-signature call stub routes
                            // float/double params through XMM0..3 per the
                            // ABI; a float/double return arrives bit-cast
                            // in RAX (stub epilogue) and is re-decoded.
#if defined(_WIN64)
                            bool bStub = stub && !sig.bCdecl;
#else
                            bool bStub = false;
#endif
                            if (bStub && sig.ret == ARVoid)
                            {
#if defined(_WIN64)
                                InvokeX64Stub(stub, fn, slots);
#endif
                                return Value();
                            }
                            if (sig.ret == ARDbl)
                            {
                                double d;
#if defined(_WIN64)
                                if (bStub)
                                {
                                    __int64 rb =
                                        InvokeX64Stub(stub, fn, slots);
                                    memcpy(&d, &rb, 8);
                                }
                                else
#endif
                                    d = sig.bCdecl
                                        ? InvokeCdl<double>(fn, slots)
                                        : InvokeStd<double>(fn, slots);
                                return Value::Num_(d);
                            }
                            __int64 r;
#if defined(_WIN64)
                            if (bStub)
                                r = InvokeX64Stub(stub, fn, slots);
                            else
#endif
                                r = sig.bCdecl
                                    ? InvokeCdl<__int64>(fn, slots)
                                    : InvokeStd<__int64>(fn, slots);
                            // decode the primary return
                            Value vRet;
                            switch (sig.ret)
                            {
                            case ARInt:
                                vRet = Value::Num_((double)(int)r);
                                break;
                            case ARWord:
                                vRet = Value::Num_(
                                    (double)(unsigned short)r);
                                break;
                            case ARByte:
                                vRet = Value::Num_(
                                    (double)(unsigned char)r);
                                break;
                            case ARBool:
                                vRet = Value::Bool_(r != 0);
                                break;
                            case ARPtr:
                                vRet = Value::Num_(
                                    (double)(__int64)(INT_PTR)r);
                                break;
                            case ARStrA:
                                vRet = Value::Str_(
                                    ReadStrA((const char*)(INT_PTR)r));
                                break;
                            case ARStrW:
                                vRet = Value::Str_(
                                    ReadStrW((const wchar_t*)
                                             (INT_PTR)r));
                                break;
                            case ARI64:
                            default:
                                vRet = Value::Num_((double)r);
                                break;
                            }
                            // aardio by-ref out-params come back as extra
                            // return values (var len,str = formatMessage(..))
                            if (!tmpRefs.empty())
                            {
                                auto extras =
                                    std::make_shared<vector<Value>>();
                                for (auto& ref : tmpRefs)
                                {
                                    if (ref.bStruct)
                                    {
                                        bool bRef = (sig.params[ref.paramIdx] & 0x100) != 0;
                                        if (ref.origVal.t == Value::TableT || ref.origVal.t == Value::Class)
                                        {
                                            StructDeserialize(ref.origVal, ref.pBuffer);
                                        }
                                        if (bRef)
                                            extras->push_back(ref.origVal);
                                        HeapFree(GetProcessHeap(), 0, ref.pBuffer);
                                    }
                                    else
                                    {
                                        Value outVal;
                                        switch (ref.paramType)
                                        {
                                        case 0:
                                            outVal = Value::Num_((double)*(int*)ref.pBuffer);
                                            break;
                                        case 1:
                                            outVal = Value::Num_((double)*(__int64*)ref.pBuffer);
                                            break;
                                        case 2:
                                            outVal = Value::Num_((double)*(unsigned short*)ref.pBuffer);
                                            break;
                                        case 3:
                                            outVal = Value::Num_((double)*(unsigned char*)ref.pBuffer);
                                            break;
                                        case 4:
                                            outVal = Value::Bool_(*(int*)ref.pBuffer != 0);
                                            break;
                                        case 5:
                                            outVal = Value::Num_((double)(__int64)*(INT_PTR*)ref.pBuffer);
                                            break;
                                        case 9:
                                        {
                                            float f = 0;
                                            memcpy(&f, ref.pBuffer, sizeof(float));
                                            outVal = Value::Num_(f);
                                            break;
                                        }
                                        case 10:
                                        {
                                            double d = 0;
                                            memcpy(&d, ref.pBuffer, sizeof(double));
                                            outVal = Value::Num_(d);
                                            break;
                                        }
                                        default:
                                            outVal = Value::Num_((double)*(int*)ref.pBuffer);
                                            break;
                                        }
                                        extras->push_back(outVal);
                                        HeapFree(GetProcessHeap(), 0, ref.pBuffer);
                                    }
                                }
                                if (!extras->empty())
                                    vRet.mrets = extras;
                            }
                            return vRet;
                            });
                            retFn.ptr = fn;
                            return retFn;
                        });
                    return Value::Table_(dll); });
                // raw memory: pointer values are plain numbers
                tb->map[L"buffer"] = NF([](vector<Value>& a) -> Value {
                    size_t n = a.empty() ? 0
                        : (size_t)(long long)BNum(a[0]);
                    if (n == 0) return Value::Num_(0);
                    void* p = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                        n);
                    return Value::Num_((double)(INT_PTR)p); });
                tb->map[L"free"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty() || a[0].t != Value::Num)
                        return Value::Bool_(false);
                    void* p = (void*)(INT_PTR)(long long)BNum(a[0]);
                    if (!p) return Value::Bool_(true);
                    return Value::Bool_(
                        HeapFree(GetProcessHeap(), 0, p) != FALSE); });
                // write(ptr, s): byte string -> raw bytes; text -> UTF-8
                tb->map[L"write"] = NF([](vector<Value>& a) -> Value {
                    if (a.size() < 2 || a[0].t != Value::Num)
                        return Value::Num_(0);
                    char* p = (char*)(INT_PTR)(long long)BNum(a[0]);
                    if (!p) return Value::Num_(0);
                    wstring s = BStr(a[1]);
                    bool bText = false;
                    for (wchar_t c : s)
                        if (c > 0xFF) { bText = true; break; }
                    if (!bText)
                    {
                        for (size_t k = 0; k < s.size(); k++)
                            p[k] = (char)(unsigned char)s[k];
                        return Value::Num_((double)s.size());
                    }
                    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(),
                        (int)s.size(), nullptr, 0, nullptr, nullptr);
                    if (n <= 0) return Value::Num_(0);
                    WideCharToMultiByte(CP_UTF8, 0, s.c_str(),
                        (int)s.size(), p, n, nullptr, nullptr);
                    return Value::Num_((double)n); });
                // read(ptr, n) -> byte string (one char per raw byte)
                tb->map[L"read"] = NF([](vector<Value>& a) -> Value {
                    if (a.size() < 2 || a[0].t != Value::Num)
                        return Value::Str_(L"");
                    const char* p =
                        (const char*)(INT_PTR)(long long)BNum(a[0]);
                    long long n = (long long)BNum(a[1]);
                    if (!p || n <= 0) return Value::Str_(L"");
                    wstring s((size_t)n, 0);
                    for (long long k = 0; k < n; k++)
                        s[(size_t)k] =
                            (wchar_t)(unsigned char)p[k];
                    return Value::Str_(s); });
                // readW(ptr, nChars) -> wide text from a UTF-16 buffer
                tb->map[L"readW"] = NF([](vector<Value>& a) -> Value {
                    if (a.size() < 2 || a[0].t != Value::Num)
                        return Value::Str_(L"");
                    const wchar_t* p = (const wchar_t*)(INT_PTR)
                        (long long)BNum(a[0]);
                    long long n = (long long)BNum(a[1]);
                    if (!p || n <= 0) return Value::Str_(L"");
                    return Value::Str_(wstring((size_t)n, L'\0')
                        .assign(p, p + n)); });
                // ---- M3-b: struct layout serialize/deserialize ----
                // serialize: table fields -> heap buffer; returns pointer
                tb->map[L"structPtr"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty()) return Value::Num_(0);
                    void* p = StructSerialize(a[0]);
                    return Value::Num_((double)(INT_PTR)p);
                });
                // deserialize: buffer -> table fields (in place)
                tb->map[L"structFrom"] = NF([](vector<Value>& a) -> Value {
                    if (a.size() < 2 || (a[0].t != Value::TableT && a[0].t != Value::Class))
                        return a.empty() ? Value() : a[0];
                    Value vTab = a[0];
                    void* p = a[1].t == Value::Pointer ? a[1].ptr
                        : (void*)(INT_PTR)(long long)BNum(a[1]);
                    StructDeserialize(vTab, p);
                    return vTab;
                });
                // byte size of a struct table
                tb->map[L"sizeOf"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty()) return Value::Num_(0);
                    if (a[0].t == Value::Str)
                        return Value::Num_((double)GetStructFieldSize(a[0].s, 1));
                    return Value::Num_((double)CalcStructTotalSize(a[0]));
                });
                tb->map[L"sizeof"] = tb->map[L"sizeOf"];
                // raw.len: string byte length, or the struct's total size.
                // builtin/raw.aardio relies on it for equal()/typeOfArray.
                tb->map[L"len"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty() || a[0].t == Value::Null) return Value();
                    if (a[0].t == Value::Str)
                        return Value::Num_((double)BStr(a[0]).size());
                    if (a[0].t == Value::TableT || a[0].t == Value::Class)
                    {
                        auto is = a[0].tab ? a[0].tab->map.find(L"_struct")
                                           : a[0].tab->map.end();
                        if (is != a[0].tab->map.end() && is->second.t == Value::Str)
                            return Value::Num_(
                                (double)CalcStructTotalSize(a[0]));
                    }
                    return Value(); });
                // raw.uint / raw.int / raw.word / raw.byte — read a little-
                // endian integer of the given width from a pointer/string.
                // Used as raw.uint(,true) to allocate an out-parameter.
                auto mkReader = [](int nWidth) {
                    return [nWidth](vector<Value>& a) -> Value {
                        bool bZero = !a.empty() &&
                                     (a[0].t == Value::Null ||
                                      (a[0].t == Value::Num && a[0].n == 0));
                        if (bZero)
                        {
                            // raw.uint(,true): allocate an out-parameter
                            // buffer of this width and return a table
                            // wrapper exposing .value
                            auto tb = std::make_shared<Table>();
                            std::vector<char> z((size_t)nWidth, 0);
                            tb->map[L"_bytes"] = Value::Str_(
                                wstring(z.begin(), z.end()));
                            tb->map[L"value"] = Value::Num_(0);
                            return Value::Table_(tb);
                        }
                        const char* p = nullptr;
                        std::string held;
                        if (!a.empty() && a[0].t == Value::Str)
                        {
                            held = ToA(BStr(a[0]));
                            p = held.c_str();
                        }
                        else if (!a.empty() && a[0].t == Value::Pointer)
                            p = (const char*)a[0].ptr;
                        else if (!a.empty() && a[0].t == Value::Num)
                            p = (const char*)(INT_PTR)(long long)a[0].n;
                        if (!p) return Value::Num_(0);
                        unsigned long long v = 0;
                        for (int k = 0; k < nWidth; k++)
                            v |= ((unsigned long long)(unsigned char)p[k]) << (8 * k);
                        return Value::Num_((double)v);
                    };
                };
                tb->map[L"uint"] = NF(mkReader(4));
                tb->map[L"int"] = NF(mkReader(4));
                tb->map[L"word"] = NF(mkReader(2));
                tb->map[L"byte"] = NF(mkReader(1));
                tb->map[L"ushort"] = NF(mkReader(2));
                tb->map[L"short"] = NF(mkReader(2));
                tb->map[L"ulong"] = NF(mkReader(4));
                tb->map[L"long"] = NF(mkReader(4));
                // raw.topointer / raw.tonumber — pointer <-> integer
                tb->map[L"topointer"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty() || a[0].t == Value::Null)
                        return Value::Pointer_(nullptr);
                    if (a[0].t == Value::Pointer) return a[0];
                    if (a[0].t == Value::Num)
                        return Value::Pointer_((void*)(INT_PTR)(long long)a[0].n);
                    if (a[0].t == Value::Str)
                        return Value::Pointer_((void*)a[0].s.c_str());
                    return Value::Pointer_(nullptr); });
                tb->map[L"tonumber"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty()) return Value::Num_(0);
                    if (a[0].t == Value::Pointer)
                        return Value::Num_((double)(INT_PTR)a[0].ptr);
                    return Value::Num_(BNum(a[0])); });
                // raw.tostring(ptr,len) — read len bytes from a pointer
                tb->map[L"tostring"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty()) return Value::Str_(L"");
                    const char* p = nullptr;
                    if (a[0].t == Value::Pointer) p = (const char*)a[0].ptr;
                    else if (a[0].t == Value::Num)
                        p = (const char*)(INT_PTR)(long long)a[0].n;
                    else if (a[0].t == Value::Str)
                    {
                        wstring w = BStr(a[0]);
                        return Value::Str_(w);
                    }
                    if (!p) return Value::Str_(L"");
                    size_t n = a.size() > 1 ? (size_t)BNum(a[1]) : 0;
                    if (!n)
                    {
                        // no length: read to the first NUL
                        n = 0;
                        while (n < (1u << 20) && p[n]) n++;
                    }
                    wstring w;
                    w.resize(n);
                    for (size_t k = 0; k < n; k++)
                        w[k] = (wchar_t)(unsigned char)p[k];
                    return Value::Str_(w); });
                // raw.copy(dst,src,size) — raw memory copy, returns dst
                tb->map[L"copy"] = NF([](vector<Value>& a) -> Value {
                    if (a.size() < 3) return a.empty() ? Value() : a[0];
                    void* d = nullptr; const char* s = nullptr;
                    std::string held;
                    if (a[0].t == Value::Pointer) d = a[0].ptr;
                    else if (a[0].t == Value::Num)
                        d = (void*)(INT_PTR)(long long)a[0].n;
                    if (a[1].t == Value::Str) { held = ToA(BStr(a[1])); s = held.c_str(); }
                    else if (a[1].t == Value::Pointer) s = (const char*)a[1].ptr;
                    else if (a[1].t == Value::Num)
                        s = (const char*)(INT_PTR)(long long)a[1].n;
                    size_t n = (size_t)BNum(a[2]);
                    if (d && s && n) memcpy(d, s, n);
                    return a[0]; });
                // raw.concat(dst,src,len) — append src bytes to a buffer
                tb->map[L"concat"] = NF([](vector<Value>& a) -> Value {
                    if (a.size() < 2) return a.empty() ? Value() : a[0];
                    std::string held;
                    const char* s = nullptr; size_t n = 0;
                    if (a[1].t == Value::Str)
                    {
                        held = ToA(BStr(a[1]));
                        s = held.c_str();
                        n = a.size() > 2 ? (size_t)BNum(a[2]) : held.size();
                    }
                    else if (a[1].t == Value::Pointer)
                    {
                        s = (const char*)a[1].ptr;
                        n = a.size() > 2 ? (size_t)BNum(a[2]) : 0;
                    }
                    if (a[0].t == Value::Str && s)
                    {
                        // string form: append the bytes
                        wstring w = BStr(a[0]);
                        for (size_t k = 0; k < n && s[k]; k++)
                            w.push_back((wchar_t)(unsigned char)s[k]);
                        return Value::Str_(w);
                    }
                    return a[0]; });
                // raw.closehandle(h) — CloseHandle
                tb->map[L"closehandle"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty()) return Value::Bool_(false);
                    HANDLE h = nullptr;
                    if (a[0].t == Value::Pointer) h = a[0].ptr;
                    else if (a[0].t == Value::Num)
                        h = (HANDLE)(INT_PTR)(long long)a[0].n;
                    if (!h) return Value::Bool_(false);
                    return Value::Bool_(CloseHandle(h) != FALSE); });
                // raw.realloc(p,size) — heap reallocate
                tb->map[L"realloc"] = NF([](vector<Value>& a) -> Value {
                    if (a.size() < 2) return Value();
                    size_t n = (size_t)BNum(a[1]);
                    void* p = nullptr;
                    if (a[0].t == Value::Pointer) p = a[0].ptr;
                    else if (a[0].t == Value::Num && a[0].n != 0)
                        p = (void*)(INT_PTR)(long long)a[0].n;
                    if (!n) { if (p) HeapFree(GetProcessHeap(), 0, p);
                              return Value::Pointer_(nullptr); }
                    void* q = p ? HeapReAlloc(GetProcessHeap(), 0, p, n)
                                : HeapAlloc(GetProcessHeap(), 0, n);
                    return Value::Pointer_(q); });
                tb->map[L"malloc"] = tb->map[L"realloc"];
                // raw.equal(a,b,size) — byte comparison
                tb->map[L"equal"] = NF([](vector<Value>& a) -> Value {
                    if (a.size() < 2) return Value::Bool_(true);
                    std::string x, y;
                    if (a[0].t == Value::Str) x = ToA(BStr(a[0]));
                    if (a[1].t == Value::Str) y = ToA(BStr(a[1]));
                    if (x.empty() || y.empty()) return Value::Bool_(x == y);
                    size_t n = a.size() > 2 ? (size_t)BNum(a[2])
                                            : min(x.size(), y.size());
                    if (n > x.size() || n > y.size()) return Value::Bool_(false);
                    return Value::Bool_(memcmp(x.data(), y.data(), n) == 0); });
                tb->map[L"toPointer"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty() || a[0].t == Value::Null) return Value::Pointer_(nullptr);
                    if (a[0].t == Value::Pointer) return a[0];
                    if (a[0].t == Value::Num) return Value::Pointer_((void*)(INT_PTR)(long long)a[0].n);
                    if (a[0].t == Value::Str) return Value::Pointer_((void*)a[0].s.c_str());
                    return Value::Pointer_((void*)(INT_PTR)(long long)BNum(a[0])); });
                tb->map[L"convert"] =
                    NF([](vector<Value>& a) -> Value {
                    if (a.size() < 2) return a.empty() ? Value() : a[0];
                    const Value& from = a[0];
                    Value to = a[1];
                    long long off = a.size() > 2 ? (long long)BNum(a[2]) : 0;
                    if (to.t != Value::TableT || !to.tab) return to;

                    if (from.t == Value::TableT || from.t == Value::Class)
                    {
                        void* pSrc = StructSerialize(from);
                        if (pSrc)
                        {
                            StructDeserialize(to, (const char*)pSrc + off);
                            HeapFree(GetProcessHeap(), 0, pSrc);
                        }
                    }
                    else if (from.t == Value::Pointer)
                    {
                        if (from.ptr) StructDeserialize(to, (const char*)from.ptr + off);
                    }
                    else if (from.t == Value::Num)
                    {
                        void* p = (void*)(INT_PTR)(long long)from.n;
                        if (p) StructDeserialize(to, (const char*)p + off);
                    }
                    else if (from.t == Value::Str)
                    {
                        StructDeserialize(to, (const char*)from.s.data() + off);
                    }
                    return to;
                });
                // M5: preloaded system DLLs, mirroring builtin/_.aardio's
                // ::User32/::Kernel32 — api.aardio assumes they exist
                {
                    auto uDll = tb->map[L"loadDll"];
                    vector<Value> ua{ Value::Str_(L"User32.dll") };
                    if (uDll.native)
                    {
                        globals.vars[L"User32"] = uDll.native(ua);
                        roots[L"User32"] = globals.vars[L"User32"];
                    }
                    vector<Value> ka{ Value::Str_(L"Kernel32.dll") };
                    if (uDll.native)
                    {
                        globals.vars[L"Kernel32"] = uDll.native(ka);
                        roots[L"Kernel32"] = globals.vars[L"Kernel32"];
                    }
                    // win/_.aardio also calls ::Shell32 / ::Gdi32 /
                    // ::Comctl32 style roots - preload the common ones
                    const wchar_t* DLLS[] = { L"Shell32.dll", L"Gdi32.dll",
                        L"Ole32.dll", L"OleAut32.dll", L"Advapi32.dll",
                        L"Comctl32.dll", L"Comdlg32.dll", L"Uxtheme.dll",
                        L"Version.dll", L"Winmm.dll", L"Msimg32.dll",
                        L"Rpcrt4.dll", nullptr };
                    for (int di = 0; DLLS[di]; di++)
                    {
                        vector<Value> da{ Value::Str_(DLLS[di]) };
                        Value dv = uDll.native(da);
                        wstring nm(DLLS[di]);
                        nm = nm.substr(0, nm.size() - 4);
                        globals.vars[nm] = dv;
                        roots[nm] = dv;
                    }
                }

                // install the late-bound dispatch used by trampolines
                if (!g_cbDispatch)
                    g_cbDispatch = [](CbCtx* ctx,
                                      vector<Value>& args) -> __int64 {
                        Value r = ctx->interp->CallOwner(
                            ctx->fn, ctx->owner, args);
                        switch (ctx->sig.ret)
                        {
                        case ARDbl: return (__int64)BNum(r);
                        case ARBool: return BNum(r) != 0 ? 1 : 0;
                        case ARStrA:
                        case ARStrW:
                        default: return (__int64)BNum(r);
                        }
                    };
                // M3-c: callback(fn, "ret(p1,p2)", owner) -> function pointer
                // (number) that APIs can call; the VM function receives
                // the caller's arguments as numbers
                auto fnCallback =
                    NF([this](vector<Value>& a) -> Value {
                    if (a.size() < 2 ||
                        !(a[0].t == Value::Func))
                    {
                        (*err) =
                            L"运行错误: raw.callback(fn, sig, owner)";
                        return Value();
                    }
                    ApiSig sig;
                    if (!ParseApiSig(BStr(a[1]), &sig))
                    {
                        (*err) = L"运行错误: 回调签名无效: " +
                                 BStr(a[1]);
                        return Value();
                    }
                    CbCtx* ctx = new CbCtx();
                    ctx->fn = a[0];
                    if (a.size() >= 3)
                        ctx->owner = a[2];
                    ctx->sig = sig;
                    ctx->interp = this;
                    CallbackTrampoline(ctx);
                    g_cbs.push_back(ctx);
                    return Value::Num_(
                        (double)(__int64)(INT_PTR)ctx->code); });
                tb->map[L"callback"] = fnCallback;
                tb->map[L"tostdcall"] = fnCallback;
                tb->map[L"tocdecl"] = fnCallback;
                globals.vars[L"tostdcall"] = fnCallback;
                globals.vars[L"tocdecl"] = fnCallback;
            }
            // ---- table ----
            {
                auto tb = GetRoot(L"table").tab;
                // table.slice(array,start,end) — aardio 1-based slice
                tb->map[L"slice"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty() || a[0].t != Value::TableT || !a[0].tab)
                        return Value();
                    auto& src = a[0].tab->arr;
                    auto out = std::make_shared<Table>();
                    long long n = (long long)src.size();
                    long long s = a.size() > 1 ? (long long)BNum(a[1]) : 1;
                    long long e = a.size() > 2 && a[2].t != Value::Null
                                      ? (long long)BNum(a[2]) : n;
                    if (s < 0) s = n + s + 1;
                    if (e < 0) e = n + e + 1;
                    if (s < 1) s = 1;
                    if (e > n) e = n;
                    for (long long i = s; i <= e; i++)
                        out->arr.push_back(src[(size_t)i - 1]);
                    return Value::Table_(out); });
                // table.clone(t) — shallow copy including the array part
                tb->map[L"clone"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty() || a[0].t != Value::TableT || !a[0].tab)
                        return Value();
                    auto out = std::make_shared<Table>();
                    out->map = a[0].tab->map;
                    out->arr = a[0].tab->arr;
                    return Value::Table_(out); });
                // table.assign(target, src...) - copy key-values from
                // each source table into the target (metaProperty ctor)
                tb->map[L"assign"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty() || a[0].t != Value::TableT || !a[0].tab)
                        return Value();
                    for (size_t si = 1; si < a.size(); si++)
                    {
                        if (a[si].t != Value::TableT || !a[si].tab)
                            continue;
                        for (auto& kv : a[si].tab->map)
                        {
                            if (kv.second.t != Value::Null)
                                a[0].tab->map[kv.first] = kv.second;
                            else
                                a[0].tab->map.erase(kv.first);
                        }
                        for (size_t ai = 0; ai < a[si].tab->arr.size(); ai++)
                            a[0].tab->arr.push_back(a[si].tab->arr[ai]);
                    }
                    return a[0];
                });
                tb->map[L"count"] = NF([](vector<Value>& a) {
                    if (a[0].t != Value::TableT || !a[0].tab) return Value::Num_(0);
                    return Value::Num_((double)(a[0].tab->arr.size() +
                                                a[0].tab->map.size())); });
                auto fnIsArray = NF([](vector<Value>& a) -> Value {
                    if (a.empty() || a[0].t != Value::TableT || !a[0].tab)
                        return Value::Bool_(false);
                    return Value::Bool_(!a[0].tab->arr.empty());
                });
                tb->map[L"isArray"] = fnIsArray;
                tb->map[L"isArrayLike"] = fnIsArray;
                globals.vars[L"isArray"] = fnIsArray;
                globals.vars[L"isArrayLike"] = fnIsArray;
                tb->map[L"append"] = NF([](vector<Value>& a) {
                    if (a[0].t == Value::TableT && a[0].tab)
                        a[0].tab->arr.push_back(a.size() > 1 ? a[1] : Value());
                    return a[0]; });
                tb->map[L"gc"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty()) return Value();
                    if (a.size() > 1 && a[0].t == Value::TableT && a[0].tab)
                        a[0].tab->map[L"_gc"] = a[1];
                    return a[0]; });
                tb->map[L"add_gc"] = tb->map[L"gc"];
                auto fnPush = NF([](vector<Value>& a) -> Value {
                    if (a.empty() || a[0].t != Value::TableT || !a[0].tab)
                        return Value::Num_(0);
                    Table* t = a[0].tab.get();
                    for (size_t i = 1; i < a.size(); i++)
                        t->arr.push_back(a[i]);
                    return Value::Num_((double)t->arr.size());
                });
                tb->map[L"push"] = fnPush;
                globals.vars[L"push"] = fnPush;

                auto fnPop = NF([](vector<Value>& a) -> Value {
                    if (a.empty() || a[0].t != Value::TableT || !a[0].tab || a[0].tab->arr.empty())
                        return Value();
                    Table* t = a[0].tab.get();
                    Value v = t->arr.back();
                    t->arr.pop_back();
                    return v;
                });
                tb->map[L"pop"] = fnPop;
                globals.vars[L"pop"] = fnPop;

                auto fnSplice = NF([](vector<Value>& a) -> Value {
                    auto res = std::make_shared<Table>();
                    if (a.empty() || a[0].t != Value::TableT || !a[0].tab)
                        return Value::Table_(res);
                    Table* t = a[0].tab.get();
                    long long idx = a.size() > 1 ? (long long)BNum(a[1]) : 1;
                    if (idx < 1) idx = (long long)t->arr.size() + idx + 1;
                    if (idx < 1) idx = 1;
                    size_t start = (size_t)idx - 1;
                    if (start > t->arr.size()) start = t->arr.size();
                    long long cnt = a.size() > 2 ? (long long)BNum(a[2]) : ((long long)t->arr.size() - (long long)start);
                    if (cnt < 0) cnt = 0;
                    size_t count = (size_t)cnt;
                    if (start + count > t->arr.size()) count = t->arr.size() - start;
                    for (size_t k = 0; k < count; k++)
                        res->arr.push_back(t->arr[start + k]);
                    t->arr.erase(t->arr.begin() + start, t->arr.begin() + start + count);
                    for (size_t k = 3; k < a.size(); k++)
                        t->arr.insert(t->arr.begin() + start + (k - 3), a[k]);
                    return Value::Table_(res);
                });
                tb->map[L"splice"] = fnSplice;
                globals.vars[L"splice"] = fnSplice;
                // ---- round 43: array ops ----
                tb->map[L"insert"] = NF([](vector<Value>& a) -> Value {
                    if (a.size() < 2 || a[0].t != Value::TableT || !a[0].tab)
                        return a.size() > 0 ? a[0] : Value();
                    Table* t = a[0].tab.get();
                    if (a.size() >= 3)
                    {
                        long long i = (long long)BNum(a[1]);
                        if (i < 1) i = 1;
                        if (i > (long long)t->arr.size() + 1)
                            i = (long long)t->arr.size() + 1;
                        t->arr.insert(t->arr.begin() + (i - 1), a[2]);
                    }
                    else
                        t->arr.push_back(a[1]);
                    return a[0]; });
                tb->map[L"remove"] = NF([](vector<Value>& a) -> Value {
                    if (a.size() < 2 || a[0].t != Value::TableT || !a[0].tab)
                        return Value();
                    Table* t = a[0].tab.get();
                    long long i = (long long)BNum(a[1]);
                    if (i < 1 || i > (long long)t->arr.size())
                        return Value();
                    Value v = t->arr[(size_t)i - 1];
                    t->arr.erase(t->arr.begin() + (i - 1));
                    return v; });
                tb->map[L"reverse"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty() || a[0].t != Value::TableT || !a[0].tab)
                        return a.empty() ? Value() : a[0];
                    std::reverse(a[0].tab->arr.begin(),
                                 a[0].tab->arr.end());
                    return a[0]; });
                tb->map[L"concat"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty() || a[0].t != Value::TableT || !a[0].tab)
                        return Value::Str_(L"");
                    wstring sep = a.size() > 1 ? BStr(a[1]) : L"";
                    wstring out;
                    auto& arr = a[0].tab->arr;
                    for (size_t k = 0; k < arr.size(); k++)
                    {
                        if (k) out += sep;
                        out += arr[k].ToString();
                    }
                    return Value::Str_(out); });
                tb->map[L"find"] = NF([](vector<Value>& a) -> Value {
                    if (a.size() < 2 || a[0].t != Value::TableT || !a[0].tab)
                        return Value();
                    auto& arr = a[0].tab->arr;
                    const Value& want = a[1];
                    for (size_t k = 0; k < arr.size(); k++)
                    {
                        const Value& v = arr[k];
                        bool eq;
                        if (v.t == Value::TableT || want.t == Value::TableT)
                            eq = v.tab == want.tab;   // identity
                        else
                            eq = v.ToString() == want.ToString() &&
                                 v.t == want.t;
                        if (eq) return Value::Num_((double)(k + 1));
                    }
                    return Value(); });
                // sort: default compares numbers numerically, everything
                // else by ToString; a custom comparator (function value)
                // is invoked re-entrantly and must return truthy when the
                // first argument belongs before the second (Lua/aardio)
                tb->map[L"sort"] = NF([this](vector<Value>& a) -> Value {
                    if (a.empty() || a[0].t != Value::TableT || !a[0].tab)
                        return a.empty() ? Value() : a[0];
                    std::vector<Value>* pArr = &a[0].tab->arr;
                    Value cmp = a.size() > 1 ? a[1] : Value();
                    // shell sort — no recursion, stable enough for VM use
                    size_t n = pArr->size();
                    size_t gap = 1;
                    while (gap < n / 3) gap = gap * 3 + 1;
                    auto before = [this, &cmp](const Value& x,
                                               const Value& y) -> bool {
                        if (cmp.t == Value::Func || cmp.native)
                        {
                            vector<Value> args{ x, y };
                            Value r = CallVmFunc(cmp, args);
                            return r.Truthy();
                        }
                        if (x.t == Value::Num && y.t == Value::Num)
                            return x.n < y.n;
                        return x.ToString() < y.ToString(); };
                    while (gap > 0)
                    {
                        for (size_t i = gap; i < n; i++)
                        {
                            Value tmp = (*pArr)[i];
                            size_t j = i;
                            while (j >= gap && before(tmp, (*pArr)[j - gap]))
                            {
                                (*pArr)[j] = (*pArr)[j - gap];
                                j -= gap;
                            }
                            (*pArr)[j] = tmp;
                        }
                        gap /= 3;
                    }
                    return a[0]; });
                tb->map[L"cache"] = NF([](vector<Value>& a) -> Value {
                    auto tbRes = std::make_shared<Table>();
                    auto meta = std::make_shared<Table>();
                    meta->map[L"_weak"] = Value::Str_(L"kv");
                    if (a.size() >= 2 && (a[1].t == Value::Func || a[1].native))
                        meta->map[L"_get"] = a[1];
                    else if (a.size() >= 1 && (a[0].t == Value::Func || a[0].native))
                        meta->map[L"_get"] = a[0];
                    tbRes->map[L"_meta"] = Value::Table_(meta);
                    return Value::Table_(tbRes); });
                tb->map[L"mix"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty()) return Value();
                    Value target = a[0];
                    if (target.t != Value::TableT || !target.tab)
                    {
                        target = Value::Table_(std::make_shared<Table>());
                    }
                    for (size_t i = 1; i < a.size(); i++)
                    {
                        if (a[i].t != Value::TableT || !a[i].tab) continue;
                        for (auto& kv : a[i].tab->map)
                        {
                            if (target.tab->map.find(kv.first) == target.tab->map.end())
                                target.tab->map[kv.first] = kv.second;
                        }
                        for (auto& v : a[i].tab->arr)
                            target.tab->arr.push_back(v);
                    }
                    return target; });
            }
            // ---- round 36: web (WinINet HTTP) ----
            {
                auto tb = GetRoot(L"web").tab;
                tb->map[L"get"] = NF([this](vector<Value>& a) -> Value {
                    wstring strErr2;
                    wstring r = HttpFetch(BStr(a[0]), L"GET", L"", L"",
                                          &strErr2);
                    if (!strErr2.empty())
                    {
                        (*err) = L"运行错误: web.get: " + strErr2;
                        return Value();
                    }
                    return Value::Str_(r); });
                tb->map[L"post"] = NF([this](vector<Value>& a) -> Value {
                    wstring strErr2;
                    wstring strCType = a.size() > 2
                        ? BStr(a[2])
                        : L"application/x-www-form-urlencoded";
                    wstring r = HttpFetch(BStr(a[0]), L"POST",
                                          a.size() > 1 ? BStr(a[1]) : L"",
                                          strCType, &strErr2);
                    if (!strErr2.empty())
                    {
                        (*err) = L"运行错误: web.post: " + strErr2;
                        return Value();
                    }
                    return Value::Str_(r); });
                tb->map[L"download"] = NF([this](vector<Value>& a) -> Value {
                    wstring strErr2;
                    wstring r = HttpFetch(BStr(a[0]), L"GET", L"", L"",
                                          &strErr2);
                    if (!strErr2.empty())
                    {
                        (*err) = L"运行错误: web.download: " + strErr2;
                        return Value::Bool_(false);
                    }
                    wstring strFile = a.size() > 1 ? BStr(a[1]) : L"web.bin";
                    FILE* pf = nullptr;
                    if (_wfopen_s(&pf, strFile.c_str(), L"wb") != 0 || !pf)
                    {
                        (*err) = L"运行错误: web.download: 无法写文件";
                        return Value::Bool_(false);
                    }
                    int nU8 = WideCharToMultiByte(CP_UTF8, 0, r.c_str(),
                                                  (int)r.size(), nullptr, 0,
                                                  nullptr, nullptr);
                    if (nU8 > 0)
                    {
                        std::vector<char> buf8((size_t)nU8);
                        WideCharToMultiByte(CP_UTF8, 0, r.c_str(), (int)r.size(),
                                            buf8.data(), nU8, nullptr,
                                            nullptr);
                        fwrite(buf8.data(), 1, (size_t)nU8, pf);
                    }
                    fclose(pf);
                    return Value::Bool_(true); });
                tb->map[L"encode"] = NF([](vector<Value>& a) -> Value {
                    wstring s = BStr(a[0]);
                    int nU8 = WideCharToMultiByte(CP_UTF8, 0, s.c_str(),
                                                  (int)s.size(), nullptr, 0,
                                                  nullptr, nullptr);
                    std::string str8((size_t)max(nU8, 0), 0);
                    if (nU8 > 0)
                        WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(),
                                            &str8[0], nU8, nullptr, nullptr);
                    wstring out;
                    const wchar_t* hex = L"0123456789ABCDEF";
                    for (unsigned char c : str8)
                    {
                        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                            (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                            c == '.' || c == '~')
                            out += (wchar_t)c;
                        else
                        {
                            out += L'%';
                            out += hex[c >> 4];
                            out += hex[c & 0xF];
                        }
                    }
                    return Value::Str_(out); });
                tb->map[L"decode"] = NF([](vector<Value>& a) -> Value {
                    wstring s = BStr(a[0]);
                    std::string str8;
                    for (size_t i = 0; i < s.size(); i++)
                    {
                        if (s[i] == L'%' && i + 2 < s.size() + 1 &&
                            i + 2 < s.size() + 1)
                        {
                            wchar_t h1 = s[i + 1], h2 = s[i + 2];
                            auto hv = [](wchar_t c) -> int {
                                if (c >= L'0' && c <= L'9') return c - L'0';
                                if (c >= L'A' && c <= L'F') return c - L'A' + 10;
                                if (c >= L'a' && c <= L'f') return c - L'a' + 10;
                                return -1; };
                            int a1 = hv(h1), a2 = hv(h2);
                            if (a1 >= 0 && a2 >= 0)
                            {
                                str8.push_back((char)((a1 << 4) | a2));
                                i += 2;
                                continue;
                            }
                        }
                        if (s[i] == L'+')
                            str8.push_back(' ');   // form-urlencoded space
                        else
                        {
                            int nU8 = WideCharToMultiByte(CP_UTF8, 0, &s[i],
                                                          1, nullptr, 0,
                                                          nullptr, nullptr);
                            std::string one((size_t)max(nU8, 0), 0);
                            if (nU8 > 0)
                                WideCharToMultiByte(CP_UTF8, 0, &s[i], 1,
                                                    &one[0], nU8, nullptr,
                                                    nullptr);
                            str8 += one;
                        }
                    }
                    int w = MultiByteToWideChar(CP_UTF8, 0, str8.data(),
                                                (int)str8.size(), nullptr, 0);
                    if (w <= 0)
                        return Value::Str_(s);
                    wstring out((size_t)w, 0);
                    MultiByteToWideChar(CP_UTF8, 0, str8.data(),
                                        (int)str8.size(), &out[0], w);
                    return Value::Str_(out); });
            }

            // ---- round 36: thread (worker threads, own VM per thread) ----
            {
                auto tb = GetRoot(L"thread").tab;
                // thread.applyCallWnd(form, wParam, lParam) - forward
                // a WM_THREAD_CALLWND payload to the owning window
                // (VM builtin in the original; residual stub here)
                tb->map[L"applyCallWnd"] = NF([](vector<Value>& a) -> Value {
                    return Value();
                });
                tb->map[L"getId"] = NF([](vector<Value>&) -> Value {
                    return Value::Num_((double)::GetCurrentThreadId());
                });
                // thread.status(handle) — 0 = still running ("active")
                tb->map[L"status"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty())
                        return Value::Str_(L"active");
                    HANDLE h = (HANDLE)(INT_PTR)(long long)BNum(a[0]);
                    if (!h)
                        return Value::Str_(L"active");
                    DWORD rc = WaitForSingleObject(h, 0);
                    if (rc == WAIT_OBJECT_0)
                    {
                        DWORD code = 0;
                        GetExitCodeThread(h, &code);
                        return Value::Num_((double)code);
                    }
                    return Value::Str_(L"active"); });
                // thread.suspend(handle) / thread.resume(handle)
                tb->map[L"suspend"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty())
                        return Value::Bool_(false);
                    HANDLE h = (HANDLE)(INT_PTR)(long long)BNum(a[0]);
                    if (!h)
                        return Value::Bool_(false);
                    return Value::Bool_(SuspendThread(h) != (DWORD)-1); });
                tb->map[L"resume"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty())
                        return Value::Bool_(false);
                    HANDLE h = (HANDLE)(INT_PTR)(long long)BNum(a[0]);
                    if (!h)
                        return Value::Bool_(false);
                    return Value::Bool_(ResumeThread(h) != (DWORD)-1); });
                tb->map[L"waitOne"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty())
                        return Value();
                    HANDLE h = (HANDLE)(INT_PTR)(long long)BNum(a[0]);
                    DWORD ms = a.size() > 1 ? (DWORD)BNum(a[1]) : INFINITE;
                    if (!h)
                        return Value();
                    DWORD rc = WaitForSingleObject(h, ms);
                    return Value::Bool_(rc == WAIT_OBJECT_0); });
                // thread.lock(name[,fn,...]) — named inter-thread lock. With
                // a function it runs under the lock and returns its results;
                // without one it acquires the lock until thread.unlock(name).
                static std::mutex g_lockMapMutex;
                static std::map<wstring, std::shared_ptr<std::mutex>>
                    g_namedLocks;
                tb->map[L"lock"] = NF([this](vector<Value>& a) -> Value {
                    if (a.empty())
                        return Value();
                    wstring nm = BStr(a[0]);
                    std::shared_ptr<std::mutex> m;
                    {
                        std::lock_guard<std::mutex> g(g_lockMapMutex);
                        auto it = g_namedLocks.find(nm);
                        if (it == g_namedLocks.end())
                        {
                            m = std::make_shared<std::mutex>();
                            g_namedLocks[nm] = m;
                        }
                        else
                            m = it->second;
                    }
                    if (a.size() > 1 && a[1].t == Value::Func)
                    {
                        std::lock_guard<std::mutex> g(*m);
                        vector<Value> args;
                        for (size_t i = 2; i < a.size(); i++)
                            args.push_back(a[i]);
                        return this->CallVmFunc(a[1], args);
                    }
                    m->lock();          // manual acquire
                    return Value::Bool_(true); });
                tb->map[L"unlock"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty())
                        return Value::Bool_(false);
                    std::shared_ptr<std::mutex> m;
                    {
                        std::lock_guard<std::mutex> g(g_lockMapMutex);
                        auto it = g_namedLocks.find(BStr(a[0]));
                        if (it == g_namedLocks.end())
                            return Value::Bool_(false);
                        m = it->second;
                    }
                    m->unlock();
                    return Value::Bool_(true); });
                tb->map[L"close"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty())
                        return Value::Bool_(false);
                    HANDLE h = (HANDLE)(INT_PTR)(long long)BNum(a[0]);
                    if (!h)
                        return Value::Bool_(false);
                    return Value::Bool_(CloseHandle(h) != 0); });
                tb->map[L"get"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty()) return Value();
                    std::lock_guard<std::mutex> lock(g_threadSharedMutex);
                    auto it = g_threadSharedMap.find(BStr(a[0]));
                    if (it != g_threadSharedMap.end()) return it->second;
                    return Value();
                });
                tb->map[L"set"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty()) return Value();
                    std::lock_guard<std::mutex> lock(g_threadSharedMutex);
                    if (a.size() > 1 && a[1].t != Value::Null)
                        g_threadSharedMap[BStr(a[0])] = a[1];
                    else
                        g_threadSharedMap.erase(BStr(a[0]));
                    return Value();
                });
                tb->map[L"getWnd"] = NF([](vector<Value>&) -> Value {
                    return Value();
                });
                tb->map[L"setWnd"] = NF([](vector<Value>&) -> Value {
                    return Value();
                });
                tb->map[L"callWnd"] = NF([](vector<Value>&) -> Value {
                    return Value();
                });
                globals.vars[L"__waitForMultipleObjects"] = NF([](vector<Value>& a) -> Value {
                    return Value::Num_(0);
                });
                globals.vars[L"__messagePeek"] = NF([](vector<Value>& a) -> Value {
                    return Value::Bool_(false);
                });
                tb->map[L"create"] = NF([this](vector<Value>& a) -> Value {
                    if (a.empty() || a[0].t != Value::Func || !a[0].fn)
                    {
                        (*err) = L"运行错误: thread.create 需要函数字面值";
                        return Value();
                    }
                    PNode fnBody = a[0].fn;
                    vector<wstring> params = a[0].params;
                    vector<Value> args;
                    for (size_t i = 1; i < a.size(); i++)
                        args.push_back(DeepCopy(a[i]));
                    auto st = std::make_shared<ThreadState>();
                    st->th = std::thread(
                        [fnBody, params, args, st]() {
                            wstring threadErr;
                            Interp wt{ &threadErr };
                            g_interpCurrent = &wt;
                            wt.InitBuiltins();
                            Env local;
                            local.parent = &wt.globals;
                            for (size_t i = 0; i < params.size(); i++)
                                local.vars[params[i]] =
                                    i < args.size() ? args[i] : Value();
                            wt.nReturn = 0;
                            Value r = wt.ExecBlock(fnBody, &local);
                            if (threadErr.empty())
                                st->result = DeepCopy(r);
                            else
                                st->error = threadErr;
                            st->done = true;
                        });
                    st->th.detach();
                    auto h = std::make_shared<Table>();
                    h->map[L"join"] = NF([st, this](vector<Value>&) -> Value {
                        while (!st->done)
                            Sleep(5);
                        if (!st->error.empty())
                        {
                            (*err) = L"运行错误: 线程内错误: " + st->error;
                            return Value();
                        }
                        return st->result; });
                    h->map[L"done"] = NF([st](vector<Value>&) -> Value {
                        return Value::Bool_(st->done); });
                    return Value::Table_(h); });
            }

            // ---- round 37: fsys (file system) ----
            {
                auto tb = GetRoot(L"fsys").tab;
                tb->map[L"joinpath"] = GetRoot(L"io").tab->map[L"joinpath"];
                tb->map[L"splitpath"] = GetRoot(L"io").tab->map[L"splitpath"];
                tb->map[L"exists"] = NF([](vector<Value>& a) -> Value {
                    return Value::Bool_(
                        GetFileAttributesW(BStr(a[0]).c_str()) !=
                        INVALID_FILE_ATTRIBUTES); });
                tb->map[L"isDir"] = NF([](vector<Value>& a) -> Value {
                    DWORD at = GetFileAttributesW(BStr(a[0]).c_str());
                    return Value::Bool_(at != INVALID_FILE_ATTRIBUTES &&
                                        (at & FILE_ATTRIBUTE_DIRECTORY)); });
                tb->map[L"createDir"] = NF([](vector<Value>& a) -> Value {
                    // recursive: create each path segment in turn
                    wstring strPath = BStr(a[0]);
                    if (strPath.empty())
                        return Value::Bool_(false);
                    DWORD at = GetFileAttributesW(strPath.c_str());
                    if (at != INVALID_FILE_ATTRIBUTES)
                        return Value::Bool_(at & FILE_ATTRIBUTE_DIRECTORY);
                    for (size_t i = 3; i < strPath.size(); i++)
                    {
                        if (strPath[i] != L'\\' && strPath[i] != L'/')
                            continue;
                        wstring part = strPath.substr(0, i);
                        if (!part.empty() && part[part.size() - 1] != L':')
                            CreateDirectoryW(part.c_str(), nullptr);
                    }
                    return Value::Bool_(CreateDirectoryW(strPath.c_str(),
                                                         nullptr) ||
                        GetFileAttributesW(strPath.c_str()) !=
                            INVALID_FILE_ATTRIBUTES); });
                tb->map[L"delete"] = NF([](vector<Value>& a) -> Value {
                    // recursive delete (files then deepest-first directories)
                    std::function<bool(const wstring&)> del =
                        [&](const wstring& strDir) -> bool {
                        WIN32_FIND_DATAW fd;
                        HANDLE h = FindFirstFileW(
                            (strDir + L"\\*").c_str(), &fd);
                        if (h != INVALID_HANDLE_VALUE)
                        {
                            do
                            {
                                if (!wcscmp(fd.cFileName, L".") ||
                                    !wcscmp(fd.cFileName, L".."))
                                    continue;
                                wstring strChild =
                                    strDir + L"\\" + fd.cFileName;
                                if (fd.dwFileAttributes &
                                    FILE_ATTRIBUTE_DIRECTORY)
                                    del(strChild);
                                else
                                    DeleteFileW(strChild.c_str());
                            } while (FindNextFileW(h, &fd));
                            FindClose(h);
                        }
                        return RemoveDirectoryW(strDir.c_str()) ||
                            DeleteFileW(strDir.c_str()) ||
                            GetFileAttributesW(strDir.c_str()) ==
                                INVALID_FILE_ATTRIBUTES;
                    };
                    return Value::Bool_(del(BStr(a[0]))); });
                tb->map[L"copy"] = NF([](vector<Value>& a) -> Value {
                    return Value::Bool_(CopyFileW(BStr(a[0]).c_str(),
                        BStr(a[1]).c_str(), FALSE)); });
                tb->map[L"move"] = NF([](vector<Value>& a) -> Value {
                    return Value::Bool_(MoveFileW(BStr(a[0]).c_str(),
                        BStr(a[1]).c_str())); });
                tb->map[L"size"] = NF([](vector<Value>& a) -> Value {
                    WIN32_FIND_DATAW fd;
                    HANDLE h = FindFirstFileW(BStr(a[0]).c_str(), &fd);
                    if (h == INVALID_HANDLE_VALUE)
                        return Value::Num_(0);
                    FindClose(h);
                    ULONGLONG n = ((ULONGLONG)fd.nFileSizeHigh << 32) |
                                  fd.nFileSizeLow;
                    return Value::Num_((double)n); });
                tb->map[L"list"] = NF([](vector<Value>& a) -> Value {
                    // array of {name, dir, size}; "." and ".." skipped
                    auto out = std::make_shared<Table>();
                    WIN32_FIND_DATAW fd;
                    HANDLE h = FindFirstFileW(
                        (BStr(a[0]) + L"\\*").c_str(), &fd);
                    if (h == INVALID_HANDLE_VALUE)
                        return Value::Table_(out);
                    do
                    {
                        if (!wcscmp(fd.cFileName, L".") ||
                            !wcscmp(fd.cFileName, L".."))
                            continue;
                        auto it = std::make_shared<Table>();
                        it->map[L"name"] = Value::Str_(fd.cFileName);
                        it->map[L"dir"] = Value::Bool_(
                            fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
                        ULONGLONG n = ((ULONGLONG)fd.nFileSizeHigh << 32) |
                                      fd.nFileSizeLow;
                        it->map[L"size"] = Value::Num_((double)n);
                        out->arr.push_back(Value::Table_(it));
                    } while (FindNextFileW(h, &fd));
                    FindClose(h);
                    return Value::Table_(out); });
                tb->map[L"tmpDir"] = NF([](vector<Value>&) -> Value {
                    wchar_t szPath[MAX_PATH + 1] = L"";
                    DWORD n = GetTempPathW(MAX_PATH, szPath);
                    wstring s(n ? szPath : L"");
                    if (!s.empty() && s[s.size() - 1] != L'\\')
                        s += L"\\";
                    return Value::Str_(s); });
            }

            // ---- round 37: process (launch + run with output) ----
            {
                auto tb = GetRoot(L"process").tab;
                tb->map[L"execute"] = NF([](vector<Value>& a) -> Value {
                    // ShellExecute: launch, do not wait
                    wstring strParams = a.size() > 1 ? BStr(a[1]) : L"";
                    HINSTANCE h = ShellExecuteW(nullptr, L"open",
                        BStr(a[0]).c_str(),
                        strParams.empty() ? nullptr : strParams.c_str(),
                        nullptr, SW_SHOWNORMAL);
                    return Value::Bool_((INT_PTR)h > 32); });
                tb->map[L"run"] = NF([](vector<Value>& a) -> Value {
                    // run a command line, wait, capture merged output
                    auto out = std::make_shared<Table>();
                    out->map[L"out"] = Value::Str_(L"");
                    out->map[L"code"] = Value::Num_(-1);
                    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
                    HANDLE hRead = nullptr, hWrite = nullptr;
                    if (!CreatePipe(&hRead, &hWrite, &sa, 0))
                        return Value::Table_(out);
                    STARTUPINFOW si = { sizeof(si) };
                    si.dwFlags = STARTF_USESTDHANDLES;
                    si.hStdOutput = hWrite;
                    si.hStdError = hWrite;
                    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
                    wstring strCmd = BStr(a[0]);
                    vector<wchar_t> cmd(strCmd.begin(), strCmd.end());
                    cmd.push_back(L'\0');
                    PROCESS_INFORMATION pi = {};
                    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr,
                        nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                        &si, &pi);
                    CloseHandle(hWrite);   // child holds the write end now
                    if (!ok)
                    {
                        CloseHandle(hRead);
                        return Value::Table_(out);
                    }
                    std::string str8;
                    char buf[4096];
                    DWORD n = 0;
                    while (ReadFile(hRead, buf, sizeof(buf), &n, nullptr) &&
                           n > 0)
                        str8.append(buf, n);
                    CloseHandle(hRead);
                    WaitForSingleObject(pi.hProcess, 15000);
                    DWORD code = 0;
                    GetExitCodeProcess(pi.hProcess, &code);
                    CloseHandle(pi.hThread);
                    CloseHandle(pi.hProcess);
                    out->map[L"code"] = Value::Num_((double)code);
                    int w = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                        str8.data(), (int)str8.size(), nullptr, 0);
                    if (w <= 0)
                        w = MultiByteToWideChar(CP_ACP, 0, str8.data(),
                                                (int)str8.size(), nullptr, 0);
                    if (w > 0)
                    {
                        wstring s((size_t)w, 0);
                        MultiByteToWideChar(CP_UTF8, 0, str8.data(),
                                            (int)str8.size(), &s[0], w);
                        out->map[L"out"] = Value::Str_(s);
                    }
                    return Value::Table_(out); });
            }

    // ---- round 38: win.form — minimal windowing ----
            {
                auto tb = GetRoot(L"win").tab;
                tb->map[L"form"] = NF([this](vector<Value>& a) -> Value {
                    if (!RegisterFormClass())
                    {
                        (*err) = L"运行错误: win.form 窗口类注册失败";
                        return Value();
                    }
                    wstring strText = a.size() > 0 ? BStr(a[0]) : L"窗体";
                    int nW = a.size() > 1 ? (int)BNum(a[1]) : 600;
                    int nH = a.size() > 2 ? (int)BNum(a[2]) : 400;
                    FormState* fs = new FormState();
                    fs->it = this;
                    HWND hwnd = CreateWindowExW(
                        0, L"AardioRE_form", strText.c_str(),
                        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                        nW, nH, nullptr, nullptr, GetModuleHandle(nullptr),
                        nullptr);
                    if (!hwnd)
                    {
                        delete fs;
                        (*err) = L"运行错误: win.form 窗口创建失败";
                        return Value();
                    }
                    g_forms[hwnd] = fs;

                    auto form = std::make_shared<Table>();

                    HFONT hGui = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
                    SendMessageW(hwnd, WM_SETFONT, (WPARAM)hGui, TRUE);

                    form->map[L"text"] = NF([hwnd](vector<Value>&) -> Value {
                        wchar_t buf[512] = L"";
                        GetWindowTextW(hwnd, buf, 512);
                        return Value::Str_(buf); });
                    form->map[L"setText"] = NF([hwnd](vector<Value>& a2) -> Value {
                        SetWindowTextW(hwnd, BStr(a2[0]).c_str());
                        return Value(); });
                    form->map[L"hwnd"] = Value::Num_((double)(INT_PTR)hwnd);
                    form->map[L"show"] = NF([hwnd](vector<Value>&) -> Value {
                        ShowWindow(hwnd, SW_SHOW);
                        UpdateWindow(hwnd);
                        return Value(); });
                    form->map[L"doModal"] = NF([fs, hwnd](vector<Value>&)
                        -> Value {
                        ShowWindow(hwnd, SW_SHOW);
                        UpdateWindow(hwnd);
                        MSG msg;
                        while (!fs->bModalDone && GetMessageW(&msg, nullptr,
                                                               0, 0) > 0)
                        {
                            TranslateMessage(&msg);
                            DispatchMessageW(&msg);
                        }
                        return Value::Num_((double)fs->nModalResult); });
                    form->map[L"endModal"] = NF([fs, hwnd](vector<Value>& a2)
                        -> Value {
                        fs->nModalResult = a2.empty() ? 0 : (int)BNum(a2[0]);
                        fs->bModalDone = true;
                        PostMessageW(hwnd, WM_QUIT, 0, 0);
                        return Value(); });
                    form->map[L"close"] = NF([hwnd](vector<Value>&) -> Value {
                        PostMessageW(hwnd, WM_CLOSE, 0, 0);
                        return Value(); });
                    // round 40: paint trigger + timers (ontimer(tid) member)
                    fs->formVal = Value::Table_(form);
                    form->map[L"invalidate"] =
                        NF([fs, hwnd](vector<Value>&) -> Value {
                        InvalidateRect(hwnd, nullptr, TRUE);
                        if (!fs->bInPaint)
                            UpdateWindow(hwnd);   // synchronous repaint
                        return Value(); });
                    form->map[L"setInterval"] =
                        NF([fs, hwnd](vector<Value>& a2) -> Value {
                        int nId = fs->nNextTimer++;
                        UINT ms = a2.empty() ? 1000 : (UINT)BNum(a2[0]);
                        SetTimer(hwnd, (UINT_PTR)nId, ms, nullptr);
                        return Value::Num_((double)nId); });
                    form->map[L"killTimer"] =
                        NF([fs, hwnd](vector<Value>& a2) -> Value {
                        if (!a2.empty())
                            KillTimer(hwnd, (UINT_PTR)(int)BNum(a2[0]));
                        return Value(); });
                    // round 41: menu bar — addMenu(text) -> popup table;
                    // popup.add(text) -> item table with .oncommand slot
                    form->map[L"addMenu"] =
                        NF([fs, hwnd, form, this](vector<Value>& a2)
                            -> Value {
                        wstring strText = a2.empty() ? L"菜单" : BStr(a2[0]);
                        if (!fs->hMenuBar)
                        {
                            fs->hMenuBar = CreateMenu();
                            SetMenu(hwnd, fs->hMenuBar);
                        }
                        HMENU hPopup = CreatePopupMenu();
                        AppendMenuW(fs->hMenuBar, MF_POPUP,
                            (UINT_PTR)hPopup, strText.c_str());
                        DrawMenuBar(hwnd);
                        auto popup = std::make_shared<Table>();
                        // popup.add(text) -> item table
                        popup->map[L"add"] = NF([fs, hPopup, hwnd, this](
                            vector<Value>& a3) -> Value {
                            wstring s = a3.empty() ? L"" : BStr(a3[0]);
                            int nMid = fs->nNextMenuId++;
                            AppendMenuW(hPopup, MF_STRING,
                                (UINT_PTR)nMid, s.c_str());
                            auto item = std::make_shared<Table>();
                            item->map[L"text"] = NF([s](
                                vector<Value>&) -> Value {
                                return Value::Str_(s); });
                            item->map[L"oncommand"] = Value();
                            item->map[L"click"] = NF([hwnd, nMid](
                                vector<Value>&) -> Value {
                                PostMessageW(hwnd, WM_COMMAND,
                                    MAKEWPARAM(nMid, 0), 0);
                                return Value(); });
                            item->map[L"enable"] = NF([fs, nMid](
                                vector<Value>& a4) -> Value {
                                EnableMenuItem(fs->hMenuBar, nMid,
                                    MF_BYCOMMAND |
                                        (!a4.empty() && BNum(a4[0]) != 0
                                             ? MF_ENABLED
                                             : MF_GRAYED));
                                return Value(); });
                            item->map[L"check"] = NF([fs, nMid](
                                vector<Value>& a4) -> Value {
                                CheckMenuItem(fs->hMenuBar, nMid,
                                    MF_BYCOMMAND |
                                        (!a4.empty() && BNum(a4[0]) != 0
                                             ? MF_CHECKED
                                             : MF_UNCHECKED));
                                return Value(); });
                            fs->menuItems[nMid] = Value::Table_(item);
                            fs->menus[nMid] = NF([fs, nMid, this](
                                vector<Value>&) -> Value {
                                auto ii = fs->menuItems.find(nMid);
                                if (ii == fs->menuItems.end() ||
                                    !ii->second.tab)
                                    return Value();
                                auto ib = ii->second.tab->map.find(
                                    L"oncommand");
                                if (ib == ii->second.tab->map.end())
                                    return Value();
                                vector<Value> noArgs;
                                return this->CallVmFunc(ib->second,
                                                        noArgs); });
                            return Value::Table_(item); });
                        // popup.sep() -> separator
                        popup->map[L"sep"] = NF([hPopup](
                            vector<Value>&) -> Value {
                            AppendMenuW(hPopup, MF_SEPARATOR, 0, nullptr);
                            return Value(); });
                        // popup.addMenu(text) -> nested popup
                        popup->map[L"addMenu"] =
                            NF([fs, hPopup, hwnd, this](
                                vector<Value>& a3) -> Value {
                            wstring s = a3.empty() ? L"" : BStr(a3[0]);
                            HMENU hSub = CreatePopupMenu();
                            AppendMenuW(hPopup, MF_POPUP,
                                (UINT_PTR)hSub, s.c_str());
                            // reuse the same item-building closure shape:
                            // return a table with add/sep bound to hSub
                            auto sub = std::make_shared<Table>();
                            sub->map[L"add"] = NF([fs, hSub, hwnd, this](
                                vector<Value>& a4) -> Value {
                                wstring s2 = a4.empty() ? L"" : BStr(a4[0]);
                                int nMid = fs->nNextMenuId++;
                                AppendMenuW(hSub, MF_STRING,
                                    (UINT_PTR)nMid, s2.c_str());
                                auto item2 = std::make_shared<Table>();
                                item2->map[L"text"] = NF([s2](
                                    vector<Value>&) -> Value {
                                    return Value::Str_(s2); });
                                item2->map[L"oncommand"] = Value();
                                item2->map[L"click"] = NF([hwnd, nMid](
                                    vector<Value>&) -> Value {
                                    PostMessageW(hwnd, WM_COMMAND,
                                        MAKEWPARAM(nMid, 0), 0);
                                    return Value(); });
                                fs->menuItems[nMid] = Value::Table_(item2);
                                fs->menus[nMid] = NF([fs, nMid, this](
                                    vector<Value>&) -> Value {
                                    auto ii = fs->menuItems.find(nMid);
                                    if (ii == fs->menuItems.end() ||
                                        !ii->second.tab)
                                        return Value();
                                    auto ib = ii->second.tab->map.find(
                                        L"oncommand");
                                    if (ib == ii->second.tab->map.end())
                                        return Value();
                                    vector<Value> noArgs;
                                    return this->CallVmFunc(ib->second,
                                                            noArgs); });
                                return Value::Table_(item2); });
                            sub->map[L"sep"] = NF([hSub](
                                vector<Value>&) -> Value {
                                AppendMenuW(hSub, MF_SEPARATOR, 0, nullptr);
                                return Value(); });
                            return Value::Table_(sub); });
                        return Value::Table_(popup); });
                    form->map[L"add"] = NF([form, fs, hwnd, this, hGui](
                        vector<Value>& a2) -> Value {
                    // aardio: winform.add(name={cls="button";text=..;left=..},
                    // name2={...}) — the named-argument syntax collapses all
                    // of them into ONE table whose keys are the control
                    // names and whose values are the init tables. Accept
                    // that shape, plus the legacy positional form
                    // (name, cls, text, x, y, w, h).
                    struct CtrlInit { wstring strName; Value vInit; };
                    vector<CtrlInit> vCtls;
                    if (a2.size() == 1 && a2[0].t == Value::TableT &&
                        a2[0].tab && !a2[0].tab->map.empty())
                    {
                        for (auto& kv : a2[0].tab->map)
                        {
                            if (kv.second.t != Value::TableT)
                            {
                                (*err) = L"运行错误: form.add 控件参数表无效: "
                                         + kv.first;
                                return Value();
                            }
                            vCtls.push_back({ kv.first, kv.second });
                        }
                    }
                    else if (a2.size() >= 7)
                    {
                        // legacy positional form: name, cls, text, x, y, w, h
                        // — synthesize the init table so one code path
                        // handles both shapes.
                        auto tb = std::make_shared<Table>();
                        tb->map[L"cls"] = Value::Str_(BStr(a2[1]));
                        tb->map[L"text"] = Value::Str_(BStr(a2[2]));
                        tb->map[L"left"] = Value::Num_(BNum(a2[3]));
                        tb->map[L"top"] = Value::Num_(BNum(a2[4]));
                        tb->map[L"width"] = Value::Num_(BNum(a2[5]));
                        tb->map[L"height"] = Value::Num_(BNum(a2[6]));
                        vCtls.push_back({ BStr(a2[0]), Value::Table_(tb) });
                    }
                    else
                    {
                        (*err) = L"运行错误: form.add(name={cls;text;left;top;"
                                 L"right;bottom}, ...)";
                        return Value();
                    }
                    // map iteration order is unordered; keep source order
                    // stable by sorting nothing (names are unique keys)
                    for (auto& ci : vCtls)
                    {
                        wstring strName = ci.strName;
                        auto itCls = ci.vInit.tab
                            ? ci.vInit.tab->map.find(L"cls") : ci.vInit.tab->map.end();
                        wstring strCls = itCls != ci.vInit.tab->map.end() &&
                                         itCls->second.t == Value::Str
                            ? BStr(itCls->second) : L"static";
                        auto GetI = [&](const wchar_t* k, double dDef) -> double {
                            if (!ci.vInit.tab) return dDef;
                            auto it = ci.vInit.tab->map.find(k);
                            return it != ci.vInit.tab->map.end() &&
                                   it->second.t == Value::Num
                                ? it->second.n : dDef; };
                        auto GetS = [&](const wchar_t* k) -> wstring {
                            if (!ci.vInit.tab) return L"";
                            auto it = ci.vInit.tab->map.find(k);
                            return it != ci.vInit.tab->map.end() &&
                                   it->second.t == Value::Str
                                ? BStr(it->second) : L""; };
                        // aardio geometry: left/top/right/bottom in client
                        // coordinates; width/height are derived from them
                        double dL = GetI(L"left", 0), dT = GetI(L"top", 0);
                        double dR = GetI(L"right", 0), dB = GetI(L"bottom", 0);
                        double dW = GetI(L"width", 0), dH = GetI(L"height", 0);
                        if (dW == 0 && dR > 0) dW = dR - dL;
                        if (dH == 0 && dB > 0) dH = dB - dT;
                        const wchar_t* szCls = nullptr;
                        DWORD dwCtlStyle = 0;
                        if (strCls == L"button") szCls = L"BUTTON";
                        else if (strCls == L"edit") szCls = L"EDIT";
                        else if (strCls == L"static") szCls = L"STATIC";
                        else if (strCls == L"checkbox")
                        {
                            szCls = L"BUTTON";
                            dwCtlStyle = BS_AUTOCHECKBOX;
                        }
                        else if (strCls == L"radio")
                        {
                            szCls = L"BUTTON";
                            dwCtlStyle = BS_AUTORADIOBUTTON;
                        }
                        else if (strCls == L"groupbox")
                        {
                            szCls = L"BUTTON";
                            dwCtlStyle = BS_GROUPBOX;
                        }
                        else if (strCls == L"listbox")
                        {
                            szCls = L"LISTBOX";
                            dwCtlStyle = LBS_NOTIFY |
                                LBS_NOINTEGRALHEIGHT |
                                WS_VSCROLL | WS_BORDER;
                        }
                        else if (strCls == L"combobox")
                        {
                            szCls = L"COMBOBOX";
                            dwCtlStyle = CBS_DROPDOWNLIST |
                                WS_VSCROLL;
                        }
                        if (!szCls)
                        {
                            (*err) = L"运行错误: 未知控件类型: " + strCls;
                            return Value();
                        }
                        int nId = fs->nNextId++;
                        HWND hC = CreateWindowExW(0, szCls,
                            GetS(L"text").c_str(),
                            WS_CHILD | WS_VISIBLE | dwCtlStyle |
                                (strCls == L"button" ? BS_PUSHBUTTON :
                                 strCls == L"edit" ?
                                     ES_LEFT | WS_BORDER : 0),
                            (int)dL, (int)dT, (int)dW, (int)dH,
                            hwnd, (HMENU)(INT_PTR)nId,
                            GetModuleHandle(nullptr), nullptr);
                        if (!hC)
                        {
                            (*err) = L"运行错误: 控件创建失败";
                            return Value();
                        }
                        SendMessageW(hC, WM_SETFONT, (WPARAM)hGui, TRUE);
                        fs->hCtrl[nId] = hC;
                        auto ctrl = std::make_shared<Table>();
                        if (strCls == L"checkbox" || strCls == L"radio")
                        {
                            ctrl->map[L"checked"] = NF([hC](vector<Value>&)
                                -> Value {
                                return Value::Bool_(
                                    SendMessageW(hC, BM_GETCHECK, 0, 0) ==
                                    BST_CHECKED); });
                            ctrl->map[L"setChecked"] =
                                NF([hC](vector<Value>& a3) -> Value {
                                SendMessageW(hC, BM_SETCHECK,
                                    a3.empty() || BNum(a3[0]) == 0
                                        ? BST_UNCHECKED
                                        : BST_CHECKED, 0);
                                return Value(); });
                        }
                        // round 41: listbox/combobox — same method surface,
                        // different message numbers (LB_* vs CB_*)
                        if (strCls == L"listbox" || strCls == L"combobox")
                        {
                            bool bCombo = strCls == L"combobox";
                            UINT mAdd = bCombo ? CB_ADDSTRING : LB_ADDSTRING;
                            UINT mCnt = bCombo ? CB_GETCOUNT : LB_GETCOUNT;
                            UINT mCur = bCombo ? CB_GETCURSEL : LB_GETCURSEL;
                            UINT mSet = bCombo ? CB_SETCURSEL : LB_SETCURSEL;
                            UINT mTxt = bCombo ? CB_GETLBTEXT : LB_GETTEXT;
                            UINT mLen = bCombo ? CB_GETLBTEXTLEN
                                               : LB_GETTEXTLEN;
                            UINT mRst = bCombo ? CB_RESETCONTENT
                                               : LB_RESETCONTENT;
                            UINT mDel = bCombo ? CB_DELETESTRING
                                               : LB_DELETESTRING;
                            auto getitem = [hC, mTxt, mLen](int i1)
                                -> wstring {
                                int n = SendMessageW(hC, mLen, i1 - 1, 0);
                                if (n < 0) return L"";
                                std::vector<wchar_t> buf((size_t)n + 1, 0);
                                SendMessageW(hC, mTxt, i1 - 1,
                                             (LPARAM)buf.data());
                                return buf.data(); };
                            ctrl->map[L"add"] = NF([hC, mAdd, mCnt](
                                vector<Value>& a3) -> Value {
                                wstring s = a3.empty() ? L"" : BStr(a3[0]);
                                SendMessageW(hC, mAdd, 0,
                                             (LPARAM)s.c_str());
                                int c = (int)SendMessageW(hC, mCnt, 0, 0);
                                return Value::Num_((double)c); });
                            ctrl->map[L"count"] = NF([hC, mCnt](
                                vector<Value>&) -> Value {
                                return Value::Num_(
                                    (double)SendMessageW(hC, mCnt, 0, 0)); });
                            ctrl->map[L"getText"] = NF([getitem](
                                vector<Value>& a3) -> Value {
                                return Value::Str_(
                                    (int)BNum(a3[0]) >= 1
                                        ? getitem((int)BNum(a3[0]))
                                        : L""); });
                            ctrl->map[L"sel"] = NF([hC, mCur](
                                vector<Value>&) -> Value {
                                int i = (int)SendMessageW(hC, mCur, 0, 0);
                                return Value::Num_((double)(i < 0 ? 0
                                                                  : i + 1)); });
                            ctrl->map[L"selText"] = NF([hC, mCur, getitem](
                                vector<Value>&) -> Value {
                                int i = (int)SendMessageW(hC, mCur, 0, 0);
                                return Value::Str_(i < 0 ? L""
                                                         : getitem(i + 1)); });
                            ctrl->map[L"setSel"] = NF([hC, mSet](
                                vector<Value>& a3) -> Value {
                                SendMessageW(hC, mSet,
                                    a3.empty() ? -1
                                               : (WPARAM)(int)BNum(a3[0]) - 1,
                                    0);
                                return Value(); });
                            ctrl->map[L"clear"] = NF([hC, mRst](
                                vector<Value>&) -> Value {
                                SendMessageW(hC, mRst, 0, 0);
                                return Value(); });
                            ctrl->map[L"del"] = NF([hC, mDel](
                                vector<Value>& a3) -> Value {
                                SendMessageW(hC, mDel,
                                    (WPARAM)(int)BNum(a3[0]) - 1, 0);
                                return Value(); });
                            // force the selection-change notification the
                            // control would send on real user interaction
                            // (LB/CB_SETCURSEL deliberately do not)
                            ctrl->map[L"notifySel"] =
                                NF([fs, nId, hwnd, hC](
                                    vector<Value>&) -> Value {
                                PostMessageW(hwnd, WM_COMMAND,
                                    MAKEWPARAM(nId, LBN_SELCHANGE),
                                    (LPARAM)hC);
                                return Value(); });
                        }
                        // every aardio control exposes its HWND
                        ctrl->map[L"hwnd"] = Value::Num_(
                            (double)(INT_PTR)hC);
                        ctrl->map[L"text"] = NF([hC](vector<Value>&)
                            -> Value {
                            wchar_t buf[512] = L"";
                            GetWindowTextW(hC, buf, 512);
                            return Value::Str_(buf); });
                        ctrl->map[L"setText"] = NF([hC](vector<Value>& a3)
                            -> Value {
                            SetWindowTextW(hC, BStr(a3[0]).c_str());
                            return Value(); });
                        ctrl->map[L"click"] = NF([nId, hwnd, hC](
                            vector<Value>&) -> Value {
                            PostMessageW(hwnd, WM_COMMAND,
                                MAKEWPARAM(nId, BN_CLICKED),
                                (LPARAM)hC);
                            return Value(); });
                        // callback slot: assign a function to .oncommand
                        ctrl->map[L"oncommand"] = Value();   // set by script
                        // the dispatch reads ctrl->map["oncommand"] live
                        fs->ctrls[nId] = Value::Table_(ctrl);
                        fs->callbacks[nId] = Value();       // filled lazily
                        // hook: keep the id in the value so dispatch can
                        // resolve; store id on the control table
                        ctrl->map[L"_id"] = Value::Num_((double)nId);
                        fs->callbacks[nId] = NF([fs, nId, this](
                            vector<Value>&) -> Value {
                            auto ic = fs->ctrls.find(nId);
                            if (ic == fs->ctrls.end() || !ic->second.tab)
                                return Value();
                            auto ib = ic->second.tab->map.find(L"oncommand");
                            if (ib == ic->second.tab->map.end())
                                return Value();
                            vector<Value> noArgs;
                            return this->CallVmFunc(ib->second, noArgs); });
                        fs->changes[nId] = NF([fs, nId, this](
                            vector<Value>&) -> Value {
                            auto ic = fs->ctrls.find(nId);
                            if (ic == fs->ctrls.end() || !ic->second.tab)
                                return Value();
                            auto ib = ic->second.tab->map.find(L"onchange");
                            if (ib == ic->second.tab->map.end())
                                return Value();
                            vector<Value> noArgs;
                            return this->CallVmFunc(ib->second, noArgs); });
                        fs->selchgs[nId] = NF([fs, nId, this](
                            vector<Value>&) -> Value {
                            auto ic = fs->ctrls.find(nId);
                            if (ic == fs->ctrls.end() || !ic->second.tab)
                                return Value();
                            auto ib = ic->second.tab->map.find(L"onselchange");
                            if (ib == ic->second.tab->map.end())
                                return Value();
                            vector<Value> noArgs;
                            return this->CallVmFunc(ib->second, noArgs); });
                        form->map[strName] = Value::Table_(ctrl);
                    }
                    // aardio returns the LAST added control
                    Value vLast;
                    if (!vCtls.empty() && form->map.size())
                    {
                        auto itL = form->map.find(vCtls.back().strName);
                        if (itL != form->map.end()) vLast = itL->second;
                    }
                    return vLast; });
                    return Value::Table_(form); });
            }

            // first-class scalar builtins (also reachable via fast paths)
            globals.vars[L"tostring"] = NF([](vector<Value>& a) {
                return Value::Str_(a.empty() ? L"null" : a[0].ToString()); });
            Value vType = NF([](vector<Value>& a) -> Value {
                if (a.empty()) return Value::Str_(L"null");
                switch (a[0].t)
                {
                case Value::Null: return Value::Str_(L"null");
                case Value::Bool: return Value::Str_(L"boolean");
                case Value::Num: return Value::Str_(L"number");
                case Value::Pointer: return Value::Str_(L"pointer");
                case Value::Str: return Value::Str_(L"string");
                case Value::TableT: return Value::Str_(L"table");
                case Value::Class: return Value::Str_(L"class");
                case Value::Func:
                    return Value::Str_(a[0].native ? L"native function"
                                                   : L"function");
                }
                return Value::Str_(L"null"); });
            vType.tab = std::make_shared<Table>();
            vType.tab->map[L"null"] = Value::Str_(L"null");
            vType.tab->map[L"boolean"] = Value::Str_(L"boolean");
            vType.tab->map[L"number"] = Value::Str_(L"number");
            vType.tab->map[L"string"] = Value::Str_(L"string");
            vType.tab->map[L"table"] = Value::Str_(L"table");
            vType.tab->map[L"function"] = Value::Str_(L"function");
            vType.tab->map[L"cdata"] = Value::Str_(L"cdata");
            vType.tab->map[L"pointer"] = Value::Str_(L"pointer");
            vType.tab->map[L"isString"] = NF([](vector<Value>& a) -> Value {
                return Value::Bool_(!a.empty() && a[0].t == Value::Str);
            });
            vType.tab->map[L"isNumber"] = NF([](vector<Value>& a) -> Value {
                return Value::Bool_(!a.empty() && a[0].t == Value::Num);
            });
            vType.tab->map[L"isTable"] = NF([](vector<Value>& a) -> Value {
                return Value::Bool_(!a.empty() && a[0].t == Value::TableT);
            });
            vType.tab->map[L"isFunction"] = NF([](vector<Value>& a) -> Value {
                return Value::Bool_(!a.empty() && (a[0].t == Value::Func || a[0].native != nullptr));
            });
            vType.tab->map[L"isPointer"] = NF([](vector<Value>& a) -> Value {
                return Value::Bool_(!a.empty() && a[0].t == Value::Pointer);
            });
            vType.tab->map[L"isClass"] = NF([](vector<Value>& a) -> Value {
                return Value::Bool_(!a.empty() && a[0].t == Value::Class);
            });
            globals.vars[L"type"] = vType;
            roots[L"type"] = vType;
            globals.vars[L"topointer"] = NF([](vector<Value>& a) -> Value {
                if (a.empty() || a[0].t == Value::Null) return Value::Pointer_(nullptr);
                if (a[0].t == Value::Pointer) return a[0];
                if (a[0].ptr != nullptr) return Value::Pointer_(a[0].ptr);
                if (a[0].t == Value::Num) return Value::Pointer_((void*)(INT_PTR)(long long)a[0].n);
                if (a[0].t == Value::Str) return Value::Pointer_((void*)a[0].s.c_str());
                return Value::Pointer_((void*)(INT_PTR)(long long)BNum(a[0])); });
            globals.vars[L"tonumber"] = NF([](vector<Value>& a) {
                return a.empty() ? Value() : Value::Num_(BNum(a[0])); });
            globals.vars[L"sleep"] = NF([](vector<Value>& a) {
                if (!a.empty()) Sleep((DWORD)BNum(a[0]));
                return Value(); });
            globals.vars[L"_HINSTANCE"] = Value::Pointer_(GetModuleHandleW(nullptr));
            globals.vars[L"_HAPPICON"] = Value::Pointer_(LoadIconW(nullptr, IDI_APPLICATION));
            globals.vars[L"_INVALID_HANDLE_VALUE"] = Value::Pointer_(INVALID_HANDLE_VALUE);

            // ---- documented reserved builtins (docs/language-reference/
            //      builtin-function/index.md). Their absence was the single
            //      largest cause of "尝试调用非函数值" in the shipped examples.
            auto MakeVmFunc = [](PNode fn) -> Value {
                Value f;
                f.t = Value::Func;
                f.fn = fn->a;
                f.params = fn->names;
                f.pdefaults = std::make_shared<vector<PNode>>(fn->defaults);
                return f;
            };
            // print(...) — each argument through tostring, then io.print
            globals.vars[L"print"] = NF([](vector<Value>& a) -> Value {
                wstring out;
                for (size_t i = 0; i < a.size(); i++)
                {
                    if (i) out += L"\t";
                    out += a[i].t == Value::Null ? L"null" : a[i].ToString();
                }
                out += L"\n";
                int n = WideCharToMultiByte(CP_UTF8, 0, out.c_str(),
                                            (int)out.size(), nullptr, 0,
                                            nullptr, nullptr);
                if (n > 0)
                {
                    std::string u8((size_t)n, 0);
                    WideCharToMultiByte(CP_UTF8, 0, out.c_str(),
                                        (int)out.size(), &u8[0], n,
                                        nullptr, nullptr);
                    fwrite(u8.data(), 1, u8.size(), stdout);
                    fflush(stdout);
                }
                return Value(); });
            // assert(value,message,...) — truthy: return all arguments;
            // falsy: throw @message
            globals.vars[L"assert"] = NF([this](vector<Value>& a) -> Value {
                if (!a.empty() && a[0].Truthy())
                {
                    if (a.size() > 1)
                    {
                        auto ex = std::make_shared<vector<Value>>();
                        for (size_t i = 1; i < a.size(); i++)
                            ex->push_back(a[i]);
                        Value r = a.size() > 1 ? a[1] : a[0];
                        r.mrets = ex;
                        return r;
                    }
                    return a.empty() ? Value() : a[0];
                }
                wstring msg = a.size() > 1 ? BStr(a[1])
                                          : L"assertion failed!";
                (*err) = L"运行错误: " + msg;
                return Value(); });
            // assert2(level,value,message,...)
            globals.vars[L"assert2"] = NF([this](vector<Value>& a) -> Value {
                size_t base = a.empty() ? 0 : 1;   // skip the level argument
                if (a.size() > base && a[base].Truthy())
                {
                    if (a.size() > base + 1)
                    {
                        auto ex = std::make_shared<vector<Value>>();
                        for (size_t i = base + 1; i < a.size(); i++)
                            ex->push_back(a[i]);
                        Value r = a[base + 1];
                        r.mrets = ex;
                        return r;
                    }
                    return a[base];
                }
                wstring msg = a.size() > base + 1 ? BStr(a[base + 1])
                                                  : L"assertion failed!";
                (*err) = L"运行错误: " + msg;
                return Value(); });
            // assertf — inverse assertion: throws when the value is truthy
            globals.vars[L"assertf"] = NF([this](vector<Value>& a) -> Value {
                if (!a.empty() && a[0].Truthy())
                {
                    wstring msg = a.size() > 1 ? BStr(a[1])
                                              : L"assertion failed!";
                    (*err) = L"运行错误: " + msg;
                    return Value();
                }
                return a.empty() ? Value() : a[0]; });
            // eval(codeString) — compiles and runs an expression immediately
            globals.vars[L"eval"] = NF([this, MakeVmFunc](vector<Value>& a) -> Value {
                if (a.empty() || a[0].t != Value::Str)
                    return Value();
                wstring src = BStr(a[0]);
                wstring e;
                PNode fn = CompileToFunc(L"return " + src, &e);
                if (!fn)
                {
                    e.clear();
                    fn = CompileToFunc(src, &e);
                    if (!fn)
                    {
                        (*err) = L"运行错误: eval: " + e;
                        return Value();
                    }
                }
                Value f = MakeVmFunc(fn);
                vector<Value> args;
                Value r = this->CallVmFunc(f, args);
                if (!err->empty())
                    return Value();
                return r; });
            // loadcode(code|filepath) — a function object, or null + message
            globals.vars[L"loadcode"] = NF([this, MakeVmFunc](vector<Value>& a) -> Value {
                if (a.empty() || a[0].t != Value::Str)
                    return Value();
                wstring src = BStr(a[0]), text;
                if (ReadCodeFile(src, &text))
                    src = text;
                wstring e;
                PNode fn = CompileToFunc(src, &e);
                if (!fn)
                {
                    // null + error message, exactly as the docs specify
                    Value nullV;
                    auto ex = std::make_shared<vector<Value>>();
                    ex->push_back(Value::Str_(L"编译失败: " + e));
                    nullV.mrets = ex;
                    return nullV;
                }
                return MakeVmFunc(fn); });
            // loadcodex(code|filepath|function,...) — run now, return values
            globals.vars[L"loadcodex"] = NF([this, MakeVmFunc](vector<Value>& a) -> Value {
                if (a.empty())
                    return Value();
                Value f;
                if (a[0].t == Value::Func)
                    f = a[0];
                else if (a[0].t == Value::Str)
                {
                    wstring src = BStr(a[0]), text;
                    if (ReadCodeFile(src, &text))
                        src = text;
                    wstring e;
                    PNode fn = CompileToFunc(src, &e);
                    if (!fn)
                    {
                        (*err) = L"运行错误: loadcodex: " + e;
                        return Value();
                    }
                    f = MakeVmFunc(fn);
                }
                else
                    return Value();
                vector<Value> args;
                for (size_t i = 1; i < a.size(); i++)
                    args.push_back(a[i]);
                return this->CallVmFunc(f, args); });
            // reduce(array,reducer,initialValue)
            globals.vars[L"reduce"] = NF([this](vector<Value>& a) -> Value {
                if (a.size() < 2 || a[0].t != Value::TableT || !a[0].tab)
                    return Value();
                auto& arr = a[0].tab->arr;
                bool bInit = (a.size() > 2 && a[2].t != Value::Null);
                Value prev;
                size_t start = 1;
                if (bInit)
                    prev = a[2];
                else
                {
                    if (arr.empty())
                    {
                        (*err) = L"运行错误: reduce 空数组且未指定初始值";
                        return Value();
                    }
                    prev = arr[0];
                    start = 2;
                }
                for (size_t i = start; i <= arr.size(); i++)
                {
                    vector<Value> args;
                    args.push_back(prev);
                    args.push_back(arr[i - 1]);
                    args.push_back(Value::Num_((double)i));
                    args.push_back(a[0]);
                    prev = this->CallVmFunc(a[1], args);
                    if (!err->empty())
                        return Value();
                }
                return prev; });
            // switch(switchValue,caseTable,default,...)
            globals.vars[L"switch"] = NF([this](vector<Value>& a) -> Value {
                if (a.size() < 2 || a[1].t != Value::TableT || !a[1].tab)
                {
                    (*err) = L"运行错误: switch 参数@2必须是表对象";
                    return Value();
                }
                Value fn;
                if (a[0].t == Value::Num)
                {
                    double n = a[0].n;
                    if (n >= 1 && (size_t)n <= a[1].tab->arr.size())
                        fn = a[1].tab->arr[(size_t)n - 1];
                }
                if (fn.t == Value::Null)
                {
                    auto it = a[1].tab->map.find(a[0].ToString());
                    if (it != a[1].tab->map.end())
                        fn = it->second;
                }
                if (fn.t != Value::Func)
                    fn = (a.size() > 2) ? a[2] : Value();
                if (fn.t != Value::Func)
                    return Value();
                vector<Value> args;
                for (size_t i = 3; i < a.size(); i++)
                    args.push_back(a[i]);
                return this->CallVmFunc(fn, args); });
            // invoke(f,owner,...) — call with an explicit owner, errors throw
            globals.vars[L"invoke"] = NF([this](vector<Value>& a) -> Value {
                if (a.empty())
                    return Value();
                Value owner = a.size() > 1 ? a[1] : Value();
                vector<Value> args;
                for (size_t i = 2; i < a.size(); i++)
                    args.push_back(a[i]);
                return this->CallOwner(a[0], owner, args); });
            // callex(f,owner,errHandler,...) — errors go to the handler
            globals.vars[L"callex"] = NF([this](vector<Value>& a) -> Value {
                if (a.empty())
                    return Value();
                Value owner = a.size() > 1 ? a[1] : Value();
                Value handler = a.size() > 2 ? a[2] : Value();
                vector<Value> args;
                for (size_t i = 3; i < a.size(); i++)
                    args.push_back(a[i]);
                wstring keep = *err;
                err->clear();
                Value r = this->CallOwner(a[0], owner, args);
                if (!err->empty())
                {
                    wstring msg = *err;
                    err->clear();
                    if (handler.t == Value::Func)
                    {
                        vector<Value> ha;
                        ha.push_back(Value::Str_(msg));
                        r = this->CallVmFunc(handler, ha);
                    }
                    else
                        r = Value();
                }
                if (keep.empty() == false && err->empty())
                    *err = keep;
                return r; });
            // rget(n,...) — pick the n-th of the trailing values
            globals.vars[L"rget"] = NF([](vector<Value>& a) -> Value {
                if (a.size() < 2 || a[0].t != Value::Num)
                    return a.empty() ? Value() : a[0];
                size_t n = (size_t)a[0].n;
                if (n < 1 || n >= a.size())
                    return Value();
                return a[n]; });
            // execute(commandLine) — run a system command and wait
            globals.vars[L"execute"] = NF([](vector<Value>& a) -> Value {
                if (a.empty() || a[0].t != Value::Str)
                    return Value();
                int rc = _wsystem(BStr(a[0]).c_str());
                return Value::Num_((double)rc); });
            // errput(message) — report without throwing
            globals.vars[L"errput"] = NF([](vector<Value>& a) -> Value {
                wstring out;
                for (size_t i = 0; i < a.size(); i++)
                {
                    if (i) out += L"\t";
                    out += a[i].t == Value::Null ? L"null" : a[i].ToString();
                }
                out += L"\n";
                int n = WideCharToMultiByte(CP_UTF8, 0, out.c_str(),
                                            (int)out.size(), nullptr, 0,
                                            nullptr, nullptr);
                if (n > 0)
                {
                    std::string u8((size_t)n, 0);
                    WideCharToMultiByte(CP_UTF8, 0, out.c_str(),
                                        (int)out.size(), &u8[0], n,
                                        nullptr, nullptr);
                    fwrite(u8.data(), 1, u8.size(), stderr);
                    fflush(stderr);
                }
                return Value(); });
            globals.vars[L"collectgarbage"] = NF([](vector<Value>&) -> Value {
                return Value(); });
            globals.vars[L"setlocale"] = NF([](vector<Value>& a) -> Value {
                int cat = LC_ALL;
                if (a.size() > 1 && a[0].t == Value::Num)
                    cat = (int)a[0].n;
                const wchar_t* loc = L"";
                if (!a.empty() && a.back().t == Value::Str)
                    loc = BStr(a.back()).c_str();
                wchar_t buf[256] = L"";
                _wsetlocale(cat, loc);
                if (wcscpy_s(buf, loc ? loc : L"") == 0)
                    return Value::Str_(buf);
                return Value(); });
            // setprivilege(name,enable) — enable/disable a process privilege
            globals.vars[L"setprivilege"] = NF([](vector<Value>& a) -> Value {
                if (a.empty() || a[0].t != Value::Str)
                    return Value::Bool_(false);
                bool bEnable = !(a.size() > 1 && !a[1].Truthy());
                HANDLE hTok = nullptr;
                if (!OpenProcessToken(GetCurrentProcess(),
                                      TOKEN_ADJUST_PRIVILEGES |
                                      TOKEN_QUERY, &hTok))
                    return Value::Bool_(false);
                LUID luid;
                if (!LookupPrivilegeValueW(nullptr, BStr(a[0]).c_str(),
                                           &luid))
                {
                    CloseHandle(hTok);
                    return Value::Bool_(false);
                }
                TOKEN_PRIVILEGES tp;
                tp.PrivilegeCount = 1;
                tp.Privileges[0].Luid = luid;
                tp.Privileges[0].Attributes =
                    bEnable ? SE_PRIVILEGE_ENABLED : 0;
                BOOL ok = AdjustTokenPrivileges(hTok, FALSE, &tp,
                                                sizeof(tp), nullptr, nullptr);
                DWORD e = GetLastError();
                CloseHandle(hTok);
                return Value::Bool_(ok && e == ERROR_SUCCESS); });
            // dumpcode — no bytecode emitter yet
            globals.vars[L"dumpcode"] = NF([](vector<Value>&) -> Value {
                return Value(); });
            // Preload builtin (official aardio VM preloads builtin on startup)
            {
                PNode nImp = std::make_shared<Node>();
                nImp->k = Node::Import;
                nImp->names = { L"builtin" };
                DoImport(nImp, &globals);
                if (err && !err->empty())
                {
                    err->clear();
                }
            }
        }
}
