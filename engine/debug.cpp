#include "engine/interp.h"

namespace NodioVM
{
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
            static const wchar_t* kScalarNatives[] = {
                L"tostring", L"tonumber", L"sleep", L"call", L"topointer",
                L"splice", L"tocdecl", L"pop", L"tostdcall", L"isArray",
                L"isArrayLike", L"push", L"_HINSTANCE", L"_HINSTANSE",
                L"__waitForMultipleObjects", L"__messagePeek",
                L"_INVALID_HANDLE_VALUE", L"_HAPPICON", L"_WIN_64",
                L"_WINXP", L"_WIN7_LATER", L"_WIN10_LATER"
            };
            for (auto& kv : env->vars)
            {
                // built-in namespaces/functions bury the script's own vars; User32, Kernel32 and type kept for debugger
                if ((roots.count(kv.first) && kv.first != L"User32" && kv.first != L"Kernel32" && kv.first != L"type") ||
                    modules.count(kv.first))
                    continue;
                bool bNativ = false;
                for (auto* p : kScalarNatives)
                    if (kv.first == p) { bNativ = true; break; }
                // engine-provided native builtins are not script variables.
                // Filtering on the value instead of a hard-coded name list
                // keeps this dump stable as more builtins are added.
                if (bNativ || (kv.second.native && kv.first != L"type"))
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
}
