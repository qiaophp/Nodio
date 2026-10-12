// json.cpp — JSON parse/encode helpers. (The pattern compiler lives in
// regex.cpp, next to the REParser it builds on.)
#include "engine/common.h"

namespace NodioVM
{
    wstring JQuote(const wstring& str)
    {
        wstring out = L"\"";
        for (wchar_t c : str)
        {
            switch (c)
            {
            case L'"': out += L"\\\""; break;
            case L'\\': out += L"\\\\"; break;
            case L'\n': out += L"\\n"; break;
            case L'\r': out += L"\\r"; break;
            case L'\t': out += L"\\t"; break;
            default:
                if (c < 0x20)
                {
                    wchar_t buf[8];
                    swprintf(buf, 8, L"\\u%04x", (unsigned)c);
                    out += buf;
                }
                else out += c;
            }
        }
        out += L'"';
        return out;
    }

    wstring JWrite(const Value& v, int indent, int depth)
    {
        wstring pad, padEnd;
        if (indent >= 0 && depth > 0)
        {
            pad.assign((size_t)indent * depth, L' ');
            padEnd.assign((size_t)indent * (depth - 1), L' ');
        }
        wstring nl = indent >= 0 ? L"\n" : L"";
        switch (v.t)
        {
        case Value::Null: return L"null";
        case Value::Bool: return v.b ? L"true" : L"false";
        case Value::Num:
        {
            if (v.n == (long long)v.n && fabs(v.n) < 1e15)
                return std::to_wstring((long long)v.n);
            wchar_t buf[32];
            swprintf(buf, 32, L"%.14g", v.n);
            return buf;
        }
        case Value::Str: return JQuote(v.s);
        case Value::TableT:
        {
            if (!v.tab)
                return L"null";
            bool asArray = !v.tab->arr.empty() && v.tab->map.empty();
            wstring out = asArray ? L"[" : L"{";
            bool bAny = !v.tab->arr.empty() || !v.tab->map.empty();
            if (indent >= 0 && bAny)
                out += nl;
            bool bFirst = true;
            if (asArray)
            {
                for (auto& e : v.tab->arr)
                {
                    if (!bFirst) { out += L","; if (indent >= 0) out += L" "; }
                    if (indent >= 0) out += nl;
                    out += pad + JWrite(e, indent, depth + 1);
                    bFirst = false;
                }
            }
            else
            {
                for (auto& kv : v.tab->map)
                {
                    if (!bFirst) { out += L","; if (indent >= 0) out += nl; }
                    out += pad + JQuote(kv.first) + L": " +
                           JWrite(kv.second, indent, depth + 1);
                    bFirst = false;
                }
            }
            if (indent >= 0 && bAny)
                out += nl + padEnd;
            out += asArray ? L"]" : L"}";
            return out;
        }
        default: return L"null";
        }
    }
}
