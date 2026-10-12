cmake_minimum_required(VERSION 3.20)
project(Nodio LANGUAGES CXX)

# Nodio — clean-room rebuild of the aardio ecosystem on a pure x64-first
# engine, with the retired MFC IDE shell replaced in later phases by a
# WTL + Scintilla front end. This file builds ONLY the engine + console
# host (zero MFC, zero managed dependencies).

add_library(engine STATIC
    engine/AardioVM.cpp
)
target_include_directories(engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(engine PRIVATE /utf-8)
target_compile_definitions(engine PRIVATE
    _UNICODE UNICODE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(engine PUBLIC
    user32 gdi32 kernel32 wininet ole32 oleaut32 uuid advapi32)
set_target_properties(engine PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

add_executable(host
    host/main.cpp
    host/host.manifest
)
target_compile_options(host PRIVATE /utf-8)
target_compile_definitions(host PRIVATE
    _UNICODE UNICODE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(host PRIVATE engine)
set_target_properties(host PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
