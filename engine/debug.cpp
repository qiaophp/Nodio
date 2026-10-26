// debug.cpp — console mini-debugger + external debug session.
//
// DebugStop keeps the classic console loop (F5 + NODIO_DEBUG). When a
// DAP adapter spawned the debuggee (NODIO_DEBUG_PIPE=<name>), DebugPipeStop
// serves each stop over that named pipe with a line-based JSON protocol:
//   adapter -> engine : {"cmd":"continue"|"next"|"step"|"vars"|"disconnect"}
//   engine -> adapter : {"event":"stopped","line":N,"vars":{...}}
//                     | {"event":"vars","vars":{...}}
//                     | {"event":"terminated"}
//
// 第五十五轮：加表达式求值（evaluate）——
//   adapter -> engine : {"cmd":"eval","expr":"<aardio 表达式>"}
//   engine -> adapter : {"event":"eval","value":"...","type":"..."}
//                     | {"event":"eval","error":"..."}
// 求值在**当前帧的环境**里做（所以能看到局部变量），实现照抄
// builtins.cpp 的 CompileToFunc 范式：Lex → Parser → ParseExpr →
// AnnotateNameIds → Interp::Eval。
#include "engine/interp.h"
#include "engine/parser.h"

#include <algorithm>

namespace NodioVM
{
    // ---------------- helpers ----------------

    static bool IsDebugBuiltinName(const wstring& name)
    {
        static const wchar_t* kNatives[] = {
            L"tostring", L"tonumber", L"sleep", L"call", L"topointer",
            L"splice", L"tocdecl", L"pop", L"tostdcall", L"isArray",
            L"isArrayLike", L"push", L"_HINSTANCE", L"_HINSTANSE",
            L"__waitForMultipleObjects", L"__messagePeek",
            L"_INVALID_HANDLE_VALUE", L"_HAPPICON", L"_WIN_64",
            L"_WINXP", L"_WIN7_LATER", L"_WIN10_LATER", nullptr
        };
        for (auto* p : kNatives)
            if (p && name == p) return true;
        return false;
    }

    static bool IsUserVarPair(Interp* it, const wstring& name,
                              const Value& v)
    {
        if ((it->roots.count(name) && name != L"User32" &&
             name != L"Kernel32" && name != L"type") ||
            it->modules.count(name))
            return false;
        if (IsDebugBuiltinName(name)) return false;
        if (v.native && name != L"type") return false;
        // implicit frames: `owner` (member-call this slot) and the unnamed
        // varargs/env slots are noise in the variables pane
        if (name == L"owner" || name.empty()) return false;
        return true;
    }

// ---- ExprToNode ----
PNode Interp::ExprToNode(const Value& v)
        {
            PNode n = NewPNode();
            if (v.t == Value::Num) { n->k = Node::Num; n->num = v.n; }
            else if (v.t == Value::Str) { n->k = Node::Str; n->str = v.s; }
            else if (v.t == Value::Bool) { n->k = Node::Bool; n->num = v.b; }
            else n->k = Node::NullK;
            return n;
        }

// ---- 第五十五轮：表达式求值（DAP evaluate）----

namespace
{
    // 从一行 JSON 里取字符串字段并**反转义**。
    //
    // ⚠ 不能用子串匹配（现有 continue/next 就是子串匹配，那没坑是因为
    //   它们是固定字面量）；表达式里可能有引号/反斜杠/换行，适配器发过来
    //   是 JSON 编码过的（\" \\ \n \r \t \uXXXX），必须还原。
    //
    // ⚠ 管道是**字节**流，一行必须是纯 ASCII 才安全（varsJson 那边已经
    //   这么做了：非 ASCII 走 \uXXXX）→ 这里同样只认 \uXXXX 形式的转义。
    bool JsonStrField(const std::string& line, const char* key,
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
                    unsigned code = 0;
                    for (int k = 0; k < 4; k++)
                    {
                        char h = line[p + k];
                        code <<= 4;
                        if (h >= '0' && h <= '9') code |= (unsigned)(h - '0');
                        else if (h >= 'a' && h <= 'f') code |= (unsigned)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') code |= (unsigned)(h - 'A' + 10);
                        else return false;
                    }
                    p += 4;
                    // UTF-8 编码（代理对不合成——调试表达式里用不到）
                    if (code < 0x80) o += (char)code;
                    else if (code < 0x800)
                    {
                        o += (char)(0xC0 | (code >> 6));
                        o += (char)(0x80 | (code & 0x3F));
                    }
                    else
                    {
                        o += (char)(0xE0 | (code >> 12));
                        o += (char)(0x80 | ((code >> 6) & 0x3F));
                        o += (char)(0x80 | (code & 0x3F));
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

    // Value 的类型名（DAP evaluate 的 type 字段）
    const wchar_t* TypeNameOf(const Value& v)
    {
        switch (v.t)
        {
        case Value::Num:   return L"number";
        case Value::Str:   return L"string";
        case Value::Bool:  return L"boolean";
        case Value::Null:  return L"null";
        case Value::TableT: return L"table";
        case Value::Func:  return L"function";
        case Value::Class: return L"class";
        default:           return L"unknown";
        }
    }

    // wstring → **纯 ASCII** 的 JSON 字符串内容（不含首尾引号）。
    //
    // ⚠ 管道是字节流：事件行必须是纯 ASCII 才安全（varsJson 那边同样
    //   这么做）→ 非 ASCII 一律转成 \uXXXX。
    wstring JsonEscapeAscii(const wstring& w)
    {
        wstring o;
        wchar_t buf[8];
        for (wchar_t c : w)
        {
            switch (c)
            {
            case L'"':  o += L"\\\""; break;
            case L'\\': o += L"\\\\"; break;
            case L'\n': o += L"\\n";  break;
            case L'\r': o += L"\\r";  break;
            case L'\t': o += L"\\t";  break;
            default:
                if (c < 0x20 || c > 0x7E)
                {
                    if (c >= 0xD800 && c <= 0xDFFF) o += c;  // 代理项原样
                    else { swprintf(buf, 8, L"\\u%04x", (unsigned)c); o += buf; }
                }
                else o += c;
            }
        }
        return o;
    }

    // 从一行 JSON 里取数字字段（"line":3 → 3）。找不到返回 false。
    bool JsonNumField(const std::string& line, const char* key, int& out)
    {
        std::string pat = std::string("\"") + key + "\":";
        size_t p = line.find(pat);
        if (p == std::string::npos) return false;
        p += pat.size();
        while (p < line.size() && (line[p] == ' ' || line[p] == '\t')) p++;
        if (p >= line.size() || line[p] == '"') return false;
        out = atoi(line.c_str() + p);
        return true;
    }

    // 第五十九轮：解析 bp2 的 bps 数组，整表重建 breakpoints + bpMeta。
    // 逐对象扫描要**感知字符串**：条件表达式里可以有 '{'/'}'（JSON 里
    // 原样输出），裸找配对括号会被字符串内容带偏。
    void ParseBpList(Interp* it, const std::string& line)
    {
        it->breakpoints.clear();
        it->bpMeta.clear();
        size_t pA = line.find("\"bps\":");
        if (pA == std::string::npos) return;
        pA = line.find('[', pA);
        if (pA == std::string::npos) return;
        size_t p = pA + 1;
        for (;;)
        {
            size_t obj0 = line.find('{', p);
            if (obj0 == std::string::npos) break;
            size_t i = obj0 + 1;
            bool inStr = false;
            int depth = 1;
            while (i < line.size() && depth > 0)
            {
                char c = line[i];
                if (inStr)
                {
                    if (c == '\\') i++;          // skip escaped char
                    else if (c == '"') inStr = false;
                }
                else
                {
                    if (c == '"') inStr = true;
                    else if (c == '{') depth++;
                    else if (c == '}') depth--;
                }
                i++;
            }
            if (i > line.size()) break;
            std::string obj = line.substr(obj0, i - obj0);
            int nL = 0;
            if (JsonNumField(obj, "line", nL) && nL > 0)
            {
                Interp::BpMeta m;   // 嵌套类型：自由函数里必须带类名限定
                std::string cond;
                if (JsonStrField(obj, "condition", cond) && !cond.empty())
                {
                    int cw = MultiByteToWideChar(CP_UTF8, 0, cond.data(),
                                                 (int)cond.size(), nullptr, 0);
                    if (cw > 0)
                    {
                        m.condition.resize((size_t)cw);
                        MultiByteToWideChar(CP_UTF8, 0, cond.data(),
                                            (int)cond.size(),
                                            &m.condition[0], cw);
                    }
                }
                int nHit = 0;
                if (!JsonNumField(obj, "hitCount", nHit))
                {
                    // DAP 规范字段是 hitCondition（字符串形态的数字）
                    std::string hs;
                    if (JsonStrField(obj, "hitCondition", hs))
                        nHit = atoi(hs.c_str());
                }
                m.hitCount = nHit > 0 ? nHit : 0;
                it->breakpoints.insert(nL);
                if (!m.condition.empty() || m.hitCount > 0)
                    it->bpMeta[nL] = m;
            }
            p = i;
        }
    }
}

// 表达式文本（UTF-8 字节）→ AST。Lex → Parser → ParseExpr →
// AnnotateNameIds 是 EvalExprInEnv（DAP evaluate）与 EvalCondInEnv
//（条件断点，第五十九轮）的公共前缀，抽出来只留一份。
static bool ParseExprUtf8(const std::string& expr, const wstring& baseDir,
                          PNode& nodeOut, wstring& errOut)
{
    nodeOut = nullptr;
    errOut.clear();
    if (expr.empty()) { errOut = L"空表达式"; return false; }

    wstring wexpr;
    int cw = MultiByteToWideChar(CP_UTF8, 0, expr.data(),
                                 (int)expr.size(), nullptr, 0);
    if (cw <= 0) { errOut = L"表达式编码无效"; return false; }
    wexpr.resize((size_t)cw);
    MultiByteToWideChar(CP_UTF8, 0, expr.data(), (int)expr.size(),
                        &wexpr[0], cw);

    wstring lerr;
    vector<Tok> toks = Lex(wexpr, &lerr);
    if (!lerr.empty()) { errOut = lerr; return false; }

    Parser ps{ std::move(toks), 0, &lerr };
    ps.strEmbedBase = baseDir;
    ps.strEmbedRoot = EmbedRoot();
    nodeOut = ps.ParseExpr();
    if (!lerr.empty()) { errOut = lerr; return false; }
    if (!nodeOut) { errOut = L"表达式解析为空"; return false; }
    AnnotateNameIds(nodeOut);
    return true;
}

// 在 env 里求值一段表达式，返回显示用的值与类型名。
//
// ⚠ 出错时（比如调用了不存在的成员）**必须把引擎的 err 清干净**：
//   err 是解释器的全局错误槽，留着会让后续执行带着一条假错误。
bool Interp::EvalExprInEnv(const std::string& expr, Env* env,
                           wstring& valueOut, wstring& typeOut,
                           wstring& errOut)
    {
    valueOut.clear();
    typeOut.clear();
    errOut.clear();

    PNode node;
    if (!ParseExprUtf8(expr, baseDir, node, errOut)) return false;

    // ⚠ 保存/清空引擎错误槽：求值失败不能污染后续执行
    wstring savedErr = err ? *err : wstring();
    if (err) err->clear();

    Value v = Eval(node, env);

    if (err && !err->empty())
    {
        errOut = *err;
        err->clear();
        if (err) *err = savedErr;
        return false;
    }
    if (err) *err = savedErr;

    wstring vs = v.ToString();
    if (vs.size() > 200) vs = vs.substr(0, 200) + L"...";
    valueOut = vs;
    typeOut = TypeNameOf(v);
    return true;
    }

// ---- 第五十九轮：条件断点求值 ----
// 在 env 里求值并按 if 的真值语义返回（Value::Truthy：Falsy 仅 null/false）。
// 真值判定必须跟 if 一致——用户写 `x >= 3` 期望的就是 if (x>=3) 的行为。
bool Interp::EvalCondInEnv(const std::string& expr, Env* env, bool& bOut,
                           wstring& errOut)
    {
    bOut = false;
    errOut.clear();
    PNode node;
    if (!ParseExprUtf8(expr, baseDir, node, errOut)) return false;

    wstring savedErr = err ? *err : wstring();
    if (err) err->clear();
    Value v = Eval(node, env);
    if (err && !err->empty())
    {
        errOut = *err;
        err->clear();
        if (err) *err = savedErr;
        return false;
    }
    if (err) *err = savedErr;
    bOut = v.Truthy();
    return true;
    }

// ---- 第五十九轮：条件/命中次数停点判定 ----
// 行号已命中 breakpoints（快速路径）后调用：
//   hitCount>0：每次到达都计数，curHits % N == 0 才可能停（每第 N 次）；
//   condition：在当前帧求值，假值或求值失败 → 不停（求值失败静默跳过，
//   否则一个写错的条件会在循环里把断点变成每步都停）。
// 两者同时设置时都满足才停；纯行号断点（无 bpMeta 条目）直接停。
bool Interp::BpShouldStop(int nLine, Env* env)
    {
    auto it = bpMeta.find(nLine);
    if (it == bpMeta.end()) return true;
    BpMeta& m = it->second;
    if (m.hitCount > 0)
    {
        m.curHits++;
        if (m.curHits % m.hitCount != 0) return false;
    }
    if (!m.condition.empty())
    {
        std::string u8;
        int cb = WideCharToMultiByte(CP_UTF8, 0, m.condition.c_str(),
                                     (int)m.condition.size(), nullptr, 0,
                                     nullptr, nullptr);
        u8.resize((size_t)(std::max)(cb, 0));
        if (cb > 0)
            WideCharToMultiByte(CP_UTF8, 0, m.condition.c_str(),
                                (int)m.condition.size(), &u8[0], cb,
                                nullptr, nullptr);
        bool bOk = false;
        wstring condErr;
        if (!EvalCondInEnv(u8, env, bOk, condErr)) return false;
        return bOk;
    }
    return true;
    }

// ---- DumpVars ----
void Interp::DumpVars(Env* env)
        {
            int n = 0;
            std::vector<std::pair<wstring, Value>> list;
            if (env && env->parent && env->parent.get() == &globals)
            {
                for (const wchar_t* g : { L"User32", L"Kernel32", L"type" })
                {
                    Value* pv = globals.FindInSelf(NameId(g));
                    if (pv)
                        list.push_back({ g, *pv });
                }
            }
            if (env)
            {
                // Round 13: slot-aware (env may be the slotted mainEnv)
                env->ForEachVar([&](uint32_t gid, Value& v)
                    { list.push_back({ NameOf(gid), v }); });
            }
            for (auto& kv : list)
            {
                // built-in namespaces/functions bury the script's own vars; User32, Kernel32 and type kept for debugger
                if (!IsUserVarPair(this, kv.first, kv.second))
                    continue;
                if (n++ >= 20) { WriteU8(stdout, L"  ...\n"); break; }
                wstring val = kv.second.ToString();
                if (val.size() > 60) val = val.substr(0, 60) + L"...";
                WriteU8(stdout, L"  " + kv.first + L" = " + val + L"\n");
            }
            if (n == 0)
                WriteU8(stdout, L"  (本层无变量)\n");
            fflush(stdout);
        }

        // console mini-debugger: prompts on the script's console; commands —
        // enter=continue, s=step, c=continue w/o stepping, v=vars, q=abort

// ---- DebugStop ----
void Interp::DebugStop(int nLine, Env* env, const char* stopReason)
        {
            if (hDebugPipe)
            {
                // external session owns the stop protocol; on session end
                // fall through to plain continue (never abort the script)
                DebugPipeStop(nLine, env, stopReason);
                return;
            }
            while (true)
            {
                WriteU8(stdout, L"\n[断点] 行 " +
                                std::to_wstring(nLine) + L"\n");
                DumpVars(env);
                WriteU8(stdout, L"(回车=继续  s=单步  "
                                L"c=连续执行  q=退出) > ");
                fflush(stdout);
                wchar_t buf[64] = {0};
                if (!fgetws(buf, 64, stdin))
                {
                    (*err) = L"调试中断";
                    return;
                }
                wchar_t c = buf[0];
                if (c == L'\n' || c == 0) { return; }
                if (c == L's') { bStep = true; return; }
                if (c == L'c') { bStep = false; return; }
                if (c == L'v') continue;
                if (c == L'q') { (*err) = L"调试终止"; return; }
            }
        }

// ---- DebugPipeStop (external DAP session) ----
bool Interp::DebugPipeStop(int nLine, Env* env, const char* stopReason)
        {
            HANDLE hEv = (HANDLE)hDebugPipe;   // events (write)
            HANDLE hCmd = (HANDLE)hDebugCmd;   // commands (read)
            if (!hEv || hEv == INVALID_HANDLE_VALUE) return false;

            auto endSession = [&]() -> bool
            {
                if (hEv) CloseHandle(hEv);
                if (hCmd) CloseHandle(hCmd);
                hDebugPipe = nullptr;
                hDebugCmd = nullptr;
                bStep = false;
                return false;
            };

            auto writeLine = [&](const wstring& s) -> bool
            {
                std::string u8;
                int cb = WideCharToMultiByte(CP_UTF8, 0, s.c_str(),
                                             (int)s.size(), nullptr, 0,
                                             nullptr, nullptr);
                u8.resize((size_t)(std::max)(cb, 0));
                if (cb > 0)
                    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(),
                                        &u8[0], cb, nullptr, nullptr);
                u8 += "\n";
                DWORD wrote = 0;
                return WriteFile(hEv, u8.data(), (DWORD)u8.size(), &wrote,
                                 nullptr) && wrote == u8.size();
            };

            auto varsJson = [&](Env* e) -> wstring
            {
                // one JSON object of the current frame's user variables;
                // values render via ToString with JSON string escaping.
                // Non-ASCII becomes \uXXXX: the pipe line is consumed as a
                // raw byte frame, and keeping it pure ASCII makes the
                // frame byte-safe for every reader.
                auto esc = [](const wstring& s) -> wstring
                {
                    wchar_t buf[8];
                    wstring o;
                    for (wchar_t c : s)
                    {
                        switch (c)
                        {
                        case L'"': o += L"\\\""; break;
                        case L'\\': o += L"\\\\"; break;
                        case L'\n': o += L"\\n"; break;
                        case L'\r': o += L"\\r"; break;
                        case L'\t': o += L"\\t"; break;
                        default:
                            if (c < 0x20 || c > 0x7E)
                            {
                                if (c >= 0xD800 && c <= 0xDBFF)
                                {
                                    // high surrogate: emit pair with the
                                    // next low surrogate (string may have
                                    // been built as UTF-16 pairs)
                                    o += c;   // pass through; low half follows
                                }
                                else if (c >= 0xDC00 && c <= 0xDFFF)
                                {
                                    o += c;
                                }
                                else
                                {
                                    swprintf(buf, 8, L"\\u%04x", (unsigned)c);
                                    o += buf;
                                }
                            }
                            else o += c;
                        }
                    }
                    return o;
                };
                wstring o = L"{";
                bool first = true;
                std::vector<std::pair<wstring, Value>> list;
                if (e)
                {
                    // Round 13: slot-aware (e may be the slotted mainEnv)
                    e->ForEachVar([&](uint32_t gid, Value& v)
                        { list.push_back({ NameOf(gid), v }); });
                }
                for (auto& kv : list)
                {
                    if (!IsUserVarPair(this, kv.first, kv.second))
                        continue;
                    if (!first) o += L",";
                    first = false;
                    wstring val = kv.second.ToString();
                    if (val.size() > 120)
                        val = val.substr(0, 120) + L"...";
                    o += L"\"" + esc(kv.first) + L"\":\"" + esc(val) + L"\"";
                }
                return o + L"}";
            };

            auto stopEvent = [&]() -> bool
            {
                // stopReason 只会是 "breakpoint"/"step" 两个 ASCII 字面量，
                // 判定成 wstring 字面量再拼（wchar_t[] + const char* 不合法）
                wstring wReason = (stopReason && strcmp(stopReason, "step") == 0)
                                  ? L"step" : L"breakpoint";
                wstring j = L"{\"event\":\"stopped\",\"line\":" +
                            std::to_wstring(nLine) + L",\"reason\":\"" +
                            wReason + L"\",\"vars\":" + varsJson(env) + L"}";
                return writeLine(j);
            };

            if (!stopEvent()) return endSession();

            // command loop until the adapter resumes us
            for (;;)
            {
                // read one line (the adapter's command)
                std::string cmd;
                char ch;
                DWORD got = 0;
                while (cmd.size() < 4096)
                {
                    if (!ReadFile(hCmd, &ch, 1, &got, nullptr) || got == 0)
                    {
                        // pipe broke — treat as disconnect: continue to end
                        return endSession();
                    }
                    if (ch == '\n') break;
                    if (ch != '\r') cmd += ch;
                }
                wstring wcmd;
                {
                    int cw = MultiByteToWideChar(CP_UTF8, 0, cmd.data(),
                                                 (int)cmd.size(), nullptr, 0);
                    wcmd.resize((size_t)(std::max)(cw, 0));
                    if (cw > 0)
                        MultiByteToWideChar(CP_UTF8, 0, cmd.data(),
                                            (int)cmd.size(), &wcmd[0], cw);
                }
                if (wcmd.find(L"\"continue\"") != wstring::npos)
                {
                    bStep = false;
                    return true;
                }
                if (wcmd.find(L"\"next\"") != wstring::npos ||
                    wcmd.find(L"\"step\"") != wstring::npos)
                {
                    // stop again on the very next statement
                    bStep = true;
                    return true;
                }
                if (wcmd.find(L"\"vars\"") != wstring::npos)
                {
                    writeLine(L"{\"event\":\"vars\",\"line\":" +
                              std::to_wstring(nLine) + L",\"vars\":" +
                              varsJson(env) + L"}");
                    continue;
                }
                if (wcmd.find(L"\"cmd\":\"eval\"") != wstring::npos)
                {
                    // 第五十五轮：表达式求值。
                    //   {"cmd":"eval","expr":"<表达式>"}
                    // 回 {"event":"eval","value":..,"type":..} 或
                    //    {"event":"eval","error":..}
                    std::string cmdU8(cmd.begin(), cmd.end());
                    std::string expr;
                    wstring val, type, evalErr;
                    if (!JsonStrField(cmdU8, "expr", expr))
                    {
                        evalErr = L"eval 命令缺少 expr 字段";
                    }
                    else
                    {
                        EvalExprInEnv(expr, env, val, type, evalErr);
                    }
                    // ⚠ 事件行必须纯 ASCII（管道按字节读）→ 值/错误都转义
                    wstring j = L"{\"event\":\"eval\",";
                    if (!evalErr.empty())
                    {
                        j += L"\"error\":\"" + JsonEscapeAscii(evalErr) + L"\"";
                    }
                    else
                    {
                        j += L"\"value\":\"" + JsonEscapeAscii(val) +
                             L"\",\"type\":\"" + type + L"\"";
                    }
                    j += L"}";
                    writeLine(j);
                    continue;
                }
                if (wcmd.find(L"\"cmd\":\"bp\"") != wstring::npos)
                {
                    // {"cmd":"bp","lines":"3,7,12"} — re-arm breakpoints
                    // live (1-based source lines)
                    auto pL = wcmd.find(L"\"lines\":\"");
                    if (pL != wstring::npos)
                    {
                        pL += 9;
                        auto pE = wcmd.find(L'"', pL);
                        if (pE != wstring::npos)
                        {
                            wstring ls = wcmd.substr(pL, pE - pL);
                            breakpoints.clear();
                            size_t pos = 0;
                            while (pos < ls.size())
                            {
                                size_t comma = ls.find(L',', pos);
                                if (comma == wstring::npos)
                                    comma = ls.size();
                                int nL = _wtoi(ls.substr(pos, comma - pos).c_str());
                                if (nL > 0) breakpoints.insert(nL);
                                pos = comma + 1;
                            }
                        }
                    }
                    continue;
                }
                if (wcmd.find(L"\"cmd\":\"bp2\"") != wstring::npos)
                {
                    // 第五十九轮：{"cmd":"bp2","bps":[{"line":3,
                    //   "condition":"x>1","hitCount":2},...]}
                    // 整表重建 breakpoints + bpMeta（条件是转义过的 JSON
                    // 字符串，走 JsonStrField 反转义；空数组 = 全部清除）。
                    std::string cmdU8(cmd.begin(), cmd.end());
                    ParseBpList(this, cmdU8);
                    continue;
                }
                if (wcmd.find(L"\"disconnect\"") != wstring::npos ||
                    wcmd.find(L"\"terminate\"") != wstring::npos)
                {
                    return endSession();
                }
                // unknown command: ignore and keep waiting
            }
        }
}
