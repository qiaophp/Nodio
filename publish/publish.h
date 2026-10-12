// publish.h — publisher interface (implementation in publish.cpp, a real
// translation unit; consumers must include THIS header, never the .cpp).
#pragma once

#include <string>

namespace NodioPublish
{
    struct Options
    {
        std::wstring strEntry;      // main.aardio / *.nodio
        std::wstring strOut;        // output exe path
        std::wstring strLibDir;     // NODIO_LIBDIR
        bool bConsole = true;       // console subsystem in the output exe
        std::wstring strIcon;       // optional .ico to stamp
    };

    // Full publish pipeline: .aproj rules, syntax gate, import closure,
    // byte-exact payload append, PE subsystem patch, optional icon stamp.
    // Returns false and fills strErr on failure.
    bool Publish(const Options& opt, std::wstring& strErr);
}
