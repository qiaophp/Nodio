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

        // PE version resource (FileVersion/ProductVersion + strings).
        // Empty fields fall back to the entry file name / "1.0".
        std::wstring strVersion;            // "1.2.3.4"
        std::wstring strCompany;
        std::wstring strProduct;
        std::wstring strDescription;
        std::wstring strCopyright;

        // Application manifest. bDpiAware stamps a per-monitor DPI-aware
        // manifest (aardio apps are pixel-positioned); strUacLevel picks the
        // requestedExecutionLevel — "" (default) omits the element, or
        // "asInvoker" / "requireAdministrator" / "highestAvailable".
        bool bDpiAware = true;
        std::wstring strUacLevel;
    };

    // Full publish pipeline: .aproj rules, syntax gate, import closure,
    // byte-exact payload append, PE subsystem patch, icon/version/manifest
    // stamping. Returns false and fills strErr on failure.
    bool Publish(const Options& opt, std::wstring& strErr);
}
