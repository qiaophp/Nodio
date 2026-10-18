cmake_minimum_required(VERSION 3.20)
project(Nodio LANGUAGES CXX)

# Nodio — clean-room rebuild of the aardio ecosystem on a pure x64-first
# engine with a self-hosted (aardio-on-the-VM) IDE.
#
# Module map (see docs/architecture.md):
#   engine/          nodio_engine  — the VM (zero MFC, Win32 + CRT only)
#   publish/         nodio_publish — payload embedder + PE stamping
#   apps/nodio/      nodio.exe     — host: run / debug / check / publish
#   apps/nodio-lsp/  nodio-lsp.exe — Language Server Protocol (stdio)
#   apps/nodio-dap/  nodio-dap.exe — Debug Adapter Protocol (stdio)
#   ide/nodio/       self-hosted IDE (aardio source, no C++ target)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# one CRT flavor for every target
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

# Every executable lands in one directory (build-<arch>/<config>/) instead of
# per-target generator subdirs, so the IDE and the test scripts find
# nodio.exe / nodio-lsp.exe / nodio-dap.exe by a single known path.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>")

# Settings every target shares. The repo root is on the include path so that
# `#include "engine/..."` / `#include "publish/..."` resolve from any source
# directory (the component dirs keep their own name in the include, which is
# why they are not nested under a src/ prefix).
add_library(nodio_common INTERFACE)
target_include_directories(nodio_common INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
if(MSVC)
    target_compile_options(nodio_common INTERFACE /utf-8)
endif()
target_compile_definitions(nodio_common INTERFACE
    _UNICODE UNICODE _CRT_SECURE_NO_WARNINGS)

add_subdirectory(engine)
add_subdirectory(publish)
add_subdirectory(apps/nodio)
add_subdirectory(apps/nodio-lsp)
add_subdirectory(apps/nodio-dap)
