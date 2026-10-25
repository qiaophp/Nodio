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
        case Str: return true;
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

    wstring ObjectKeyOf(const Value& v)
    {
        switch (v.t)
        {
        case Value::TableT:
            if (v.tab)
            {
                wchar_t buf[32];
                swprintf(buf, 32, L"table:%p", (void*)v.tab.get());
                return buf;
            }
            return L"table:null";
        case Value::Class:
            if (v.tab)
            {
                wchar_t buf[32];
                swprintf(buf, 32, L"class:%p", (void*)v.tab.get());
                return buf;
            }
            return L"class:null";
        case Value::Func:
        {
            wchar_t buf[32];
            swprintf(buf, 32, L"func:%p",
                     v.native ? (void*)&v.native : (void*)v.fnData->fn.get());
            return buf;
        }
        case Value::Pointer:
        {
            wchar_t buf[32];
            swprintf(buf, 32, L"ptr:%p", v.ptr);
            return buf;
        }
        default: return v.ToString();
        }
    }
}
