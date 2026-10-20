// json.cpp — JSON parse/encode helpers. (The pattern compiler lives in
// regex.cpp, next to the REParser it builds on.)
#include "engine/common.h"

namespace NodioVM
{
    void JQuote(const wstring& str, wstring& out)
    {
        out += L'"';
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
    }

    wstring JQuote(const wstring& str)
    {
        wstring out;
        out.reserve(str.size() + 2);
        JQuote(str, out);
        return out;
    }

    void JWriteInternal(const Value& v, int indent, int depth, wstring& out)
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
        case Value::Null: out += L"null"; break;
        case Value::Bool: out += (v.b ? L"true" : L"false"); break;
        case Value::Num:
        {
            if (v.n == (long long)v.n && fabs(v.n) < 1e15)
            {
                out += std::to_wstring((long long)v.n);
                break;
            }
            wchar_t buf[32];
            swprintf(buf, 32, L"%.14g", v.n);
            out += buf;
            break;
        }
        case Value::Str: JQuote(v.s, out); break;
        case Value::TableT:
        {
            if (!v.tab)
            {
                out += L"null";
                break;
            }
            bool asArray = !v.tab->arr.empty() && v.tab->map.empty();
            out += (asArray ? L"[" : L"{");
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
                    out += pad;
                    JWriteInternal(e, indent, depth + 1, out);
                    bFirst = false;
                }
            }
            else
            {
                for (auto& kv : v.tab->map)
                {
                    if (!bFirst) { out += L","; if (indent >= 0) out += nl; }
                    out += pad;
                    JQuote(kv.first, out);
                    out += L": ";
                    JWriteInternal(kv.second, indent, depth + 1, out);
                    bFirst = false;
                }
            }
            if (indent >= 0 && bAny)
                out += nl + padEnd;
            out += (asArray ? L"]" : L"}");
            break;
        }
        default: out += L"null"; break;
        }
    }

    wstring JWrite(const Value& v, int indent, int depth)
    {
        wstring out;
        out.reserve(256);
        JWriteInternal(v, indent, depth, out);
        return out;
    }
}
