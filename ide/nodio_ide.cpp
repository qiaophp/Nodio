// Nodio IDE — native Win32 shell around the Scintilla editing component.
//
// Plain Win32 (no MFC, no WTL, no .NET): a project tree, a Scintilla editor
// with an aardio/nodio highlighter, an output pane and a Run command (F5)
// that shells out to nodio.exe. Files are edited as UTF-8, which is what the
// engine's lexer expects.
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "Scintilla.h"
#include "SciLexer.h"

#pragma comment(lib, "comctl32")
#pragma comment(lib, "comdlg32")
#pragma comment(linker, \
    "\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace
{
constexpr wchar_t kClass[] = L"NodioIdeClass";
constexpr wchar_t kTitle[] = L"Nodio IDE";

constexpr int kTreeId = 200;
constexpr int kEditId = 201;
constexpr int kOutId = 202;
constexpr int kStatusId = 203;

constexpr int kTreeW = 220;
constexpr int kOutH = 170;
constexpr int kGap = 4;

constexpr int kStyleComment = 1;
constexpr int kStyleString = 2;
constexpr int kStyleNumber = 3;
constexpr int kStyleKeyword = 4;
constexpr int kStyleOperator = 5;

// ---------- menu ids ----------
enum : WORD
{
    IDM_FILE_NEW = 1001,
    IDM_FILE_OPEN,
    IDM_FILE_SAVE,
    IDM_FILE_SAVEAS,
    IDM_FILE_OPENFOLDER,
    IDM_FILE_EXIT,
    IDM_EDIT_UNDO = 1011,
    IDM_EDIT_REDO,
    IDM_EDIT_CUT,
    IDM_EDIT_COPY,
    IDM_EDIT_PASTE,
    IDM_EDIT_SELECTALL,
    IDM_RUN_RUN = 1021,
    IDM_RUN_STOP,
    IDM_VIEW_OUTPUT = 1031,
    IDM_HELP_ABOUT = 1041
};

constexpr UINT WM_APP_APPEND = WM_APP + 1;   // output text from the run thread
constexpr UINT WM_APP_RUNEND = WM_APP + 2;   // child process exited

// ---------- globals ----------
HWND g_hwnd = nullptr;
HWND g_tree = nullptr;
HWND g_edit = nullptr;
HWND g_out = nullptr;
HWND g_status = nullptr;
HMENU g_menu = nullptr;
std::wstring g_file;      // current file, empty = untitled
std::wstring g_folder;    // project folder shown in the tree
bool g_bom = false;       // current file carried a UTF-8 BOM
bool g_outVisible = true;

std::atomic<HANDLE> g_child{ nullptr };   // running child process

// ---------- small helpers ----------
LRESULT Sci(HWND h, UINT msg, WPARAM w = 0, LPARAM l = 0)
{
    return SendMessageW(h, msg, w, l);
}

std::wstring Utf8ToWide(const char* s, size_t n)
{
    if (n == 0)
        return L"";
    int cch = MultiByteToWideChar(CP_UTF8, 0, s, (int)n, nullptr, 0);
    if (cch <= 0)
        return L"";
    std::wstring out((size_t)cch, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, (int)n, &out[0], cch);
    return out;
}

std::string WideToUtf8(const std::wstring& s)
{
    if (s.empty())
        return "";
    int cb = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(),
                                 nullptr, 0, nullptr, nullptr);
    std::string out((size_t)cb, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), &out[0], cb,
                        nullptr, nullptr);
    return out;
}

std::wstring FolderOf(const std::wstring& path)
{
    auto p = path.find_last_of(L"\\/");
    return p == std::wstring::npos ? L"" : path.substr(0, p);
}

std::wstring ExeDir()
{
    wchar_t buf[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return FolderOf(buf);
}

void SetStatus(const std::wstring& text)
{
    if (g_status)
        SetWindowTextW(g_status, text.c_str());
}

void UpdateTitle()
{
    std::wstring t = g_file.empty() ? L"未命名" : g_file;
    if (Sci(g_edit, SCI_GETMODIFY))
        t += L" *";
    t += L" — ";
    t += kTitle;
    SetWindowTextW(g_hwnd, t.c_str());
}

// ---------- output pane ----------
void OutClear()
{
    Sci(g_out, SCI_SETREADONLY, 0);
    Sci(g_out, SCI_CLEARALL);
    Sci(g_out, SCI_SETREADONLY, 1);
}

void OutAppend(const std::wstring& text)
{
    if (text.empty())
        return;
    Sci(g_out, SCI_SETREADONLY, 0);
    std::string u8 = WideToUtf8(text);
    Sci(g_out, SCI_APPENDTEXT, (WPARAM)u8.size(), (LPARAM)u8.data());
    Sci(g_out, SCI_SETREADONLY, 1);
    Sci(g_out, SCI_SCROLLTOEND);
}

// ---------- file I/O ----------
bool ReadFileBytes(const std::wstring& path, std::string& out, bool& bom)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f)
        return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.assign((size_t)(n < 0 ? 0 : n), '\0');
    if (n > 0)
        fread(&out[0], 1, (size_t)n, f);
    fclose(f);
    bom = out.size() >= 3 && (unsigned char)out[0] == 0xEF &&
          (unsigned char)out[1] == 0xBB && (unsigned char)out[2] == 0xBF;
    if (bom)
        out.erase(0, 3);
    return true;
}

bool WriteFileBytes(const std::wstring& path, const std::string& data, bool bom)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f)
        return false;
    if (bom)
    {
        const char b[3] = { (char)0xEF, (char)0xBB, (char)0xBF };
        fwrite(b, 1, 3, f);
    }
    if (!data.empty())
        fwrite(data.data(), 1, data.size(), f);
    fclose(f);
    return true;
}

// ---------- aardio keyword set ----------
const std::unordered_set<std::string>& Keywords()
{
    static const std::unordered_set<std::string> k = {
        "and", "begin", "break", "case", "catch", "class", "continue",
        "default", "define", "else", "elseif", "end", "false", "for",
        "function", "global", "if", "import", "in", "lambda", "namespace",
        "not", "null", "or", "owner", "property", "return", "select", "self",
        "this", "true", "try", "var", "while", "with", "ctor", "meta",
        "get", "set", "finally", "gcdata", "λ"
    };
    return k;
}

bool IsIdentStart(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
           (unsigned char)c >= 0x80;
}

bool IsIdentChar(char c)
{
    return IsIdentStart(c) || (c >= '0' && c <= '9');
}

bool IsDigit(char c) { return c >= '0' && c <= '9'; }

bool IsOperator(char c)
{
    return c && strchr("+-*/%<>=!&|^~?:;,()[]{}@#$.", c) != nullptr;
}

// ---------- aardio highlighter (container lexer) ----------
// aardio block comments match by star count: /*** ... ***/ may contain
// "*/" safely, so the opener's star count is carried across lines in the
// per-line state.
void StyleLine(HWND ed, LRESULT line, int& state)
{
    LRESULT len = Sci(ed, SCI_LINELENGTH, line);
    if (len < 0)
        return;
    std::vector<char> buf((size_t)len + 1, '\0');
    LRESULT got = Sci(ed, SCI_GETLINE, (WPARAM)line, (LPARAM)buf.data());
    if (got < 0 || got > len)
        got = len;
    const char* s = buf.data();
    LRESULT base = Sci(ed, SCI_POSITIONFROMLINE, (WPARAM)line);

    struct Run { size_t len; int style; };
    std::vector<Run> runs;
    size_t i = 0;
    const size_t n = (size_t)got;

    auto emit = [&](size_t count, int style)
    {
        if (count == 0)
            return;
        if (!runs.empty() && runs.back().style == style)
            runs.back().len += count;
        else
            runs.push_back({ count, style });
    };

    while (i < n)
    {
        if (state > 0)
        {
            // inside a block comment: look for `state` stars followed by '/'
            size_t j = i;
            bool closed = false;
            while (j < n)
            {
                if (s[j] == '*')
                {
                    size_t k = j;
                    while (k < n && s[k] == '*')
                        k++;
                    if (k < n && s[k] == '/' && (k - j) == (size_t)state)
                    {
                        emit(k + 1 - i, kStyleComment);
                        i = k + 1;
                        state = 0;
                        closed = true;
                        break;
                    }
                    j = k;
                }
                else
                    j++;
            }
            if (!closed)
            {
                emit(n - i, kStyleComment);
                i = n;
            }
            continue;
        }
        if (s[i] == '/' && i + 1 < n && s[i + 1] == '/')
        {
            emit(n - i, kStyleComment);
            i = n;
            continue;
        }
        if (s[i] == '/' && i + 1 < n && s[i + 1] == '*')
        {
            size_t k = i + 1;
            while (k < n && s[k] == '*')
                k++;
            state = (int)(k - i - 1);
            if (state < 1)
                state = 1;
            emit(n - i, kStyleComment);
            i = n;
            continue;
        }
        if (s[i] == '"' || s[i] == '\'')
        {
            char quote = s[i];
            size_t j = i + 1;
            while (j < n)
            {
                if (quote == '"' && s[j] == '\\' && j + 1 < n)
                {
                    j += 2;
                    continue;
                }
                if (quote == '\'' && s[j] == '\'' && j + 1 < n && s[j + 1] == '\'')
                {
                    j += 2;
                    continue;
                }
                if (s[j] == quote)
                {
                    j++;
                    break;
                }
                if (quote == '\'' && s[j] == '\n')
                    break;
                j++;
            }
            emit(j - i, kStyleString);
            i = j;
            continue;
        }
        if (IsDigit(s[i]) ||
            (s[i] == '.' && i + 1 < n && IsDigit(s[i + 1])))
        {
            size_t j = i;
            while (j < n && (IsIdentChar(s[j]) || s[j] == '.' ||
                             ((s[j] == '+' || s[j] == '-') &&
                              (s[j - 1] == 'e' || s[j - 1] == 'E'))))
                j++;
            emit(j - i, kStyleNumber);
            i = j;
            continue;
        }
        if (IsIdentStart(s[i]))
        {
            size_t j = i;
            while (j < n && IsIdentChar(s[j]))
                j++;
            std::string word(s + i, j - i);
            emit(j - i, Keywords().count(word) ? kStyleKeyword : 0);
            i = j;
            continue;
        }
        if (IsOperator(s[i]))
        {
            size_t j = i;
            while (j < n && IsOperator(s[j]))
                j++;
            emit(j - i, kStyleOperator);
            i = j;
            continue;
        }
        emit(1, 0);
        i++;
    }

    Sci(ed, SCI_STARTSTYLING, (WPARAM)base);
    for (auto& r : runs)
        Sci(ed, SCI_SETSTYLING, (WPARAM)r.len, (LPARAM)r.style);
    Sci(ed, SCI_SETLINESTATE, (WPARAM)line, (LPARAM)state);
}

void StyleRange(HWND ed, LRESULT posEnd)
{
    LRESULT posStart = Sci(ed, SCI_GETENDSTYLED);
    LRESULT lineA = Sci(ed, SCI_LINEFROMPOSITION, (WPARAM)posStart);
    LRESULT lineB = Sci(ed, SCI_LINEFROMPOSITION, (WPARAM)posEnd);
    int state = (int)Sci(ed, SCI_GETLINESTATE, (WPARAM)lineA);
    for (LRESULT l = lineA; l <= lineB; l++)
        StyleLine(ed, l, state);
}

// ---------- editor setup ----------
void InitEditor(HWND ed, bool outputPane)
{
    Sci(ed, SCI_SETCODEPAGE, SC_CP_UTF8);
    Sci(ed, SCI_SETEOLMODE, SC_EOL_CRLF);
    Sci(ed, SCI_SETTABWIDTH, 4);
    Sci(ed, SCI_STYLESETFONT, STYLE_DEFAULT, (LPARAM)"Consolas");
    Sci(ed, SCI_STYLESETSIZE, STYLE_DEFAULT, 11);
    Sci(ed, SCI_STYLECLEARALL);
    Sci(ed, SCI_SETMARGINWIDTHN, 1, 0);
    Sci(ed, SCI_SETMARGINWIDTHN, 2, 0);

    if (outputPane)
    {
        Sci(ed, SCI_SETMARGINWIDTHN, 0, 0);
        Sci(ed, SCI_STYLESETBACK, STYLE_DEFAULT, RGB(0x1E, 0x1E, 0x1E));
        Sci(ed, SCI_STYLESETFORE, STYLE_DEFAULT, RGB(0xD4, 0xD4, 0xD4));
        Sci(ed, SCI_SETREADONLY, 1);
        return;
    }

    Sci(ed, SCI_SETMARGINTYPEN, 0, SC_MARGIN_NUMBER);
    Sci(ed, SCI_SETMARGINWIDTHN, 0, 44);
    Sci(ed, SCI_STYLESETBACK, STYLE_DEFAULT, RGB(0xFF, 0xFF, 0xFF));
    Sci(ed, SCI_STYLESETFORE, STYLE_DEFAULT, RGB(0x1A, 0x1A, 0x1A));
    Sci(ed, SCI_STYLESETFORE, kStyleComment, RGB(0x0B, 0x7A, 0x33));
    Sci(ed, SCI_STYLESETFORE, kStyleString, RGB(0xA3, 0x15, 0x15));
    Sci(ed, SCI_STYLESETFORE, kStyleNumber, RGB(0x09, 0x5A, 0xA5));
    Sci(ed, SCI_STYLESETFORE, kStyleKeyword, RGB(0x00, 0x00, 0xC8));
    Sci(ed, SCI_STYLESETBOLD, kStyleKeyword, 1);
    Sci(ed, SCI_STYLESETFORE, kStyleOperator, RGB(0x66, 0x66, 0x66));
    Sci(ed, SCI_SETSELBACK, 1, RGB(0xCC, 0xE4, 0xF7));
    Sci(ed, SCI_SETCARETLINEVISIBLE, 1);
    Sci(ed, SCI_SETCARETLINEBACK, RGB(0xF2, 0xF6, 0xFA));
    Sci(ed, SCI_SETLEXER, SCLEX_CONTAINER);
}

// pick a lexer for the file type; aardio/nodio use the container lexer
void ApplyFileType(HWND ed, const std::wstring& path)
{
    std::wstring ext;
    auto p = path.find_last_of(L".\\/");
    if (p != std::wstring::npos && path[p] == L'.')
    {
        ext = path.substr(p);
        for (auto& c : ext)
            c = (wchar_t)tolower(c);
    }
    if (ext == L".c" || ext == L".cpp" || ext == L".cxx" || ext == L".h" ||
        ext == L".hpp")
    {
        Sci(ed, SCI_SETLEXER, SCLEX_CPP);
        Sci(ed, SCI_SETKEYWORDS, 0,
            (LPARAM)"if else for while do return break continue switch case "
                    "default class struct namespace using template typename "
                    "const static void int char bool float double new delete "
                    "try catch throw public private protected virtual");
        return;
    }
    if (ext == L".json")
    {
        Sci(ed, SCI_SETLEXER, SCLEX_JSON);
        return;
    }
    if (ext == L".py")
    {
        Sci(ed, SCI_SETLEXER, SCLEX_PYTHON);
        return;
    }
    if (ext == L".js")
    {
        Sci(ed, SCI_SETLEXER, SCLEX_CPP);
        Sci(ed, SCI_SETKEYWORDS, 0,
            (LPARAM)"var let const function return if else for while class "
                    "new this typeof null true false");
        return;
    }
    Sci(ed, SCI_SETLEXER, SCLEX_CONTAINER);
}

// ---------- document commands ----------
void LoadFile(const std::wstring& path)
{
    std::string data;
    bool bom = false;
    if (!ReadFileBytes(path, data, bom))
    {
        MessageBoxW(g_hwnd, (L"无法打开文件:\n" + path).c_str(), kTitle,
                    MB_ICONERROR);
        return;
    }
    Sci(g_edit, SCI_SETREADONLY, 0);
    Sci(g_edit, SCI_SETTEXT, 0, (LPARAM)data.c_str());
    Sci(g_edit, SCI_EMPTYUNDOBUFFER);
    Sci(g_edit, SCI_SETSAVEPOINT);
    Sci(g_edit, SCI_GOTOPOS, 0);
    ApplyFileType(g_edit, path);
    g_file = path;
    g_bom = bom;
    UpdateTitle();
    SetStatus(path);
}

bool DoSave(const std::wstring& path)
{
    LRESULT len = Sci(g_edit, SCI_GETLENGTH);
    std::vector<char> buf((size_t)len + 1, '\0');
    Sci(g_edit, SCI_GETTEXT, (WPARAM)len + 1, (LPARAM)buf.data());
    std::string data(buf.data(), (size_t)len);
    if (!WriteFileBytes(path, data, g_bom))
    {
        MessageBoxW(g_hwnd, (L"保存失败:\n" + path).c_str(), kTitle,
                    MB_ICONERROR);
        return false;
    }
    Sci(g_edit, SCI_SETSAVEPOINT);
    g_file = path;
    UpdateTitle();
    SetStatus(L"已保存 " + path);
    return true;
}

bool SaveCurrent(bool askForPath)
{
    if (g_file.empty() || askForPath)
    {
        wchar_t buf[MAX_PATH] = L"";
        OPENFILENAMEW of{};
        of.lStructSize = sizeof(of);
        of.hwndOwner = g_hwnd;
        of.lpstrFile = buf;
        of.nMaxFile = MAX_PATH;
        of.lpstrFilter = L"aardio/nodio 源码\0*.aardio;*.nodio\0所有文件\0*.*\0";
        of.nFilterIndex = 1;
        of.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
        of.lpstrDefExt = L"aardio";
        if (!GetSaveFileNameW(&of))
            return false;
        g_bom = false;
        return DoSave(buf);
    }
    return DoSave(g_file);
}

void NewFile()
{
    Sci(g_edit, SCI_SETREADONLY, 0);
    Sci(g_edit, SCI_CLEARALL);
    Sci(g_edit, SCI_EMPTYUNDOBUFFER);
    Sci(g_edit, SCI_SETSAVEPOINT);
    ApplyFileType(g_edit, L"");
    g_file.clear();
    g_bom = false;
    UpdateTitle();
    SetStatus(L"新建文件");
}

void OpenFile()
{
    wchar_t buf[MAX_PATH] = L"";
    OPENFILENAMEW of{};
    of.lStructSize = sizeof(of);
    of.hwndOwner = g_hwnd;
    of.lpstrFile = buf;
    of.nMaxFile = MAX_PATH;
    of.lpstrFilter = L"aardio/nodio 源码\0*.aardio;*.nodio\0所有文件\0*.*\0";
    of.nFilterIndex = 1;
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&of))
        return;
    LoadFile(buf);
}

// ---------- project tree ----------
void AddTreeFiles(HTREEITEM parent, const std::wstring& dir)
{
    WIN32_FIND_DATAW fd{};
    std::wstring query = dir + L"\\*";
    HANDLE h = FindFirstFileW(query.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do
    {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..")
            continue;
        std::wstring full = dir + L"\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            TVINSERTSTRUCTW ins{};
            ins.hParent = parent;
            ins.hInsertAfter = TVI_LAST;
            ins.item.mask = TVIF_TEXT | TVIF_PARAM;
            ins.item.pszText = &name[0];
            ins.item.lParam = 0;
            HTREEITEM sub = TreeView_InsertItem(g_tree, &ins);
            AddTreeFiles(sub, full);
        }
        else
        {
            auto p = name.find_last_of(L'.');
            std::wstring ext = p == std::wstring::npos ? L"" : name.substr(p);
            for (auto& c : ext)
                c = (wchar_t)tolower(c);
            if (ext != L".aardio" && ext != L".nodio")
                continue;
            TVINSERTSTRUCTW ins{};
            ins.hParent = parent;
            ins.hInsertAfter = TVI_LAST;
            ins.item.mask = TVIF_TEXT | TVIF_PARAM;
            ins.item.pszText = &name[0];
            auto* stored = new std::wstring(full);
            ins.item.lParam = (LPARAM)stored;
            TreeView_InsertItem(g_tree, &ins);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

void OpenFolder()
{
    wchar_t title[] = L"选择项目目录";
    BROWSEINFOW bi{};
    bi.hwndOwner = g_hwnd;
    bi.lpszTitle = title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl)
        return;
    wchar_t path[MAX_PATH] = L"";
    SHGetPathFromIDListW(pidl, path);
    CoTaskMemFree(pidl);
    TreeView_DeleteAllItems(g_tree);
    g_folder = path;
    AddTreeFiles(TVI_ROOT, g_folder);
    TreeView_Expand(g_tree, TreeView_GetRoot(g_tree), TVE_EXPAND);
    SetStatus(L"项目目录 " + g_folder);
}

#pragma comment(lib, "shell32")
#pragma comment(lib, "ole32")

// ---------- run ----------
void StopRun()
{
    HANDLE h = g_child.exchange(nullptr);
    if (h)
        TerminateProcess(h, 1);
}

void RunCurrent()
{
    StopRun();
    if (g_file.empty() && !SaveCurrent(true))
        return;
    if (!g_file.empty() && Sci(g_edit, SCI_GETMODIFY))
        SaveCurrent(false);

    std::wstring exe = ExeDir() + L"\\nodio.exe";
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        OutAppend(L"找不到 nodio.exe（应与 IDE 位于同一目录）: " + exe + L"\n");
        return;
    }

    OutClear();
    OutAppend(L"> 运行 " + g_file + L"\n");

    SetEnvironmentVariableW(L"NODIO_RUN", g_file.c_str());
    SetEnvironmentVariableW(L"NODIO_PAUSE", L"0");

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE hRead = nullptr, hWrite = nullptr;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0))
    {
        OutAppend(L"创建管道失败\n");
        return;
    }
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;
    si.hStdInput = nullptr;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + exe + L"\"";
    std::wstring cwd = FolderOf(g_file);
    if (cwd.empty())
        cwd = ExeDir();
    BOOL ok = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, cwd.c_str(), &si, &pi);
    CloseHandle(hWrite);
    if (!ok)
    {
        CloseHandle(hRead);
        OutAppend(L"启动 nodio.exe 失败\n");
        return;
    }
    g_child = pi.hProcess;
    CloseHandle(pi.hThread);

    std::thread([hRead, hProc = pi.hProcess]()
    {
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(hRead, buf, sizeof(buf), &n, nullptr) && n > 0)
        {
            auto* s = new std::wstring(Utf8ToWide(buf, n));
            PostMessageW(g_hwnd, WM_APP_APPEND, 0, (LPARAM)s);
        }
        CloseHandle(hRead);
        WaitForSingleObject(hProc, INFINITE);
        CloseHandle(hProc);
        PostMessageW(g_hwnd, WM_APP_RUNEND, 0, 0);
    }).detach();
}

// ---------- layout ----------
void Layout()
{
    RECT rc{};
    GetClientRect(g_hwnd, &rc);
    int W = rc.right - rc.left;
    int H = rc.bottom - rc.top;
    int statusH = 0;
    if (g_status)
    {
        RECT sr{};
        GetWindowRect(g_status, &sr);
        statusH = sr.bottom - sr.top;
        MoveWindow(g_status, 0, H - statusH, W, statusH, TRUE);
    }
    int bodyH = H - statusH;
    int outH = g_outVisible ? kOutH : 0;

    MoveWindow(g_tree, 0, 0, kTreeW, bodyH - outH - (outH ? kGap : 0), TRUE);
    MoveWindow(g_edit, kTreeW + kGap, 0, W - kTreeW - kGap,
               bodyH - outH - (outH ? kGap : 0), TRUE);
    MoveWindow(g_out, 0, bodyH - outH, W, outH, TRUE);
    ShowWindow(g_out, g_outVisible ? SW_SHOW : SW_HIDE);
}

// ---------- menus ----------
HMENU BuildMenu()
{
    HMENU m = CreateMenu();
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, IDM_FILE_NEW, L"新建\tCtrl+N");
    AppendMenuW(file, MF_STRING, IDM_FILE_OPEN, L"打开...\tCtrl+O");
    AppendMenuW(file, MF_STRING, IDM_FILE_SAVE, L"保存\tCtrl+S");
    AppendMenuW(file, MF_STRING, IDM_FILE_SAVEAS, L"另存为...");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IDM_FILE_OPENFOLDER, L"打开项目目录...");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IDM_FILE_EXIT, L"退出\tAlt+F4");

    HMENU edit = CreatePopupMenu();
    AppendMenuW(edit, MF_STRING, IDM_EDIT_UNDO, L"撤销\tCtrl+Z");
    AppendMenuW(edit, MF_STRING, IDM_EDIT_REDO, L"重做\tCtrl+Y");
    AppendMenuW(edit, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(edit, MF_STRING, IDM_EDIT_CUT, L"剪切\tCtrl+X");
    AppendMenuW(edit, MF_STRING, IDM_EDIT_COPY, L"复制\tCtrl+C");
    AppendMenuW(edit, MF_STRING, IDM_EDIT_PASTE, L"粘贴\tCtrl+V");
    AppendMenuW(edit, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(edit, MF_STRING, IDM_EDIT_SELECTALL, L"全选\tCtrl+A");

    HMENU run = CreatePopupMenu();
    AppendMenuW(run, MF_STRING, IDM_RUN_RUN, L"运行\tF5");
    AppendMenuW(run, MF_STRING, IDM_RUN_STOP, L"停止");

    HMENU view = CreatePopupMenu();
    AppendMenuW(view, MF_STRING, IDM_VIEW_OUTPUT, L"输出窗口");

    HMENU help = CreatePopupMenu();
    AppendMenuW(help, MF_STRING, IDM_HELP_ABOUT, L"关于");

    AppendMenuW(m, MF_POPUP, (UINT_PTR)file, L"文件");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)edit, L"编辑");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)run, L"运行");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)view, L"视图");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)help, L"帮助");
    return m;
}

// ---------- window proc ----------
LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    switch (msg)
    {
    case WM_CREATE:
        g_menu = BuildMenu();
        SetMenu(h, g_menu);

        g_tree = CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"",
                                 WS_CHILD | WS_VISIBLE | TVS_HASLINES |
                                     TVS_HASBUTTONS | TVS_LINESATROOT |
                                     TVS_SHOWSELALWAYS,
                                 0, 0, 10, 10, h, (HMENU)(INT_PTR)kTreeId,
                                 GetModuleHandleW(nullptr), nullptr);
        g_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"Scintilla", L"",
                                 WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS |
                                     WS_HSCROLL | WS_VSCROLL,
                                 0, 0, 10, 10, h, (HMENU)(INT_PTR)kEditId,
                                 GetModuleHandleW(nullptr), nullptr);
        g_out = CreateWindowExW(WS_EX_CLIENTEDGE, L"Scintilla", L"",
                                WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS |
                                    WS_HSCROLL | WS_VSCROLL,
                                0, 0, 10, 10, h, (HMENU)(INT_PTR)kOutId,
                                GetModuleHandleW(nullptr), nullptr);
        g_status = CreateWindowExW(0, STATUSCLASSNAMEW, L"就绪",
                                   WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                                   0, 0, 10, 10, h, (HMENU)(INT_PTR)kStatusId,
                                   GetModuleHandleW(nullptr), nullptr);
        InitEditor(g_edit, false);
        InitEditor(g_out, true);
        ApplyFileType(g_edit, L"");
        UpdateTitle();
        Layout();
        SetFocus(g_edit);
        return 0;

    case WM_SIZE:
        Layout();
        return 0;

    case WM_APP_APPEND:
    {
        auto* s = (std::wstring*)l;
        OutAppend(*s);
        delete s;
        return 0;
    }
    case WM_APP_RUNEND:
        if (g_child.exchange(nullptr))
            OutAppend(L"\n[进程已退出]\n");
        return 0;

    case WM_NOTIFY:
    {
        auto* nm = (NMHDR*)l;
        if (nm->idFrom == kEditId)
        {
            auto* scn = (SCNotification*)l;
            if (scn->nmhdr.code == SCN_STYLENEEDED)
            {
                if (Sci(g_edit, SCI_GETLEXER) == SCLEX_CONTAINER)
                    StyleRange(g_edit, scn->position);
                return 0;
            }
            if (scn->nmhdr.code == SCN_SAVEPOINTREACHED ||
                scn->nmhdr.code == SCN_SAVEPOINTLEFT)
            {
                UpdateTitle();
                return 0;
            }
        }
        if (nm->idFrom == kTreeId && nm->code == NM_DBLCLK)
        {
            TVITEMW it{};
            it.hItem = TreeView_GetSelection(g_tree);
            it.mask = TVIF_PARAM;
            if (it.hItem && TreeView_GetItem(g_tree, &it) && it.lParam)
                LoadFile(*(std::wstring*)it.lParam);
            return 0;
        }
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(w))
        {
        case IDM_FILE_NEW: NewFile(); return 0;
        case IDM_FILE_OPEN: OpenFile(); return 0;
        case IDM_FILE_SAVE: SaveCurrent(false); return 0;
        case IDM_FILE_SAVEAS: SaveCurrent(true); return 0;
        case IDM_FILE_OPENFOLDER: OpenFolder(); return 0;
        case IDM_FILE_EXIT: PostMessageW(h, WM_CLOSE, 0, 0); return 0;
        case IDM_EDIT_UNDO: Sci(g_edit, SCI_UNDO); return 0;
        case IDM_EDIT_REDO: Sci(g_edit, SCI_REDO); return 0;
        case IDM_EDIT_CUT: Sci(g_edit, SCI_CUT); return 0;
        case IDM_EDIT_COPY: Sci(g_edit, SCI_COPY); return 0;
        case IDM_EDIT_PASTE: Sci(g_edit, SCI_PASTE); return 0;
        case IDM_EDIT_SELECTALL: Sci(g_edit, SCI_SELECTALL); return 0;
        case IDM_RUN_RUN: RunCurrent(); return 0;
        case IDM_RUN_STOP:
            StopRun();
            OutAppend(L"[已停止]\n");
            return 0;
        case IDM_VIEW_OUTPUT:
            g_outVisible = !g_outVisible;
            CheckMenuItem(g_menu, IDM_VIEW_OUTPUT,
                          MF_BYCOMMAND |
                              (g_outVisible ? MF_CHECKED : MF_UNCHECKED));
            Layout();
            return 0;
        case IDM_HELP_ABOUT:
            MessageBoxW(h,
                        L"Nodio IDE\n\n原生 Win32 外壳 + Scintilla 编辑组件。\n"
                        L"F5 运行当前文件，输出显示在下方输出窗口。",
                        L"关于 Nodio IDE", MB_OK);
            return 0;
        }
        return 0;

    case WM_KEYDOWN:
        if (w == VK_F5)
        {
            RunCurrent();
            return 0;
        }
        return 0;

    case WM_DESTROY:
        StopRun();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}
} // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR cmdLine, int showCmd)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);

    INITCOMMONCONTROLSEX ic{};
    ic.dwSize = sizeof(ic);
    ic.dwICC = ICC_TREEVIEW_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&ic);

    Scintilla_RegisterClasses(hInst);

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = kClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    if (!RegisterClassW(&wc))
        return 1;

    g_hwnd = CreateWindowExW(0, kClass, kTitle,
                             WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, 1000, 700, nullptr,
                             nullptr, hInst, nullptr);
    if (!g_hwnd)
        return 1;
    CheckMenuItem(g_menu, IDM_VIEW_OUTPUT, MF_BYCOMMAND | MF_CHECKED);

    ShowWindow(g_hwnd, showCmd);
    UpdateWindow(g_hwnd);

    // an optional file argument opens straight into the editor
    if (cmdLine && *cmdLine)
    {
        std::wstring arg = cmdLine;
        if (arg.size() >= 2 && arg.front() == L'"')
        {
            arg = arg.substr(1);
            if (!arg.empty() && arg.back() == L'"')
                arg.pop_back();
        }
        if (!arg.empty())
        {
            LoadFile(arg);
            if (!FolderOf(arg).empty())
            {
                g_folder = FolderOf(arg);
                AddTreeFiles(TVI_ROOT, g_folder);
                TreeView_Expand(g_tree, TreeView_GetRoot(g_tree), TVE_EXPAND);
            }
        }
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
