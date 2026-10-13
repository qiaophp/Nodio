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

# ---- Nodio IDE: native shell around the Scintilla editing component ----
# Scintilla is a third-party component and is NOT committed here (see
# thirdparty/README.md for how to fetch the pinned release). The IDE target
# is skipped when the sources are absent, so the engine/host build stays
# self-contained.
option(NODIO_BUILD_IDE "Build the Nodio IDE shell (needs Scintilla sources)" ON)
set(NODIO_SCINTILLA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/scintilla")
if(NODIO_BUILD_IDE AND EXISTS "${NODIO_SCINTILLA_DIR}/src/Editor.cxx")
    add_library(scintilla STATIC
        "${NODIO_SCINTILLA_DIR}/win32/PlatWin.cxx"
        "${NODIO_SCINTILLA_DIR}/win32/ScintillaWin.cxx"
        "${NODIO_SCINTILLA_DIR}/win32/HanjaDic.cxx"
    )
    file(GLOB SCINTILLA_SRC "${NODIO_SCINTILLA_DIR}/src/*.cxx")
    file(GLOB SCINTILLA_LEXLIB "${NODIO_SCINTILLA_DIR}/lexlib/*.cxx")
    file(GLOB SCINTILLA_LEXERS "${NODIO_SCINTILLA_DIR}/lexers/*.cxx")
    target_sources(scintilla PRIVATE
        ${SCINTILLA_SRC} ${SCINTILLA_LEXLIB} ${SCINTILLA_LEXERS})
    target_include_directories(scintilla PUBLIC
        "${NODIO_SCINTILLA_DIR}/include"
        "${NODIO_SCINTILLA_DIR}/src"
        "${NODIO_SCINTILLA_DIR}/lexlib")
    target_compile_definitions(scintilla PRIVATE
        _UNICODE UNICODE _CRT_SECURE_NO_WARNINGS
        _CRT_SECURE_CPP_OVERLOAD_STANDARD_NAMES=1
        _CRT_SECURE_NO_DEPRECATE=1 _SCL_SECURE_NO_WARNINGS=1
        SCI_LEXER NO_CXX11_REGEX)
    target_compile_options(scintilla PRIVATE /utf-8 /W0)
    set_target_properties(scintilla PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    target_link_libraries(scintilla PUBLIC
        user32 gdi32 kernel32 imm32 ole32 oleaut32 msimg32 comctl32)

    add_executable(nodio-ide WIN32 ide/nodio_ide.cpp ide/ide.rc)
    target_compile_options(nodio-ide PRIVATE /utf-8)
    target_compile_definitions(nodio-ide PRIVATE
        _UNICODE UNICODE _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(nodio-ide PRIVATE scintilla comctl32)
    set_target_properties(nodio-ide PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()
