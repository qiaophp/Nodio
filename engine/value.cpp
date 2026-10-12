// value.cpp — Value member definitions (truthiness, string form).
#include "engine/common.h"

namespace NodioVM
{
    bool Value::Truthy() const
    {
        switch (t)
        {
        case Null: return false;
        case Bool: return b;
        case Num: return n != 0;
        case Pointer: return ptr != nullptr;
        case Str: return !s.empty();
        default: return true;
        }
    }

    wstring Value::ToString() const
    {
        switch (t)
        {
        case Null: return L"null";
        case Bool: return b ? L"true" : L"false";
        case Num:
        {
            wchar_t buf[32];
            if (n == (long long)n)
                swprintf(buf, 32, L"%lld", (long long)n);
            else
                swprintf(buf, 32, L"%.14g", n);
            return buf;
        }
        case Pointer:
        {
            wchar_t buf[32];
            swprintf(buf, 32, L"%lld", (long long)(INT_PTR)ptr);
            return buf;
        }
        case Str: return s;
        case TableT: return L"table";
        case Class: return L"class";
        case Func: return L"function";
        }
        return L"?";
    }

    // ---------------- struct layout & serialization helpers ----------------
}
