// lexer.cpp — tokens, keywords and the lexer.
#include "engine/common.h"

namespace NodioVM
{
    bool IsIdentChar(wchar_t c)
    {
        return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
               (c >= L'0' && c <= L'9') || c == L'_' || c == L'$' || c > 0x7F;
    }

    vector<Tok> Lex(const wstring& src, wstring* strError)
    {
        vector<Tok> out;
        size_t i = 0, n = src.size();
        int line = 1;
        while (i < n)
        {
            wchar_t c = src[i];
            if (c == 0xFEFF) { i++; continue; }
            if (c == L'\r') { i++; continue; }
            if (c == L'\n') { out.push_back({ TNL, L"", 0, line++ }); i++; continue; }
            if (c == L' ' || c == L'\t') { i++; continue; }
            // comments
            if (c == L'/' && i + 1 < n && src[i + 1] == L'/')
            {
                while (i < n && src[i] != L'\n') i++;
                continue;
            }
            if (c == L'/' && i + 1 < n && src[i + 1] == L'*')
            {
                i += 2;
                // '/**intellisense()' doc blocks (shipped lib convention):
                // bodies contain '*/' (e.g. __/*param*/), so they end only
                // at the literal terminator 'end intellisense' + '**/'
                // '/**intellisense(...)' (two stars) = doc block ending at
                // 'end intellisense'; '/*intellisense(::)*/' = normal block
                size_t iStar = i;
                while (iStar < n && src[iStar] == L'*') iStar++;
                // '/**word(...)' doc blocks (intellisense/details/...)
                // contain plain '*/' inside; they end at 'end word**/'
                bool bDoc = (iStar > i && iStar < n &&
                             src[iStar] >= L'a' && src[iStar] <= L'z');
                if (bDoc)
                {
                    size_t wLen = 0;
                    while (iStar + wLen < n &&
                           ((src[iStar + wLen] >= L'a' &&
                             src[iStar + wLen] <= L'z') ||
                            src[iStar + wLen] == L'_')) wLen++;
                    std::wstring word = src.substr(iStar, wLen);
                    std::wstring endMark = L"end " + word;
                    while (i + (int)endMark.size() < n)
                    {
                        if (src[i] == L'\n') line++;
                        if (src[i] == L'e' &&
                            src.compare(i, (int)endMark.size(),
                                        endMark.c_str(),
                                        (int)endMark.size()) == 0)
                            break;
                        i++;
                    }
                    while (i + 1 < n && !(src[i] == L'*' && src[i + 1] == L'/'))
                    { if (src[i] == L'\n') line++; i++; }
                }
                else
                {
                    while (i + 1 < n && !(src[i] == L'*' && src[i + 1] == L'/'))
                    { if (src[i] == L'\n') line++; i++; }
                }
                i += 2;
                continue;
            }
            // strings
            if (c == L'"' || c == L'`')
            {
                // aardio raw string: 双引号 = 原样字符串，不处理转义符
                // (original docs: "\" is a literal backslash; `...` same)
                wchar_t q = c;
                i++;
                wstring s;
                while (i < n && src[i] != q)
                {
                    if (src[i] == L'\n') line++;
                    s += src[i++];
                }
                i++;  // closing quote
                if (i < n && src[i] == L'#')
                {
                    i++;
                    std::string u8;
                    int len = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
                    if (len > 0)
                    {
                        u8.resize(len);
                        WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &u8[0], len, nullptr, nullptr);
                    }
                    double val = 0;
                    for (size_t k = 0; k < u8.size() && k < 4; k++)
                        val += (double)(((unsigned int)(unsigned char)u8[k]) << (k * 8));
                    out.push_back({ TNum, std::to_wstring((long long)val), val, line });
                    continue;
                }
                out.push_back({ TStr, s, 0, line });
                continue;
            }
            if (c == L'\'')
            {
                // aardio escaped string: 单引号 = 编译时转义字符串,
                // C-like \ escapes ('\\' == "\"" per the original docs)
                wchar_t q = c;
                i++;
                wstring s;
                while (i < n && src[i] != q)
                {
                    if (src[i] == L'\\' && i + 1 < n)
                    {
                        wchar_t e = src[++i];
                        switch (e)
                        {
                        case L'n': s += L'\n'; break;
                        case L'r': s += L'\r'; break;
                        case L't': s += L'\t'; break;
                        case L'0': s += L'\0'; break;
                        case L'\\': s += L'\\'; break;
                        case L'"': s += L'"'; break;
                        case L'\'': s += L'\''; break;
                        default:
                            if (e == L'x' && i + 2 < n)
                            {
                                s += (wchar_t)wcstoul(src.substr(i + 1, 2).c_str(), nullptr, 16);
                                i += 2;
                            }
                            else
                            {
                                // unknown escape: keep verbatim (patterns
                                // rely on '\d'/'\w' surviving the literal)
                                s += L'\\';
                                s += e;
                            }
                        }
                        i++;
                    }
                    else
                    {
                        if (src[i] == L'\n') line++;
                        s += src[i++];
                    }
                }
                i++;  // closing quote
                if (i < n && src[i] == L'#')
                {
                    i++;
                    std::string u8;
                    int len = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
                    if (len > 0)
                    {
                        u8.resize(len);
                        WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &u8[0], len, nullptr, nullptr);
                    }
                    double val = 0;
                    for (size_t k = 0; k < u8.size() && k < 4; k++)
                        val += (double)(((unsigned int)(unsigned char)u8[k]) << (k * 8));
                    out.push_back({ TNum, std::to_wstring((long long)val), val, line });
                    continue;
                }
                out.push_back({ TStr, s, 0, line });
                continue;
            }
            // numbers
            if ((c >= L'0' && c <= L'9') ||
                (c == L'.' && i + 1 < n && src[i + 1] >= L'0' && src[i + 1] <= L'9'))
            {
                size_t b = i;
                if (c == L'0' && i + 1 < n && (src[i + 1] == L'x' || src[i + 1] == L'X'))
                {
                    i += 2;
                    while (i < n && iswxdigit(src[i])) i++;
                    Tok t{ TNum, src.substr(b, i - b), 0, line };
                    t.num = (double)wcstoul(src.substr(b + 2, i - b - 2).c_str(), nullptr, 16);
                    out.push_back(t);
                    continue;
                }
                // aardio radix literal: <radix>#<digits> (e.g. 2#100, 8#77, 16#FF, 36#Z)
                size_t digEnd = i;
                while (digEnd < n && src[digEnd] >= L'0' && src[digEnd] <= L'9') digEnd++;
                if (digEnd < n && src[digEnd] == L'#' && digEnd + 1 < n && iswalnum(src[digEnd + 1]))
                {
                    int radix = _wtoi(src.substr(b, digEnd - b).c_str());
                    if (radix >= 2 && radix <= 36)
                    {
                        i = digEnd + 1;
                        size_t valStart = i;
                        auto isValidRadix = [radix](wchar_t ch) {
                            if (ch >= L'0' && ch <= L'9') return (ch - L'0') < radix;
                            if (ch >= L'a' && ch <= L'z') return (ch - L'a' + 10) < radix;
                            if (ch >= L'A' && ch <= L'Z') return (ch - L'A' + 10) < radix;
                            return false;
                        };
                        while (i < n && isValidRadix(src[i])) i++;
                        wstring numStr = src.substr(valStart, i - valStart);
                        wchar_t* endp = nullptr;
                        double val = (double)_wcstoui64(numStr.c_str(), &endp, radix);
                        Tok t{ TNum, src.substr(b, i - b), val, line };
                        out.push_back(t);
                        continue;
                    }
                }
                while (i < n && ((src[i] >= L'0' && src[i] <= L'9') || src[i] == L'.')) i++;
                if (i < n && (src[i] == L'e' || src[i] == L'E'))
                {
                    i++;
                    if (i < n && (src[i] == L'+' || src[i] == L'-')) i++;
                    while (i < n && src[i] >= L'0' && src[i] <= L'9') i++;
                }
                Tok t{ TNum, L"", 0, line };
                t.num = wcstod(src.substr(b, i - b).c_str(), nullptr);
                out.push_back(t);
                continue;
            }
            // identifiers / keywords
            if (IsIdentChar(c))
            {
                size_t b = i;
                while (i < n && IsIdentChar(src[i])) i++;
                wstring w = src.substr(b, i - b);
                for (int k = 0; KEYWORDS[k]; k++)
                    if (w == KEYWORDS[k]) { out.push_back({ TKw, w, 0, line }); goto next; }
                out.push_back({ TIdent, w, 0, line });
            next:
                continue;
            }
            // operators (longest match first)
            struct { const wchar_t* op; } OPS[] = {
                L"<<=", L">>>", L">>=", L"...", L"::=", L":=", L"::", L"++", L"--",
                L"..", L"!==", L"**=", L"**",
                L"===", L"==", L"!=", L"<=", L">=", L"&&", L"||", L"+=", L"|=",
                L"&=", L"^=", L"<<", L">>", L"-=", L"*=", L"/=", L"%=",
                L"?.",
                nullptr
            };
            bool bMatched = false;
            for (int k = 0; OPS[k].op; k++)
            {
                if (wcscmp(OPS[k].op, L"?.") == 0 && i + 2 < n && src[i + 2] == L'.')
                    continue;
                size_t l = wcslen(OPS[k].op);
                if (src.compare(i, l, OPS[k].op) == 0)
                {
                    out.push_back({ TPunct, OPS[k].op, 0, line });
                    i += l;
                    bMatched = true;
                    break;
                }
            }
            if (bMatched) continue;
            out.push_back({ TPunct, wstring(1, c), 0, line });
            i++;
        }
        out.push_back({ TEnd, L"", 0, line });
        return out;
    }

    // ---------------- AST ----------------
}
