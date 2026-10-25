// sysapi.cpp — bare `::` system API resolution.
//
// Official aardio's `::Name` resolves a Win32 API across the system DLLs
// (and a few aardio aliases such as ::SetWindowPointer, which is
// SetWindowLongPtrW). Nodio only knew its own root namespaces, so every
// official library module that reaches for the Win32 API through `::`
// failed with "尝试调用非函数值" — most importantly win/ui/_.aardio, which
// installs each control's window procedure with ::SetWindowPointer. With
// that missing, the official control event routing was completely dead:
// the IDE's controls painted but never delivered a mouse event.
//
// The table below carries the export name and the call signature for each
// API. Signatures use the engine's FFI type names:
//   int int64 word byte bool addr str(A) wstr(W) double
// "addr" is a pointer-sized integer slot, "str" is converted to UTF-16
// automatically for W-suffixed exports.
#include "engine/interp.h"

namespace NodioVM
{
    struct SysApiDef
    {
        const wchar_t* name;       // the name used in scripts: ::Name
        const wchar_t* dll;
        const wchar_t* exportName; // the real export (may differ: aliases)
        const wchar_t* sig;
    };

    // user32 — window/message/control plumbing
    static const SysApiDef s_user32[] = {
        { L"SetWindowPointer", L"user32.dll", L"SetWindowLongPtrW",
          L"int64(addr hwnd,int index,int64 value)" },
        { L"GetWindowPointer", L"user32.dll", L"GetWindowLongPtrW",
          L"int64(addr hwnd,int index)" },
        { L"SetWindowLong", L"user32.dll", L"SetWindowLongPtrW",
          L"int64(addr hwnd,int index,int64 value)" },
        { L"GetWindowLong", L"user32.dll", L"GetWindowLongPtrW",
          L"int64(addr hwnd,int index)" },
        { L"CallWindowProc", L"user32.dll", L"CallWindowProcW",
          L"int64(addr proc,addr hwnd,int msg,addr wparam,addr lparam)" },
        { L"PostMessage", L"user32.dll", L"PostMessageW",
          L"int(addr hwnd,int msg,addr wparam,addr lparam)" },
        { L"SendMessage", L"user32.dll", L"SendMessageW",
          L"int64(addr hwnd,int msg,addr wparam,addr lparam)" },
        { L"PostThreadMessage", L"user32.dll", L"PostThreadMessageW",
          L"int(int id,int msg,addr wparam,addr lparam)" },
        { L"RegisterClassEx", L"user32.dll", L"RegisterClassExW",
          L"int(addr wc)" },
        { L"RegisterWindowMessage", L"user32.dll", L"RegisterWindowMessageW",
          L"int(str name)" },
        { L"InvalidateRect", L"user32.dll", L"InvalidateRect",
          L"int(addr hwnd,addr rect,int erase)" },
        { L"UpdateWindow", L"user32.dll", L"UpdateWindow",
          L"int(addr hwnd)" },
        { L"RedrawWindow", L"user32.dll", L"RedrawWindow",
          L"int(addr hwnd,addr rect,int flags,addr rgn)" },
        { L"GetClientRect", L"user32.dll", L"GetClientRect",
          L"int(addr hwnd,addr rect)" },
        { L"GetWindowRect", L"user32.dll", L"GetWindowRect",
          L"int(addr hwnd,addr rect)" },
        { L"ScreenToClient", L"user32.dll", L"ScreenToClient",
          L"int(addr hwnd,addr pt)" },
        { L"ClientToScreen", L"user32.dll", L"ClientToScreen",
          L"int(addr hwnd,addr pt)" },
        { L"MapWindowPoints", L"user32.dll", L"MapWindowPoints",
          L"int(addr hwnd,addr hwnd2,addr pts,int count)" },
        { L"SetWindowPos", L"user32.dll", L"SetWindowPos",
          L"int(addr hwnd,addr after,int x,int y,int cx,int cy,int flags)" },
        { L"MoveWindow", L"user32.dll", L"MoveWindow",
          L"int(addr hwnd,int x,int y,int w,int h,int repaint)" },
        { L"GetWindowDC", L"user32.dll", L"GetWindowDC",
          L"addr(addr hwnd)" },
        { L"GetDC", L"user32.dll", L"GetDC", L"addr(addr hwnd)" },
        { L"ReleaseDC", L"user32.dll", L"ReleaseDC",
          L"int(addr hwnd,addr hdc)" },
        { L"BeginPaint", L"user32.dll", L"BeginPaint",
          L"addr(addr hwnd,addr ps)" },
        { L"EndPaint", L"user32.dll", L"EndPaint",
          L"int(addr hwnd,addr ps)" },
        { L"DrawText", L"user32.dll", L"DrawTextW",
          L"int(addr hdc,str text,addr rect,int flags)" },
        { L"GetSystemMetrics", L"user32.dll", L"GetSystemMetrics",
          L"int(int index)" },
        { L"SystemParametersInfo", L"user32.dll", L"SystemParametersInfoW",
          L"int(int action,int param,addr pv,int winini)" },
        { L"LoadCursor", L"user32.dll", L"LoadCursorW",
          L"addr(int id)" },
        { L"LoadIcon", L"user32.dll", L"LoadIconW", L"addr(int id)" },
        { L"LoadImage", L"user32.dll", L"LoadImageW",
          L"addr(addr hinst,str name,int type,int cx,int cy,int flags)" },
        { L"SetCursor", L"user32.dll", L"SetCursor", L"addr(addr hcur)" },
        { L"GetCursorPos", L"user32.dll", L"GetCursorPos", L"int(addr pt)" },
        { L"GetCursor", L"user32.dll", L"GetCursor", L"addr(int flags)" },
        { L"GetAsyncKeyState", L"user32.dll", L"GetAsyncKeyState",
          L"int(int vkey)" },
        { L"GetKeyState", L"user32.dll", L"GetKeyState", L"int(int vkey)" },
        { L"MapVirtualKey", L"user32.dll", L"MapVirtualKeyW",
          L"int(int code,int map)" },
        { L"DestroyWindow", L"user32.dll", L"DestroyWindow",
          L"int(addr hwnd)" },
        { L"CreateWindowEx", L"user32.dll", L"CreateWindowExW",
          L"addr(int exstyle,str cls,str text,int style,int x,int y,"
          L"int w,int h,addr parent,addr menu,addr inst,addr param)" },
        { L"GetClassInfoEx", L"user32.dll", L"GetClassInfoExW",
          L"int(addr hinst,addr wc,int flags)" },
        { L"PrintWindow", L"user32.dll", L"PrintWindow",
          L"int(addr hwnd,addr hdc,int flags)" },
        { L"SetWindowRgn", L"user32.dll", L"SetWindowRgn",
          L"int(addr hwnd,addr rgn,int redraw)" },
        { L"CombineRgn", L"gdi32.dll", L"CombineRgn",
          L"int(addr dest,addr src1,addr src2,int op)" },
        { L"CreateRectRgn", L"gdi32.dll", L"CreateRectRgn",
          L"addr(int l,int t,int r,int b)" },
        // the region APIs live in gdi32.dll, NOT user32.dll. Attributing them
        // to user32 made the bare `::CreateRoundRectRgn` resolve to a missing
        // export, and — worse — made official lib\win\region\_.aardio's
        // `::CreateRoundRectRgn := ::Gdi32.api(...)` abort with
        // "找不到 API: CreateRoundRectRgn" (the assignment target is looked
        // up through the same table). 6 official modules depended on it.
        { L"CreateRoundRectRgn", L"gdi32.dll", L"CreateRoundRectRgn",
          L"addr(int l,int t,int r,int b,int w,int h)" },
        { L"OffsetRect", L"user32.dll", L"OffsetRect",
          L"int(addr rect,int dx,int dy)" },
        { L"PtInRect", L"user32.dll", L"PtInRect",
          L"int(addr rect,addr pt)" },
        { L"GetLastError", L"kernel32.dll", L"GetLastError", L"int()" },
        { L"GetModuleHandle", L"kernel32.dll", L"GetModuleHandleW",
          L"addr(str name)" },
        { L"GlobalAlloc", L"kernel32.dll", L"GlobalAlloc",
          L"addr(int flags,int size)" },
        { L"GlobalFree", L"kernel32.dll", L"GlobalFree", L"addr(addr hmem)" },
        { L"GlobalLock", L"kernel32.dll", L"GlobalLock", L"addr(addr hmem)" },
        { L"GlobalUnlock", L"kernel32.dll", L"GlobalUnlock",
          L"int(addr hmem)" },
        { L"GlobalSize", L"kernel32.dll", L"GlobalSize", L"int(addr hmem)" },
        { L"GlobalReAlloc", L"kernel32.dll", L"GlobalReAlloc",
          L"addr(addr hmem,int size,int flags)" },
        // clipboard (CF_HTML / colored-copy support for the IDE). Registered
        // with explicit signatures because the dynamic bare-call path
        // truncates HANDLE returns to 32 bits (capability-gap §1.38.2):
        // GetClipboardData must return addr or the HGLOBAL is unusable.
        { L"OpenClipboard", L"user32.dll", L"OpenClipboard",
          L"int(addr hwnd)" },
        { L"CloseClipboard", L"user32.dll", L"CloseClipboard", L"int()" },
        { L"EmptyClipboard", L"user32.dll", L"EmptyClipboard", L"int()" },
        { L"SetClipboardData", L"user32.dll", L"SetClipboardData",
          L"addr(int format,addr hmem)" },
        { L"GetClipboardData", L"user32.dll", L"GetClipboardData",
          L"addr(int format)" },
        { L"RegisterClipboardFormat", L"user32.dll", L"RegisterClipboardFormatW",
          L"int(str name)" },
    };

    // gdi32 — drawing
    static const SysApiDef s_gdi32[] = {
        { L"CreateCompatibleDC", L"gdi32.dll", L"CreateCompatibleDC",
          L"addr(addr hdc)" },
        { L"CreateCompatibleBitmap", L"gdi32.dll", L"CreateCompatibleBitmap",
          L"addr(addr hdc,int w,int h)" },
        { L"CreateBitmap", L"gdi32.dll", L"CreateBitmap",
          L"addr(int w,int h,int planes,int bits,addr bits)" },
        { L"BitBlt", L"gdi32.dll", L"BitBlt",
          L"int(addr dst,int x,int y,int w,int h,addr src,int sx,int sy,int rop)" },
        { L"PatBlt", L"gdi32.dll", L"PatBlt",
          L"int(addr dst,int x,int y,int w,int h,addr src,int rop)" },
        { L"DeleteDC", L"gdi32.dll", L"DeleteDC", L"int(addr hdc)" },
        { L"DeleteObject", L"gdi32.dll", L"DeleteObject", L"int(addr obj)" },
        { L"SelectObject", L"gdi32.dll", L"SelectObject",
          L"addr(addr hdc,addr obj)" },
        { L"GetObject", L"gdi32.dll", L"GetObjectW",
          L"int(addr obj,int size,addr buf)" },
        { L"GetStockObject", L"gdi32.dll", L"GetStockObject",
          L"addr(int index)" },
        { L"GetDeviceCaps", L"gdi32.dll", L"GetDeviceCaps",
          L"int(addr hdc,int index)" },
        { L"GetPixel", L"gdi32.dll", L"GetPixel",
          L"int(addr hdc,int x,int y)" },
        { L"SetBkColor", L"gdi32.dll", L"SetBkColor", L"int(addr hdc,int color)" },
        { L"SetBkMode", L"gdi32.dll", L"SetBkMode", L"int(addr hdc,int mode)" },
        { L"SetTextColor", L"gdi32.dll", L"SetTextColor",
          L"int(addr hdc,int color)" },
        { L"SetTextAlign", L"gdi32.dll", L"SetTextAlign",
          L"int(addr hdc,int align)" },
        { L"GetTextAlign", L"gdi32.dll", L"GetTextAlign", L"int(addr hdc)" },
        { L"FillRect", L"user32.dll", L"FillRect",
          L"int(addr hdc,addr rect,addr brush)" },
        { L"LineTo", L"gdi32.dll", L"LineTo", L"int(addr hdc,int x,int y)" },
        { L"TextOut", L"gdi32.dll", L"TextOutW",
          L"int(addr hdc,int x,int y,str text,int len)" },
        { L"RoundRect", L"gdi32.dll", L"RoundRect",
          L"int(addr hdc,int l,int t,int r,int b,int w,int h)" },
        { L"CreateSolidBrush", L"gdi32.dll", L"CreateSolidBrush",
          L"addr(int color)" },
        { L"CreatePatternBrush", L"gdi32.dll", L"CreatePatternBrush",
          L"addr(addr bits)" },
        { L"CreatePen", L"gdi32.dll", L"CreatePen",
          L"addr(int style,int width,int color)" },
        { L"CreateFontIndirect", L"gdi32.dll", L"CreateFontIndirectW",
          L"addr(addr logfont)" },
        { L"GetSysColor", L"user32.dll", L"GetSysColor", L"int(int index)" },
        { L"CopyImage", L"user32.dll", L"CopyImage",
          L"addr(addr h,int type,int x,int y,int cx,int cy,int rop)" },
        { L"GdiGradientFill", L"gdi32.dll", L"GradientFill",
          L"int(addr h,int x1,int y1,int x2,int y2,addr colors,int count,int mode)" },
    };

    static const SysApiDef* FindSysApi(const wstring& name)
    {
        for (const auto& d : s_user32)
            if (name == d.name) return &d;
        for (const auto& d : s_gdi32)
            if (name == d.name) return &d;
        return nullptr;
    }

    // Resolve a bare `::Name` system API. Returns a null Value when the
    // name is not one of the known APIs.
    Value ResolveSysApi(Interp* it, const wstring& name)
    {
        if (!it) return Value();
        auto& cache = it->sysApis;
        auto itc = cache.find(name);
        if (itc != cache.end()) return itc->second;

        const SysApiDef* def = FindSysApi(name);
        if (!def) return Value();

        // raw.loadDll(path) -> dll.api(export, sig)
        auto itRaw = it->roots.find(L"raw");
        if (itRaw == it->roots.end() || itRaw->second.t != Value::TableT ||
            !itRaw->second.tab)
            return Value();
        auto itLoad = itRaw->second.tab->map.find(L"loadDll");
        if (itLoad == itRaw->second.tab->map.end() || !itLoad->second.native)
            return Value();

        vector<Value> da{ Value::Str_(def->dll) };
        Value dv = itLoad->second.native(itLoad->second.nativeCtx.get(), da);
        if (dv.t != Value::TableT || !dv.tab) return Value();
        auto itApi = dv.tab->map.find(L"api");
        if (itApi == dv.tab->map.end() || !itApi->second.native) return Value();

        vector<Value> aa{ Value::Str_(def->exportName),
                          Value::Str_(def->sig) };
        Value fn = itApi->second.native(itApi->second.nativeCtx.get(), aa);
        if (fn.t == Value::Null) return Value();
        cache[name] = fn;
        return fn;
    }

}
