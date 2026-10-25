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
        // token 数约在源字符数的 1/2 量级（注释不产 token，成员链
        // a.b.c() 每 2-3 字符 1 token）；按 1/3 预留消灭 vector 倍增
        // 的整段搬移（普查：>32K 桶 775 次 133MB 全是这 churn）
        out.reserve(src.size() / 3 + 32);
        size_t i = 0, n = src.size();
        int line = 1;
        bool hadSpace = false;
        // Start offset of the token currently being lexed. pushTok() copies
        // it into the token so callers can map a token back to an exact
        // column (LSP `character` / editor caret position).
        size_t tokStart = 0;
        auto pushTok = [&](Tok t) {
            t.bLeadingSpace = hadSpace;
            t.pos = tokStart;
            hadSpace = false;
            out.push_back(t);
        };
        while (i < n)
        {
            wchar_t c = src[i];
            if (c == 0xFEFF) { i++; continue; }
            if (c == L'\r') { i++; continue; }
            if (c == L'\n') { tokStart = i; pushTok({ TNL, L"", 0, line++ }); hadSpace = true; i++; continue; }
            if (c == L' ' || c == L'\t') { hadSpace = true; i++; continue; }
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
                    tokStart = i;
                    pushTok({ TStr, src.substr(i + 2, e - i - 2), 0, line });
                    i = e;
                    continue;
                }
                while (i < n && src[i] != L'\n') i++;
                hadSpace = true;
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
                    hadSpace = true;
                    continue;
                }
                size_t s = i + k;                 // content start
                size_t strStart = i;              // token start (the "/*")
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
                    tokStart = strStart;
                    pushTok({ TStr, norm, 0, line });
                    continue;
                }
                for (size_t q = i; q < e && q < n; q++)
                    if (src[q] == L'\n') line++;
                i = e;
                hadSpace = true;
                continue;
            }
            // strings
            if (c == L'"' || c == L'`')
            {
                // raw string: 双引号/反引号 = 原样字符串，不处理反斜杠
                // 转义。但两个连续的同类引号表示
                // 该引号自身，所以 "a""b" 是 a"b，`a``b` 是 a`b。
                wchar_t q = c;
                size_t strStart = i;      // token start (the opening quote)
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
                // 第四十二轮：EOF 不能当终止符——截断/损坏文件会静默执行
                // 前缀（审计实测：未闭合引号 exit 0 无任何报告）
                if (i >= n)
                {
                    if (strError && strError->empty())
                        *strError = L"词法错误: 字符串未终止 @ 行 " +
                                    std::to_wstring(line);
                    return out;
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
                    tokStart = strStart;
                    pushTok({ TNum, std::to_wstring((long long)val), val, line });
                    continue;
                }
                bool bIsRawU = false;
                if (i < n && src[i] == L'u' &&
                    !(i + 1 < n && IsIdentChar(src[i + 1])))
                {
                    i++;
                    bIsRawU = true;
                }
                tokStart = strStart;
                pushTok({ TStr, s, 0, line, false, bIsRawU });
                continue;
            }
            if (c == L'\'')
            {
                // escaped string: 单引号 = 编译时转义字符串,
                // C-like \ escapes ('\\' == "\"" per the original docs)
                wchar_t q = c;
                size_t strStart = i;      // token start (the opening quote)
                i++;
                wstring s;
                bool bHasByteEscape = false;
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
                        case L'0': case L'1': case L'2': case L'3': case L'4':
                        case L'5': case L'6': case L'7': case L'8': case L'9':
                        {
                            bHasByteEscape = true;
                            // If \0 is immediately followed by a newline:
                            // "如果只想表示'\0'，并且后面有其他不相关的数字，可以在\0后面加一个换行"
                            if (e == L'0' && i + 1 < n && (src[i + 1] == L'\r' || src[i + 1] == L'\n'))
                            {
                                if (src[i + 1] == L'\r' && i + 2 < n && src[i + 2] == L'\n') i += 2;
                                else i++;
                                s += L'\0';
                                break;
                            }
                            int val = (e - L'0');
                            int digits = 1;
                            while (digits < 3 && i + 1 < n && src[i + 1] >= L'0' && src[i + 1] <= L'9')
                            {
                                val = val * 10 + (src[++i] - L'0');
                                digits++;
                            }
                            s += (wchar_t)(val & 0xFF);
                            break;
                        }
                        case L'a': s += L'\a'; break;
                        case L'b': s += L'\b'; break;
                        case L'f': s += L'\f'; break;
                        case L'v': s += L'\v'; break;
                        case L'e': s += L'\x1b'; break;
                        case L'\\': s += L'\\'; break;
                        case L'"': s += L'"'; break;
                        case L'\'': s += L'\''; break;
                        case L'[': s += L'['; break;
                        case L']': s += L']'; break;
                        case L'u':
                        {
                            int hexLen = 0;
                            while (hexLen < 4 && i + 1 + hexLen < n && iswxdigit(src[i + 1 + hexLen]))
                                hexLen++;
                            if (hexLen > 0)
                            {
                                unsigned long cp = wcstoul(src.substr(i + 1, hexLen).c_str(), nullptr, 16);
                                s += (wchar_t)cp;
                                i += hexLen;
                            }
                            else
                            {
                                s += L'\\';
                                s += L'u';
                            }
                            break;
                        }
                        case L'U':
                        {
                            int hexLen = 0;
                            while (hexLen < 8 && i + 1 + hexLen < n && iswxdigit(src[i + 1 + hexLen]))
                                hexLen++;
                            if (hexLen > 0)
                            {
                                unsigned long cp = wcstoul(src.substr(i + 1, hexLen).c_str(), nullptr, 16);
                                if (cp <= 0xFFFF)
                                {
                                    s += (wchar_t)cp;
                                }
                                else if (cp <= 0x10FFFF)
                                {
                                    cp -= 0x10000;
                                    s += (wchar_t)((cp >> 10) + 0xD800);
                                    s += (wchar_t)((cp & 0x3FF) + 0xDC00);
                                }
                                i += hexLen;
                            }
                            else
                            {
                                s += L'\\';
                                s += L'U';
                            }
                            break;
                        }
                        default:
                            if (e == L'x')
                            {
                                bHasByteEscape = true;
                                int hexLen = 0;
                                while (hexLen < 2 && i + 1 + hexLen < n && iswxdigit(src[i + 1 + hexLen]))
                                    hexLen++;
                                if (hexLen > 0)
                                {
                                    s += (wchar_t)wcstoul(src.substr(i + 1, hexLen).c_str(), nullptr, 16);
                                    i += hexLen;
                                }
                                else
                                {
                                    s += L'\\';
                                    s += L'x';
                                }
                            }
                            else
                            {
                                // unknown escape: keep verbatim (patterns
                                // rely on '\d'/'\w' surviving the literal)
                                s += L'\\';
                                s += e;
                            }
                            break;
                        }
                        i++;
                    }
                    else
                    {
                        if (src[i] == L'\n') line++;
                        s += src[i++];
                    }
                }
                // 第四十二轮：同 raw 字符串——EOF 未终止要报错
                if (i >= n)
                {
                    if (strError && strError->empty())
                        *strError = L"词法错误: 字符串未终止 @ 行 " +
                                    std::to_wstring(line);
                    return out;
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
                    tokStart = strStart;
                    pushTok({ TNum, std::to_wstring((long long)val), val, line });
                    continue;
                }
                // a trailing lowercase 'u' marks a UTF-16 (ustring) string
                // (docs/datatype/string.md). Values are already wide, so
                // just consume the marker and emit the string token.
                bool bIsU = false;
                if (i < n && src[i] == L'u' &&
                    !(i + 1 < n && IsIdentChar(src[i + 1])))
                {
                    i++;
                    bIsU = true;
                }
                Tok tokStr = { TStr, s, 0, line, false, bIsU, bHasByteEscape };
                tokStart = strStart;
                pushTok(tokStr);
                continue;
            }
            // numbers
            bool bDotNumber = (c == L'.' && i + 1 < n && src[i + 1] >= L'0' && src[i + 1] <= L'9');
            if (bDotNumber && !hadSpace && !out.empty())
            {
                const Tok& prev = out.back();
                if (prev.t == TIdent || prev.t == TKw || prev.t == TNum ||
                    (prev.t == TPunct && (prev.text == L")" || prev.text == L"]" || prev.text == L"}")))
                {
                    bDotNumber = false;
                }
            }
            if ((c >= L'0' && c <= L'9') || bDotNumber)
            {
                size_t b = i;
                tokStart = b;
                if (c == L'0' && i + 1 < n && (src[i + 1] == L'x' || src[i + 1] == L'X'))
                {
                    i += 2;
                    while (i < n && iswxdigit(src[i])) i++;
                    Tok t{ TNum, src.substr(b, i - b), 0, line };
                    t.num = (double)wcstoul(src.substr(b + 2, i - b - 2).c_str(), nullptr, 16);
                    pushTok(t);
                    continue;
                }
                // radix literal: <radix>#<digits> (e.g. 2#100, 8#77, 16#FF, 36#Z)
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
                        pushTok(t);
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
                Tok t{ TNum, src.substr(b, i - b), 0, line };
                t.num = wcstod(src.substr(b, i - b).c_str(), nullptr);
                pushTok(t);
                continue;
            }
            // identifiers / keywords
            if (IsIdentChar(c))
            {
                size_t b = i;
                while (i < n && IsIdentChar(src[i])) i++;
                wstring w = src.substr(b, i - b);
                for (int k = 0; KEYWORDS[k]; k++)
                    if (w == KEYWORDS[k]) { tokStart = b; pushTok({ TKw, w, 0, line }); goto next; }
                tokStart = b;
                pushTok({ TIdent, w, 0, line });
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
                    tokStart = i;
                    pushTok({ TPunct, OPS[k].op, 0, line });
                    i += l;
                    bMatched = true;
                    break;
                }
            }
            if (bMatched) continue;
            tokStart = i;
            pushTok({ TPunct, wstring(1, c), 0, line });
            i++;
        }
        tokStart = i;
        // 第四十五轮：EOF 哨兵携带最后真实 token 的行号。line 此时已数过
        // 尾部换行，报错点在 EOF 会报「文件尾后一行」（单行文件报行 2），
        // 定位误导。回退跳过 TNL 取最后内容行。
        for (size_t b = out.size(); b > 0; b--)
        {
            if (out[b - 1].t != TNL) { line = out[b - 1].line; break; }
        }
        pushTok({ TEnd, L"", 0, line });
        return out;
    }

    // ---------------- AST ----------------
}
