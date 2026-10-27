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

// 第五十五轮：表达式求值（evaluate）的请求-响应关联。
// 引擎回一条 {"event":"eval",...}，适配器把它转成 DAP response body。
// 顺序：先写 g_lastEvalBody，再置 g_evalReady（主循环看到标志才读串）。
static volatile long g_evalReady = 0;
static std::string g_lastEvalBody;

// 第五十九轮续：stackTrace 的请求-响应关联（{"cmd":"stack"} →
// {"event":"stack","frames":[...]}）。g_lastStackBody 保存 frames 数组
// 的**原文**（名字是 \uXXXX 转义，本来就是合法 JSON 片段，可复用）。
static volatile long g_stackReady = 0;
static std::string g_lastStackBody;
static int g_lastStopLine0 = 0;   // 最近一次 stopped 的 0 基行号（兜底帧用）

// 第五十九轮续：变量树（varsRef）与 setVariable 的请求-响应关联。
// g_lastVarsJson / g_lastVarsRefJson 保存引擎 vars 数组的**原文**
// ([{"name","type","value","ref"}]，\uXXXX 转义、字符串感知切分后才
// 取字段)，variables 请求统一走 VarsBody 转 DAP 形状。
static volatile long g_varsRefReady = 0;
static std::string g_lastVarsRefJson;
static volatile long g_setVarReady = 0;
static std::string g_lastSetVarBody;

static bool JsonStrFieldDecoded(const std::string& line, const char* key,
                                 std::string& out);
static std::string Quote(const std::string& s);

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
    // 第五十九轮续：varsJson 已是引擎的**结构化数组**——
    //   [{"name":"x","type":"number","value":"7","ref":0},...]
    // 逐对象（字符串感知配平花括号——值串里可能有 {} 或引号）转成
    // DAP variables 条目；ref 原样透传（0=不可展开，>100=varsRef 容器）。
    std::string out = "[";
    size_t pos = 0;
    bool first = true;
    while ((pos = varsJson.find('{', pos)) != std::string::npos)
    {
        size_t j = pos + 1;
        bool oStr = false;
        int oDepth = 1;
        while (j < varsJson.size() && oDepth > 0)
        {
            char c = varsJson[j];
            if (oStr)
            {
                if (c == '\\') j++;
                else if (c == '"') oStr = false;
            }
            else
            {
                if (c == '"') oStr = true;
                else if (c == '{') oDepth++;
                else if (c == '}') oDepth--;
            }
            j++;
        }
        if (j > varsJson.size()) j = varsJson.size();
        std::string obj = varsJson.substr(pos, j - pos);
        pos = j;
        std::string nm, ty, vl;
        bool bNm = JsonStrFieldDecoded(obj, "name", nm);
        JsonStrFieldDecoded(obj, "type", ty);
        bool bVl = JsonStrFieldDecoded(obj, "value", vl);
        std::string rf = JsonNumber(obj, "ref");
        if (!bNm && !bVl) continue;
        if (!first) out += ",";
        first = false;
        out += "{\"name\":" + Quote(nm) + ",\"value\":" + Quote(vl) +
               ",\"type\":" + Quote(ty) + ",\"variablesReference\":" +
               (rf.empty() ? "0" : rf) + "}";
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
            // 第五十九轮续：vars 现在是数组——字符串感知配平到匹配的
            // ']'（值串里可能有 ']'，JSON 不转义它，rfind 会吃错）。
            size_t v = line.find("\"vars\":[");
            if (v != std::string::npos)
            {
                size_t i = v + 8;
                bool inStr = false;
                int depth = 1;
                while (i < line.size() && depth > 0)
                {
                    char c = line[i];
                    if (inStr)
                    {
                        if (c == '\\') i++;
                        else if (c == '"') inStr = false;
                    }
                    else
                    {
                        if (c == '"') inStr = true;
                        else if (c == '[') depth++;
                        else if (c == ']') depth--;
                    }
                    i++;
                }
                if (i > line.size()) i = line.size();
                g_lastVarsJson = line.substr(v + 8, i - v - 9);
            }
            else g_lastVarsJson = "[]";
            // forward the engine's 1-based stop line (0-based for DAP)
            std::string ln = JsonNumber(line, "line");
            int nLine0 = ln.empty() ? 0 : atoi(ln.c_str()) - 1;
            if (nLine0 < 0) nLine0 = 0;
            g_lastStopLine0 = nLine0;
            // 第五十九轮：停因（"breakpoint"/"step"）原样透传给 DAP 客户端
            std::string reason = FindStringField(line, "reason");
            if (reason != "step") reason = "breakpoint";
            InterlockedExchange(&g_stopped, 1);
            Event("stopped",
                  "{\"reason\":\"" + reason + "\",\"threadId\":1,"
                  "\"line\":" + std::to_string(nLine0) +
                  ",\"allThreadsStopped\":true}");
        }
        else if (ev == "terminated")
        {
            Event("terminated", "");
            break;
        }
        else if (ev == "stack")
        {
            // 第五十九轮续：引擎回的真实调用栈 → 保存 frames 数组原文
            //   {"event":"stack","frames":[{"name":"b()","line":5},...]}
            // 名字是 \uXXXX 转义（纯 ASCII），原文即合法 JSON 片段。
            size_t p = line.find("\"frames\":[");
            if (p != std::string::npos)
            {
                size_t a = p + 9;                    // '[' 位置
                size_t e = line.rfind(']');
                if (e != std::string::npos && e > a)
                    g_lastStackBody = line.substr(a + 1, e - a - 1);
            }
            else g_lastStackBody.clear();
            InterlockedExchange(&g_stackReady, 1);
        }
        else if (ev == "varsRef")
        {
            // 第五十九轮续：varsRef 应答 → 保存 vars 数组原文。
            //   {"event":"varsRef","vars":[...][,"error":"..."]}
            // 展开失败且数组为空时伪造一条错误条目，客户端至少能看到原因。
            std::string errTxt;
            bool bErr = JsonStrFieldDecoded(line, "error", errTxt);
            size_t p = line.find("\"vars\":[");
            if (p != std::string::npos)
            {
                size_t i = p + 8;
                bool inStr = false;
                int depth = 1;
                while (i < line.size() && depth > 0)
                {
                    char c = line[i];
                    if (inStr)
                    {
                        if (c == '\\') i++;
                        else if (c == '"') inStr = false;
                    }
                    else
                    {
                        if (c == '"') inStr = true;
                        else if (c == '[') depth++;
                        else if (c == ']') depth--;
                    }
                    i++;
                }
                if (i > line.size()) i = line.size();
                g_lastVarsRefJson = line.substr(p + 8, i - p - 9);
            }
            else g_lastVarsRefJson.clear();
            if (bErr && !errTxt.empty() && g_lastVarsRefJson.empty())
                g_lastVarsRefJson =
                    "{\"name\":\"(展开失败)\",\"type\":\"error\","
                    "\"value\":" + Quote(errTxt) + ",\"ref\":0}";
            InterlockedExchange(&g_varsRefReady, 1);
        }
        else if (ev == "setVar")
        {
            // 第五十九轮续：setVar 应答 → setVariable 响应体。
            //   {"event":"setVar","ok":true,"value":"..","type":".."}
            //   {"event":"setVar","ok":false,"error":".."}
            std::string body;
            if (line.find("\"ok\":true") != std::string::npos)
            {
                std::string val, type;
                JsonStrFieldDecoded(line, "value", val);
                JsonStrFieldDecoded(line, "type", type);
                body = "{\"success\":true,\"value\":" + Quote(val) +
                       ",\"type\":" + Quote(type) + "}";
            }
            else
            {
                std::string errTxt;
                JsonStrFieldDecoded(line, "error", errTxt);
                body = "{\"success\":false,\"message\":" + Quote(errTxt) + "}";
            }
            g_lastSetVarBody = body;
            InterlockedExchange(&g_setVarReady, 1);
        }
        else if (ev == "eval")
        {
            // 第五十五轮：引擎回的求值结果 → DAP evaluate 响应体
            //   {"event":"eval","value":"...","type":"..."}
            //   {"event":"eval","error":"..."}
            std::string val, type, errTxt;
            bool bErr = JsonStrFieldDecoded(line, "error", errTxt);
            std::string body;
            if (bErr)
            {
                body = "{\"result\":" + Quote(errTxt) +
                       ",\"type\":\"error\",\"variablesReference\":0}";
            }
            else
            {
                JsonStrFieldDecoded(line, "value", val);
                JsonStrFieldDecoded(line, "type", type);
                body = "{\"result\":" + Quote(val) + ",\"type\":" +
                       Quote(type) + ",\"variablesReference\":0}";
            }
            g_lastEvalBody = body;
            InterlockedExchange(&g_evalReady, 1);
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

// JSON 字符串转义（**含引号**）。
//
// ⚠ 第五十五轮修掉一个**既存 bug**：原来这里只是加引号、不转义 →
//   process / stackTrace 事件里的 Windows 路径（"C:\Users\...\x.aardio"）
//   会把 JSON 弄坏（\U 不是合法转义）→ 客户端 json.loads 直接抛
//   "Invalid \escape"。改成真转义后这些事件才合法。
// ⚠ 非 ASCII 一律转 \uXXXX：管道/事件行都是字节流，纯 ASCII 最安全。
static std::string Quote(const std::string& s)
{
    std::string o = "\"";
    char buf[8];
    for (size_t i = 0; i < s.size(); i++)
    {
        unsigned char c = (unsigned char)s[i];
        switch (c)
        {
        case '"':  o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n";  break;
        case '\r': o += "\\r";  break;
        case '\t': o += "\\t";  break;
        case '\b': o += "\\b";  break;
        case '\f': o += "\\f";  break;
        default:
            if (c < 0x20 || c > 0x7E)
            {
                // 多字节 UTF-8 → 解码成码点再转 \uXXXX（不能按字节转，
                // 那是 Latin-1 语义，会把中文变成乱码）
                unsigned cp = 0;
                int extra = 0;
                if ((c & 0xE0) == 0xC0) { cp = c & 0x1Fu; extra = 1; }
                else if ((c & 0xF0) == 0xE0) { cp = c & 0x0Fu; extra = 2; }
                else if ((c & 0xF8) == 0xF0) { cp = c & 0x07u; extra = 3; }
                else { snprintf(buf, sizeof(buf), "\\u%04x", c); o += buf; break; }
                if (i + (size_t)extra >= s.size()) { cp = 0xFFFD; extra = 0; }
                for (int k = 1; k <= extra; k++)
                {
                    unsigned char cc = (unsigned char)s[i + (size_t)k];
                    if ((cc & 0xC0) != 0x80) { cp = 0xFFFD; extra = 0; break; }
                    cp = (cp << 6) | (cc & 0x3Fu);
                }
                i += (size_t)extra;
                if (cp >= 0x10000)
                {
                    cp -= 0x10000;
                    snprintf(buf, sizeof(buf), "\\u%04x", 0xD800 + (cp >> 10));
                    o += buf;
                    snprintf(buf, sizeof(buf), "\\u%04x", 0xDC00 + (cp & 0x3FF));
                    o += buf;
                }
                else
                {
                    snprintf(buf, sizeof(buf), "\\u%04x", cp);
                    o += buf;
                }
                break;
            }
            o += (char)c;
        }
    }
    o += "\"";
    return o;
}

// 从一行 JSON 里取字符串字段并**反转义**（\uXXXX → UTF-8）。
//
// ⚠ FindStringField 不反转义 → 引擎那边一律发 \uXXXX（纯 ASCII），
//   不解码的话 IDE 侧看到的就是一串 "\u4e2d\u6587" 而不是中文。
static bool JsonStrFieldDecoded(const std::string& line, const char* key,
                                 std::string& out)
{
    std::string pat = std::string("\"") + key + "\":";
    size_t p = line.find(pat);
    if (p == std::string::npos) return false;
    p += pat.size();
    while (p < line.size() && (line[p] == ' ' || line[p] == '\t')) p++;
    if (p >= line.size() || line[p] != '"') return false;
    p++;
    std::string o;
    while (p < line.size())
    {
        char c = line[p];
        if (c == '"') { out = o; return true; }
        if (c == '\\' && p + 1 < line.size())
        {
            char e = line[p + 1];
            p += 2;
            switch (e)
            {
            case '"':  o += '"';  break;
            case '\\': o += '\\'; break;
            case '/':  o += '/';  break;
            case 'n':  o += '\n'; break;
            case 'r':  o += '\r'; break;
            case 't':  o += '\t'; break;
            case 'b':  o += '\b'; break;
            case 'f':  o += '\f'; break;
            case 'u':
            {
                if (p + 4 > line.size()) return false;
                unsigned cp = 0;
                for (int k = 0; k < 4; k++)
                {
                    char h = line[p + k];
                    cp <<= 4;
                    if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                    else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                    else return false;
                }
                p += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF && p + 6 <= line.size() &&
                    line[p] == '\\' && line[p + 1] == 'u')
                {
                    unsigned lo = 0;
                    bool okLo = true;
                    for (int k = 0; k < 4; k++)
                    {
                        char h = line[p + 2 + k];
                        lo <<= 4;
                        if (h >= '0' && h <= '9') lo |= (unsigned)(h - '0');
                        else if (h >= 'a' && h <= 'f') lo |= (unsigned)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') lo |= (unsigned)(h - 'A' + 10);
                        else { okLo = false; break; }
                    }
                    if (okLo && lo >= 0xDC00 && lo <= 0xDFFF)
                    {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        p += 6;
                    }
                }
                if (cp < 0x80) o += (char)cp;
                else if (cp < 0x800)
                {
                    o += (char)(0xC0 | (cp >> 6));
                    o += (char)(0x80 | (cp & 0x3F));
                }
                else if (cp < 0x10000)
                {
                    o += (char)(0xE0 | (cp >> 12));
                    o += (char)(0x80 | ((cp >> 6) & 0x3F));
                    o += (char)(0x80 | (cp & 0x3F));
                }
                else
                {
                    o += (char)(0xF0 | (cp >> 18));
                    o += (char)(0x80 | ((cp >> 12) & 0x3F));
                    o += (char)(0x80 | ((cp >> 6) & 0x3F));
                    o += (char)(0x80 | (cp & 0x3F));
                }
                break;
            }
            default: o += e; break;
            }
            continue;
        }
        o += c;
        p++;
    }
    return false;
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
                "\"supportsConditionalBreakpoints\":true,"
                "\"supportsSetVariable\":true,"
                "\"supportsHitCounts\":true,"
                "\"supportsExceptionInfoRequest\":true,"
                "\"supportsLoadedSourcesRequest\":true,"
                "\"supportsGotoTargetsRequest\":true}");
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
            // body: {breakpoints:[{line:N, condition:"...", hitCondition:"N"}
            //       ...]} — 1-based source lines.
            // 第五十九轮：条件/命中次数断点 —— 每个断点的 condition /
            // hitCondition（DAP 规范字段，字符串形态的数字）原样转发给引擎
            // 的 bp2 命令，整表重建。**空数组也必须转发**（= 全部清除），
            // 旧实现 lines 为空就不发 → 消掉最后一个断点后引擎侧永远留着。
            std::string bpsJson = "[";
            std::string verified = "[";
            {
                std::string b = msg;
                size_t p = b.find("\"breakpoints\"");
                if (p != std::string::npos)
                {
                    size_t pA = b.find('[', p);
                    if (pA != std::string::npos)
                    {
                        // string-aware scan to the matching ']' — a
                        // condition string may contain ']'
                        size_t i = pA + 1;
                        bool inStr = false;
                        int depth = 1;
                        while (i < b.size() && depth > 0)
                        {
                            char c = b[i];
                            if (inStr)
                            {
                                if (c == '\\') i++;
                                else if (c == '"') inStr = false;
                            }
                            else
                            {
                                if (c == '"') inStr = true;
                                else if (c == '[') depth++;
                                else if (c == ']') depth--;
                            }
                            i++;
                        }
                        if (i > b.size()) i = b.size();
                        b = b.substr(pA + 1, i - pA - 1);
                        size_t pos = 0;
                        bool first = true;
                        while ((pos = b.find('{', pos)) != std::string::npos)
                        {
                            // split one object (string-aware braces)
                            size_t j = pos + 1;
                            bool oStr = false;
                            int oDepth = 1;
                            while (j < b.size() && oDepth > 0)
                            {
                                char c = b[j];
                                if (oStr)
                                {
                                    if (c == '\\') j++;
                                    else if (c == '"') oStr = false;
                                }
                                else
                                {
                                    if (c == '"') oStr = true;
                                    else if (c == '{') oDepth++;
                                    else if (c == '}') oDepth--;
                                }
                                j++;
                            }
                            if (j > b.size()) j = b.size();
                            std::string obj = b.substr(pos, j - pos);
                            pos = j;
                            std::string ln = JsonNumber(obj, "line");
                            if (ln.empty()) continue;
                            if (!first) { bpsJson += ","; verified += ","; }
                            first = false;
                            bpsJson += "{\"line\":" + ln;
                            verified += "{\"verified\":true,\"line\":" + ln + "}";
                            std::string cond;
                            if (JsonStrFieldDecoded(obj, "condition", cond) &&
                                !cond.empty())
                                bpsJson += ",\"condition\":" + Quote(cond);
                            std::string hit;
                            int nHit = 0;
                            if (JsonStrFieldDecoded(obj, "hitCondition", hit))
                                nHit = atoi(hit.c_str());
                            if (nHit <= 0)
                            {
                                // IDE 侧也可能直接给数字形态的 hitCount
                                size_t h0 = obj.find("\"hitCount\":");
                                if (h0 != std::string::npos)
                                    nHit = atoi(obj.c_str() + h0 + 11);
                            }
                            if (nHit > 0)
                                bpsJson += ",\"hitCount\":" +
                                           std::to_string(nHit);
                            bpsJson += "}";
                        }
                    }
                }
            }
            bpsJson += "]";
            verified += "]";
            SendToEngine("{\"cmd\":\"bp2\",\"bps\":" + bpsJson + "}");
            Reply(seq, command, "{\"breakpoints\":" + verified + "}");
        }
        else if (command == "threads")
        {
            Reply(seq, command,
                  "{\"threads\":[{\"id\":1,\"name\":\"main\"}]}");
        }
        else if (command == "stackTrace")
        {
            // 第五十九轮续：向引擎要真实调用栈。请求前清标志，等
            // ReaderThread 收 {"event":"stack",...}（引擎正停在
            // DebugPipeStop 里，应答是即时的；最多等 2 秒）。
            g_lastStackBody.clear();
            InterlockedExchange(&g_stackReady, 0);
            SendToEngine("{\"cmd\":\"stack\"}");
            for (int i = 0; i < 400; i++)
            {
                if (InterlockedCompareExchange(&g_stackReady, 0, 0) == 1)
                    break;
                Sleep(5);
            }
            std::string fileName = g_sourcePath;
            size_t slash = fileName.find_last_of("\\/");
            if (slash != std::string::npos)
                fileName = fileName.substr(slash + 1);
            // 组 DAP stackFrames。引擎 frames：[0]=当前帧（停在停点行），
            // 之后是调用方链，最后 main 帧。行号 1 基 → 0 基。
            // main 帧（name=="main"）的 source 用脚本路径；其余帧
            // 路径未知（用户模块），只给 name。
            std::string frames = "[";
            int id = 0;
            if (!g_lastStackBody.empty())
            {
                size_t pos = 0;
                while ((pos = g_lastStackBody.find('{', pos)) !=
                       std::string::npos)
                {
                    size_t j = pos + 1;
                    bool oStr = false;
                    int oDepth = 1;
                    while (j < g_lastStackBody.size() && oDepth > 0)
                    {
                        char c = g_lastStackBody[j];
                        if (oStr)
                        {
                            if (c == '\\') j++;
                            else if (c == '"') oStr = false;
                        }
                        else
                        {
                            if (c == '"') oStr = true;
                            else if (c == '{') oDepth++;
                            else if (c == '}') oDepth--;
                        }
                        j++;
                    }
                    if (j > g_lastStackBody.size()) j = g_lastStackBody.size();
                    std::string obj = g_lastStackBody.substr(pos, j - pos);
                    pos = j;
                    std::string rawName;
                    JsonStrFieldDecoded(obj, "name", rawName);
                    std::string ln2 = JsonNumber(obj, "line");
                    int nLine1 = ln2.empty() ? 0 : atoi(ln2.c_str());
                    int dapLine = nLine1 > 0 ? nLine1 - 1 : 0;
                    bool bMain = (rawName == "main");
                    std::string dispName = bMain ? fileName : rawName;
                    if (dispName.empty()) dispName = "(fn)";
                    if (id++) frames += ",";
                    frames += "{\"id\":" + std::to_string(id) +
                              ",\"name\":" + Quote(dispName) +
                              ",\"line\":" + std::to_string(dapLine) +
                              ",\"column\":1,\"source\":{\"name\":" +
                              Quote(dispName);
                    if (bMain)
                        frames += ",\"path\":" + Quote(g_sourcePath);
                    frames += "}}";
                }
            }
            if (id == 0)
            {
                // 引擎没应答（超时/管道断）——兜底单帧，行号用最近停点
                frames += "{\"id\":1,\"name\":" + Quote(fileName) +
                          ",\"line\":" + std::to_string(g_lastStopLine0) +
                          ",\"column\":1,\"source\":{\"name\":" +
                          Quote(fileName) + ",\"path\":" +
                          Quote(g_sourcePath) + "}}";
                id = 1;
            }
            frames += "]";
            Reply(seq, command,
                  "{\"stackFrames\":" + frames + ",\"totalFrames\":" +
                  std::to_string(id) + "}");
        }
        else if (command == "scopes")
        {
            Reply(seq, command,
                  "{\"scopes\":[{\"name\":\"Local\",\"variablesReference\":100,"
                  "\"expensive\":false}]}");
        }
        else if (command == "variables")
        {
            // 第五十九轮续：ref=100（Local 作用域）→ 最近停点的顶层
            // 变量快照；ref>100 → 转发 varsRef 展开第 ref 个容器
            //（引擎此刻停在 DebugPipeStop 命令循环里，等回包）。
            std::string rs = JsonNumber(msg, "variablesReference");
            int ref = rs.empty() ? 100 : atoi(rs.c_str());
            if (ref > 100)
            {
                g_lastVarsRefJson.clear();
                InterlockedExchange(&g_varsRefReady, 0);
                SendToEngine("{\"cmd\":\"varsRef\",\"ref\":" +
                             std::to_string(ref) + "}");
                for (int i = 0; i < 400; i++)
                {
                    if (InterlockedCompareExchange(&g_varsRefReady, 0, 0) == 1)
                        break;
                    Sleep(5);
                }
                Reply(seq, command,
                      "{\"variables\":" + VarsBody(g_lastVarsRefJson) + "}");
            }
            else
            {
                Reply(seq, command,
                      "{\"variables\":" + VarsBody(g_lastVarsJson) + "}");
            }
        }
        else if (command == "setVariable")
        {
            // 第五十九轮续：DAP setVariable → 引擎 setVar（值是表达式，
            // 引擎在当前帧求值后写入；应答带写入后的新值与类型）。
            std::string nm, vexpr;
            JsonStrFieldDecoded(msg, "name", nm);
            JsonStrFieldDecoded(msg, "value", vexpr);
            std::string rs = JsonNumber(msg, "variablesReference");
            int ref = rs.empty() ? 100 : atoi(rs.c_str());
            g_lastSetVarBody.clear();
            InterlockedExchange(&g_setVarReady, 0);
            SendToEngine("{\"cmd\":\"setVar\",\"ref\":" +
                         std::to_string(ref) + ",\"name\":" + Quote(nm) +
                         ",\"value\":" + Quote(vexpr) + "}");
            for (int i = 0; i < 600; i++)
            {
                if (InterlockedCompareExchange(&g_setVarReady, 0, 0) == 1)
                    break;
                Sleep(5);
            }
            std::string body = g_lastSetVarBody;
            if (body.empty())
                body = "{\"success\":false,\"message\":\"引擎没有响应\"}";
            Reply(seq, command, body, false);
        }
        else if (command == "exceptionInfo")
        {
            // 第六十一轮：DAP exceptionInfo（异常详情）
            std::string desc = "Nodio 运行时异常";
            std::string body = "{\"exceptionId\":\"runtime_error\","
                               "\"description\":" + Quote(desc) + ","
                               "\"breakMode\":\"userUnhandled\","
                               "\"details\":{\"message\":" + Quote(desc) + ","
                               "\"typeName\":\"RuntimeError\","
                               "\"stackTrace\":" + Quote(g_lastStackBody) + "}}";
            Reply(seq, command, body);
        }
        else if (command == "loadedSources")
        {
            // 第六十一轮：DAP loadedSources（已载入源码清单）
            std::string body = "{\"sources\":[";
            if (!g_sourcePath.empty())
            {
                std::string fname = g_sourcePath;
                size_t p = fname.find_last_of("\\/");
                if (p != std::string::npos) fname = fname.substr(p + 1);
                body += "{\"name\":" + Quote(fname) + ",\"path\":" + Quote(g_sourcePath) + "}";
            }
            body += "]}";
            Reply(seq, command, body);
        }
        else if (command == "gotoTargets")
        {
            // 第六十一轮：DAP gotoTargets
            std::string lineStr = JsonNumber(msg, "line");
            int line = lineStr.empty() ? 1 : atoi(lineStr.c_str());
            std::string body = "{\"targets\":[{\"id\":1,\"label\":\"line " +
                               std::to_string(line) + "\",\"line\":" +
                               std::to_string(line) + "}]}";
            Reply(seq, command, body);
        }
        else if (command == "goto")
        {
            Reply(seq, command, "");
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
        else if (command == "evaluate")
        {
            // 第五十五轮：表达式求值。
            //   请求体：{"expression":"<aardio 表达式>","context":"repl"}
            // 流程：清标志 → 把 eval 命令写进命令管道 → 等引擎回
            //   {"event":"eval",...}（ReaderThread 收，转成响应体）→ 回 DAP。
            std::string expr;
            if (!JsonStrFieldDecoded(msg, "expression", expr))
                JsonStrFieldDecoded(msg, "expr", expr);
            if (expr.empty())
            {
                Reply(seq, command,
                      "{\"result\":\"\",\"type\":\"\",\"variablesReference\":0}",
                      false);
                continue;
            }
            g_lastEvalBody.clear();
            InterlockedExchange(&g_evalReady, 0);
            SendToEngine("{\"cmd\":\"eval\",\"expr\":" + Quote(expr) + "}");
            // 等引擎回包（最多 3 秒；引擎此刻正停在 DebugPipeStop 里）
            for (int i = 0; i < 600; i++)
            {
                if (InterlockedCompareExchange(&g_evalReady, 0, 0) == 1) break;
                Sleep(5);
            }
            std::string body = g_lastEvalBody;
            if (body.empty())
            {
                Reply(seq, command,
                      "{\"result\":\"(引擎没有响应求值)\",\"type\":\"error\","
                      "\"variablesReference\":0}", false);
                continue;
            }
            Reply(seq, command, body);
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
