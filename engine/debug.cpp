// debug.cpp — console mini-debugger + external debug session.
//
// DebugStop keeps the classic console loop (F5 + NODIO_DEBUG). When a
// DAP adapter spawned the debuggee (NODIO_DEBUG_PIPE=<name>), DebugPipeStop
// serves each stop over that named pipe with a line-based JSON protocol:
//   adapter -> engine : {"cmd":"continue"|"next"|"step"|"vars"|"disconnect"}
//   engine -> adapter : {"event":"stopped","line":N,"vars":{...}}
//                     | {"event":"vars","vars":{...}}
//                     | {"event":"terminated"}
#include "engine/interp.h"

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
            PNode n = std::make_shared<Node>();
            if (v.t == Value::Num) { n->k = Node::Num; n->num = v.n; }
            else if (v.t == Value::Str) { n->k = Node::Str; n->str = v.s; }
            else if (v.t == Value::Bool) { n->k = Node::Bool; n->num = v.b; }
            else n->k = Node::NullK;
            return n;
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
void Interp::DebugStop(int nLine, Env* env)
        {
            if (hDebugPipe)
            {
                // external session owns the stop protocol; on session end
                // fall through to plain continue (never abort the script)
                DebugPipeStop(nLine, env);
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
bool Interp::DebugPipeStop(int nLine, Env* env)
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
                wstring j = L"{\"event\":\"stopped\",\"line\":" +
                            std::to_wstring(nLine) + L",\"vars\":" +
                            varsJson(env) + L"}";
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
                if (wcmd.find(L"\"disconnect\"") != wstring::npos ||
                    wcmd.find(L"\"terminate\"") != wstring::npos)
                {
                    return endSession();
                }
                // unknown command: ignore and keep waiting
            }
        }
}
