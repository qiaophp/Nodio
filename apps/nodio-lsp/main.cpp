// apps/nodio-lsp/main.cpp — Nodio Language Server (LSP over stdio).
//
// Backed by the engine's lexer + parser (the same front end the VM uses):
//   - textDocument sync (didOpen/didChange/didClose) keeps source per URI
//   - diagnostics   : full Lex + Parse, error mapped to its line/range
//   - completion    : keyword + in-document symbol candidates, '.' member
//                     context adds namespace members seen in the document
//   - hover         : the word under the cursor, with symbol kind info
//   - documentSymbol: namespaces / classes / functions / variables from AST
#include "apps/jsonrpc_stdio.h"

#include "engine/common.h"
#include "engine/parser.h"

#include <algorithm>
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#include <map>
#include <set>

using namespace nodio::rpc;
using namespace NodioVM;

// ---------------- narrow/wide + JSON helpers ----------------

static std::string Utf8(const wstring& w)
{
    if (w.empty()) return {};
    int cb = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                 nullptr, 0, nullptr, nullptr);
    if (cb < 0) cb = 0;
    std::string out((size_t)cb, 0);
    if (cb > 0)
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0],
                            cb, nullptr, nullptr);
    return out;
}

static wstring Wide(const std::string& u8)
{
    if (u8.empty()) return {};
    int cw = MultiByteToWideChar(CP_UTF8, 0, u8.data(), (int)u8.size(),
                                 nullptr, 0);
    if (cw < 0) cw = 0;
    wstring out((size_t)cw, 0);
    if (cw > 0)
        MultiByteToWideChar(CP_UTF8, 0, u8.data(), (int)u8.size(), &out[0],
                            cw);
    return out;
}

static std::string JStr(const wstring& w)
{
    // Pure-ASCII JSON string escaping: control chars AND every non-ASCII
    // codepoint become \uXXXX. The Nodio VM's string.mid() mixes units
    // (start = character index, count = byte limit), so any raw multi-byte
    // UTF-8 on the wire makes Content-Length (a BYTE count) impossible to
    // honor from the aardio side. Escaped output keeps bytes == chars, so
    // one number serves both.
    std::string u8 = Utf8(w);
    // decode u8 back to codepoints for escaping
    std::string out = "\"";
    size_t i = 0;
    while (i < u8.size())
    {
        unsigned char c = (unsigned char)u8[i];
        if (c < 0x80)
        {
            switch (c)
            {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20)
                {
                    char buf[8];
                    sprintf(buf, "\\u%04x", c);
                    out += buf;
                }
                else out += (char)c;
            }
            i++;
            continue;
        }
        // UTF-8 lead byte -> codepoint
        unsigned cp = 0;
        int n = 0;
        if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; n = 4; }
        else { i++; continue; }   // stray continuation byte: drop
        if (i + n > u8.size()) break;
        bool ok = true;
        for (int k = 1; k < n; k++)
        {
            unsigned char cc = (unsigned char)u8[i + k];
            if ((cc & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (!ok) { i++; continue; }
        if (cp >= 0x10000)
        {
            // surrogate pair
            unsigned v = cp - 0x10000;
            char buf[16];
            sprintf(buf, "\\u%04x\\u%04x",
                    0xD800 + (v >> 10), 0xDC00 + (v & 0x3FF));
            out += buf;
        }
        else
        {
            char buf[8];
            sprintf(buf, "\\u%04x", cp);
            out += buf;
        }
        i += n;
    }
    return out + "\"";
}

// LSP position/line are 0-based; engine lines are 1-based.
static std::string Range(int line1, int col0, int endLine1, int endCol0)
{
    return "{\"start\":{\"line\":" + std::to_string(line1 - 1) +
           ",\"character\":" + std::to_string(col0) +
           "},\"end\":{\"line\":" + std::to_string(endLine1 - 1) +
           ",\"character\":" + std::to_string(endCol0) + "}}";
}

static void Respond(const std::string& id, const std::string& result)
{
    if (id.empty()) return;
    WriteMessage("{\"jsonrpc\":\"2.0\",\"id\":" + id +
                 ",\"result\":" + result + "}");
}

static void Notify(const char* method, const std::string& params)
{
    WriteMessage(std::string("{\"jsonrpc\":\"2.0\",\"method\":\"") + method +
                 "\",\"params\":" + params + "}");
}

// ---------------- URI <-> path (file:///d%3A/... style) ----------------

static wstring UriToPath(const wstring& uri)
{
    wstring p = uri;
    if (p.rfind(L"file:///", 0) == 0) p = p.substr(8);
    else if (p.rfind(L"file://", 0) == 0) p = p.substr(7);
    // decode %xx
    wstring out;
    for (size_t i = 0; i < p.size(); i++)
    {
        if (p[i] == L'%' && i + 2 < p.size() &&
            iswxdigit(p[i + 1]) && iswxdigit(p[i + 2]))
        {
            wchar_t hex[3] = { p[i + 1], p[i + 2], 0 };
            out += (wchar_t)wcstoul(hex, nullptr, 16);
            i += 2;
        }
        else out += p[i];
    }
    // a drive path from a URI is "/d:/..." or "/d|/..." — drop the leading
    // slash before the single-letter drive
    if (out.size() > 2 && out[0] == L'/' && out[2] == L':')
        out = out.substr(1);
    std::replace(out.begin(), out.end(), L'/', L'\\');
    return out;
}

// ---------------- document store ----------------

struct Document
{
    wstring text;
    int version = 0;
};
static std::map<wstring, Document> g_docs;   // URI -> text

// ---------------- project index ----------------
//
// 「对整个项目的代码掌控」的地基：把工程里的每个源文件扫一遍，
// 抽出**定义**（namespace / class / function / variable）及其精确列号，
// 并保留 token 流以便按名字反查**引用**。
//
// 为什么在服务端做：aardio 侧无法正确判断 aardio 的注释/字符串边界
// （`//` 右邻 `=` 是 raw string、块注释按星号配对、反引号原样串），
// 重写一套词法扫描必然出错 —— 服务端复用引擎真正的 Lexer/Parser。

// 一个定义
struct SymEntry
{
    wstring name;       // 简单名（namespace/class 取最后一段）
    wstring qual;       // 限定名（app.shell.editor）
    wstring kind;       // namespace | class | function | variable
    wstring uri;
    wstring path;
    int line = 0;       // 1 基行号
    int col = 0;        // 0 基列号（UTF-16 码元，与 LSP character 同单位）
    int len = 0;        // 名字长度（码元）
};

// 一次出现
struct Occ
{
    int line = 0;       // 1 基行号
    int col = 0;        // 0 基列号
    int len = 0;
};

// 一个文件的索引
struct FileIdx
{
    wstring uri;
    wstring path;
    wstring text;
    std::vector<Tok> toks;
    std::vector<SymEntry> syms;
};

static std::map<wstring, FileIdx> g_index;   // path -> index
static wstring g_root;                       // 工程根目录

// ---------------- small helpers ----------------

static wstring PathToUri(const wstring& path)
{
    std::wstring s = path;
    std::replace(s.begin(), s.end(), L'\\', L'/');
    return L"file:///" + s;
}

// 路径归一化：把重复的分隔符折叠掉，并去掉尾部分隔符。
//
// aardio 的**双引号串是原样串** —— "\\a.nodio" 里是两个反斜杠，
// 客户端很容易把路径拼成 "D:\dir\\file.nodio"。同一个文件于是有了
// 两个索引键（"D:\dir\file.nodio" 和 "D:\dir\\file.nodio"），
// 查找引用会重复计数、重命名会把同一处改两遍。
static wstring NormalizePath(const wstring& in)
{
    wstring out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); i++)
    {
        wchar_t c = in[i];
        if ((c == L'\\' || c == L'/') && !out.empty() &&
            (out.back() == L'\\' || out.back() == L'/'))
            continue;                       // 折叠重复分隔符
        out.push_back(c);
    }
    while (out.size() > 1 && (out.back() == L'\\' || out.back() == L'/'))
        out.pop_back();                     // 去掉尾部分隔符
    return out;
}

static bool ReadFileUtf8(const wstring& path, std::string& out)
{
    FILE* f = nullptr;
    _wfopen_s(&f, path.c_str(), L"rb");
    if (!f) return false;
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    // UTF-8 BOM：Wide() 不认识 BOM，会把 BOM 当成一个字符
    if (out.size() >= 3 && (unsigned char)out[0] == 0xEF &&
        (unsigned char)out[1] == 0xBB && (unsigned char)out[2] == 0xBF)
        out.erase(0, 3);
    return true;
}

// 每行首字符的绝对偏移（0 基行号 -> 偏移）
static void ComputeLineOffsets(const wstring& text, std::vector<size_t>& off)
{
    off.clear();
    off.push_back(0);
    for (size_t i = 0; i < text.size(); i++)
        if (text[i] == L'\n') off.push_back(i + 1);
}

// 行内列号（0 基）-> 绝对偏移
static size_t OffsetOf(const std::vector<size_t>& off, int line1, int col0)
{
    if (line1 < 1) return 0;
    if ((size_t)(line1 - 1) >= off.size()) return 0;
    return off[line1 - 1] + (size_t)col0;
}

// 光标处的标识符
struct WordAt
{
    wstring word;
    int line = 0;       // 1 基
    int col = 0;        // 0 基
    int len = 0;
    bool ok = false;
};

static WordAt WordAtPos(const wstring& text, int line1, int col0)
{
    WordAt r;
    std::vector<size_t> off;
    ComputeLineOffsets(text, off);
    if (line1 < 1 || (size_t)line1 > off.size()) return r;
    size_t ls = off[line1 - 1];
    size_t le = (size_t)line1 < off.size() ? off[line1] : text.size();
    while (le > ls && (text[le - 1] == L'\n' || text[le - 1] == L'\r')) le--;
    if (col0 < 0) col0 = 0;
    if ((size_t)col0 > le - ls) col0 = (int)(le - ls);
    size_t pos = ls + (size_t)col0;
    if (pos >= le)
    {
        if (pos > ls) pos--;
        else return r;
    }
    if (!IsIdentChar(text[pos])) return r;
    size_t b = pos, e = pos;
    while (b > ls && IsIdentChar(text[b - 1])) b--;
    while (e + 1 < le && IsIdentChar(text[e + 1])) e++;
    r.word = text.substr(b, e - b + 1);
    r.col = (int)(b - ls);
    r.len = (int)(e - b + 1);
    r.line = line1;
    r.ok = true;
    return r;
}

// ---------------- AST walk: collect definitions ----------------

static wstring JoinNs(const wstring& prefix, const wstring& name);

static void CollectDefs(const PNode& n, const wstring& nsPrefix,
                        std::vector<SymEntry>& out);

static void CollectDefs(const PNode& n, const wstring& nsPrefix,
                        std::vector<SymEntry>& out)
{
    if (!n) return;
    switch (n->k)
    {
    case Node::Namespace:
    case Node::Class:
    {
        wstring full = nsPrefix;
        for (auto& seg : n->names) full = JoinNs(full, seg);
        SymEntry s;
        s.name = n->names.empty() ? full : n->names.back();
        s.qual = full;
        s.kind = (n->k == Node::Namespace) ? L"namespace" : L"class";
        s.line = n->line;
        out.push_back(s);
        if (n->a) CollectDefs(n->a, full, out);
        break;
    }
    case Node::VarDecl:
    {
        for (auto& nm : n->names)
        {
            if (nm.empty()) continue;
            SymEntry s;
            s.name = nm;
            s.qual = JoinNs(nsPrefix, nm);
            s.kind = L"variable";
            s.line = n->line;
            out.push_back(s);
        }
        break;
    }
    case Node::Assign:
        // function f(){} / f = function(){} parses to Assign(Ident f, FuncLit)
        if (n->a && n->a->k == Node::Ident && n->b &&
            n->b->k == Node::FuncLit)
        {
            SymEntry s;
            s.name = n->a->str;
            s.qual = JoinNs(nsPrefix, n->a->str);
            s.kind = L"function";
            s.line = n->line;
            out.push_back(s);
        }
        break;
    default:
        for (auto& c : n->list) CollectDefs(c, nsPrefix, out);
        if (n->a) CollectDefs(n->a, nsPrefix, out);
        if (n->b) CollectDefs(n->b, nsPrefix, out);
        break;
    }
}

// 给每个定义补上精确列号。
//
// AST 节点只有行号（Node::line），列号得靠 token 流反查：
//   · function：名字 token 后面紧跟 '('（函数声明）或 '='（函数字面量赋值）
//   · 其它：同一行上第一个同名的标识符 token
static void ResolveColumns(FileIdx& fi)
{
    std::vector<size_t> off;
    ComputeLineOffsets(fi.text, off);
    for (auto& s : fi.syms)
    {
        int lineIdx = s.line - 1;
        if (lineIdx < 0 || (size_t)lineIdx >= off.size()) continue;
        size_t lineStart = off[lineIdx];
        for (size_t k = 0; k < fi.toks.size(); k++)
        {
            const Tok& t = fi.toks[k];
            if (t.line != s.line) continue;
            if (t.t != TIdent || t.text != s.name) continue;
            if (s.kind == L"function")
            {
                if (k + 1 >= fi.toks.size()) continue;
                const Tok& nx = fi.toks[k + 1];
                bool okNext = (nx.t == TPunct &&
                               (nx.text == L"(" || nx.text == L"="));
                if (!okNext) continue;
            }
            s.col = (int)(t.pos - lineStart);
            s.len = (int)t.text.size();
            break;
        }
    }
}

// 把一个文件纳入索引（文本由调用方给：磁盘内容或内存中的未保存内容）
static void IndexFileWithText(const wstring& path, const wstring& text)
{
    FileIdx fi;
    fi.path = NormalizePath(path);
    fi.uri = PathToUri(path);
    fi.text = text;
    wstring err;
    fi.toks = Lex(text, &err);
    if (err.empty())
    {
        Parser ps{ fi.toks, 0, &err };
        PNode ast = ps.ParseBlock();
        if (ast) CollectDefs(ast, L"", fi.syms);
    }
    ResolveColumns(fi);
    for (auto& s : fi.syms)
    {
        s.uri = fi.uri;
        s.path = fi.path;
    }
    g_index[path] = std::move(fi);
}

static void IndexFileFromDisk(const wstring& path)
{
    std::string raw;
    if (!ReadFileUtf8(path, raw)) return;
    IndexFileWithText(path, Wide(raw));
}

// 取目标标识符**前面**的限定链（"a.b.c"）；不是成员访问时返回空串
static wstring QualifierChain(const std::vector<Tok>& toks, size_t k)
{
    if (k == 0) return L"";
    const Tok& p = toks[k - 1];
    if (!(p.t == TPunct && p.text == L".")) return L"";
    std::vector<wstring> parts;
    size_t j = k - 1;
    while (j >= 1)
    {
        const Tok& id = toks[j - 1];
        if (id.t != TIdent) break;
        parts.push_back(id.text);
        if (j >= 2 && toks[j - 2].t == TPunct && toks[j - 2].text == L".")
        {
            j = j - 2;
            continue;
        }
        break;
    }
    std::reverse(parts.begin(), parts.end());
    wstring out;
    for (size_t i = 0; i < parts.size(); i++)
    {
        if (i) out += L'.';
        out += parts[i];
    }
    return out;
}

// 找出文件里所有**同一符号**的出现。
//
// 名字相同不等于同一个符号：obj.foo 和 bar.foo 是两个 foo。
// 判定规则（锚点是「这个符号的定义」）：
//   · 独立标识符（前面不是 '.'）—— 直接算引用
//   · 成员访问 a.b.c —— 限定链 Q 必须正好指向定义所在的模块，
//     或者是定义所在命名空间路径的前缀。
//     这样 app.editor.contextMenu 会被算成 editor.nodio 里 contextMenu 的
//     引用，而某个对象上的 .contextMenu 不会被误算。
static std::vector<Occ> FindOccurrences(const std::vector<Tok>& toks,
                                        const wstring& text, const wstring& word,
                                        const wstring& defModuleKey,
                                        const wstring& defQual)
{
    std::vector<Occ> out;
    std::vector<size_t> off;
    ComputeLineOffsets(text, off);
    for (size_t k = 0; k < toks.size(); k++)
    {
        const Tok& t = toks[k];
        if (t.t != TIdent || t.text != word) continue;
        if (k > 0)
        {
            const Tok& pv = toks[k - 1];
            if (pv.t == TPunct && pv.text == L"..") { /* 全局表访问：算引用 */ }
            else if (pv.t == TPunct && pv.text == L".")
            {
                wstring q = QualifierChain(toks, k);
                bool ok = false;
                if (!defModuleKey.empty() &&
                    _wcsicmp(q.c_str(), defModuleKey.c_str()) == 0) ok = true;
                if (!ok && !defQual.empty() && !q.empty())
                {
                    // defQual 以 "q." 开头（按段边界）
                    if (defQual.size() > q.size() &&
                        _wcsicmp(defQual.substr(0, q.size()).c_str(), q.c_str()) == 0 &&
                        defQual[q.size()] == L'.') ok = true;
                }
                if (!ok) continue;
            }
        }
        int lineIdx = t.line - 1;
        if (lineIdx < 0 || (size_t)lineIdx >= off.size()) continue;
        Occ o;
        o.line = t.line;
        o.col = (int)(t.pos - off[lineIdx]);
        o.len = (int)t.text.size();
        out.push_back(o);
    }
    return out;
}

// ---------------- project setup ----------------

static void IndexLibFiles();     // 见后面的「库根扫描」
static wstring ModuleKeyOf(const wstring& path);   // 见「库根扫描」之后（补全要用）

// nodio/setProject —— 告诉服务端工程根目录和全部源文件
static void SetProject(const std::string& msg)
{
    g_root = Wide(JsonString(msg, "root"));
    // 工程根可能自带一个尾部分隔符（IDE 的 proj.root 就是这样），
    // 再拼一个 '\' 会变成双反斜杠 —— 路径看着能用，
    // 但 ModuleKeyOf 算模块名时会把多出来的那个当成路径的一部分
    // （"nodio\\app\\editor.nodio" -> ".app.editor.nodio"），匹配全废。
    while (g_root.size() > 1 &&
           (g_root[g_root.size() - 1] == L'\\' || g_root[g_root.size() - 1] == L'/'))
        g_root.pop_back();
    auto files = JsonStringArray(msg, "files");
    g_index.clear();
    for (auto& f : files)
    {
        wstring rel = Wide(f);
        wstring full = rel;
        if (!g_root.empty() && !rel.empty() && rel.find(L':') == wstring::npos)
            full = g_root + L"\\" + rel;
        IndexFileFromDisk(full);
    }
    // 库（overlay lib）也要进索引：转到定义要能跳进 nodio.ui.editor 这类
    // 标准库组件，工程清单里并没有它们。
    IndexLibFiles();
    // ⚠ g_index.clear() 把**已经打开的文档**也清掉了 —— 它们不在工程清单
    //   也不在 lib 里（比如一个还没保存到工程目录的草稿）。
    //   不补回来的话，这些文档里的符号就没了锚点：
    //   「app.hello」的成员访问不再被算成 hello 的引用（引用/重命名漏数），
    //   补全也拿不到它们定义的成员。按**当前缓冲区内容**重新编入索引。
    for (auto& kv : g_docs)
        IndexFileWithText(UriToPath(kv.first), kv.second.text);
}

// 找出某个名字的全部定义（跨文件）
static std::vector<SymEntry> FindDefs(const wstring& word)
{
    std::vector<SymEntry> out;
    for (auto& kv : g_index)
        for (auto& s : kv.second.syms)
            if (s.name == word) out.push_back(s);
    return out;
}

// 引用/重命名要覆盖的文本：索引里的文件 + 当前打开但还没索引的文档
static bool IsIndexed(const wstring& uri)
{
    for (auto& kv : g_index)
        if (kv.second.uri == uri) return true;
    return false;
}

// ---------------- JSON emitters ----------------

static std::string LocationJson(const wstring& uri, int line1, int col0, int len)
{
    return "{\"uri\":" + JStr(uri) + ",\"range\":" +
           Range(line1, col0, line1, col0 + (len > 0 ? len : 1)) + "}";
}


// a symbol collected from the AST — a structured entity, not parallel lists
struct DocSymbol
{
    wstring name;
    wstring kind;          // "namespace" | "class" | "function" | "variable"
    int line = 0;          // 1-based
    int endLine = 0;       // 1-based, last line of the symbol's body
    std::vector<DocSymbol> children;
};

// Max line of a node's whole subtree.
//
// The AST only records a node's **start** line, but "is the cursor inside
// this symbol?" needs the symbol's **extent**: without it, the enclosing
// symbol of a cursor sitting on a top-level statement would be the
// function that happens to end just above it.
//
// ⚠ 不能用 std::max —— windows.h 把 max 定义成了宏，std::max(a,b) 会被
//   展开成 `std::((a) > (b) ? (a) : (b))` 直接编译不过。
static int MaxOf2(int a, int b) { return a > b ? a : b; }

static int MaxLineOf(const PNode& n)
{
    if (!n) return 0;
    int m = n->line;
    m = MaxOf2(m, MaxLineOf(n->a));
    m = MaxOf2(m, MaxLineOf(n->b));
    m = MaxOf2(m, MaxLineOf(n->c));
    m = MaxOf2(m, MaxLineOf(n->d));
    for (auto& c : n->list) m = MaxOf2(m, MaxLineOf(c));
    for (auto& d : n->defaults) m = MaxOf2(m, MaxLineOf(d));
    return m;
}

// ---------------- AST walk: collect symbols ----------------

static void CollectFromBlock(const PNode& blk, const wstring& nsPrefix,
                             std::vector<DocSymbol>& out);

static wstring JoinNs(const wstring& prefix, const wstring& name)
{
    return prefix.empty() ? name : prefix + L"." + name;
}

static void CollectFromStatement(const PNode& n, const wstring& nsPrefix,
                                 std::vector<DocSymbol>& out)
{
    if (!n) return;
    switch (n->k)
    {
    case Node::Namespace:
    {
        wstring full = nsPrefix;
        for (auto& seg : n->names) full = JoinNs(full, seg);
        DocSymbol s{ full, L"namespace", n->line, 0, {} };
        if (n->a) CollectFromBlock(n->a, full, s.children);
        s.endLine = MaxOf2(n->line, MaxLineOf(n->a));
        out.push_back(std::move(s));
        break;
    }
    case Node::Class:
    {
        wstring full = nsPrefix;
        for (auto& seg : n->names) full = JoinNs(full, seg);
        DocSymbol s{ full, L"class", n->line, 0, {} };
        if (n->a) CollectFromBlock(n->a, full, s.children);
        s.endLine = MaxOf2(n->line, MaxLineOf(n->a));
        out.push_back(std::move(s));
        break;
    }
    case Node::VarDecl:
    {
        // `var f = function(){...}` is a FUNCTION, not a plain variable —
        // the IDE's navigation bar (breadcrumb) shows it as a scope, so it
        // has to be recognized as one (and to know where it ends).
        // names[0] pairs with n->a; names[k] (k>0) pairs with n->b->list[k-1].
        for (size_t k = 0; k < n->names.size(); ++k)
        {
            if (n->names[k].empty()) continue;
            PNode val = n->a;
            if (k > 0 && n->b && k - 1 < n->b->list.size())
                val = n->b->list[k - 1];
            bool bFunc = (val && val->k == Node::FuncLit);
            DocSymbol s{ n->names[k], bFunc ? L"function" : L"variable",
                         n->line, 0, {} };
            if (bFunc) s.endLine = MaxOf2(n->line, MaxLineOf(val));
            else s.endLine = n->line;
            out.push_back(std::move(s));
        }
        break;
    }
    case Node::Assign:
        // function f(){} parses to Assign(Ident f, FuncLit)
        if (n->a && n->a->k == Node::Ident && n->b &&
            n->b->k == Node::FuncLit)
        {
            DocSymbol s{ n->a->str, L"function", n->line, 0, {} };
            s.endLine = MaxOf2(n->line, MaxLineOf(n->b));
            out.push_back(std::move(s));
        }
        break;
    default:
        // recurse into statement children
        for (auto& c : n->list) CollectFromStatement(c, nsPrefix, out);
        if (n->a) CollectFromStatement(n->a, nsPrefix, out);
        if (n->b) CollectFromStatement(n->b, nsPrefix, out);
        break;
    }
}

static void CollectFromBlock(const PNode& blk, const wstring& nsPrefix,
                             std::vector<DocSymbol>& out)
{
    if (!blk) return;
    for (auto& st : blk->list)
        CollectFromStatement(st, nsPrefix, out);
}

// collect ALL identifiers/keywords that appear in the document (completion
// fallback — words actually used in this file)
static void CollectWords(const PNode& n, std::set<wstring>& words)
{
    if (!n) return;
    if (n->k == Node::Ident && !n->str.empty()) words.insert(n->str);
    for (auto& c : n->list) CollectWords(c, words);
    CollectWords(n->a, words);
    CollectWords(n->b, words);
    CollectWords(n->c, words);
    CollectWords(n->d, words);
    for (auto& nm : n->names) if (!nm.empty()) words.insert(nm);
}

// ---------------- parse + diagnostics ----------------

struct ParseResult
{
    bool ok = false;
    wstring errText;        // with "@ 行 N" suffix
    int errLine = 0;
    PNode ast;
};

static ParseResult ParseDocument(const wstring& src)
{
    ParseResult r;
    wstring localErr;
    vector<Tok> toks = Lex(src, &localErr);
    if (localErr.empty())
    {
        Parser ps{ toks, 0, &localErr };
        r.ast = ps.ParseBlock();
    }
    r.ok = localErr.empty();
    if (!r.ok)
    {
        r.errText = localErr;
        // engine error text ends with "@ 行 N" (bare line) — or, since
        // S-2, with "@ <file>:<line>" when the owning file is known
        size_t at = localErr.rfind(L"@ 行 ");
        if (at != wstring::npos)
            r.errLine = (int)wcstol(localErr.c_str() + at + 4, nullptr, 10);
        else
        {
            size_t colon = localErr.rfind(L":");
            if (colon != wstring::npos)
            {
                const wchar_t* q = localErr.c_str() + colon + 1;
                bool bDigits = (*q >= L'0' && *q <= L'9');
                for (const wchar_t* r = q; *r; r++)
                    if (*r < L'0' || *r > L'9') { bDigits = false; break; }
                if (bDigits)
                    r.errLine = (int)wcstol(q, nullptr, 10);
            }
        }
        if (r.errLine <= 0) r.errLine = 1;
    }
    return r;
}

static void PublishDiagnostics(const wstring& uri)
{
    auto it = g_docs.find(uri);
    std::string diags = "[]";
    if (it != g_docs.end())
    {
        ParseResult pr = ParseDocument(it->second.text);
        if (!pr.ok)
        {
            // end column: walk the line length for a non-empty range
            int lineLen = 0;
            {
                const wstring& t = it->second.text;
                int ln = 1;
                size_t i = 0;
                while (i < t.size() && ln < pr.errLine)
                    if (t[i++] == L'\n') ln++;
                while (i < t.size() && t[i] != L'\n') { lineLen++; i++; }
            }
            std::string msg = JStr(pr.errText);
            diags = "[{\"range\":" +
                    Range(pr.errLine, 0, pr.errLine,
                          lineLen > 1 ? lineLen : 1) +
                    ",\"severity\":1,\"source\":\"nodio\",\"message\":" +
                    msg + "}]";
        }
    }
    Notify("textDocument/publishDiagnostics",
           "{\"uri\":" + JStr(uri) + ",\"diagnostics\":" + diags + "}");
}

// ---------------- document symbols ----------------

static std::string SymbolKindNum(const wstring& kind)
{
    // LSP SymbolKind: File1 Module2 Namespace3 Package4 Class5 Method6
    // Property7 Field8 Constructor9 Enum10 Interface11 Function12
    if (kind == L"namespace") return "3";
    if (kind == L"class") return "5";
    if (kind == L"function") return "12";
    return "13";   // Variable
}

static std::string SymbolsJson(const std::vector<DocSymbol>& syms,
                               const wstring& uri)
{
    if (syms.empty()) return "[]";
    std::string out = "[";
    bool first = true;
    for (auto& s : syms)
    {
        if (!first) out += ",";
        first = false;
        out += "{\"name\":" + JStr(s.name) +
               ",\"kind\":" + SymbolKindNum(s.kind) +
               ",\"line\":" + std::to_string(s.line) +
               ",\"endLine\":" + std::to_string(s.endLine) +
               ",\"range\":" + Range(s.line, 0, s.line, 1) +
               ",\"selectionRange\":" + Range(s.line, 0, s.line, 1) +
               ",\"children\":" + SymbolsJson(s.children, uri) + "}";
    }
    return out + "]";
}

// ---------------- completion ----------------

static const wchar_t* const COMPLETION_KEYWORDS[] = {
    L"var", L"function", L"if", L"else", L"elseif", L"while", L"for",
    L"in", L"break", L"continue", L"return", L"null", L"true", L"false",
    L"and", L"or", L"not", L"do", L"import", L"namespace",
    L"try", L"catch", L"class", L"select", L"case", L"with", nullptr
};

static std::string CompletionItem(const wstring& label, const char* kind,
                                  const wchar_t* detail)
{
    return "{\"label\":" + JStr(label) + ",\"kind\":" + kind +
           ",\"detail\":" + JStr(detail ? detail : L"") + "}";
}

// 光标前的限定链：插入点左边跳过空白后是 '.' 时，把 "a.b.c" 收回来。
// 有了它，补全才能「按上下文过滤」——nodio.ui. 后面只出 ui 模块的成员，
// 而不是把全文 200 个词全糊上去。
static wstring QualifierAtCursor(const wstring& text, int line, int col)
{
    int ln = 1;
    size_t i = 0;
    while (i < text.size() && ln < line)
        if (text[i++] == L'\n') ln++;
    size_t ls = i, le = i;
    while (le < text.size() && text[le] != L'\n') le++;
    if ((size_t)col > le - ls) col = (int)(le - ls);
    size_t p = ls + (size_t)col;              // 插入点
    while (p > ls && (text[p - 1] == L' ' || text[p - 1] == L'\t')) p--;
    if (p <= ls || text[p - 1] != L'.') return L"";
    p--;                                       // 吃掉 '.'
    size_t e = p;
    while (p > ls && (IsIdentChar(text[p - 1]) || text[p - 1] == L'.')) p--;
    wstring q = text.substr(p, e - p);
    while (!q.empty() && q[0] == L'.') q = q.substr(1);
    while (!q.empty() && q.back() == L'.') q.pop_back();
    return q;
}

static std::string CompletionsFor(const Document& doc, int line, int col,
                                  const wstring& uri)
{
    std::string items;
    bool first = true;
    auto add = [&](const wstring& label, const char* kind, const wchar_t* detail)
    {
        if (label.empty()) return;
        if (!first) items += ",";
        first = false;
        items += CompletionItem(label, kind, detail);
    };
    ParseResult pr = ParseDocument(doc.text);
    std::set<wstring> words;
    if (pr.ast) CollectWords(pr.ast, words);

    wstring qual = QualifierAtCursor(doc.text, line, col);
    if (!qual.empty())
    {
        // ---- 成员上下文：`nodio.ui.` 之后只出这个命名空间的东西 ----
        std::set<wstring> seen;
        wstring dot = qual + L".";
        // ① 项目/库索引里的符号：qual 正好是 "qual.成员"（成员不再带点）
        for (auto& kv : g_index)
        {
            for (auto& s : kv.second.syms)
            {
                if (s.qual.size() <= dot.size()) continue;
                if (_wcsicmp(s.qual.substr(0, dot.size()).c_str(), dot.c_str()) != 0)
                    continue;
                wstring rest = s.qual.substr(dot.size());
                if (rest.find(L'.') != wstring::npos) continue;
                if (seen.count(rest)) continue;
                seen.insert(rest);
                // CompletionItemKind：function→3(函数) class→7(类)
                //                     variable→6(变量) namespace→9(模块)
                const char* cik = "6";
                if (s.kind == L"function") cik = "3";
                else if (s.kind == L"class") cik = "7";
                else if (s.kind == L"namespace") cik = "9";
                add(rest, cik, s.kind.c_str());
            }
        }
        // ② 子模块：nodio.ui.editor 是 nodio.ui 的成员（模块）
        for (auto& kv : g_index)
        {
            wstring mk = ModuleKeyOf(kv.second.path);
            if (mk.size() <= dot.size()) continue;
            if (_wcsicmp(mk.substr(0, dot.size()).c_str(), dot.c_str()) != 0)
                continue;
            wstring rest = mk.substr(dot.size());
            size_t d2 = rest.find(L'.');
            if (d2 != wstring::npos) rest = rest.substr(0, d2);
            if (seen.count(rest)) continue;
            seen.insert(rest);
            add(rest, "9", L"模块");           // CompletionItemKind Module
        }
        // ③ 本文档里 `qual.xxx` 直接用过的成员（self.foo 这类进不了索引的）
        {
            wstring needle = dot;
            const wstring& t = doc.text;
            size_t pos = 0;
            while ((pos = t.find(needle, pos)) != wstring::npos)
            {
                size_t k = pos + needle.size();
                if (k < t.size() && IsIdentChar(t[k]))
                {
                    size_t b = k;
                    while (b < t.size() && IsIdentChar(t[b])) b++;
                    wstring nm = t.substr(k, b - k);
                    if (nm.empty() == false && !seen.count(nm))
                    {
                        seen.insert(nm);
                        add(nm, "1", L"文档符号");
                    }
                    pos = b;
                }
                else pos = k;
            }
        }
        if (!first)   // 上下文里有东西就不再糊全文候选
            return "[{\"isIncomplete\":false,\"items\":[" + items + "]}]";
        // 上下文里没匹配到 → 退回全文候选（比返回空列表有用）
    }

    for (auto& w : words)
        add(w, "1", L"文档符号");          // Text kind
    for (int k = 0; COMPLETION_KEYWORDS[k]; k++)
        add(COMPLETION_KEYWORDS[k], "14", L"关键字");   // Keyword kind
    return "[{\"isIncomplete\":false,\"items\":[" + items + "]}]";
}

// ---------------- hover ----------------

static std::string HoverFor(const Document& doc, int line, int col)
{
    // 1-based line, 0-based col -> word at position
    const wstring& t = doc.text;
    int ln = 1;
    size_t i = 0;
    while (i < t.size() && ln < line)
        if (t[i++] == L'\n') ln++;
    size_t ls = i, le = i;
    while (le < t.size() && t[le] != L'\n') le++;
    // clamp col
    if ((size_t)col > le - ls) col = (int)(le - ls);
    size_t pos = ls + col;
    if (pos >= le) return "null";
    if (!IsIdentChar(t[pos]))
    {
        if (pos > ls) pos--;           // char right of '.' etc.
        else return "null";
    }
    size_t b = pos, e = pos;
    while (b > ls && IsIdentChar(t[b - 1])) b--;
    while (e + 1 < le && IsIdentChar(t[e + 1])) e++;
    wstring word = t.substr(b, e - b + 1);
    if (word.empty()) return "null";

    // symbol kind from the AST
    wstring kind;
    ParseResult pr = ParseDocument(t);
    if (pr.ast)
    {
        std::vector<DocSymbol> syms;
        CollectFromBlock(pr.ast, L"", syms);
        std::function<const DocSymbol*(const std::vector<DocSymbol>&)> find =
            [&](const std::vector<DocSymbol>& ss) -> const DocSymbol*
        {
            for (auto& s : ss)
            {
                if (s.name == word) return &s;
                if (auto r = find(s.children)) return r;
            }
            return nullptr;
        };
        if (const DocSymbol* hit = find(syms)) kind = hit->kind;
    }
    wstring md = L"**" + word + L"**" +
                 (kind.empty() ? L"" : L"  \n*" + kind + L"*");
    return "{\"contents\":{\"kind\":\"markdown\",\"value\":" +
           JStr(md) + "}}";
}

// ---------------- message params readers ----------------

// params.textDocument.uri
static wstring ParamUri(const std::string& msg)
{
    std::string u = JsonString(msg, "uri");
    return Wide(u);
}

// full-sync text: didChange carries it in contentChanges[0].text;
// didOpen carries it directly in textDocument.text
static wstring ParamNewText(const std::string& msg)
{
    size_t p = msg.find("\"contentChanges\"");
    if (p != std::string::npos)
        return Wide(JsonString(msg.substr(p), "text"));
    p = msg.find("\"textDocument\"");
    if (p != std::string::npos)
        return Wide(JsonString(msg.substr(p), "text"));
    return {};
}

static void ParamPos(const std::string& msg, int* pLine, int* pChar)
{
    std::string l = JsonNumber(msg, "line");
    std::string c = JsonNumber(msg, "character");
    *pLine = l.empty() ? 1 : (int)wcstol(Wide(l).c_str(), nullptr, 10) + 1;
    *pChar = c.empty() ? 0 : (int)wcstol(Wide(c).c_str(), nullptr, 10);
}

// ---------------- project resolution ----------------

// 库根目录（overlay lib）：和引擎 import.cpp 的解析顺序一致
static wstring GetLibRoot()
{
    wchar_t szEnv[MAX_PATH] = L"";
    if (GetEnvironmentVariableW(L"NODIO_OVERLAY_LIBDIR", szEnv, MAX_PATH) > 0)
        return wstring(szEnv);
    wchar_t szPath[MAX_PATH] = L"";
    if (GetModuleFileNameW(nullptr, szPath, MAX_PATH) > 0)
    {
        wstring exeDir(szPath);
        size_t sl = exeDir.find_last_of(L"\\/");
        if (sl != wstring::npos)
        {
            exeDir = exeDir.substr(0, sl);
            wstring c1 = exeDir + L"\\lib";
            if (_waccess(c1.c_str(), 0) == 0) return c1;
            size_t sl2 = exeDir.find_last_of(L'\\');
            if (sl2 != wstring::npos)
            {
                wstring c2 = exeDir.substr(0, sl2) + L"\\lib";
                if (_waccess(c2.c_str(), 0) == 0) return c2;
            }
        }
    }
    if (_waccess(L"D:\\Nodio\\lib", 0) == 0) return wstring(L"D:\\Nodio\\lib");
    return L"";
}

static void ScanDirRecursive(const wstring& dir, std::vector<wstring>& out)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do
    {
        wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        wstring full = dir + L"\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            ScanDirRecursive(full, out);
        }
        else
        {
            size_t dot = name.find_last_of(L'.');
            if (dot == wstring::npos) continue;
            wstring e = name.substr(dot);
            std::transform(e.begin(), e.end(), e.begin(), ::towlower);
            if (e == L".aardio" || e == L".nodio") out.push_back(full);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// 把库目录下的源文件也纳入索引
static void IndexLibFiles()
{
    static wstring libRoot = GetLibRoot();
    if (libRoot.empty()) return;
    std::vector<wstring> files;
    ScanDirRecursive(libRoot, files);
    for (auto& f : files)
    {
        if (g_index.count(f)) continue;
        IndexFileFromDisk(f);
    }
}

// 文件的「模块名」：相对工程根或库根，去掉扩展名，分隔符换成点
//   D:\Nodio\lib\nodio\ui\editor.aardio  ->  nodio.ui.editor
static wstring ModuleKeyOf(const wstring& path)
{
    static wstring libRoot = GetLibRoot();
    wstring roots[2] = { g_root, libRoot };
    for (int k = 0; k < 2; k++)
    {
        const wstring& root = roots[k];
        if (root.empty()) continue;
        if (path.size() <= root.size()) continue;
        if (_wcsicmp(path.substr(0, root.size()).c_str(), root.c_str()) != 0) continue;
        wchar_t sep = path[root.size()];
        if (sep != L'\\' && sep != L'/') continue;
        wstring rel = path.substr(root.size() + 1);
        size_t dot = rel.find_last_of(L'.');
        if (dot != wstring::npos) rel = rel.substr(0, dot);
        std::replace(rel.begin(), rel.end(), L'\\', L'/');
        std::replace(rel.begin(), rel.end(), L'/', L'.');
        return rel;
    }
    return L"";
}

// 收集 AST 里的 import 语句
static void CollectImports(const PNode& n, std::set<wstring>& out)
{
    if (!n) return;
    if (n->k == Node::Import && !n->names.empty())
    {
        wstring s;
        for (size_t i = 0; i < n->names.size(); i++)
        {
            if (i) s += L'.';
            s += n->names[i];
        }
        out.insert(s);
    }
    for (auto& c : n->list) CollectImports(c, out);
    CollectImports(n->a, out);
    CollectImports(n->b, out);
    CollectImports(n->c, out);
    CollectImports(n->d, out);
}

static std::set<wstring> ImportsOf(const wstring& text)
{
    std::set<wstring> out;
    wstring err;
    vector<Tok> toks = Lex(text, &err);
    if (!err.empty()) return out;
    Parser ps{ toks, 0, &err };
    PNode ast = ps.ParseBlock();
    if (ast) CollectImports(ast, out);
    return out;
}

// ---------------- definition / references / rename ----------------

// 「这个符号到底指谁」的锚点：定义所在文件 + 限定名
struct Anchor
{
    wstring path;
    wstring qual;
    wstring moduleKey;
    bool ok = false;
};

// 选锚点：
//   1) 同文件 —— 光标**上方**最近的一个定义（上方没有就取下方第一个）
//   2) 本文件 import 到的模块
//   3) 任意同名定义
static Anchor PickAnchor(const std::vector<SymEntry>& defs, const wstring& curPath,
                         int line, const std::set<wstring>& imports)
{
    Anchor a;
    const SymEntry* best = nullptr;
    for (auto& s : defs)
    {
        if (curPath.empty() || _wcsicmp(s.path.c_str(), curPath.c_str()) != 0)
            continue;
        if (!best) { best = &s; continue; }
        bool sAbove = s.line <= line;
        bool bAbove = best->line <= line;
        if (sAbove && !bAbove) best = &s;                       // 上方优先
        else if (sAbove == bAbove && s.line > best->line) best = &s;   // 同侧取更近的
    }
    if (!best)
    {
        for (auto& s : defs)
        {
            wstring key = ModuleKeyOf(s.path);
            if (key.empty() || !imports.count(key)) continue;
            best = &s;
            break;
        }
    }
    if (!best && !defs.empty()) best = &defs[0];
    if (best)
    {
        a.path = best->path;
        a.qual = best->qual;
        a.moduleKey = ModuleKeyOf(best->path);
        a.ok = true;
    }
    return a;
}

// 转到定义
static std::string DefinitionFor(const wstring& text, const wstring& curPath,
                                 int line, int col)
{
    WordAt w = WordAtPos(text, line, col);
    if (!w.ok) return "null";
    std::vector<SymEntry> defs = FindDefs(w.word);
    if (defs.empty()) return "null";

    std::set<wstring> imports = ImportsOf(text);

    // ---- 同文件：光标上方最近的一个 ----
    const SymEntry* best = nullptr;
    for (auto& s : defs)
    {
        if (curPath.empty() || _wcsicmp(s.path.c_str(), curPath.c_str()) != 0)
            continue;
        if (!best) { best = &s; continue; }
        bool sAbove = s.line <= line;
        bool bAbove = best->line <= line;
        if (sAbove && !bAbove) best = &s;
        else if (sAbove == bAbove && s.line > best->line) best = &s;
    }
    if (best)
        return "[" + LocationJson(best->uri, best->line, best->col, best->len) + "]";

    // ---- 被 import 的模块 ----
    std::string out = "[";
    bool first = true;
    for (auto& s : defs)
    {
        wstring key = ModuleKeyOf(s.path);
        if (key.empty() || !imports.count(key)) continue;
        if (!first) out += ",";
        first = false;
        out += LocationJson(s.uri, s.line, s.col, s.len);
    }
    if (!first) return out + "]";

    // ---- 兜底：所有同名定义 ----
    for (auto& s : defs)
    {
        if (!first) out += ",";
        first = false;
        out += LocationJson(s.uri, s.line, s.col, s.len);
    }
    return out + "]";
}

// 查找引用：全项目里所有**同一符号**的出现（含定义处）
static std::string ReferencesFor(const wstring& text, const wstring& uri,
                                 int line, int col)
{
    WordAt w = WordAtPos(text, line, col);
    if (!w.ok) return "[]";
    std::vector<SymEntry> defs = FindDefs(w.word);
    std::set<wstring> imports = ImportsOf(text);
    Anchor a = PickAnchor(defs, UriToPath(uri), line, imports);

    std::string out = "[";
    bool first = true;
    auto emit = [&](const wstring& u, const std::vector<Tok>& toks,
                    const wstring& body)
    {
        std::vector<Occ> occs = FindOccurrences(toks, body, w.word,
                                                a.moduleKey, a.qual);
        for (auto& o : occs)
        {
            if (!first) out += ",";
            first = false;
            out += LocationJson(u, o.line, o.col, o.len);
        }
    };
    for (auto& kv : g_index) emit(kv.second.uri, kv.second.toks, kv.second.text);
    bool inIndex = false;
    for (auto& kv : g_index)
        if (kv.second.uri == uri) { inIndex = true; break; }
    auto it = g_docs.find(uri);
    if (it != g_docs.end() && !inIndex)
    {
        wstring err;
        vector<Tok> toks = Lex(it->second.text, &err);
        emit(uri, toks, it->second.text);
    }
    return out + "]";
}

// 引用标记（同符号高亮）：只看当前文档，光标下的符号在**本文档**的
// 全部出现（含定义处）。服务端不做的事：跨文件 —— 编辑器里的高亮
// 只要本文件的，全项目引用走 textDocument/references。
static std::string DocumentHighlightsFor(const wstring& text, const wstring& uri,
                                         int line, int col)
{
    WordAt w = WordAtPos(text, line, col);
    if (!w.ok) return "[]";
    std::vector<SymEntry> defs = FindDefs(w.word);
    std::set<wstring> imports = ImportsOf(text);
    Anchor a = PickAnchor(defs, UriToPath(uri), line, imports);
    wstring err;
    vector<Tok> toks = Lex(text, &err);
    if (!err.empty()) return "[]";
    std::vector<Occ> occs = FindOccurrences(toks, text, w.word, a.moduleKey, a.qual);
    std::string out = "[";
    bool first = true;
    for (auto& o : occs)
    {
        if (!first) out += ",";
        first = false;
        out += "{\"range\":" +
               Range(o.line, o.col, o.line, o.col + (o.len > 0 ? o.len : 1)) +
               ",\"kind\":1}";          // DocumentHighlightKind.Text
    }
    return out + "]";
}

// 重命名：返回 WorkspaceEdit
//   {"changes": { "<uri>": [ {"range":..., "newText":"新名字"} ] }}
static std::string RenameFor(const wstring& text, const wstring& uri,
                             int line, int col, const wstring& newName)
{
    WordAt w = WordAtPos(text, line, col);
    if (!w.ok) return "null";
    // 名字合法性：只能是标识符
    if (newName.empty() || !IsIdentChar(newName[0])) return "null";
    for (wchar_t c : newName)
        if (!IsIdentChar(c)) return "null";

    std::vector<SymEntry> defs = FindDefs(w.word);
    std::set<wstring> imports = ImportsOf(text);
    Anchor a = PickAnchor(defs, UriToPath(uri), line, imports);

    std::map<wstring, std::vector<std::string>> edits;
    auto collect = [&](const wstring& u, const std::vector<Tok>& toks,
                       const wstring& body)
    {
        std::vector<Occ> occs = FindOccurrences(toks, body, w.word,
                                                a.moduleKey, a.qual);
        for (auto& o : occs)
        {
            std::string e = "{\"range\":" +
                Range(o.line, o.col, o.line, o.col + o.len) +
                ",\"newText\":" + JStr(newName) + "}";
            edits[u].push_back(e);
        }
    };
    for (auto& kv : g_index) collect(kv.second.uri, kv.second.toks, kv.second.text);
    bool inIndex = false;
    for (auto& kv : g_index)
        if (kv.second.uri == uri) { inIndex = true; break; }
    auto it = g_docs.find(uri);
    if (it != g_docs.end() && !inIndex)
    {
        wstring err;
        vector<Tok> toks = Lex(it->second.text, &err);
        collect(uri, toks, it->second.text);
    }
    if (edits.empty()) return "null";

    std::string out = "{\"changes\":{";
    bool first = true;
    for (auto& kv : edits)
    {
        if (!first) out += ",";
        first = false;
        out += JStr(kv.first) + ":[";
        for (size_t i = 0; i < kv.second.size(); i++)
        {
            if (i) out += ",";
            out += kv.second[i];
        }
        out += "]";
    }
    return out + "}}";
}

// ---------------- 第五十三轮：LSP 协议补全（九方法中的八个） ----------------
//
//  · declaration / typeDefinition / implementation —— aardio 单一符号
//    宇宙，三类跳转与 definition 同义（分发层直接复用 DefinitionFor）。
//  · prepareRename —— 位置上符号的范围 + placeholder。
//  · selectionRange —— 词范围单层（parent=null）。
//  · foldingRange —— 缩进折叠（与编辑器行号栏折叠同族：下一非空行
//    缩进更深则成区，回到或更浅则收区；空行不计）。
//  · formatting —— 全文重排：花括号深度缩进（编辑器 Shift+Alt+F 同族
//    算法的精简版；行首连续 } 先减深度再套层级）。字符串字面量里的
//    括号会影响深度计数——与客户端 v1 同类限制，语义层修复挂账。
//  · signatureHelp —— 形参数据源暂缺（客户端悬浮签名/词库承担），
//    返回 null（协议完整，客户端不阻塞）。
//  · codeAction —— v1 无 quick-fix 源，返回空数组。
//
// semanticTokens 需要 LSP 侧词法模型，本轮不做（台账挂账）。

static std::string PrepareRenameFor(const wstring& text, int line1, int col0)
{
    WordAt w = WordAtPos(text, line1, col0);
    if (!w.ok) return "null";
    return "{\"range\":" + Range(w.line, w.col, w.line, w.col + w.len) +
           ",\"placeholder\":\"" + JStr(w.word) + "\"}";
}

static std::string SelectionRangeFor(const wstring& text, int line1, int col0)
{
    WordAt w = WordAtPos(text, line1, col0);
    if (!w.ok) return "null";
    return "{\"range\":" + Range(w.line, w.col, w.line, w.col + w.len) +
           ",\"parent\":null}";
}

static std::string FoldingRangeFor(const wstring& text)
{
    std::vector<size_t> off;
    ComputeLineOffsets(text, off);
    int nLines = (int)off.size();
    // 每行首非空白列（-1 = 空行/纯空白行）
    std::vector<int> ind(nLines, -1);
    for (int i = 0; i < nLines; i++)
    {
        size_t ls = off[i];
        size_t le = ((size_t)i + 1 < off.size()) ? off[i + 1] : text.size();
        while (le > ls && (text[le - 1] == L'\n' || text[le - 1] == L'\r')) le--;
        size_t p = ls;
        while (p < le && (text[p] == L' ' || text[p] == L'\t')) p++;
        if (p < le) ind[i] = (int)(p - ls);
    }
    std::string out;
    std::vector<std::pair<int, int>> stk;   // {indent, startLine0}
    for (int i = 0; i < nLines; i++)
    {
        if (ind[i] < 0) continue;
        while (!stk.empty() && ind[i] <= stk.back().first)
        {
            int s = stk.back().second;
            stk.pop_back();
            if (i - 1 > s)
            {
                if (!out.empty()) out += ",";
                out += "{\"startLine\":" + std::to_string(s) +
                       ",\"endLine\":" + std::to_string(i - 1) + "}";
            }
        }
        stk.push_back(std::make_pair(ind[i], i));
    }
    while (!stk.empty())
    {
        int s = stk.back().second;
        stk.pop_back();
        if (nLines - 1 > s)
        {
            if (!out.empty()) out += ",";
            out += "{\"startLine\":" + std::to_string(s) +
                   ",\"endLine\":" + std::to_string(nLines - 1) + "}";
        }
    }
    return "[" + out + "]";
}

static std::string FormattingFor(const wstring& text)
{
    std::vector<size_t> off;
    ComputeLineOffsets(text, off);
    int nLines = (int)off.size();
    std::string out = "[";
    int depth = 0;
    bool first = true;
    for (int i = 0; i < nLines; i++)
    {
        size_t ls = off[i];
        size_t le = ((size_t)i + 1 < off.size()) ? off[i + 1] : text.size();
        while (le > ls && (text[le - 1] == L'\n' || text[le - 1] == L'\r')) le--;
        size_t p = ls, q = le;
        while (p < q && (text[p] == L' ' || text[p] == L'\t')) p++;
        while (q > p && (text[q - 1] == L' ' || text[q - 1] == L'\t' ||
                         text[q - 1] == L'\r'))
            q--;
        wstring line = text.substr(p, q - p);
        if (!line.empty())
        {
            // 行首连续 } 属于上一层的闭合：先减深度再套层级
            int closes = 0;
            size_t k = 0;
            while (k < line.size() && line[k] == L'}') { closes++; k++; }
            int d = depth - closes;
            if (d < 0) d = 0;
            std::wstring ind;
            for (int t = 0; t < d; t++) ind += L'\t';
            if (!first) out += ",";
            first = false;
            out += "{\"range\":" + Range(i + 1, 0, i + 1, (int)(le - ls)) +
                   ",\"newText\":\"" + JStr(ind + line) + "\"}";
        }
        for (size_t k2 = 0; k2 < line.size(); k2++)
        {
            if (line[k2] == L'{') depth++;
            else if (line[k2] == L'}') depth--;
        }
        if (depth < 0) depth = 0;
    }
    out += "]";
    return out;
}

// ---------------- 语义高亮 / 快速修复 / 参数提示（第六十一轮）----------------

struct RawToken
{
    int line0 = 0;
    int col0 = 0;
    int len = 0;
    int type = 0; // 0:comment, 1:keyword, 2:string, 3:number, 4:function, 5:variable, 6:type, 7:parameter, 8:property
    int modifiers = 0;
    size_t pos = 0;
};

static std::string SemanticTokensFor(const wstring& text, int startLine = -1, int endLine = -1)
{
    std::vector<size_t> off;
    ComputeLineOffsets(text, off);
    if (off.empty()) return "{\"data\":[]}";

    auto posToLineCol = [&](size_t pos, int& l0, int& c0) {
        auto it = std::upper_bound(off.begin(), off.end(), pos);
        l0 = (it == off.begin()) ? 0 : (int)(it - off.begin() - 1);
        c0 = (int)(pos - off[l0]);
    };

    std::vector<RawToken> tokens;

    // 1. 注释扫描
    for (size_t i = 0; i < text.size(); i++)
    {
        if (text[i] == L'/' && i + 1 < text.size())
        {
            if (text[i + 1] == L'/')
            {
                size_t start = i;
                while (i < text.size() && text[i] != L'\n') i++;
                int l0 = 0, c0 = 0;
                posToLineCol(start, l0, c0);
                tokens.push_back({ l0, c0, (int)(i - start), 0, 0, start });
            }
            else if (text[i + 1] == L'*')
            {
                size_t start = i;
                i += 2;
                while (i + 1 < text.size() && !(text[i] == L'*' && text[i + 1] == L'/')) i++;
                if (i + 1 < text.size()) i += 2;
                int l0 = 0, c0 = 0;
                posToLineCol(start, l0, c0);
                tokens.push_back({ l0, c0, (int)(i - start), 0, 0, start });
            }
        }
    }

    // 2. 词法 token
    wstring err;
    std::vector<Tok> toks = Lex(text, &err);
    for (size_t k = 0; k < toks.size(); k++)
    {
        const Tok& t = toks[k];
        if (t.pos >= text.size() || t.t == TNL || t.t == TEnd || t.t == TPunct) continue;
        int l0 = 0, c0 = 0;
        posToLineCol(t.pos, l0, c0);
        int tokType = 5;
        int len = (int)t.text.size();
        if (t.t == TKw) tokType = 1;
        else if (t.t == TStr)
        {
            tokType = 2;
            len += 2;
        }
        else if (t.t == TNum) tokType = 3;
        else if (t.t == TIdent)
        {
            bool isCall = false;
            size_t nextIdx = k + 1;
            while (nextIdx < toks.size() && toks[nextIdx].t == TNL) nextIdx++;
            if (nextIdx < toks.size() && toks[nextIdx].t == TPunct && toks[nextIdx].text == L"(")
                isCall = true;
            if (isCall) tokType = 4;
            else if (!t.text.empty() && t.text[0] >= L'A' && t.text[0] <= L'Z') tokType = 6;
            else tokType = 5;
        }
        if (len <= 0) len = 1;
        tokens.push_back({ l0, c0, len, tokType, 0, t.pos });
    }

    // 3. 排序
    std::sort(tokens.begin(), tokens.end(), [](const RawToken& a, const RawToken& b) {
        if (a.line0 != b.line0) return a.line0 < b.line0;
        return a.col0 < b.col0;
    });

    // 4. 相对增量编码
    std::string dataStr;
    int prevLine = 0;
    int prevCol = 0;
    bool first = true;
    for (const auto& tk : tokens)
    {
        if (startLine >= 0 && endLine >= 0)
        {
            if (tk.line0 < startLine || tk.line0 > endLine) continue;
        }
        int dl = tk.line0 - prevLine;
        int dc = (dl == 0) ? (tk.col0 - prevCol) : tk.col0;
        if (dc < 0) continue;
        if (!first) dataStr += ",";
        first = false;
        dataStr += std::to_string(dl) + "," + std::to_string(dc) + "," +
                   std::to_string(tk.len) + "," + std::to_string(tk.type) + "," +
                   std::to_string(tk.modifiers);
        prevLine = tk.line0;
        prevCol = tk.col0;
    }
    return "{\"resultId\":\"1\",\"data\":[" + dataStr + "]}";
}

static std::string CodeActionFor(const wstring& uri, const std::string& msg, const wstring& text)
{
    // context.diagnostics 为空时回空数组（契约）
    size_t diagPos = msg.find("\"diagnostics\":[");
    bool hasDiag = false;
    if (diagPos != std::string::npos)
    {
        size_t nextChar = diagPos + 15;
        while (nextChar < msg.size() && (msg[nextChar] == ' ' || msg[nextChar] == '\t' || msg[nextChar] == '\r' || msg[nextChar] == '\n')) nextChar++;
        if (nextChar < msg.size() && msg[nextChar] != ']') hasDiag = true;
    }
    if (!hasDiag) return "[]";

    std::string fmtEdits = FormattingFor(text);
    return "[{"
           "\"title\":\"格式化此文档 (Format Document)\","
           "\"kind\":\"source.fixAll\","
           "\"edit\":{\"changes\":{" + JStr(uri) + ":" + fmtEdits + "}}"
           "}]";
}

static std::string SignatureHelpFor(const wstring& text, int line1, int col0)
{
    std::vector<size_t> off;
    ComputeLineOffsets(text, off);
    if (line1 < 1 || (size_t)line1 > off.size()) return "null";
    size_t ls = off[line1 - 1];
    size_t le = (size_t)line1 < off.size() ? off[line1] : text.size();
    while (le > ls && (text[le - 1] == L'\n' || text[le - 1] == L'\r')) le--;
    if (col0 < 0) col0 = 0;
    if ((size_t)col0 > le - ls) col0 = (int)(le - ls);
    size_t pos = ls + col0;

    int parenDepth = 0;
    size_t callParen = std::string::npos;
    int commas = 0;
    for (size_t i = pos; i > ls; --i)
    {
        wchar_t c = text[i - 1];
        if (c == L')') parenDepth++;
        else if (c == L'(')
        {
            if (parenDepth == 0)
            {
                callParen = i - 1;
                break;
            }
            parenDepth--;
        }
        else if (c == L',' && parenDepth == 0)
        {
            commas++;
        }
    }
    if (callParen == std::string::npos) return "null";

    size_t fnEnd = callParen;
    while (fnEnd > ls && (text[fnEnd - 1] == L' ' || text[fnEnd - 1] == L'\t')) fnEnd--;
    size_t fnStart = fnEnd;
    while (fnStart > ls && (::iswalnum(text[fnStart - 1]) || text[fnStart - 1] == L'_' || text[fnStart - 1] == L'.')) fnStart--;
    if (fnStart >= fnEnd) return "null";
    wstring fnName = text.substr(fnStart, fnEnd - fnStart);

    wstring sig = fnName + L"(...)";
    for (auto& kv : g_index)
    {
        for (auto& s : kv.second.syms)
        {
            if (s.name == fnName)
            {
                sig = s.qual.empty() ? s.name : s.qual;
                sig += L"(...)";
                break;
            }
        }
    }
    return "{\"signatures\":[{"
           "\"label\":" + JStr(sig) + ","
           "\"documentation\":\"Nodio 函数调用\","
           "\"parameters\":[{\"label\":\"arg0\"},{\"label\":\"arg1\"},{\"label\":\"arg2\"}]"
           "}],"
           "\"activeSignature\":0,"
           "\"activeParameter\":" + std::to_string(commas) + "}";
}

// 工作区符号搜索（项目符号搜索框）
static std::string WorkspaceSymbols(const wstring& query)
{
    wstring q = query;
    std::transform(q.begin(), q.end(), q.begin(), ::towlower);
    std::string out = "[";
    bool first = true;
    for (auto& kv : g_index)
    {
        for (auto& s : kv.second.syms)
        {
            if (!q.empty())
            {
                wstring n = s.name;
                std::transform(n.begin(), n.end(), n.begin(), ::towlower);
                if (n.find(q) == wstring::npos) continue;
            }
            if (!first) out += ",";
            first = false;
            out += "{\"name\":" + JStr(s.qual.empty() ? s.name : s.qual) +
                   ",\"kind\":" + SymbolKindNum(s.kind) +
                   ",\"location\":" +
                   LocationJson(s.uri, s.line, s.col, s.len) + "}";
        }
    }
    return out + "]";
}

// ---------------- main loop ----------------
int main()
{
    // LSP framing counts bytes — stdout/stdin must be binary so \n inside
    // the payload is not translated to \r\n on Windows
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);

    std::string msg;
    while (ReadMessage(msg))
    {
        std::string method = JsonString(msg, "method");
        std::string id = JsonId(msg);

        if (method == "initialize")
        {
            Respond(id,
                "{\"capabilities\":{"
                "\"textDocumentSync\":1,"
                "\"completionProvider\":{\"triggerCharacters\":[\".\"]},"
                "\"hoverProvider\":true,"
                "\"documentSymbolProvider\":true,"
                "\"documentHighlightProvider\":true,"
                "\"definitionProvider\":true,"
                "\"referencesProvider\":true,"
                "\"renameProvider\":true,"
                "\"workspaceSymbolProvider\":true,"
                "\"codeActionProvider\":true,"
                "\"signatureHelpProvider\":{\"triggerCharacters\":[\"(\",\",\"]},"
                "\"semanticTokensProvider\":{\"legend\":{\"tokenTypes\":[\"comment\",\"keyword\",\"string\",\"number\",\"function\",\"variable\",\"type\",\"parameter\",\"property\"],\"tokenModifiers\":[\"declaration\",\"definition\",\"readonly\",\"static\"]},\"full\":true,\"range\":true}"
                "},"
                "\"serverInfo\":{\"name\":\"nodio-lsp\",\"version\":\"0.3\"}}");
        }
        else if (method == "initialized")
        {
            // handshake ack — nothing to do
        }
        else if (method == "nodio/setProject")
        {
            SetProject(msg);
        }
        else if (method == "shutdown")
        {
            Respond(id, "null");
        }
        else if (method == "exit")
        {
            return 0;
        }
        else if (method == "textDocument/didOpen")
        {
            wstring uri = ParamUri(msg);
            wstring text = ParamNewText(msg);
            g_docs[uri] = { text, 0 };
            // 打开的文档立刻纳入索引：转到定义要看到**当前缓冲区**的符号，
            // 而不是磁盘上的旧版本。
            IndexFileWithText(UriToPath(uri), text);
            PublishDiagnostics(uri);
        }
        else if (method == "textDocument/didChange")
        {
            wstring uri = ParamUri(msg);
            wstring text = ParamNewText(msg);
            if (!text.empty() || g_docs.count(uri))
            {
                g_docs[uri] = { text, 0 };
                IndexFileWithText(UriToPath(uri), text);
            }
            PublishDiagnostics(uri);
        }
        else if (method == "textDocument/didClose")
        {
            g_docs.erase(ParamUri(msg));
        }
        else if (method == "textDocument/completion")
        {
            int line, col;
            ParamPos(msg, &line, &col);
            wstring uri = ParamUri(msg);
            auto it = g_docs.find(uri);
            std::string result = "null";
            if (it != g_docs.end())
                result = CompletionsFor(it->second, line, col, uri);
            Respond(id, result);
        }
        else if (method == "textDocument/hover")
        {
            int line, col;
            ParamPos(msg, &line, &col);
            wstring uri = ParamUri(msg);
            auto it = g_docs.find(uri);
            std::string result = "null";
            if (it != g_docs.end())
                result = HoverFor(it->second, line, col);
            Respond(id, result);
        }
        else if (method == "textDocument/documentSymbol")
        {
            wstring uri = ParamUri(msg);
            auto it = g_docs.find(uri);
            std::string result = "[]";
            if (it != g_docs.end())
            {
                ParseResult pr = ParseDocument(it->second.text);
                std::vector<DocSymbol> syms;
                if (pr.ast) CollectFromBlock(pr.ast, L"", syms);
                result = SymbolsJson(syms, uri);
            }
            Respond(id, result);
        }
        else if (method == "textDocument/documentHighlight")
        {
            int line, col;
            ParamPos(msg, &line, &col);
            wstring uri = ParamUri(msg);
            auto it = g_docs.find(uri);
            std::string result = "[]";
            if (it != g_docs.end())
                result = DocumentHighlightsFor(it->second.text, uri, line, col);
            Respond(id, result);
        }
        else if (method == "textDocument/definition")
        {
            int line, col;
            ParamPos(msg, &line, &col);
            wstring uri = ParamUri(msg);
            auto it = g_docs.find(uri);
            std::string result = "null";
            if (it != g_docs.end())
                result = DefinitionFor(it->second.text, UriToPath(uri), line, col);
            Respond(id, result);
        }
        else if (method == "textDocument/references")
        {
            int line, col;
            ParamPos(msg, &line, &col);
            wstring uri = ParamUri(msg);
            auto it = g_docs.find(uri);
            std::string result = "[]";
            if (it != g_docs.end())
                result = ReferencesFor(it->second.text, uri, line, col);
            Respond(id, result);
        }
        else if (method == "textDocument/rename")
        {
            int line, col;
            ParamPos(msg, &line, &col);
            wstring uri = ParamUri(msg);
            wstring newName = Wide(JsonString(msg, "newName"));
            auto it = g_docs.find(uri);
            std::string result = "null";
            if (it != g_docs.end() && !newName.empty())
                result = RenameFor(it->second.text, uri, line, col, newName);
            Respond(id, result);
        }
        else if (method == "textDocument/declaration" ||
              method == "textDocument/typeDefinition" ||
              method == "textDocument/implementation")
        {
            // aardio 单一符号宇宙：三类跳转与 definition 同义（第五十三轮）
            int line, col;
            ParamPos(msg, &line, &col);
            wstring uri = ParamUri(msg);
            auto it = g_docs.find(uri);
            std::string result = "null";
            if (it != g_docs.end())
                result = DefinitionFor(it->second.text, UriToPath(uri), line, col);
            Respond(id, result);
        }
        else if (method == "textDocument/prepareRename")
        {
            int line, col;
            ParamPos(msg, &line, &col);
            wstring uri = ParamUri(msg);
            auto it = g_docs.find(uri);
            std::string result = "null";
            if (it != g_docs.end())
                result = PrepareRenameFor(it->second.text, line, col);
            Respond(id, result);
        }
        else if (method == "textDocument/selectionRange")
        {
            int line, col;
            ParamPos(msg, &line, &col);
            wstring uri = ParamUri(msg);
            auto it = g_docs.find(uri);
            std::string result = "null";
            if (it != g_docs.end())
                result = SelectionRangeFor(it->second.text, line, col);
            Respond(id, result);
        }
        else if (method == "textDocument/foldingRange")
        {
            wstring uri = ParamUri(msg);
            auto it = g_docs.find(uri);
            std::string result = "[]";
            if (it != g_docs.end())
                result = FoldingRangeFor(it->second.text);
            Respond(id, result);
        }
        else if (method == "textDocument/formatting")
        {
            wstring uri = ParamUri(msg);
            auto it = g_docs.find(uri);
            std::string result = "[]";
            if (it != g_docs.end())
                result = FormattingFor(it->second.text);
            Respond(id, result);
        }
        else if (method == "textDocument/semanticTokens/full")
        {
            wstring uri = ParamUri(msg);
            auto it = g_docs.find(uri);
            std::string result = "{\"data\":[]}";
            if (it != g_docs.end())
                result = SemanticTokensFor(it->second.text);
            Respond(id, result);
        }
        else if (method == "textDocument/semanticTokens/range")
        {
            wstring uri = ParamUri(msg);
            auto it = g_docs.find(uri);
            std::string result = "{\"data\":[]}";
            if (it != g_docs.end())
            {
                int sl = 0, el = 999999;
                size_t pRange = msg.find("\"range\":");
                if (pRange != std::string::npos)
                {
                    std::string sStart = JsonNumber(msg.substr(pRange), "line");
                    if (!sStart.empty()) sl = atoi(sStart.c_str());
                    size_t pEnd = msg.find("\"end\":", pRange);
                    if (pEnd != std::string::npos)
                    {
                        std::string sEnd = JsonNumber(msg.substr(pEnd), "line");
                        if (!sEnd.empty()) el = atoi(sEnd.c_str());
                    }
                }
                result = SemanticTokensFor(it->second.text, sl, el);
            }
            Respond(id, result);
        }
        else if (method == "textDocument/codeAction")
        {
            wstring uri = ParamUri(msg);
            auto it = g_docs.find(uri);
            std::string result = "[]";
            if (it != g_docs.end())
                result = CodeActionFor(uri, msg, it->second.text);
            Respond(id, result);
        }
        else if (method == "textDocument/signatureHelp")
        {
            wstring uri = ParamUri(msg);
            auto it = g_docs.find(uri);
            std::string result = "null";
            if (it != g_docs.end())
            {
                int line = 1, col = 0;
                ParamPos(msg, &line, &col);
                result = SignatureHelpFor(it->second.text, line, col);
            }
            Respond(id, result);
        }
        else if (method == "workspace/symbol")
        {
            wstring query = Wide(JsonString(msg, "query"));
            Respond(id, WorkspaceSymbols(query));
        }
        else if (!id.empty())
        {
            // unimplemented request: reply null so the client never blocks
            Respond(id, "null");
        }
        // notifications (no id) we do not handle are dropped
    }
    return 0;
}
