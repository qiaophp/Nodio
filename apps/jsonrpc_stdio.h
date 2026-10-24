// apps/jsonrpc_stdio.h — minimal Content-Length framed JSON-RPC transport
// over stdio, shared by nodio-lsp and nodio-dap.
//
// This is deliberately small: the servers own their message schemas, this
// only handles the framing (LSP/DAP both use `Content-Length: N\r\n\r\n`
// headers) and a few best-effort field readers good enough to peek at
// "method" / "id" / "command" / "seq" without pulling in a full JSON DOM.
// The engine's json.cpp is write-only, so the readers below stay hand-rolled
// until a real DOM is needed.
#pragma once

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <windows.h>

namespace nodio
{
    namespace rpc
    {
        // Read one framed message from stdin. Returns false on EOF/short read.
        // Header lines and the body are read with raw ReadFile instead of
        // the CRT's fgets/fread: a peer that writes the exact framed bytes
        // without a trailing newline (e.g. an aardio popen host) never
        // satisfies fgets' full-buffer refill on anonymous pipes, while
        // ReadFile returns whatever has already arrived.
        inline bool ReadMessage(std::string& out)
        {
            out.clear();
            int nLen = -1;
            HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
            auto rdByte = [&](char* c) -> bool
            {
                DWORD got = 0;
                BOOL ok = ReadFile(hIn, c, 1, &got, nullptr);
                return ok && got == 1;
            };
            for (;;)
            {
                std::string h;
                for (;;)
                {
                    char c;
                    if (!rdByte(&c)) return false;
                    if (c == '\n') break;
                    if (c != '\r') h += c;
                    if (h.size() > 4096) return false;
                }
                if (h.empty())
                {
                    // blank line: end of headers — but tolerate stray
                    // blank lines BETWEEN messages (clients may append a
                    // newline after the body to flush pipe buffers)
                    if (nLen < 0) continue;
                    break;
                }
                static const char* kCL = "Content-Length:";
                if (h.rfind(kCL, 0) == 0)
                    nLen = atoi(h.c_str() + strlen(kCL));
            }
            if (nLen < 0) return false;
            out.resize((size_t)nLen);
            DWORD got = 0;
            size_t total = 0;
            while (total < (size_t)nLen)
            {
                if (!ReadFile(hIn, &out[total], (DWORD)((size_t)nLen - total),
                              &got, nullptr) || got == 0)
                    return false;
                total += got;
            }
            return true;
        }

        inline void WriteMessage(const std::string& body)
        {
            fprintf(stdout, "Content-Length: %zu\r\n\r\n", body.size());
            fwrite(body.data(), 1, body.size(), stdout);
            fflush(stdout);
        }

        // ---- JSON string escape decoding ----
        //
        // The transport carries source code, and source code is full of
        // control characters (CRLF especially). Every escape the JSON spec
        // defines has to be decoded EXACTLY, because a mis-decoded escape
        // silently rewrites the payload instead of failing loudly:
        // decoding "\r" as the literal letter 'r' turned every CRLF line
        // ending into a stray 'r' token and made the parser report bogus
        // syntax errors on perfectly valid files.
        inline int HexVal(char c)
        {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        }

        inline void AppendUtf8(std::string& out, unsigned cp)
        {
            if (cp < 0x80)
                out.push_back((char)cp);
            else if (cp < 0x800)
            {
                out.push_back((char)(0xC0 | (cp >> 6)));
                out.push_back((char)(0x80 | (cp & 0x3F)));
            }
            else if (cp < 0x10000)
            {
                out.push_back((char)(0xE0 | (cp >> 12)));
                out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back((char)(0x80 | (cp & 0x3F)));
            }
            else
            {
                out.push_back((char)(0xF0 | (cp >> 18)));
                out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
                out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back((char)(0x80 | (cp & 0x3F)));
            }
        }

        // Decode the escape sequence starting at s[i] (s[i] == '\\').
        // Appends the decoded bytes to `out` and returns how many source
        // characters were consumed.
        inline size_t JsonUnescape(const std::string& s, size_t i, std::string& out)
        {
            if (i + 1 >= s.size()) { out.push_back('\\'); return 1; }
            char n = s[i + 1];
            switch (n)
            {
            case 'n': out.push_back('\n'); return 2;
            case 't': out.push_back('\t'); return 2;
            case 'r': out.push_back('\r'); return 2;
            case 'b': out.push_back('\b'); return 2;
            case 'f': out.push_back('\f'); return 2;
            case '"': out.push_back('"'); return 2;
            case '\\': out.push_back('\\'); return 2;
            case '/': out.push_back('/'); return 2;
            case 'u':
            {
                if (i + 5 >= s.size()) { out.push_back('\\'); out.push_back('u'); return 2; }
                int h0 = HexVal(s[i + 2]), h1 = HexVal(s[i + 3]);
                int h2 = HexVal(s[i + 4]), h3 = HexVal(s[i + 5]);
                if (h0 < 0 || h1 < 0 || h2 < 0 || h3 < 0)
                { out.push_back('\\'); out.push_back('u'); return 2; }
                unsigned cp = (unsigned)((h0 << 12) | (h1 << 8) | (h2 << 4) | h3);
                size_t used = 6;
                // surrogate pair -> one code point
                if (cp >= 0xD800 && cp <= 0xDBFF && i + 11 < s.size() &&
                    s[i + 6] == '\\' && s[i + 7] == 'u')
                {
                    int g0 = HexVal(s[i + 8]), g1 = HexVal(s[i + 9]);
                    int g2 = HexVal(s[i + 10]), g3 = HexVal(s[i + 11]);
                    if (g0 >= 0 && g1 >= 0 && g2 >= 0 && g3 >= 0)
                    {
                        unsigned lo = (unsigned)((g0 << 12) | (g1 << 8) | (g2 << 4) | g3);
                        if (lo >= 0xDC00 && lo <= 0xDFFF)
                        {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            used = 12;
                        }
                    }
                }
                AppendUtf8(out, cp);
                return used;
            }
            default:
                out.push_back(n); return 2;
            }
        }

        // First string value of a top-level "key": "..." (escapes unescaped
        // for the common cases; returns {} when absent).
        inline std::string JsonString(const std::string& s, const char* key)
        {
            std::string pat = std::string("\"") + key + "\"";
            size_t p = s.find(pat);
            if (p == std::string::npos) return {};
            p = s.find(':', p + pat.size());
            if (p == std::string::npos) return {};
            p = s.find('"', p + 1);
            if (p == std::string::npos) return {};
            std::string out;
            for (size_t i = p + 1; i < s.size(); ++i)
            {
                char c = s[i];
                if (c == '\\')
                {
                    i += JsonUnescape(s, i, out) - 1;
                    continue;
                }
                if (c == '"') break;
                out.push_back(c);
            }
            return out;
        }

        // Raw numeric value of "key": N (returns {} when absent / not numeric).
        inline std::string JsonNumber(const std::string& s, const char* key)
        {
            std::string pat = std::string("\"") + key + "\"";
            size_t p = s.find(pat);
            if (p == std::string::npos) return {};
            p = s.find(':', p + pat.size());
            if (p == std::string::npos) return {};
            ++p;
            while (p < s.size() && isspace((unsigned char)s[p])) ++p;
            size_t e = p;
            while (e < s.size() &&
                   (isdigit((unsigned char)s[e]) || s[e] == '-' ||
                    s[e] == '+' || s[e] == '.' || s[e] == 'e' || s[e] == 'E'))
                ++e;
            return e > p ? s.substr(p, e - p) : std::string();
        }

        // String elements of a top-level "key": [ "a", "b", ... ].
        // The transport has no JSON DOM, and the project index needs the
        // whole file list at once, so this reads the array directly.
        inline std::vector<std::string> JsonStringArray(const std::string& s,
                                                       const char* key)
        {
            std::vector<std::string> out;
            std::string pat = std::string("\"") + key + "\"";
            size_t p = s.find(pat);
            if (p == std::string::npos) return out;
            p = s.find(':', p + pat.size());
            if (p == std::string::npos) return out;
            p = s.find('[', p + 1);
            if (p == std::string::npos) return out;
            ++p;
            while (p < s.size())
            {
                while (p < s.size() && (isspace((unsigned char)s[p]) || s[p] == ',')) ++p;
                if (p >= s.size() || s[p] == ']') break;
                if (s[p] != '"') break;
                ++p;
                std::string v;
                while (p < s.size() && s[p] != '"')
                {
                    char c = s[p];
                    if (c == '\\' && p + 1 < s.size())
                    {
                        p += JsonUnescape(s, p, v) - 1;
                        ++p;
                        continue;
                    }
                    v.push_back(c);
                    ++p;
                }
                if (p < s.size()) ++p;    // closing quote
                out.push_back(v);
            }
            return out;
        }

        // JSON-RPC "id": may be a number or a string. Returns the raw token
        // for echoing back (numbers verbatim, strings re-quoted).
        inline std::string JsonId(const std::string& s)
        {
            size_t p = s.find("\"id\"");
            if (p == std::string::npos) return {};
            p = s.find(':', p + 4);
            if (p == std::string::npos) return {};
            ++p;
            while (p < s.size() && isspace((unsigned char)s[p])) ++p;
            if (p < s.size() && s[p] == '"')
                return "\"" + JsonString(s, "id") + "\"";
            return JsonNumber(s, "id");
        }
    } // namespace rpc
} // namespace nodio
