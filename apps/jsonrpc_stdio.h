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

namespace nodio
{
    namespace rpc
    {
        // Read one framed message from stdin. Returns false on EOF/short read.
        inline bool ReadMessage(std::string& out)
        {
            out.clear();
            int nLen = -1;
            for (;;)
            {
                char line[512];
                if (!fgets(line, sizeof line, stdin)) return false;
                std::string h(line);
                while (!h.empty() &&
                       (h.back() == '\n' || h.back() == '\r'))
                    h.pop_back();
                if (h.empty()) break;
                static const char* kCL = "Content-Length:";
                if (h.rfind(kCL, 0) == 0)
                    nLen = atoi(h.c_str() + strlen(kCL));
            }
            if (nLen < 0) return false;
            out.resize((size_t)nLen);
            size_t got = fread(&out[0], 1, (size_t)nLen, stdin);
            return got == (size_t)nLen;
        }

        inline void WriteMessage(const std::string& body)
        {
            fprintf(stdout, "Content-Length: %zu\r\n\r\n", body.size());
            fwrite(body.data(), 1, body.size(), stdout);
            fflush(stdout);
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
                    char n = s[++i];
                    out.push_back(n == 'n' ? '\n' : n == 't' ? '\t' : n);
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
