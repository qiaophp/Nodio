// Nodio engine pch — pre-compiled header for the standalone VM engine.
// Deliberately MFC-free: the engine is a pure Win32 + CRT component so it
// can back the WTL/Scintilla IDE shell and the standalone publisher
// without dragging the retired MFC framework along.
#pragma once

#ifndef WINVER
#define WINVER 0x0601
#endif

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef UNICODE
#define UNICODE
#endif

#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <wininet.h>
