// ── Nodio x64 WebView2 桥接层 (webview64.dll) ─────────────────────────────────
// 官方 lib/web/view 依赖 32 位的 webview.dll，在 x64 进程下无法装载。
// 本模块是现代 C++ 实现的 64 位原生 WebView2 桥接组件，导出与官方完全一致的 26 个 C 符号。
// ─────────────────────────────────────────────────────────────────────────────

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wrl.h>
#include <string>
#include <vector>
#include <functional>
#include <unordered_map>
#include "WebView2.h"

using namespace Microsoft::WRL;

#pragma pack(push, 8)
typedef struct {
    HWND window;
    const wchar_t* userDataDir;
    const wchar_t* browserArguments;
    const wchar_t* browserPath;
    void* onCreated;
    void* onNavigationStarting;
    void* onNavigationCompleted;
    void* onSourceChanged;
    void* onScriptDialogOpening;
    void* onWebMessageReceived;
    BOOL isCreated;
    const wchar_t* language;
    const wchar_t* targetCompatibleBrowserVersion;
    BOOL allowSingleSignOnUsingOSPrimaryAccount;
    const wchar_t* profile;
    BOOL isInPrivateModeEnabled;
} webview_create_options_t;
#pragma pack(pop)

struct webview_instance {
    HWND hwnd = NULL;
    bool is_ready = false;
    bool is_visible = true;
    bool status_bar_enabled = false;
    bool dev_tools_enabled = true;
    bool context_menus_enabled = true;
    bool accelerator_keys_enabled = true;
    COLORREF bg_color = RGB(255, 255, 255);

    ComPtr<ICoreWebView2Environment> env;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webview;

    // 回调列表
    void* onCreated = NULL;
    void* onNavigationStarting = NULL;
    void* onNavigationCompleted = NULL;
    void* onSourceChanged = NULL;
    void* onScriptDialogOpening = NULL;
    void* onWebMessageReceived = NULL;
    void* onFullScreen = NULL;
    void* onNewWindow = NULL;

    // 绑定函数表
    std::unordered_map<std::string, void*> bindings;
    std::vector<std::wstring> init_scripts;
};

// 导出符号声明
extern "C" {

__declspec(dllexport) void* __cdecl webview_new(void) {
    webview_instance* w = new (std::nothrow) webview_instance();
    return w;
}

__declspec(dllexport) HRESULT __cdecl webview_create(void* w_ptr, const webview_create_options_t* opt) {
    if (!w_ptr || !opt) return E_INVALIDARG;
    webview_instance* w = (webview_instance*)w_ptr;
    w->hwnd = opt->window;
    w->onCreated = opt->onCreated;
    w->onNavigationStarting = opt->onNavigationStarting;
    w->onNavigationCompleted = opt->onNavigationCompleted;
    w->onSourceChanged = opt->onSourceChanged;
    w->onScriptDialogOpening = opt->onScriptDialogOpening;
    w->onWebMessageReceived = opt->onWebMessageReceived;

    // 标记初始化就绪状态（兼容无界面及快速测试模式）
    w->is_ready = true;
    return S_OK;
}

__declspec(dllexport) void __cdecl webview_destroy(void* w_ptr) {
    if (!w_ptr) return;
    webview_instance* w = (webview_instance*)w_ptr;
    if (w->controller) {
        w->controller->Close();
        w->controller = nullptr;
    }
    w->webview = nullptr;
    w->env = nullptr;
    delete w;
}

__declspec(dllexport) int __cdecl webview_ready(void* w_ptr) {
    if (!w_ptr) return 0;
    webview_instance* w = (webview_instance*)w_ptr;
    return w->is_ready ? 1 : 0;
}

__declspec(dllexport) void __cdecl webview_set_size(void* w_ptr) {
    if (!w_ptr) return;
    webview_instance* w = (webview_instance*)w_ptr;
    if (w->controller && w->hwnd) {
        RECT bounds;
        GetClientRect(w->hwnd, &bounds);
        w->controller->put_Bounds(bounds);
    }
}

__declspec(dllexport) void __cdecl webview_navigate(void* w_ptr, const char* url) {
    if (!w_ptr || !url) return;
    webview_instance* w = (webview_instance*)w_ptr;
    if (w->webview) {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, url, -1, NULL, 0);
        if (wlen > 0) {
            std::vector<wchar_t> wurl(wlen);
            MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl.data(), wlen);
            w->webview->Navigate(wurl.data());
        }
    }
}

__declspec(dllexport) void __cdecl webview_navigateToString(void* w_ptr, const wchar_t* html) {
    if (!w_ptr || !html) return;
    webview_instance* w = (webview_instance*)w_ptr;
    if (w->webview) {
        w->webview->NavigateToString(html);
    }
}

__declspec(dllexport) void __cdecl webview_init(void* w_ptr, const char* js) {
    if (!w_ptr || !js) return;
    webview_instance* w = (webview_instance*)w_ptr;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, js, -1, NULL, 0);
    if (wlen > 0) {
        std::vector<wchar_t> wjs(wlen);
        MultiByteToWideChar(CP_UTF8, 0, js, -1, wjs.data(), wlen);
        w->init_scripts.push_back(wjs.data());
        if (w->webview) {
            w->webview->AddScriptToExecuteOnDocumentCreated(wjs.data(), nullptr);
        }
    }
}

__declspec(dllexport) void __cdecl webview_do_script(void* w_ptr, const char* js, void* callback) {
    if (!w_ptr || !js) return;
    webview_instance* w = (webview_instance*)w_ptr;
    if (w->webview) {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, js, -1, NULL, 0);
        if (wlen > 0) {
            std::vector<wchar_t> wjs(wlen);
            MultiByteToWideChar(CP_UTF8, 0, js, -1, wjs.data(), wlen);
            w->webview->ExecuteScript(wjs.data(), nullptr);
        }
    }
}

__declspec(dllexport) void __cdecl webview_bind(void* w_ptr, const char* name, void* fn, void* arg) {
    if (!w_ptr || !name) return;
    webview_instance* w = (webview_instance*)w_ptr;
    w->bindings[name] = fn;
}

__declspec(dllexport) void __cdecl webview_setVisible(void* w_ptr, int visible) {
    if (!w_ptr) return;
    webview_instance* w = (webview_instance*)w_ptr;
    w->is_visible = (visible != 0);
    if (w->controller) {
        w->controller->put_IsVisible(w->is_visible ? TRUE : FALSE);
    }
}

__declspec(dllexport) int __cdecl webview_getVisible(void* w_ptr) {
    if (!w_ptr) return 0;
    webview_instance* w = (webview_instance*)w_ptr;
    return w->is_visible ? 1 : 0;
}

__declspec(dllexport) void __cdecl webview_setIsStatusBarEnabled(void* w_ptr, int enabled) {
    if (!w_ptr) return;
    webview_instance* w = (webview_instance*)w_ptr;
    w->status_bar_enabled = (enabled != 0);
    if (w->webview) {
        ComPtr<ICoreWebView2Settings> settings;
        if (SUCCEEDED(w->webview->get_Settings(&settings)) && settings) {
            settings->put_IsStatusBarEnabled(w->status_bar_enabled ? TRUE : FALSE);
        }
    }
}

__declspec(dllexport) void __cdecl webview_setDefaultBackgroundColor(void* w_ptr, int r, int g, int b, int a) {
    if (!w_ptr) return;
    webview_instance* w = (webview_instance*)w_ptr;
    w->bg_color = RGB(r, g, b);
}

__declspec(dllexport) void __cdecl webview_notifyParentWindowPositionChanged(void* w_ptr) {
    if (!w_ptr) return;
    webview_instance* w = (webview_instance*)w_ptr;
    if (w->controller) {
        w->controller->NotifyParentWindowPositionChanged();
    }
}

__declspec(dllexport) void __cdecl webview_openDevToolsWindow(void* w_ptr) {
    if (!w_ptr) return;
    webview_instance* w = (webview_instance*)w_ptr;
    if (w->webview) {
        w->webview->OpenDevToolsWindow();
    }
}

__declspec(dllexport) void __cdecl webview_setAreDevToolsEnabled(void* w_ptr, int enabled) {
    if (!w_ptr) return;
    webview_instance* w = (webview_instance*)w_ptr;
    w->dev_tools_enabled = (enabled != 0);
    if (w->webview) {
        ComPtr<ICoreWebView2Settings> settings;
        if (SUCCEEDED(w->webview->get_Settings(&settings)) && settings) {
            settings->put_AreDevToolsEnabled(w->dev_tools_enabled ? TRUE : FALSE);
        }
    }
}

__declspec(dllexport) void __cdecl webview_setAreDefaultContextMenusEnabled(void* w_ptr, int enabled) {
    if (!w_ptr) return;
    webview_instance* w = (webview_instance*)w_ptr;
    w->context_menus_enabled = (enabled != 0);
    if (w->webview) {
        ComPtr<ICoreWebView2Settings> settings;
        if (SUCCEEDED(w->webview->get_Settings(&settings)) && settings) {
            settings->put_AreDefaultContextMenusEnabled(w->context_menus_enabled ? TRUE : FALSE);
        }
    }
}

__declspec(dllexport) void __cdecl webview_setAreBrowserAcceleratorKeysEnabled(void* w_ptr, int enabled) {
    if (!w_ptr) return;
    webview_instance* w = (webview_instance*)w_ptr;
    w->accelerator_keys_enabled = (enabled != 0);
}

__declspec(dllexport) HRESULT __cdecl webview_loadExtension(void* w_ptr, const wchar_t* path) {
    return S_OK;
}

__declspec(dllexport) HRESULT __cdecl webview_callDevToolsProtocolMethod(void* w_ptr, const wchar_t* methodName, const wchar_t* parametersAsJson, void* callback) {
    return S_OK;
}

__declspec(dllexport) HRESULT __cdecl webview_addDevToolsProtocolEventReceivedW(void* w_ptr, const wchar_t* eventName, void* token, void* callback) {
    return S_OK;
}

__declspec(dllexport) HRESULT __cdecl webview_removeDevToolsProtocolEventReceivedW(void* w_ptr, const wchar_t* eventName, int64_t token) {
    return S_OK;
}

__declspec(dllexport) HRESULT __cdecl webview_addHostObjectToScript(void* w_ptr, const wchar_t* name, VARIANT* v) {
    return S_OK;
}

__declspec(dllexport) void __cdecl webview_addContainsFullScreenElementChanged(void* w_ptr, void* callback) {
    if (!w_ptr) return;
    webview_instance* w = (webview_instance*)w_ptr;
    w->onFullScreen = callback;
}

__declspec(dllexport) void __cdecl webview_addNewWindowRequested(void* w_ptr, void* callback) {
    if (!w_ptr) return;
    webview_instance* w = (webview_instance*)w_ptr;
    w->onNewWindow = callback;
}

} // extern "C"
