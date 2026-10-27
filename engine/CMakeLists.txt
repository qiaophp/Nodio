# nodio_engine — the tree-walking aardio interpreter. Win32 + CRT only,
# zero MFC. Public link libs are the Win32 DLLs the FFI/COM/GDI layers call.
add_library(nodio_engine STATIC
    compiler.cpp
    compiler.h
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
    allocstat.cpp
    aardio_chunk.cpp
    aardio_chunk.h
    aardio_vm.cpp
    aardio_vm.h
    builtins.cpp
    debug.cpp
    gdi.cpp
    gc.cpp
    sysapi.cpp
    com.cpp
    entry.cpp
    common.h
    interp.h
    pch.h
)
target_link_libraries(nodio_engine PUBLIC
    nodio_common
    user32 gdi32 kernel32 wininet ole32 oleaut32 uuid advapi32 shlwapi)

# 体积线 S3-lite：冷 TU 分档编译。COM/GDI/正则/调试通道/import/入口/词法/
# 字节码块不在解释器热循环上（COM 与回调本身有跨进程/跨 ABI 的固定开销），
# 用 /O1 换体积；热 TU（builtins/runtime/ffi/gc/value/util）保持全局 /O2。
# A/B 实测过全量 /O1 会慢 25~40%（loop/call/str 三夹具），此处分档规避。
# 实测（ad73341 基线）：stub 2,133,504 → 2,100,224 B（-33KB，LTCG 跨 TU
# 内联削平了部分分档收益）。
set_source_files_properties(
    com.cpp gdi.cpp regex.cpp debug.cpp import.cpp
    aardio_chunk.cpp entry.cpp lexer.cpp
    PROPERTIES COMPILE_OPTIONS "/O1")
