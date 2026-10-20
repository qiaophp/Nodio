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

// a symbol collected from the AST — a structured entity, not parallel lists
struct DocSymbol
{
    wstring name;
    wstring kind;          // "namespace" | "class" | "function" | "variable"
    int line = 0;          // 1-based
    std::vector<DocSymbol> children;
};

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
        DocSymbol s{ full, L"namespace", n->line, {} };
        if (n->a) CollectFromBlock(n->a, full, s.children);
        out.push_back(std::move(s));
        break;
    }
    case Node::Class:
    {
        wstring full = nsPrefix;
        for (auto& seg : n->names) full = JoinNs(full, seg);
        DocSymbol s{ full, L"class", n->line, {} };
        if (n->a) CollectFromBlock(n->a, full, s.children);
        // class body members live in n->a->list (assignment / ctor forms)
        out.push_back(std::move(s));
        break;
    }
    case Node::VarDecl:
    {
        for (auto& nm : n->names)
        {
            if (!nm.empty())
                out.push_back({ nm, L"variable", n->line, {} });
        }
        break;
    }
    case Node::Assign:
        // function f(){} parses to Assign(Ident f, FuncLit)
        if (n->a && n->a->k == Node::Ident && n->b &&
            n->b->k == Node::FuncLit)
        {
            out.push_back({ n->a->str, L"function", n->line, {} });
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

static std::string CompletionsFor(const Document& doc, int line, int col)
{
    // member completion after '.': offer namespace/class names + members
    // seen in the document; otherwise offer keywords + document words
    std::string items;
    bool first = true;
    auto add = [&](const wstring& label, const char* kind, const wchar_t* detail)
    {
        if (!first) items += ",";
        first = false;
        items += CompletionItem(label, kind, detail);
    };
    ParseResult pr = ParseDocument(doc.text);
    std::set<wstring> words;
    if (pr.ast) CollectWords(pr.ast, words);
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
                "\"documentSymbolProvider\":true"
                "},"
                "\"serverInfo\":{\"name\":\"nodio-lsp\",\"version\":\"0.2\"}}");
        }
        else if (method == "initialized")
        {
            // handshake ack — nothing to do
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
            PublishDiagnostics(uri);
        }
        else if (method == "textDocument/didChange")
        {
            wstring uri = ParamUri(msg);
            wstring text = ParamNewText(msg);
            if (!text.empty() || g_docs.count(uri))
                g_docs[uri] = { text, 0 };
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
                result = CompletionsFor(it->second, line, col);
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
        else if (!id.empty())
        {
            // unimplemented request: reply null so the client never blocks
            Respond(id, "null");
        }
        // notifications (no id) we do not handle are dropped
    }
    return 0;
}
