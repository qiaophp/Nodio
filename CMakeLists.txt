cmake_minimum_required(VERSION 3.20)
project(Nodio LANGUAGES CXX)

# Nodio — clean-room rebuild of the aardio ecosystem on a pure x64-first
# engine, with the retired MFC IDE shell replaced in later phases by a
# WTL + Scintilla front end. This file builds ONLY the engine + console
# host (zero MFC, zero managed dependencies).

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(engine STATIC
    engine/lexer.cpp
    engine/parser.h
    engine/value.cpp
    engine/util.cpp
    engine/ffi.cpp
    engine/memdll.cpp
    engine/regex.cpp
    engine/json.cpp
    engine/runtime.cpp
    engine/import.cpp
    engine/builtins.cpp
    engine/debug.cpp
    engine/gdi.cpp
    engine/entry.cpp
    engine/common.h
    engine/interp.h
    engine/pch.h
)
target_include_directories(engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(engine PRIVATE /utf-8)
target_compile_definitions(engine PRIVATE
    _UNICODE UNICODE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(engine PUBLIC
    user32 gdi32 kernel32 wininet ole32 oleaut32 uuid advapi32)
set_target_properties(engine PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

add_executable(nodio
    host/main.cpp
    host/host.rc
    publish/publish.cpp
    publish/publish.h
    publish/payload.h
)
target_compile_options(nodio PRIVATE /utf-8)
target_compile_definitions(nodio PRIVATE
    _UNICODE UNICODE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(nodio PRIVATE engine)
# host.rc embeds the manifest + version info as real PE resources at the
# ids the Windows loader reads. The VS generator also emits its own default
# manifest and merges it at link time (CVT1100 duplicate on RT_MANIFEST
# id 1), so disable that merge with the linker switch.
set_target_properties(nodio PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
if(MSVC)
    target_link_options(nodio PRIVATE "/MANIFEST:NO")
endif()
