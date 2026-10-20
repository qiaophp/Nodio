// apps/nodio-dap/main.cpp — Nodio Debug Adapter (DAP over stdio).
//
// The adapter owns the DAP session and bridges it to a debuggee:
//   launch  -> spawn nodio.exe <script> with two unidirectional control
//              pipes: NODIO_DEBUG_PIPE (events, engine->adapter) and
//              NODIO_DEBUG_CMD (commands, adapter->engine)
//   engine  -> {"event":"stopped","line":N,"vars":{...}} on the events pipe
//   adapter -> DAP stopped event; continue/next -> {"cmd":...} on cmds pipe
//
// Two pipes (not one duplex) because a synchronous handle cannot reliably
// support a blocking reader thread and concurrent writer threads.
#include "apps/jsonrpc_stdio.h"

#include <windows.h>

#include <cstdio>
#include <fcntl.h>
#include <io.h>
#include <string>
#include <vector>

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

// ---------------- debuggee session state ----------------

static HANDLE g_hEvPipe = INVALID_HANDLE_VALUE;  // events (engine -> us)
static HANDLE g_hCmdPipe = INVALID_HANDLE_VALUE; // commands (us -> engine)
static HANDLE g_hProcess = nullptr;
static volatile long g_stopped = 0;              // engine is stopped
static std::string g_sourcePath;
static std::string g_lastVarsJson = "{}";        // latest vars from the engine

// minimal JSON string field reader (values without escapes)
static std::string FindStringField(const std::string& s, const char* key)
{
    std::string pat = std::string("\"") + key + "\":";
    size_t p = s.find(pat);
    if (p == std::string::npos) return "";
    p += pat.size();
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t'))
        p++;   // tolerate ": value" spacing from client JSON encoders
    if (p < s.size() && s[p] == '"')
    {
        size_t e = s.find('"', p + 1);
        if (e == std::string::npos) return "";
        return s.substr(p + 1, e - p - 1);
    }
    // number
    size_t e = s.find_first_of(",}\r\n", p);
    return e == std::string::npos ? s.substr(p) : s.substr(p, e - p);
}

static std::string VarsBody(const std::string& varsJson)
{
    // varsJson: {"name":"value",...} -> DAP variables array (flat)
    std::string out = "[";
    size_t i = 0, n = varsJson.size();
    bool first = true;
    while (i < n)
    {
        size_t k0 = varsJson.find('"', i);
        if (k0 == std::string::npos) break;
        size_t k1 = varsJson.find('"', k0 + 1);
        size_t c0 = varsJson.find(':', k1);
        if (k1 == std::string::npos || c0 == std::string::npos) break;
        size_t v0 = varsJson.find('"', c0);
        size_t v1 = v0 == std::string::npos ? std::string::npos
                                            : varsJson.find('"', v0 + 1);
        if (v1 == std::string::npos) break;
        if (!first) out += ",";
        first = false;
        out += "{\"name\":\"" + varsJson.substr(k0 + 1, k1 - k0 - 1) +
               "\",\"value\":\"" + varsJson.substr(v0 + 1, v1 - v0 - 1) +
               "\",\"type\":\"string\",\"variablesReference\":0}";
        i = v1 + 1;
    }
    return out + "]";
}

// reader thread: consumes {"event":...} lines from the events pipe
static DWORD WINAPI ReaderThread(LPVOID)
{
    std::string line;
    char ch;
    DWORD got = 0;
    while (ReadFile(g_hEvPipe, &ch, 1, &got, nullptr) && got == 1)
    {
        if (ch == '\r') continue;
        if (ch != '\n') { line += ch; continue; }
        if (line.empty()) continue;
        std::string ev = FindStringField(line, "event");
        if (ev == "stopped")
        {
            size_t v = line.find("\"vars\":");
            if (v != std::string::npos)
            {
                size_t vStart = v + 7;
                size_t vEnd = line.rfind('}');
                if (vEnd != std::string::npos && vEnd > vStart)
                    g_lastVarsJson = line.substr(vStart, vEnd - vStart);
            }
            // forward the engine's 1-based stop line (0-based for DAP)
            std::string ln = JsonNumber(line, "line");
            int nLine0 = ln.empty() ? 0 : atoi(ln.c_str()) - 1;
            if (nLine0 < 0) nLine0 = 0;
            InterlockedExchange(&g_stopped, 1);
            Event("stopped",
                  "{\"reason\":\"breakpoint\",\"threadId\":1,"
                  "\"line\":" + std::to_string(nLine0) +
                  ",\"allThreadsStopped\":true}");
        }
        else if (ev == "terminated")
        {
            Event("terminated", "");
            break;
        }
        line.clear();
    }
    // events pipe closed — the debuggee is gone
    InterlockedExchange(&g_stopped, 0);
    Event("terminated", "");
    return 0;
}

static bool SendToEngine(const std::string& cmd)
{
    if (g_hCmdPipe == INVALID_HANDLE_VALUE) return false;
    std::string wire = cmd + "\n";
    DWORD wrote = 0;
    return WriteFile(g_hCmdPipe, wire.data(), (DWORD)wire.size(), &wrote,
                     nullptr) && wrote == wire.size();
}

static std::string Quote(const std::string& s)
{
    return "\"" + s + "\"";
}

int main()
{
    // DAP framing is byte-counted — stdio must be binary on Windows
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);

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
            std::string program = JsonString(msg, "program");
            g_sourcePath = program;
            // the debuggee exe: arguments.nodioExe, or $NODIO_EXE, or a
            // nodio.exe next to this adapter
            std::string exe = JsonString(msg, "nodioExe");
            if (exe.empty())
            {
                char szEnv[MAX_PATH] = "";
                if (GetEnvironmentVariableA("NODIO_EXE", szEnv, MAX_PATH) > 0)
                    exe = szEnv;
            }
            if (exe.empty())
            {
                char szSelf[MAX_PATH] = "";
                if (GetModuleFileNameA(nullptr, szSelf, MAX_PATH) > 0)
                {
                    std::string dir = szSelf;
                    size_t s = dir.find_last_of("\\/");
                    exe = dir.substr(0, s + 1) + "nodio.exe";
                }
            }
            // create the two control pipes
            g_hEvPipe = CreateNamedPipeA(
                "\\\\.\\pipe\\nodio-dap-events",
                PIPE_ACCESS_INBOUND, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE |
                                     PIPE_WAIT,
                1, 4096, 4096, 0, nullptr);
            g_hCmdPipe = CreateNamedPipeA(
                "\\\\.\\pipe\\nodio-dap-cmds",
                PIPE_ACCESS_OUTBOUND, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE |
                                      PIPE_WAIT,
                1, 4096, 4096, 0, nullptr);
            if (g_hEvPipe == INVALID_HANDLE_VALUE ||
                g_hCmdPipe == INVALID_HANDLE_VALUE)
            {
                Reply(seq, command,
                      "{\"error\":\"cannot create control pipes\"}", false);
                continue;
            }
            // spawn the debuggee. The child must inherit a FULL environment
            // (SystemRoot etc.), so set the variables in our own env and
            // pass nullptr — the adapter is short-lived, this is safe.
            SetEnvironmentVariableA("NODIO_DEBUG_PIPE",
                                    "\\\\.\\pipe\\nodio-dap-events");
            SetEnvironmentVariableA("NODIO_DEBUG_CMD",
                                    "\\\\.\\pipe\\nodio-dap-cmds");
            std::string cmdStr = "\"" + exe + "\" \"" + program + "\"";
            std::vector<char> cmdLine(cmdStr.begin(), cmdStr.end());
            cmdLine.push_back('\0');
            STARTUPINFOA si{};
            si.cb = sizeof(si);
            PROCESS_INFORMATION pi{};
            BOOL ok = CreateProcessA(nullptr, cmdLine.data(),
                                     nullptr, nullptr, FALSE,
                                     CREATE_NEW_CONSOLE, nullptr,
                                     nullptr, &si, &pi);
            if (!ok)
            {
                CloseHandle(g_hEvPipe);
                CloseHandle(g_hCmdPipe);
                g_hEvPipe = g_hCmdPipe = INVALID_HANDLE_VALUE;
                Reply(seq, command, "{\"error\":\"cannot launch debuggee\"}",
                      false);
                continue;
            }
            g_hProcess = pi.hProcess;
            // wait for the engine to connect BOTH pipes
            BOOL cEv = ConnectNamedPipe(g_hEvPipe, nullptr);
            DWORD gleEv = GetLastError();
            BOOL cCmd = ConnectNamedPipe(g_hCmdPipe, nullptr);
            DWORD gleCmd = GetLastError();
            if ((!cEv && gleEv != ERROR_PIPE_CONNECTED) ||
                (!cCmd && gleCmd != ERROR_PIPE_CONNECTED))
            {
                Reply(seq, command, "{\"error\":\"engine never connected\"}",
                      false);
                continue;
            }
            CreateThread(nullptr, 0, ReaderThread, nullptr, 0, nullptr);
            Reply(seq, command, "");
            Event("process",
                  "{\"name\":" + Quote(program) + ",\"systemProcessId\":" +
                  std::to_string(pi.dwProcessId) + ",\"isLocalProcess\":true,"
                  "\"startMethod\":\"launch\"}");
            // the engine stops at the first statement (bStep=true on connect)
            Event("initialized", "");
        }
        else if (command == "setBreakPoints" || command == "setBreakpoints")
        {
            // body: {breakpoints:[{line:N}...]} — 1-based source lines.
            // Forward live to the engine via the command pipe (also works
            // while stopped; the engine re-arms its breakpoint set).
            std::string body = JsonString(msg, "body");
            std::string lines;
            {
                std::string b = msg;
                size_t p = b.find("\"breakpoints\"");
                if (p != std::string::npos)
                {
                    size_t e = b.find(']', p);
                    if (e != std::string::npos)
                        b = b.substr(p, e - p);
                    size_t pos = 0;
                    bool first = true;
                    while ((pos = b.find("\"line\":", pos)) !=
                           std::string::npos)
                    {
                        pos += 7;
                        size_t e2 = b.find_first_of(",}]", pos);
                        if (e2 == std::string::npos) break;
                        if (!first) lines += ",";
                        lines += b.substr(pos, e2 - pos);
                        first = false;
                        pos = e2;
                    }
                }
            }
            if (!lines.empty())
                SendToEngine("{\"cmd\":\"bp\",\"lines\":\"" + lines + "\"}");
            Reply(seq, command, "{\"breakpoints\":[]}");
        }
        else if (command == "threads")
        {
            Reply(seq, command,
                  "{\"threads\":[{\"id\":1,\"name\":\"main\"}]}");
        }
        else if (command == "stackTrace")
        {
            std::string name = g_sourcePath;
            size_t slash = name.find_last_of("\\/");
            if (slash != std::string::npos) name = name.substr(slash + 1);
            Reply(seq, command,
                  "{\"stackFrames\":[{\"id\":1,\"name\":" + Quote(name) +
                  ",\"line\":1,\"column\":1,\"source\":{\"name\":" +
                  Quote(name) + ",\"path\":" + Quote(g_sourcePath) +
                  "}}],\"totalFrames\":1}");
        }
        else if (command == "scopes")
        {
            Reply(seq, command,
                  "{\"scopes\":[{\"name\":\"Local\",\"variablesReference\":100,"
                  "\"expensive\":false}]}");
        }
        else if (command == "variables")
        {
            // serve the latest vars snapshot captured at the last stop
            Reply(seq, command,
                  "{\"variables\":" + VarsBody(g_lastVarsJson) + "}");
        }
        else if (command == "continue")
        {
            SendToEngine("{\"cmd\":\"continue\"}");
            InterlockedExchange(&g_stopped, 0);
            Reply(seq, command, "{\"allThreadsContinued\":true}");
        }
        else if (command == "next" || command == "stepIn" ||
                 command == "stepOut")
        {
            SendToEngine("{\"cmd\":\"next\"}");
            InterlockedExchange(&g_stopped, 0);
            Reply(seq, command, "");
        }
        else if (command == "pause")
        {
            Reply(seq, command, "");
        }
        else if (command == "configurationDone")
        {
            Reply(seq, command, "");
        }
        else if (command == "disconnect" || command == "terminate")
        {
            SendToEngine("{\"cmd\":\"disconnect\"}");
            if (g_hProcess)
            {
                TerminateProcess(g_hProcess, 0);
                CloseHandle(g_hProcess);
                g_hProcess = nullptr;
            }
            Reply(seq, command, "");
            Event("terminated", "");
            return 0;
        }
        else
        {
            Reply(seq, command, "");
        }
    }
    return 0;
}
