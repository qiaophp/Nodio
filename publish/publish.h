// publish.h — publisher interface (implementation in publish.cpp, a real
// translation unit; consumers must include THIS header, never the .cpp).
#pragma once

#include <string>

namespace NodioPublish
{
    struct Options
    {
        std::wstring strEntry;      // entry script path
        std::wstring strOut;        // output exe path
        std::wstring strLibDir;     // NODIO_LIBDIR
        bool bConsole = true;       // console subsystem in the output exe
        std::wstring strIcon;       // optional .ico to stamp

        // Explicit project manifest (.aproj/.nproj). When empty the pipeline
        // auto-probes <entry dir>\default.aproj, then default.nproj.
        std::wstring strProj;

        // PE version resource (FileVersion/ProductVersion + strings).
        // Empty fields fall back to the entry file name / "1.0".
        std::wstring strVersion;            // "1.2.3.4"
        std::wstring strCompany;
        std::wstring strProduct;
        std::wstring strDescription;
        std::wstring strCopyright;

        // Application manifest. bDpiAware stamps a per-monitor DPI-aware
        // manifest (apps are pixel-positioned); strUacLevel picks the
        // requestedExecutionLevel — "" (default) omits the element, or
        // "asInvoker" / "requireAdministrator" / "highestAvailable".
        bool bDpiAware = true;
        std::wstring strUacLevel;

        // Rewrite the manifest + PE version resource in the produced exe.
        // Off by default: rewriting the manifest resource in place is
        // rejected by the Windows loader in some configurations (the exe
        // then fails with "side-by-side configuration is incorrect"), so
        // the default keeps the manifest the host was built with — which
        // already declares DPI awareness. The IDE's publish wizard turns
        // this on explicitly (it enables the version resource too).
        bool bPatchManifest = false;
    };

    // Full publish pipeline: .aproj rules, syntax gate, import closure,
    // byte-exact payload append, PE subsystem patch, icon/version/manifest
    // stamping. Returns false and fills strErr on failure.
    bool Publish(const Options& opt, std::wstring& strErr);
}
