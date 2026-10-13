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
            bool bRightOfAssign = false;
            {
                size_t b = out.size();
                while (b > 0 && out[b - 1].t == TNL) b--;
                if (b > 0 && out[b - 1].t == TPunct &&
                    (out[b - 1].text == L"=" || out[b - 1].text == L":="))
                    bRightOfAssign = true;
            }
            if (c == L'/' && i + 1 < n && src[i + 1] == L'/')
            {
                // 注释字符串 (docs/datatype/string.md §4): a '//' comment
                // immediately right of '=' IS a raw single-line string —
                // everything to the end of the line, no escapes, no CR/LF.
                if (bRightOfAssign)
                {
                    size_t e = i + 2;
                    while (e < n && src[e] != L'\n' && src[e] != L'\r') e++;
                    out.push_back({ TStr, src.substr(i + 2, e - i - 2), 0, line });
                    i = e;
                    continue;
                }
                while (i < n && src[i] != L'\n') i++;
                continue;
            }
            if (c == L'/' && i + 1 < n && src[i + 1] == L'*')
            {
                // Block comment delimited by star-count matching: 块注释首尾
                // 标记的星号数目必须相同 (docs/datatype/string.md §4). The
                // closing run must carry EXACTLY the opening count. That is
                // what lets the shipped libraries nest '/*_ERR_XXX*/'-style
                // inline comments inside a '/*** … ***/' block, and what makes
                // '/**intellisense(...)' blocks end at '**/' rather than at
                // the first inner '*/'.
                size_t k = 1;
                while (i + k < n && src[i + k] == L'*') k++;
                size_t nStars = k - 1;
                if (i + k < n && src[i + k] == L'/')
                {
                    // '/*' + stars + '/' with nothing between: an empty
                    // comment ('/**/' / '/*/'); the trailing star closes it.
                    i += k + 1;
                    continue;
                }
                size_t s = i + k;                 // content start
                size_t j = s, endPos = wstring::npos;
                while (j + nStars + 1 <= n)
                {
                    if (src[j] != L'*') { j++; continue; }
                    size_t q = j;
                    while (q < n && src[q] == L'*') q++;
                    if (q - j == nStars && q < n && src[q] == L'/')
                    { endPos = j; break; }
                    j = q;
                }
                size_t e = (endPos == wstring::npos)
                               ? n : (endPos + nStars + 1);
                if (bRightOfAssign && endPos != wstring::npos)
                {
                    // 注释字符串: the body is real data (CSS/SQL/HTML), not a
                    // comment. Newlines normalize to CRLF; the blank line
                    // hugging the opening/closing stars is dropped; escapes
                    // are not processed.
                    wstring content = src.substr(s, endPos - s);
                    if (!content.empty() && content[0] == L'\n')
                        content.erase(0, 1);
                    else if (content.size() >= 2 && content[0] == L'\r' &&
                             content[1] == L'\n')
                        content.erase(0, 2);
                    while (!content.empty() &&
                           (content.back() == L' ' ||
                            content.back() == L'\t'))
                        content.pop_back();
                    if (!content.empty() && content.back() == L'\n')
                    {
                        content.pop_back();
                        if (!content.empty() && content.back() == L'\r')
                            content.pop_back();
                    }
                    wstring norm;      // normalize line endings to CRLF
                    norm.reserve(content.size());
                    for (size_t q = 0; q < content.size(); q++)
                    {
                        if (content[q] == L'\r') continue;
                        if (content[q] == L'\n') { norm += L"\r\n"; continue; }
                        norm += content[q];
                    }
                    for (size_t q = i; q < e && q < n; q++)
                        if (src[q] == L'\n') line++;
                    i = e;
                    out.push_back({ TStr, norm, 0, line });
                    continue;
                }
                for (size_t q = i; q < e && q < n; q++)
                    if (src[q] == L'\n') line++;
                i = e;
                continue;
            }
            // strings
            if (c == L'"' || c == L'`')
            {
                // aardio raw string: 双引号/反引号 = 原样字符串，不处理反斜杠
                // 转义 (docs/datatype/string.md)。但两个连续的同类引号表示
                // 该引号自身，所以 "a""b" 是 a"b，`a``b` 是 a`b。
                wchar_t q = c;
                i++;
                wstring s;
                while (i < n && src[i] != q)
                {
                    if (src[i] == L'\n') line++;
                    s += src[i++];
                }
                // a doubled quote is an escaped literal quote, not the end
                while (i + 1 < n && src[i] == q && src[i + 1] == q)
                {
                    s += q;
                    i += 2;
                    while (i < n && src[i] != q)
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
                // a trailing lowercase 'u' marks a UTF-16 (ustring) string
                // (docs/datatype/string.md). Values are already wide, so
                // just consume the marker and emit the string token.
                if (i < n && src[i] == L'u' &&
                    !(i + 1 < n && IsIdentChar(src[i + 1])))
                    i++;
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
                L"<<=", L">>>", L">>=", L"...", L"::=", L":=", L"::",
                L"++=", L"--=", L"++", L"--",
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
