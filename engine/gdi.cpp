// gdi.cpp — WM_PAINT device-context tables, FormState registry and
// the form window class registration.
#include "engine/common.h"
#include "engine/interp.h"

namespace NodioVM
{
    std::map<HWND, FormState*> g_forms;

    struct GdiCtx
    {
        HDC hdc = nullptr;
        COLORREF clrPen = RGB(0, 0, 0);
        int nPenW = 1;
        COLORREF clrBrush = RGB(255, 255, 255);
        COLORREF clrText = RGB(0, 0, 0);
        int nBkMode = TRANSPARENT;
        // round 43: font created lazily by setFont(); null = DEFAULT_GUI_FONT
        HFONT hFont = nullptr;
        int nFontH = 0;
        wstring sFace;
        bool bBold = false;
        bool bItalic = false;
    };

    Value MakeGdiTable(const std::shared_ptr<GdiCtx>& g)
    {
        auto tb = std::make_shared<Table>();
        tb->map[L"setPen"] = NF([g](vector<Value>& a) -> Value {
            g->clrPen = a.empty() ? g->clrPen : (COLORREF)(INT_PTR)BNum(a[0]);
            g->nPenW = a.size() > 1 ? (int)BNum(a[1]) : 1;
            if (g->nPenW < 1) g->nPenW = 1;
            return Value(); });
        tb->map[L"setBrush"] = NF([g](vector<Value>& a) -> Value {
            if (!a.empty()) g->clrBrush = (COLORREF)(INT_PTR)BNum(a[0]);
            return Value(); });
        tb->map[L"setTextColor"] = NF([g](vector<Value>& a) -> Value {
            if (!a.empty()) g->clrText = (COLORREF)(INT_PTR)BNum(a[0]);
            return Value(); });
        // round 43: setFont(height, face, bold, italic) — subsequent
        // text() uses the font; height is positive pixel height
        tb->map[L"setFont"] = NF([g](vector<Value>& a) -> Value {
            g->nFontH = a.empty() ? 0 : (int)BNum(a[0]);
            g->sFace = a.size() > 1 ? BStr(a[1]) : L"";
            g->bBold = a.size() > 2 && BNum(a[2]) != 0;
            g->bItalic = a.size() > 3 && BNum(a[3]) != 0;
            if (g->hFont) { DeleteObject(g->hFont); g->hFont = nullptr; }
            if (g->nFontH > 0)
            {
                g->hFont = CreateFontW(
                    -g->nFontH, 0, 0, 0,
                    g->bBold ? FW_BOLD : FW_NORMAL,
                    g->bItalic ? TRUE : FALSE, 0, 0, DEFAULT_CHARSET,
                    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                    CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                    g->sFace.empty() ? L"Microsoft YaHei"
                                     : g->sFace.c_str());
            }
            return Value(); });
        tb->map[L"setBkMode"] = NF([g](vector<Value>& a) -> Value {
            // 1 = transparent, 2 = opaque
            g->nBkMode = (!a.empty() && BNum(a[0]) == 2) ? OPAQUE
                                                         : TRANSPARENT;
            return Value(); });
        tb->map[L"line"] = NF([g](vector<Value>& a) -> Value {
            if (a.size() < 4) return Value();
            HPEN hPen = CreatePen(PS_SOLID, g->nPenW, g->clrPen);
            HGDIOBJ hOld = SelectObject(g->hdc, hPen);
            MoveToEx(g->hdc, (int)BNum(a[0]), (int)BNum(a[1]), nullptr);
            LineTo(g->hdc, (int)BNum(a[2]), (int)BNum(a[3]));
            SelectObject(g->hdc, hOld);
            DeleteObject(hPen);
            return Value(); });
        tb->map[L"rect"] = NF([g](vector<Value>& a) -> Value {
            if (a.size() < 4) return Value();
            HPEN hPen = CreatePen(PS_SOLID, g->nPenW, g->clrPen);
            HBRUSH hBr = CreateSolidBrush(g->clrBrush);
            HGDIOBJ hOldP = SelectObject(g->hdc, hPen);
            HGDIOBJ hOldB = SelectObject(g->hdc, hBr);
            Rectangle(g->hdc, (int)BNum(a[0]), (int)BNum(a[1]),
                      (int)BNum(a[2]), (int)BNum(a[3]));
            SelectObject(g->hdc, hOldP);
            SelectObject(g->hdc, hOldB);
            DeleteObject(hPen);
            DeleteObject(hBr);
            return Value(); });
        tb->map[L"ellipse"] = NF([g](vector<Value>& a) -> Value {
            if (a.size() < 4) return Value();
            HPEN hPen = CreatePen(PS_SOLID, g->nPenW, g->clrPen);
            HBRUSH hBr = CreateSolidBrush(g->clrBrush);
            HGDIOBJ hOldP = SelectObject(g->hdc, hPen);
            HGDIOBJ hOldB = SelectObject(g->hdc, hBr);
            Ellipse(g->hdc, (int)BNum(a[0]), (int)BNum(a[1]),
                    (int)BNum(a[2]), (int)BNum(a[3]));
            SelectObject(g->hdc, hOldP);
            SelectObject(g->hdc, hOldB);
            DeleteObject(hPen);
            DeleteObject(hBr);
            return Value(); });
        tb->map[L"fillRect"] = NF([g](vector<Value>& a) -> Value {
            if (a.size() < 4) return Value();
            RECT rc = { (int)BNum(a[0]), (int)BNum(a[1]),
                        (int)BNum(a[2]), (int)BNum(a[3]) };
            HBRUSH hBr = CreateSolidBrush(
                a.size() > 4 ? (COLORREF)(INT_PTR)BNum(a[4]) : g->clrBrush);
            FillRect(g->hdc, &rc, hBr);
            DeleteObject(hBr);
            return Value(); });
        tb->map[L"clear"] = NF([g](vector<Value>& a) -> Value {
            RECT rc;
            GetClientRect(WindowFromDC(g->hdc), &rc);
            HBRUSH hBr = CreateSolidBrush(
                a.empty() ? g->clrBrush : (COLORREF)(INT_PTR)BNum(a[0]));
            FillRect(g->hdc, &rc, hBr);
            DeleteObject(hBr);
            return Value(); });
        tb->map[L"text"] = NF([g](vector<Value>& a) -> Value {
            if (a.size() < 3) return Value();
            SetTextColor(g->hdc, g->clrText);
            SetBkMode(g->hdc, g->nBkMode);
            HFONT hF = g->hFont
                ? g->hFont
                : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
            HGDIOBJ hOld = SelectObject(g->hdc, hF);
            wstring s = BStr(a[2]);
            TextOutW(g->hdc, (int)BNum(a[0]), (int)BNum(a[1]),
                     s.c_str(), (int)s.size());
            SelectObject(g->hdc, hOld);
            return Value(); });
        tb->map[L"pixel"] = NF([g](vector<Value>& a) -> Value {
            if (a.size() < 2) return Value();
            COLORREF c = GetPixel(g->hdc, (int)BNum(a[0]), (int)BNum(a[1]));
            return Value::Num_((double)c); });
        tb->map[L"width"] = NF([g](vector<Value>&) -> Value {
            HWND hW = WindowFromDC(g->hdc);
            RECT rc;
            GetClientRect(hW, &rc);
            return Value::Num_((double)(rc.right - rc.left)); });
        tb->map[L"height"] = NF([g](vector<Value>&) -> Value {
            HWND hW = WindowFromDC(g->hdc);
            RECT rc;
            GetClientRect(hW, &rc);
            return Value::Num_((double)(rc.bottom - rc.top)); });
        return Value::Table_(tb);
    }

            LRESULT CALLBACK FormWndProc(HWND hwnd, UINT msg, WPARAM wp,
                                        LPARAM lp)
    {
        auto it = g_forms.find(hwnd);
        FormState* fs = it != g_forms.end() ? it->second : nullptr;

        // Form-level custom wndproc interceptor:
        // winform.wndproc = function(hwnd, message, wParam, lParam)
        if (fs && fs->it && fs->formVal.tab)
        {
            auto iw = fs->formVal.tab->map.find(L"wndproc");
            if (iw != fs->formVal.tab->map.end() &&
                (iw->second.t == Value::Func || iw->second.native))
            {
                vector<Value> wpArgs{
                    Value::Num_((double)(INT_PTR)hwnd),
                    Value::Num_((double)msg),
                    Value::Num_((double)(INT_PTR)wp),
                    Value::Num_((double)(INT_PTR)lp)
                };
                Value ret = fs->it->CallVmFunc(iw->second, wpArgs);
                if (ret.t == Value::Num) return (LRESULT)(INT_PTR)ret.n;
                if (ret.t == Value::Bool) return (LRESULT)(ret.b ? 1 : 0);
            }
        }

        switch (msg)
        {
        case WM_PAINT:
            if (fs && fs->it)
            {
                PAINTSTRUCT psP;
                HDC hdcP = BeginPaint(hwnd, &psP);
                auto if2 = fs->formVal.tab
                    ? fs->formVal.tab->map.find(L"onpaint")
                    : fs->formVal.tab->map.end();
                if (fs->formVal.tab && if2 != fs->formVal.tab->map.end() &&
                    (if2->second.t == Value::Func || if2->second.native))
                {
                    auto g = std::make_shared<GdiCtx>();
                    g->hdc = hdcP;
                    vector<Value> args{ MakeGdiTable(g) };
                    fs->bInPaint = true;
                    fs->it->CallVmFunc(if2->second, args);
                    fs->bInPaint = false;
                }
                EndPaint(hwnd, &psP);
                return 0;
            }
            break;
        case WM_TIMER:
            if (fs && fs->it && fs->formVal.tab)
            {
                auto it2 = fs->formVal.tab->map.find(L"ontimer");
                if (it2 != fs->formVal.tab->map.end() &&
                    (it2->second.t == Value::Func || it2->second.native))
                {
                    vector<Value> args{ Value::Num_((double)(int)wp) };
                    fs->it->CallVmFunc(it2->second, args);
                }
                return 0;
            }
            break;
        case WM_COMMAND:
            if (fs && fs->it)
            {
                WORD id = LOWORD(wp), code = HIWORD(wp);
                HWND hCtrlWnd = (HWND)lp;
                // round 41: menu commands carry lParam==0 and live in the
                // 5000+ id range — check them before control dispatch
                auto im = fs->menus.find(id);
                if (im != fs->menus.end())
                {
                    vector<Value> noArgs;
                    fs->it->CallVmFunc(im->second, noArgs);
                }
                else
                {
                    Value ctrlVal;
                    auto ic = fs->ctrls.find(id);
                    if (ic != fs->ctrls.end())
                    {
                        ctrlVal = ic->second;
                    }
                    else if (hCtrlWnd)
                    {
                        auto ir = fs->hCtrlRev.find(hCtrlWnd);
                        if (ir != fs->hCtrlRev.end())
                        {
                            auto itc = fs->ctrls.find(ir->second);
                            if (itc != fs->ctrls.end())
                            {
                                ctrlVal = itc->second;
                                id = (WORD)ir->second;
                            }
                        }
                    }

                    bool bConsumed = false;
                    bool bStopBubble = false;
                    if (ctrlVal.tab)
                    {
                        auto ioc = ctrlVal.tab->map.find(L"oncommand");
                        if (ioc != ctrlVal.tab->map.end() &&
                            (ioc->second.t == Value::Func || ioc->second.native))
                        {
                            vector<Value> cArgs{ Value::Num_((double)id), Value::Num_((double)code) };
                            Value ret = fs->it->CallVmFunc(ioc->second, cArgs);
                            bConsumed = true;
                            if (ret.t == Value::Num) return (LRESULT)(INT_PTR)ret.n;
                            if (ret.t == Value::Bool && !ret.b) bStopBubble = true;
                        }
                    }

                    if (!bConsumed)
                    {
                        bool bChange = code == EN_CHANGE && fs->changes.count(id);
                        bool bClick = code == BN_CLICKED && fs->callbacks.count(id);
                        bool bSel = code == LBN_SELCHANGE && fs->selchgs.count(id);
                        if (bChange)
                        {
                            Value f = fs->changes[id];
                            vector<Value> cArgs{ Value::Num_((double)id), Value::Num_((double)code) };
                            fs->it->CallVmFunc(f, cArgs);
                        }
                        else if (bClick)
                        {
                            Value f = fs->callbacks[id];
                            vector<Value> cArgs{ Value::Num_((double)id), Value::Num_((double)code) };
                            fs->it->CallVmFunc(f, cArgs);
                        }
                        else if (bSel)
                        {
                            Value f = fs->selchgs[id];
                            vector<Value> cArgs{ Value::Num_((double)id), Value::Num_((double)code) };
                            fs->it->CallVmFunc(f, cArgs);
                        }
                    }

                    // Form-level oncommand reflection: winform.oncommand(id, event)
                    // Dispatched unless explicitly stopped by child control returning false
                    if (!bStopBubble && fs->formVal.tab)
                    {
                        auto ifoc = fs->formVal.tab->map.find(L"oncommand");
                        if (ifoc != fs->formVal.tab->map.end() &&
                            (ifoc->second.t == Value::Func || ifoc->second.native))
                        {
                            vector<Value> fArgs{ Value::Num_((double)id), Value::Num_((double)code) };
                            Value ret = fs->it->CallVmFunc(ifoc->second, fArgs);
                            if (ret.t == Value::Num) return (LRESULT)(INT_PTR)ret.n;
                            if (ret.t == Value::Bool && !ret.b) return 0;
                        }
                    }
                }
            }
            return 0;
        case WM_NOTIFY:
            if (fs && fs->it && lp)
            {
                LPNMHDR nmhdr = (LPNMHDR)lp;
                HWND hFrom = nmhdr->hwndFrom;
                UINT_PTR idFrom = nmhdr->idFrom;
                int code = nmhdr->code;

                Value ctrlVal;
                auto ic = fs->ctrls.find((int)idFrom);
                if (ic != fs->ctrls.end())
                {
                    ctrlVal = ic->second;
                }
                else if (hFrom)
                {
                    auto ir = fs->hCtrlRev.find(hFrom);
                    if (ir != fs->hCtrlRev.end())
                    {
                        auto itc = fs->ctrls.find(ir->second);
                        if (itc != fs->ctrls.end())
                            ctrlVal = itc->second;
                    }
                }

                bool bStopBubble = false;
                if (ctrlVal.tab)
                {
                    auto ion = ctrlVal.tab->map.find(L"onnotify");
                    if (ion != ctrlVal.tab->map.end() &&
                        (ion->second.t == Value::Func || ion->second.native))
                    {
                        vector<Value> nArgs{
                            Value::Num_((double)idFrom),
                            Value::Num_((double)code),
                            Value::Num_((double)(INT_PTR)lp)
                        };
                        Value ret = fs->it->CallVmFunc(ion->second, nArgs);
                        if (ret.t == Value::Num) return (LRESULT)(INT_PTR)ret.n;
                        if (ret.t == Value::Bool)
                        {
                            if (!ret.b) bStopBubble = true;
                            else return 1;
                        }
                    }
                }

                if (!bStopBubble && fs->formVal.tab)
                {
                    auto ifon = fs->formVal.tab->map.find(L"onnotify");
                    if (ifon != fs->formVal.tab->map.end() &&
                        (ifon->second.t == Value::Func || ifon->second.native))
                    {
                        vector<Value> fnArgs{
                            Value::Num_((double)idFrom),
                            Value::Num_((double)code),
                            Value::Num_((double)(INT_PTR)lp)
                        };
                        Value ret = fs->it->CallVmFunc(ifon->second, fnArgs);
                        if (ret.t == Value::Num) return (LRESULT)(INT_PTR)ret.n;
                        if (ret.t == Value::Bool) return (LRESULT)(ret.b ? 1 : 0);
                    }
                }
            }
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (fs)
            {
                fs->bModalDone = true;
                PostQuitMessage(0);
            }
            return 0;
        case WM_NCDESTROY:
            if (fs)
            {
                g_forms.erase(hwnd);
                delete fs;
            }
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    bool RegisterFormClass()
    {
        // STATIC: the window class is registered once per process. A local
        // flag would try to register it again on every win.form() call and
        // RegisterClassExW then fails with ERROR_CLASS_ALREADY_EXISTS —
        // so the SECOND form in a script could never be created.
        static bool bDone = false;
        if (bDone)
            return true;
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = FormWndProc;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = GetNodioConfig().windowClass.c_str();
        bDone = RegisterClassExW(&wc) != 0;
        return bDone;
    }

}
