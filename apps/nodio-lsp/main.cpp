// apps/nodio-lsp/main.cpp — Nodio Language Server (LSP over stdio).
//
// Milestone M2. Current stage: protocol skeleton — it negotiates
// capabilities and answers/acks every message so an editor can attach and
// stay connected. The requests are wired to the engine parser (lexer.cpp /
// parser.h are linked here) next; until then unknown requests get a null
// result rather than a hang.
#include "apps/jsonrpc_stdio.h"

#include <string>

using namespace nodio::rpc;

static void Respond(const std::string& id, const std::string& result)
{
    WriteMessage("{\"jsonrpc\":\"2.0\",\"id\":" + id +
                 ",\"result\":" + result + "}");
}

int main()
{
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
                "\"definitionProvider\":true,"
                "\"documentSymbolProvider\":true"
                "},"
                "\"serverInfo\":{\"name\":\"nodio-lsp\",\"version\":\"0.1\"}}");
        }
        else if (method == "shutdown")
        {
            Respond(id, "null");
        }
        else if (method == "exit")
        {
            return 0;
        }
        else if (!id.empty())
        {
            // request we do not implement yet: reply null so the client
            // never blocks waiting on an answer
            Respond(id, "null");
        }
        // notifications (no id) are ignored in the skeleton
    }
    return 0;
}
