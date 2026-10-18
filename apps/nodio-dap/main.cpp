// apps/nodio-dap/main.cpp — Nodio Debug Adapter (DAP over stdio).
//
// Milestone M3. Current stage: protocol skeleton — it negotiates
// capabilities, accepts launch/attach, and drives the request/response +
// event handshake so a client can connect. The actual debuggee control
// channel (named pipe into the engine's DebugStop) is wired next.
#include "apps/jsonrpc_stdio.h"

#include <string>

using namespace nodio::rpc;

static int g_seq = 1;

static void Reply(const std::string& requestSeq, const std::string& command,
                  const std::string& body, bool success = true)
{
    std::string j = "{\"seq\":" + std::to_string(g_seq++) +
        ",\"type\":\"response\",\"request_seq\":" +
        (requestSeq.empty() ? "0" : requestSeq) +
        ",\"success\":" + (success ? "true" : "false") +
        ",\"command\":\"" + command + "\"";
    if (!body.empty()) j += ",\"body\":" + body;
    j += "}";
    WriteMessage(j);
}

static void Event(const std::string& name, const std::string& body)
{
    std::string j = "{\"seq\":" + std::to_string(g_seq++) +
        ",\"type\":\"event\",\"event\":\"" + name + "\"";
    if (!body.empty()) j += ",\"body\":" + body;
    j += "}";
    WriteMessage(j);
}

int main()
{
    std::string msg;
    while (ReadMessage(msg))
    {
        std::string command = JsonString(msg, "command");
        std::string seq = JsonNumber(msg, "seq");

        if (command == "initialize")
        {
            Reply(seq, command,
                "{\"supportsConfigurationDoneRequest\":true,"
                "\"supportsFunctionBreakpoints\":false,"
                "\"supportsConditionalBreakpoints\":false}");
        }
        else if (command == "launch" || command == "attach")
        {
            Reply(seq, command, "");
            // DAP handshake: the adapter announces it is ready for
            // breakpoints, the client answers with configurationDone.
            Event("initialized", "");
        }
        else if (command == "configurationDone")
        {
            Reply(seq, command, "");
        }
        else if (command == "setBreakpoints")
        {
            Reply(seq, command, "{\"breakpoints\":[]}");
        }
        else if (command == "disconnect")
        {
            Reply(seq, command, "");
            return 0;
        }
        else
        {
            Reply(seq, command, "");
        }
    }
    return 0;
}
