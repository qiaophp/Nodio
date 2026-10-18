# nodio_engine — the tree-walking aardio interpreter. Win32 + CRT only,
# zero MFC. Public link libs are the Win32 DLLs the FFI/COM/GDI layers call.
add_library(nodio_engine STATIC
    lexer.cpp
    parser.h
    value.cpp
    util.cpp
    ffi.cpp
    memdll.cpp
    regex.cpp
    json.cpp
    runtime.cpp
    import.cpp
    builtins.cpp
    debug.cpp
    gdi.cpp
    com.cpp
    entry.cpp
    common.h
    interp.h
    pch.h
)
target_link_libraries(nodio_engine PUBLIC
    nodio_common
    user32 gdi32 kernel32 wininet ole32 oleaut32 uuid advapi32 shlwapi)
