#include "engine/interp.h"
#include "engine/parser.h"
#include <clocale>
#include <emmintrin.h>
#include <fcntl.h>
#include <io.h>

namespace NodioVM
{
    // UTF-16 (engine internal) -> UTF-8 (byte semantics)
    static std::string WToU8(const std::wstring& s)
    {
        int len = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(),
                                      nullptr, 0, nullptr, nullptr);
        std::string u8(len > 0 ? (size_t)len : 0, '\0');
        if (len > 0)
            WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(),
                                &u8[0], len, nullptr, nullptr);
        return u8;
    }

    // ---- published payload: the embedded file system ----
    // A published exe carries its resources inside the appended payload; the
    // manifest keys are lib-relative paths with '\' separators. "~\res\x"
    // (EXE root), "/res/x" (app root) and bare relative keys all resolve to
    // the same key.
    //
    // The lookup lives HERE and not on the io.readFile builtin because the
    // official builtin library replaces global.io with its own namespace,
    // where io.readFile is the deprecated file.read alias — anything hung
    // off the io.readFile builtin is unreachable (the official alias wins).
    // io.file is the funnel every file read goes through, so the embedded
    // lookup has to live there.
    static wstring EmbeddedPayloadKey(const wstring& path)
    {
        if (!g_interpCurrent || path.empty()) return L"";
        auto& emb = g_interpCurrent->embedded;
        if (emb.empty()) return L"";
        wstring key = path;
        if (key[0] == L'~') key = key.substr(1);
        if (!key.empty() && (key[0] == L'\\' || key[0] == L'/'))
            key = key.substr(1);
        for (auto& c : key)
            if (c == L'/') c = L'\\';
        if (emb.find(key) != emb.end()) return key;
        // the original spelling may itself be the manifest key
        if (key != path && emb.find(path) != emb.end()) return path;
        return L"";
    }

    // A file handle: a disk FILE* or a published-payload resource buffer.
    // The payload buffer holds the raw bytes (one byte per wchar in the
    // engine's byte-string convention becomes one std::string byte here).
    struct IoFileState
    {
        FILE* f = nullptr;
        std::string mem;
        size_t memPos = 0;
        bool bMem = false;
        bool bClosed = false;
        // Official io.file default is TEXT mode ('t'; 'b' opts out):
        // reads translate CRLF -> '\n' and stop at '\0'/'\x1A', writes
        // translate '\n' -> CRLF. The CRT fd itself stays binary (live
        // pipes deadlock on CRT text mode), so the translation is done in
        // the byte layer here (ioFileTextKeep / ioFileWriteTranslate).
        bool bText = false;
        ~IoFileState() { if (f) fclose(f); }
    };
    // a file state is "open" when it has a disk handle or a payload buffer
    static bool ioFileOpen(const std::shared_ptr<IoFileState>& st)
    {
        return st && !st->bClosed && (st->f != nullptr || st->bMem);
    }
    static long long ioFilePos(const std::shared_ptr<IoFileState>& st)
    {
        if (!st) return -1;
        if (st->bMem) return (long long)st->memPos;
        if (!st->f) return -1;
        return _ftelli64(st->f);
    }
    static bool ioFileSetPos(const std::shared_ptr<IoFileState>& st, long long p)
    {
        if (!st) return false;
        if (st->bMem)
        {
            if (p < 0) return false;
            st->memPos = (size_t)p;
            return true;
        }
        if (!st->f) return false;
        return _fseeki64(st->f, p, SEEK_SET) == 0;
    }

    // ---- io.lines 需要的文件状态访问（第二十三轮）----
    //
    // io.lines 官方契约（docs/library-guide/builtin/io/lines.md）：
    //   io.lines(pathOrFile = io.stdin) —— 自动打开 @pathOrFile 指定的文件，
    //   创建一个**迭代器**，在泛型 for 循环中逐行读取，读取完毕后**自动关闭**。
    //   @pathOrFile 也可以是 io.file 打开的文件对象；省略则用 io.stdin。
    //
    // 文件对象的内部状态是 ioFileMake 的闭包捕获（shared_ptr），脚本层拿不到。
    // 这里把状态包进一张隐藏表：闭包持有 shared_ptr 保证生命周期，
    // 表里再放一个裸指针供取用。隐藏键用 \x01 前缀 —— 不是合法标识符，
    // 不会和脚本里的成员名冲突。
    static Value ioFileStateBox(const std::shared_ptr<IoFileState>& st)
    {
        auto box = std::make_shared<Table>();
        box->map[L"\x01ptr"] = Value::Pointer_((void*)st.get());
        box->map[L"\x01keep"] = NF([st](vector<Value>&) -> Value { return Value(); });
        return Value::Table_(box);
    }
    static IoFileState* ioFileStateOf(const Value& fileObj)
    {
        if (fileObj.t != Value::TableT || !fileObj.tab) return nullptr;
        auto sit = fileObj.tab->map.find(L"\x01state");
        if (sit == fileObj.tab->map.end()) return nullptr;
        if (sit->second.t != Value::TableT || !sit->second.tab) return nullptr;
        auto pit = sit->second.tab->map.find(L"\x01ptr");
        if (pit == sit->second.tab->map.end()) return nullptr;
        if (pit->second.t != Value::Pointer) return nullptr;
        return (IoFileState*)pit->second.ptr;
    }
    // Official io.file TEXT-mode read filter (docs/language-reference/
    // datatype/newline.md §读写文件的文本模式 + docs/library-guide/builtin/
    // io/file.md): reads translate CRLF -> '\n' and treat '\0'/'\x1A' as
    // terminators — a text-mode read never returns those bytes. Puts the
    // filtered text in `out` and returns the number of RAW input bytes the
    // output consumed; the caller seeks the fd / rewinds the buffer cursor
    // by (raw.size() - keep) so the terminator is not swallowed and later
    // reads stay stopped at it.
    static size_t ioFileTextKeep(const std::string& raw, std::string& out)
    {
        size_t term = raw.find('\0');
        size_t t2 = raw.find('\x1A');
        if (t2 != std::string::npos &&
            (term == std::string::npos || t2 < term))
            term = t2;
        size_t keepRaw =
            (term == std::string::npos) ? raw.size() : term;
        out.clear();
        out.reserve(keepRaw);
        for (size_t k = 0; k < keepRaw; k++)
        {
            if (raw[k] == '\r' && k + 1 < keepRaw && raw[k + 1] == '\n')
                continue; // CRLF -> '\n' (drop the CR)
            out.push_back(raw[k]);
        }
        return keepRaw;
    }
    // Text-mode write filter: every '\n' not already preceded by '\r'
    // becomes CRLF (docs: 「写入时又自动将'\n'转换为'\r\n'」).
    static void ioFileWriteTranslate(std::string& out)
    {
        std::string r;
        r.reserve(out.size() + out.size() / 8);
        for (size_t k = 0; k < out.size(); k++)
        {
            if (out[k] == '\n' && (k == 0 || out[k - 1] != '\r'))
                r.push_back('\r');
            r.push_back(out[k]);
        }
        out.swap(r);
    }
    // 逐行读取（裸指针版）：返回 false = 已到末尾。行尾的 CR/LF 不计入结果。
    static bool ioFileReadLineRaw(IoFileState* st, wstring& out)
    {
        out.clear();
        if (!st || st->bClosed) return false;
        if (st->bMem)
        {
            if (st->memPos >= st->mem.size()) return false;
            size_t nl = st->mem.find('\n', st->memPos);
            size_t end = (nl == std::string::npos) ? st->mem.size() : nl;
            out.reserve(end - st->memPos);
            for (size_t k = st->memPos; k < end; k++)
                out.push_back((wchar_t)(unsigned char)st->mem[k]);
            st->memPos = (nl == std::string::npos) ? st->mem.size() : nl + 1;
            if (!out.empty() && out.back() == L'\r') out.pop_back();
            return true;
        }
        if (!st->f) return false;
        std::string line;
        int c = 0;
        bool any = false;
        while ((c = fgetc(st->f)) != EOF)
        {
            any = true;
            if (c == '\n') break;
            line.push_back((char)c);
        }
        if (!any) return false;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out.reserve(line.size());
        for (char ch : line) out.push_back((wchar_t)(unsigned char)ch);
        return true;
    }
    // 读完自动关闭（官方：读取完毕以后自动关闭文件对象）
    static void ioFileCloseRaw(IoFileState* st)
    {
        if (!st || st->bClosed) return;
        if (st->f) { fclose(st->f); st->f = nullptr; }
        st->bClosed = true;
    }

    // ---- fibers (纤程) ----
    // A fiber has its own stack and can be suspended and resumed, but it is not
    // a thread and fibers never run concurrently. Implemented on Win32 fibers.
    //
    // A fiber runs on its OWN C++ stack, so the VM's stack-structured state
    // has to travel with it (see FiberCtx): the GC frame stack, the explicit
    // root stack, the epoch stack, the control-flow flags and the
    // meta-dispatch owner. Without this, a fiber that yields in the middle of
    // a call leaves its frame on top of the resumer's frame stack, and the
    // resumer's pop_back() then removes the WRONG frame - the collector
    // loses live frames (the same failure mode as "a running frame got
    // collected"), and the two execution streams' control flow gets mixed
    // up (a `return` inside a fiber aborts the resumer's loop).
    // VMFiber itself lives in common.h: the collector needs it to release
    // the execution context of an abandoned fiber.
    // The thread's root fiber and the fiber running right now are
    // per-thread: a Win32 fiber can only be resumed on the thread that
    // created it.
    static thread_local void* g_mainFiber = nullptr;

    // The fiber that is running right now. It belongs to the execution
    // context, not to the thread: a fiber may resume another fiber, and
    // then BOTH are on the stack - a single slot would make the inner
    // fiber's yield() hand control to the wrong place.
    static VMFiber* CurRunningFiber()
    {
        Interp* it = g_interpCurrent;
        if (it && it->curCtx) return it->curCtx->runningFiber;
        return nullptr;
    }

    static VMFiber* FiberOf(const Value& v)
    {
        if (v.t != Value::TableT || !v.tab)
            return nullptr;
        auto it = v.tab->map.find(L"__fiber");
        if (it == v.tab->map.end() || it->second.t != Value::Pointer)
            return nullptr;
        return (VMFiber*)it->second.ptr;
    }

    static Value MakeFiberValue(VMFiber* co)
    {
        auto tb = std::make_shared<Table>();
        tb->map[L"__fiber"] = Value::Pointer_(co);
        // The collector uses this to tell a live fiber from an abandoned
        // one: when the script drops the fiber value, the table becomes
        // unreachable and the fiber's context can be released.
        if (co && co->ctx) co->ctx->fiberVal = tb;
        Value v = Value::Table_(tb);
        // yield() / resume() / status() use the pointer key above
        return v;
    }

    // Move the VM's stack-structured state from the running context into the
    // target context and load the target's state. After the call the
    // interpreter's live stacks belong to the target context.
    static void FiberCtxSwitch(Interp* it, FiberCtx* dst)
    {
        if (!it || !dst) return;
        FiberCtx* src = it->curCtx;
        if (!src || src == dst) return;

        src->gcFrames.swap(it->gcFrames);
        it->gcFrames.swap(dst->gcFrames);
        src->roots.swap(g_gcRoots);
        g_gcRoots.swap(dst->roots);
        src->epochStack.swap(g_gcEpochStack);
        g_gcEpochStack.swap(dst->epochStack);

        std::swap(src->nReturn, it->nReturn);
        std::swap(it->nReturn, dst->nReturn);
        std::swap(src->nBreak, it->nBreak);
        std::swap(it->nBreak, dst->nBreak);
        std::swap(src->nContinue, it->nContinue);
        std::swap(it->nContinue, dst->nContinue);

        Value tmp = src->ret; src->ret = it->ret; it->ret = tmp;
        tmp = dst->ret; dst->ret = it->ret; it->ret = tmp;

        tmp = src->owner; src->owner = Interp::GetCurrentOwner();
        Interp::SetCurrentOwner(tmp);
        tmp = dst->owner; dst->owner = Interp::GetCurrentOwner();
        Interp::SetCurrentOwner(tmp);

        std::swap(src->metaDepth, g_metaDispatchDepth);
        std::swap(g_metaDispatchDepth, dst->metaDepth);
        src->metaActive.swap(g_metaDispatchActive);
        g_metaDispatchActive.swap(dst->metaActive);

        it->curCtx = dst;
    }

    static void CALLBACK VMFiberEntry(void* pParam)
    {
        VMFiber* co = (VMFiber*)pParam;
        Interp* it = g_interpCurrent;
        Value r;
        if (it && co->ctx && co->ctx->fn.t == Value::Func)
        {
            vector<Value> args = co->inArgs;
            r = it->CallVmFunc(co->ctx->fn, args);
        }
        co->outVals.clear();
        co->outVals.push_back(r);
        if (r.mrets)
            for (auto& v : *r.mrets) co->outVals.push_back(v);
        co->status = L"dead";
        // Hand the execution back to whoever resumed this fiber. The
        // context switch has to happen BEFORE the cleanup below: the
        // fiber's own leftovers live in its context only after the switch.
        FiberCtx* back = co->resumeCtx;
        if (it && back) FiberCtxSwitch(it, back);
        // The fiber is finished: its frames, its explicit roots and its
        // entry function are no longer live, so they must leave the GC root
        // set - otherwise every fiber that ever ran would keep its closure
        // alive for the rest of the process.
        if (co->ctx)
        {
            co->ctx->fn = Value();
            co->ctx->gcFrames.clear();
            co->ctx->roots.clear();
            co->ctx->ret = Value();
        }
        if (it && co->ctx)
        {
            for (size_t i = 0; i < it->fiberCtxs.size(); i++)
            {
                if (it->fiberCtxs[i] == co->ctx)
                {
                    it->fiberCtxs.erase(it->fiberCtxs.begin() + i);
                    break;
                }
            }
        }
        SwitchToFiber(co->resumeFiber);   // never returns
    }

    static void FiberMainInit(Interp* it)
    {
        if (!g_mainFiber)
        {
            void* f = ConvertThreadToFiber(nullptr);
            if (!f) f = GetCurrentFiber();   // the thread is already a fiber
            g_mainFiber = f;
        }
        if (it && it->mainCtx.osFiber == nullptr)
            it->mainCtx.osFiber = g_mainFiber;
    }

    // one shared implementation for create / fiber.create
    static Value FiberCreate(const Value& fn)
    {
        if (fn.t != Value::Func)
            return Value();
        Interp* it = g_interpCurrent;
        if (!it)
            return Value();
        FiberMainInit(it);
        VMFiber* co = new VMFiber();
        co->ctx = std::make_shared<FiberCtx>();
        co->ctx->fn = fn;
        co->hFiber = CreateFiber(0, VMFiberEntry, co);
        if (!co->hFiber)
        {
            delete co;
            return Value();
        }
        co->ctx->osFiber = co->hFiber;
        co->ctx->fiber = co;
        co->ctx->runningFiber = co;
        // The fiber's context holds its frames, its explicit roots and its
        // entry function. The VM root graph cannot reach any of it (a fiber
        // value only carries a native pointer), so the collector has to be
        // told about it explicitly.
        it->fiberCtxs.push_back(co->ctx);
        return MakeFiberValue(co);
    }

    // resume(fiber, ...) -> true, values... ; on failure: false, message
    static Value FiberResume(const Value& coV, const vector<Value>& args,
                             wstring* errMsg)
    {
        VMFiber* co = FiberOf(coV);
        if (!co)
        {
            if (errMsg) *errMsg = L"参数不是纤程对象";
            return Value();
        }
        if (co->status == L"dead" || !co->hFiber)
        {
            Value f = Value::Bool_(false);
            auto ex = std::make_shared<vector<Value>>();
            ex->push_back(Value::Str_(L"cannot resume dead fiber"));
            f.mrets = ex;
            return f;
        }
        Interp* it = g_interpCurrent;
        if (!it)
        {
            if (errMsg) *errMsg = L"没有活动的解释器";
            return Value();
        }
        FiberMainInit(it);
        co->inArgs = args;
        co->outVals.clear();
        co->status = L"running";
        // Switch the VM's execution context to the fiber's before handing
        // over the CPU: the fiber runs on its own C++ stack, so the frame
        // stack, the root stack and the control-flow flags must travel
        // with it.
        co->resumeCtx = it->curCtx;
        co->resumeFiber = (co->resumeCtx && co->resumeCtx->osFiber)
                        ? co->resumeCtx->osFiber : g_mainFiber;
        if (co->ctx) FiberCtxSwitch(it, co->ctx.get());
        SwitchToFiber(co->hFiber);
        // the fiber switched the context back to the resumer by itself
        if (co->status != L"dead")
            co->status = L"suspended";
        Value ok = Value::Bool_(true);
        auto ex = std::make_shared<vector<Value>>();
        for (auto& v : co->outVals) ex->push_back(v);
        ok.mrets = ex;
        return ok;
    }

    // yield(...) — suspend the running fiber, handing values to resume
    static Value FiberYield(const vector<Value>& vals)
    {
        VMFiber* co = CurRunningFiber();
        if (!co)
            return Value();
        co->outVals = vals;
        co->status = L"suspended";
        // Hand the execution back to the resumer. The VM's execution
        // context has to travel with the fiber, or both flows would share
        // one frame stack and the collector would lose live frames.
        Interp* it = g_interpCurrent;
        if (it && co->resumeCtx) FiberCtxSwitch(it, co->resumeCtx);
        SwitchToFiber(co->resumeFiber);
        // resumed: the values passed to resume become the yield's result
        if (!co->inArgs.empty())
            return co->inArgs[0];
        return Value();
    }

    // ---- runtime compilation (eval / loadcode / loadcodex) ----
    // Transform template syntax (<? ... ?> and ?> raw text <? / <?= expr ?>) into pure code calling print(...)
    static wstring PreprocessTemplate(const wstring& src)
    {
        auto escapeRawString = [](const wstring& s) -> wstring {
            wstring esc;
            esc.reserve(s.size() + 10);
            esc += L"`";
            for (wchar_t ch : s)
            {
                if (ch == L'`')
                    esc += L"``";
                else
                    esc += ch;
            }
            esc += L"`";
            return esc;
        };

        // Check if template syntax is present
        size_t pQ = src.find(L"?>");
        size_t pO = src.find(L"<?");
        if (pQ == wstring::npos && pO == wstring::npos)
        {
            // If it starts with an HTML-like tag (e.g. <!DOCTYPE, <html>), treat as raw HTML template
            size_t firstNonWs = 0;
            while (firstNonWs < src.size() && iswspace(src[firstNonWs])) firstNonWs++;
            if (firstNonWs < src.size() && src[firstNonWs] == L'<')
            {
                wstring head = src.substr(firstNonWs, 15);
                for (auto& c : head) c = towlower(c);
                if (head.find(L"<!doctype") == 0 || head.find(L"<html") == 0 ||
                    head.find(L"<head") == 0 || head.find(L"<body") == 0 ||
                    head.find(L"<!--") == 0 || head.find(L"<div") == 0 ||
                    head.find(L"<table") == 0 || head.find(L"<span") == 0)
                {
                    return L"..print(" + escapeRawString(src) + L");\n";
                }
            }
            return src;
        }

        wstring out;
        size_t pos = 0;
        size_t n = src.size();

        // Skip leading whitespace to inspect template opening
        size_t firstNonWs = 0;
        while (firstNonWs < n && iswspace(src[firstNonWs])) firstNonWs++;

        bool inCode = false;
        if (firstNonWs + 1 < n && src[firstNonWs] == L'<' && src[firstNonWs + 1] == L'?')
        {
            inCode = true;
            pos = firstNonWs + 2;
            while (pos < n && iswalpha(src[pos]))
                pos++;
        }
        else if (firstNonWs + 1 < n && src[firstNonWs] == L'?' && src[firstNonWs + 1] == L'>')
        {
            inCode = false;
            pos = firstNonWs + 2;
        }
        else
        {
            inCode = false;
            pos = 0;
        }

        while (pos < n)
        {
            if (inCode)
            {
                size_t endCode = src.find(L"?>", pos);
                if (endCode == wstring::npos)
                {
                    out += src.substr(pos);
                    break;
                }
                out += src.substr(pos, endCode - pos);
                out += L"\n";
                pos = endCode + 2;
                inCode = false;
            }
            else
            {
                size_t startCode = src.find(L"<?", pos);
                wstring rawText;
                if (startCode == wstring::npos)
                {
                    rawText = src.substr(pos);
                    pos = n;
                }
                else
                {
                    rawText = src.substr(pos, startCode - pos);
                    pos = startCode + 2;
                    inCode = true;
                }
                if (!rawText.empty())
                {
                    out += L"..print(" + escapeRawString(rawText) + L");\n";
                }
                if (inCode && pos < n)
                {
                    size_t pExpr = pos;
                    while (pExpr < n && (src[pExpr] == L' ' || src[pExpr] == L'\t' || src[pExpr] == L'\r' || src[pExpr] == L'\n')) pExpr++;
                    if (pExpr < n && src[pExpr] == L'=')
                    {
                        pos = pExpr + 1;
                        size_t endExpr = src.find(L"?>", pos);
                        wstring exprStr;
                        if (endExpr == wstring::npos)
                        {
                            exprStr = src.substr(pos);
                            pos = n;
                            inCode = true;
                        }
                        else
                        {
                            exprStr = src.substr(pos, endExpr - pos);
                            pos = endExpr + 2;
                            inCode = false;
                        }
                        out += L"..print(" + exprStr + L");\n";
                    }
                }
            }
        }
        return out;
    }

    // Compiles 'src' into an anonymous function: the code becomes the body
    // and '...' collects the arguments, because a loaded code
    // string or file behaves exactly like an anonymous function.
    static PNode CompileToFunc(const wstring& src, const wstring* pBaseDir, wstring* err)
    {
        wstring processed = PreprocessTemplate(src);
        wstring e;
        vector<Tok> toks = Lex(processed, &e);
        if (!e.empty())
        {
            if (err) *err = e;
            return nullptr;
        }
        Parser ps{ toks, 0, &e };
        if (pBaseDir && !pBaseDir->empty())
            ps.strEmbedBase = *pBaseDir;
        else if (g_interpCurrent && !g_interpCurrent->baseDir.empty())
            ps.strEmbedBase = g_interpCurrent->baseDir;
        ps.strEmbedRoot = EmbedRoot();
        PNode blk = ps.ParseBlock();
        AnnotateNameIds(blk);
        if (!e.empty())
        {
            if (err) *err = e;
            return nullptr;
        }
        PNode fn = NewPNode();
        fn->k = Node::FuncLit;
        fn->a = blk;
        fn->names.push_back(L"...");
        fn->defaults.push_back(nullptr);
        return fn;
    }

    // loadcode/loadcodex accept either a code string or a file path.
    // Root-relative ('/x') resolves against the app root, '~' against the
    // installation root; a multi-line argument is always code.
    static bool ReadCodeFile(const wstring& s, wstring* out)
    {
        if (s.empty() || s.find(L'\n') != wstring::npos ||
            s.find(L'\r') != wstring::npos)
            return false;
        wstring p = s;
        if (p[0] == L'/' || p[0] == L'\\')
        {
            wstring root;
            if (g_interpCurrent && !g_interpCurrent->baseDir.empty())
                root = g_interpCurrent->baseDir;
            if (root.empty())
            {
                wchar_t szExe[MAX_PATH] = L"";
                GetModuleFileNameW(nullptr, szExe, MAX_PATH);
                wchar_t* q = wcsrchr(szExe, L'\\');
                if (q) *q = 0;
                root = szExe;
            }
            p = root + L"\\" + p.substr(1);
        }
        else if (p[0] == L'~')
            p = ResolveTildeDisk(p);
        FILE* pf = nullptr;
        if (_wfopen_s(&pf, p.c_str(), L"rb") != 0 || !pf)
            return false;
        fseek(pf, 0, SEEK_END);
        long sz = ftell(pf);
        fseek(pf, 0, SEEK_SET);
        std::string raw;
        if (sz > 0)
        {
            raw.resize((size_t)sz);
            fread(&raw[0], 1, (size_t)sz, pf);
        }
        fclose(pf);
        size_t off = (raw.size() >= 3 && (unsigned char)raw[0] == 0xEF &&
                      (unsigned char)raw[1] == 0xBB &&
                      (unsigned char)raw[2] == 0xBF) ? 3 : 0;
        int w = MultiByteToWideChar(CP_UTF8, 0, raw.data() + off,
                                    (int)(raw.size() - off), nullptr, 0);
        wstring r((size_t)(w > 0 ? w : 0), 0);
        if (w > 0)
            MultiByteToWideChar(CP_UTF8, 0, raw.data() + off,
                                (int)(raw.size() - off), &r[0], w);
        *out = r;
        return true;
    }
    // wide string -> ANSI bytes (the raw.* memory helpers work on bytes)
    static std::string ToA(const wstring& w)
    {
        if (w.empty()) return std::string();
        int n = WideCharToMultiByte(CP_ACP, 0, w.c_str(), (int)w.size(),
                                    nullptr, 0, nullptr, nullptr);
        if (n <= 0) return std::string();
        std::string s((size_t)n, 0);
        WideCharToMultiByte(CP_ACP, 0, w.c_str(), (int)w.size(),
                            &s[0], n, nullptr, nullptr);
        return s;
    }

    // official aardio exposes UTF-8 byte positions everywhere: #s, s[i],
    // and every string position argument/return (find/match/sub/slice/...)
    // operate on BYTES, while the engine stores strings as wstring. These
    // helpers translate a 1-based UTF-8 byte position <-> wchar index.
    // Perf note (A/B benchmarked): these walkers are on the hot path of
    // every position-based string builtin (find/match/index/lastIndexOf/
    // sub/left/right/len). They must stay tight: only astral HIGH
    // surrogates pay the next-wchar lookahead; loading w[i+1] on every
    // step (a shared WcharUtf8ByteCount(c1,c2,isPair&) helper) measured
    // ~2.6x slower per wchar and made string.find regress 2.7x end to end.
    static size_t WcharToUtf8Pos(const wstring& w, size_t wcharIdx1)
    {
        // wcharIdx1: 1-based wchar index → returns 1-based UTF-8 byte pos.
        // Pure arithmetic (no kernel API): O(prefix) with per-wchar byte
        // counts; a wchar index landing mid-surrogate-pair left-clamps to
        // the pair start, mirroring Utf8PosToWcharIdx.
        if (wcharIdx1 <= 1) return 1;
        size_t lim = min(wcharIdx1 - 1, w.size());
        size_t acc = 0;
        for (size_t i = 0; i < lim; i++)
        {
            wchar_t c = w[i];
            if (c < 0x80) { acc += 1; continue; }
            if (c < 0x800) { acc += 2; continue; }
            if (c >= 0xD800 && c <= 0xDBFF)
            {
                if (i + 1 < w.size() && w[i + 1] >= 0xDC00 &&
                    w[i + 1] <= 0xDFFF)
                {
                    if (i + 1 < lim) { acc += 4; i++; continue; }
                    return acc + 1; // lim cuts inside the pair → clamp
                }
            }
            acc += 3;
        }
        return acc + 1;
    }
    static size_t Utf8PosToWcharIdx(const wstring& w, size_t bytePos1)
    {
        // bytePos1: 1-based UTF-8 byte pos → wchar index (clamped)
        if (bytePos1 <= 1) return 1;
        size_t acc = 0;
        for (size_t i = 0; i < w.size(); i++)
        {
            wchar_t c = w[i];
            int n;
            if (c < 0x80) n = 1;
            else if (c < 0x800) n = 2;
            else if (c >= 0xD800 && c <= 0xDBFF && i + 1 < w.size() &&
                     w[i + 1] >= 0xDC00 && w[i + 1] <= 0xDFFF)
                n = 4;
            else n = 3;
            if (acc + (size_t)n >= bytePos1)
                return i + 1;
            acc += (size_t)n;
            if (n == 4) i++;
        }
        return w.size() + 1;
    }
    static void Utf8TwoPosToWchar(const wstring& w, size_t b1, size_t b2,
                                  size_t& wa, size_t& wb)
    {
        // fused twin of Utf8PosToWcharIdx: resolves TWO 1-based UTF-8 byte
        // positions to wchar indexes in ONE walk (callers used to pay
        // Utf8LenOf + two Utf8PosToWcharIdx passes). Per-position semantics
        // are identical to Utf8PosToWcharIdx: pos <= 1 → 1, a byte landing
        // mid-codepoint clamps to the codepoint start, and a position past
        // the byte end yields w.size() + 1. b2 may exceed any real position
        // (e.g. (size_t)-1) to mean "to the end of the string".
        wa = wb = w.size() + 1;
        bool f1 = b1 <= 1, f2 = b2 <= 1;
        if (f1) wa = 1;
        if (f2) wb = 1;
        if (f1 && f2) return;
        size_t acc = 0;
        for (size_t i = 0; i < w.size(); i++)
        {
            wchar_t c = w[i];
            int n;
            if (c < 0x80) n = 1;
            else if (c < 0x800) n = 2;
            else if (c >= 0xD800 && c <= 0xDBFF && i + 1 < w.size() &&
                     w[i + 1] >= 0xDC00 && w[i + 1] <= 0xDFFF)
                n = 4;
            else n = 3;
            if (!f1 && acc + (size_t)n >= b1) { wa = i + 1; f1 = true; }
            if (!f2 && acc + (size_t)n >= b2) { wb = i + 1; f2 = true; }
            if (f1 && f2) return;
            acc += (size_t)n;
            if (n == 4) i++;
        }
    }
    static size_t Utf8LenOf(const wstring& w)
    {
        // single source of truth: the tight walk lives in common.h
        // (Utf8ByteLenOf); keeping one copy avoids drift and lets #s,
        // publish and other translation units share it.
        return Utf8ByteLenOf(w);
    }
    static size_t CharLenOf(const wstring& w)
    {
        // round 15: SSE2 surrogate sweep. If the string contains NO
        // wchar in U+D800..U+DFFF at all, the codepoint count equals the
        // wchar count exactly — the exact walk below only shrinks the
        // count for PAIRED surrogates and leaves unpaired ones at 1, so
        // zero surrogates means zero shrinking. One 128-bit sweep at 8
        // wchar/step (~6x faster than the scalar walk on long strings)
        // either proves the fast case or finds a hit and falls back to
        // the original walk: pure subset check, zero semantic drift.
        const size_t n = w.size();
        const wchar_t* p = w.data();
        size_t i = 0;
        bool fSurr = false;
        {
            // unsigned range [0xD800,0xDFFF] as a signed compare after
            // flipping the top bit: x^0x8000 lands the range at
            // [0x5800,0x5FFF], with every other 16-bit value mapped
            // outside it (ASCII/CJK go negative, 0xE000.. go >= 0x6000).
            const __m128i xorBit = _mm_set1_epi16((short)0x8000);
            const __m128i loLim = _mm_set1_epi16((short)0x57FF);
            const __m128i hiLim = _mm_set1_epi16((short)0x6000);
            for (; i + 8 <= n; i += 8)
            {
                __m128i v = _mm_loadu_si128((const __m128i*)(p + i));
                __m128i xs = _mm_xor_si128(v, xorBit);
                __m128i m = _mm_and_si128(_mm_cmpgt_epi16(xs, loLim),
                                          _mm_cmpgt_epi16(hiLim, xs));
                if (_mm_movemask_epi8(m))
                {
                    fSurr = true;
                    break;
                }
            }
            if (!fSurr)
            {
                for (; i < n; i++)
                {
                    unsigned int c = (unsigned int)(unsigned short)p[i];
                    if (c >= 0xD800 && c <= 0xDFFF)
                    {
                        fSurr = true;
                        break;
                    }
                }
            }
        }
        if (!fSurr)
            return n;
        size_t count = 0;
        for (size_t k = 0; k < n; k++)
        {
            wchar_t c = p[k];
            if (c >= 0xD800 && c <= 0xDBFF && k + 1 < n &&
                p[k + 1] >= 0xDC00 && p[k + 1] <= 0xDFFF)
            {
                k++;
            }
            count++;
        }
        return count;
    }
    static size_t CharPosToWcharIdx(const wstring& w, size_t charPos1)
    {
        if (charPos1 <= 1) return 1;
        size_t cur = 1;
        for (size_t i = 0; i < w.size(); i++)
        {
            if (cur == charPos1) return i + 1;
            wchar_t c = w[i];
            if (c >= 0xD800 && c <= 0xDBFF && i + 1 < w.size() && w[i + 1] >= 0xDC00 && w[i + 1] <= 0xDFFF)
            {
                i++;
            }
            cur++;
        }
        return w.size() + 1;
    }
    static void CharTwoPosToWchar(const wstring& w, size_t c1, size_t c2,
                                  size_t& wa, size_t& wb)
    {
        // fused twin of CharPosToWcharIdx: resolves TWO 1-based codepoint
        // positions to wchar indexes in ONE walk (callers used to pay
        // CharLenOf + two CharPosToWcharIdx passes). Per-position semantics
        // identical: pos <= 1 → 1, a position past the codepoint count
        // yields w.size() + 1. c2 may exceed any real position to mean
        // "to the end of the string".
        wa = wb = w.size() + 1;
        bool f1 = c1 <= 1, f2 = c2 <= 1;
        if (f1) wa = 1;
        if (f2) wb = 1;
        if (f1 && f2) return;
        size_t cur = 1; // 1-based codepoint index of w[i]
        for (size_t i = 0; i < w.size(); i++)
        {
            if (!f1 && cur == c1) { wa = i + 1; f1 = true; }
            if (!f2 && cur == c2) { wb = i + 1; f2 = true; }
            if (f1 && f2) return;
            wchar_t ch = w[i];
            if (ch >= 0xD800 && ch <= 0xDBFF && i + 1 < w.size() &&
                w[i + 1] >= 0xDC00 && w[i + 1] <= 0xDFFF)
                i++;
            cur++;
        }
    }

    // raw memory bytes of a string value:
    // When bRawBytes is set (e.g. from raw.tostring or binary string& out-params),
    // unpack each byte directly without codepage conversion so binary data is preserved.
    // Otherwise, text strings encode as standard UTF-8.
    static std::string RawBytesOf(const Value& v)
    {
        if (v.s.empty()) return std::string();
        if (v.bRawBytes)
        {
            std::string out;
            out.reserve(v.s.size());
            for (wchar_t ch : v.s)
                out.push_back((char)(unsigned char)ch);
            return out;
        }
        int cb = WideCharToMultiByte(CP_UTF8, 0, v.s.c_str(), (int)v.s.size(),
                                     nullptr, 0, nullptr, nullptr);
        std::string out((size_t)max(cb, 0), 0);
        if (cb > 0)
            WideCharToMultiByte(CP_UTF8, 0, v.s.c_str(), (int)v.s.size(),
                                &out[0], cb, nullptr, nullptr);
        return out;
    }
    static std::string RawBytesOf(const wstring& w, bool bRawBytes = false)
    {
        if (w.empty()) return std::string();
        if (bRawBytes)
        {
            std::string out;
            out.reserve(w.size());
            for (wchar_t ch : w)
                out.push_back((char)(unsigned char)ch);
            return out;
        }
        int cb = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                     nullptr, 0, nullptr, nullptr);
        std::string out((size_t)max(cb, 0), 0);
        if (cb > 0)
            WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                &out[0], cb, nullptr, nullptr);
        return out;
    }

// ---- InitBuiltins ----
// thread.create state registry: HANDLE -> result slot. Worker threads
// write through the mutex; waitOne/getExitCode read the HANDLE directly.
struct ThreadJob
{
    std::shared_ptr<ThreadState> st;
    PNode fnBody;
    vector<wstring> params;
    vector<Value> args;
    Value owner;
    wstring baseDir;
};
std::map<HANDLE, std::shared_ptr<ThreadState>> g_threadStates;
std::mutex g_threadStatesMutex;

DWORD WINAPI ThreadWorkerProc(LPVOID pv)
{
    auto* job = (ThreadJob*)pv;
    wstring threadErr;
    // ⚠⚠ The worker's interpreter must NOT live on the worker's STACK.
    //
    //   A thread's stack is released the moment the thread exits, but the
    //   GC (running later on the MAIN thread) can still reach envs owned by
    //   this interpreter - `Interp::globals` is an Env **member** of the
    //   interpreter object, so it dies with the thread's stack, while a
    //   Value::envUp / Env::parent chain that still points at it survives.
    //   The collector then dereferences that freed stack memory:
    //       [FATAL CRASH] code=0xC0000005 (access violation)
    //   Reproducer (10 lines, deterministic):
    //       import thread;
    //       thread.create(function(a){ return a; }, "x");
    //       for(i=1; 300000; 1){ var junk = {a=i; b="str" ++ i; c={d=i}}; }
    //   -> crash on the first GC after the worker exited; with
    //      NODIO_GC_THRESHOLD=0 (collector off) the same script exits 0.
    //
    //   Fix: heap-allocate the interpreter and keep it alive with the
    //   thread state (g_threadStates keeps the state for the process
    //   lifetime). One interpreter per worker thread stays resident - the
    //   alternative (letting the thread free it) is exactly the bug.
    auto wtOwner = std::make_shared<Interp>(&threadErr);
    Interp& wt = *wtOwner;
    job->st->interp = wtOwner;
    g_interpCurrent = &wt;
    wt.InitBuiltins();
    if (!job->baseDir.empty())
        wt.baseDir = job->baseDir;
    auto local = std::make_shared<Env>();
    local->self = local;
    local->parent = wt.globals.self;
    Interp::GcFrameScope fsThread(wt, local);
    if (job->owner.t != Value::Null)
        local->VarAt(IdOwner()) = job->owner;
for (size_t i = 0; i < job->params.size(); i++)
    {
        if (job->params[i] == L"...")
        {
            // trailing `...` parameter packs ALL remaining args (bDots
            // shape) - binding it like a plain param truncated the
            // varargs to one inside worker threads
            auto vargs = std::make_shared<vector<Value>>();
            auto tbV = std::make_shared<Table>();
            for (size_t k = i; k < job->args.size(); k++)
            {
                vargs->push_back(job->args[k]);
                tbV->arr.push_back(job->args[k]);
            }
            Value vD = tbV->arr.empty() ? Value() : Value::Table_(tbV);
            if (vD.tab) { vD.mrets = vargs; vD.bDots = true; }
            local->VarAt(IdDots()) = vD;
            break;
        }
        local->VarAt(NameId(job->params[i])) =
            i < job->args.size() ? job->args[i] : Value();
    }
    wt.nReturn = 0;
    Value r = wt.ExecBlock(job->fnBody, local.get());
    CleanDeadEnv(local);
    std::lock_guard<std::mutex> lk(g_threadStatesMutex);
    if (threadErr.empty())
        job->st->result = DeepCopy(r);
    else
        job->st->error = threadErr;
    job->st->done = true;
    delete job;
    return 0;
}

// ---- math.size64 implementation ----
static shared_ptr<Table> g_size64_meta;

bool IsSize64(const Value& v)
{
    if (v.t != Value::TableT || !v.tab) return false;
    auto it = v.tab->map.find(L"_isSize64");
    if (it != v.tab->map.end() && it->second.Truthy()) return true;
    auto im = v.tab->map.find(L"_meta");
    if (im != v.tab->map.end() && g_size64_meta && im->second.tab == g_size64_meta)
        return true;
    return false;
}

uint64_t GetSize64Val(const Value& v)
{
    if (!IsSize64(v)) return 0;
    auto ilo = v.tab->map.find(L"_size64_lo");
    auto ihi = v.tab->map.find(L"_size64_hi");
    if (ilo != v.tab->map.end() && ihi != v.tab->map.end())
    {
        uint32_t lo = (uint32_t)(unsigned long long)ilo->second.n;
        uint32_t hi = (uint32_t)(unsigned long long)ihi->second.n;
        return ((uint64_t)hi << 32) | lo;
    }
    auto iv = v.tab->map.find(L"_size64_val");
    if (iv != v.tab->map.end())
        return (uint64_t)iv->second.n;
    return 0;
}

void SetSize64Val(Value& v, uint64_t val)
{
    if (!v.tab) return;
    uint32_t lo = (uint32_t)(val & 0xFFFFFFFF);
    uint32_t hi = (uint32_t)(val >> 32);
    v.tab->map[L"_size64_lo"] = Value::Num_((double)lo);
    v.tab->map[L"_size64_hi"] = Value::Num_((double)hi);
    v.tab->map[L"_size64_val"] = Value::Num_((double)val);
}

uint64_t ToSize64Uint(const Value& v)
{
    if (IsSize64(v)) return GetSize64Val(v);
    if (v.t == Value::Num) return (uint64_t)(long long)v.n;
    if (v.t == Value::Str)
    {
        wchar_t* end = nullptr;
        return _wcstoui64(v.s.c_str(), &end, 0);
    }
    if (v.t == Value::Pointer) return (uint64_t)(INT_PTR)v.ptr;
    return (uint64_t)BNum(v);
}

Value CreateSize64(uint64_t val)
{
    auto t = std::make_shared<Table>();
    Value v = Value::Table_(t);
    t->map[L"_isSize64"] = Value::Bool_(true);
    t->map[L"_struct"] = Value::Str_(L"LONG value;");
    SetSize64Val(v, val);

    t->map[L"split32"] = NF([](vector<Value>& a) -> Value {
        Value self = Interp::GetCurrentOwner();
        if (!IsSize64(self) && !a.empty() && IsSize64(a[0])) self = a[0];
        uint64_t u = GetSize64Val(self);
        uint32_t lo = (uint32_t)(u & 0xFFFFFFFF);
        uint32_t hi = (uint32_t)(u >> 32);
        Value vLo = Value::Num_((double)lo);
        auto ex = std::make_shared<vector<Value>>();
        ex->push_back(Value::Num_((double)hi));
        vLo.mrets = ex;
        return vLo;
    });

    t->map[L"swap"] = NF([](vector<Value>& a) -> Value {
        Value self = (!a.empty() && IsSize64(a[0])) ? a[0] : Interp::GetCurrentOwner();
        if (!IsSize64(self)) return self;
        uint64_t u = _byteswap_uint64(GetSize64Val(self));
        SetSize64Val(self, u);
        return self;
    });

    t->map[L"format"] = NF([](vector<Value>& a) -> Value {
        Value self = (!a.empty() && IsSize64(a[0])) ? a[0] : Interp::GetCurrentOwner();
        uint64_t u = GetSize64Val(self);
        wchar_t buf[128] = L"";
        if (StrFormatByteSizeW((LONGLONG)u, buf, 128))
            return Value::Str_(buf);
        if (u < 1024)
            swprintf_s(buf, 128, L"%llu 字节", u);
        else if (u < 1024 * 1024)
            swprintf_s(buf, 128, L"%.2f KB", (double)u / 1024.0);
        else if (u < 1024ULL * 1024 * 1024)
            swprintf_s(buf, 128, L"%.2f MB", (double)u / (1024.0 * 1024.0));
        else
            swprintf_s(buf, 128, L"%.2f GB", (double)u / (1024.0 * 1024.0 * 1024.0));
        return Value::Str_(buf);
    });

    t->map[L"add"] = NF([](vector<Value>& a) -> Value {
        Value self = Interp::GetCurrentOwner();
        int idx = 0;
        if ((!IsSize64(self) || a.size() >= 2) && !a.empty() && IsSize64(a[0])) { self = a[0]; idx = 1; }
        if (!IsSize64(self)) return self;
        uint64_t d = (idx < (int)a.size()) ? ToSize64Uint(a[idx]) : 0;
        SetSize64Val(self, GetSize64Val(self) + d);
        return self;
    });

    t->map[L"sub"] = NF([](vector<Value>& a) -> Value {
        Value self = Interp::GetCurrentOwner();
        int idx = 0;
        if ((!IsSize64(self) || a.size() >= 2) && !a.empty() && IsSize64(a[0])) { self = a[0]; idx = 1; }
        if (!IsSize64(self)) return self;
        uint64_t d = (idx < (int)a.size()) ? ToSize64Uint(a[idx]) : 0;
        SetSize64Val(self, GetSize64Val(self) - d);
        return self;
    });

    t->map[L"mul"] = NF([](vector<Value>& a) -> Value {
        Value self = Interp::GetCurrentOwner();
        int idx = 0;
        if ((!IsSize64(self) || a.size() >= 2) && !a.empty() && IsSize64(a[0])) { self = a[0]; idx = 1; }
        if (!IsSize64(self)) return self;
        uint64_t factor = (idx < (int)a.size()) ? ToSize64Uint(a[idx]) : 1;
        SetSize64Val(self, GetSize64Val(self) * factor);
        return self;
    });

    t->map[L"div"] = NF([](vector<Value>& a) -> Value {
        Value self = Interp::GetCurrentOwner();
        int idx = 0;
        if ((!IsSize64(self) || a.size() >= 2) && !a.empty() && IsSize64(a[0])) { self = a[0]; idx = 1; }
        if (!IsSize64(self)) return self;
        uint64_t divisor = (idx < (int)a.size()) ? ToSize64Uint(a[idx]) : 1;
        if (divisor != 0)
            SetSize64Val(self, GetSize64Val(self) / divisor);
        return self;
    });

    t->map[L"mod"] = NF([](vector<Value>& a) -> Value {
        Value self = Interp::GetCurrentOwner();
        int idx = 0;
        if ((!IsSize64(self) || a.size() >= 2) && !a.empty() && IsSize64(a[0])) { self = a[0]; idx = 1; }
        if (!IsSize64(self)) return self;
        uint64_t divisor = (idx < (int)a.size()) ? ToSize64Uint(a[idx]) : 1;
        if (divisor != 0)
            SetSize64Val(self, GetSize64Val(self) % divisor);
        return self;
    });

    t->map[L"reset"] = NF([](vector<Value>& a) -> Value {
        Value self = (!a.empty() && IsSize64(a[0]) && a.size() > 1) ? a[0] : Interp::GetCurrentOwner();
        if (!IsSize64(self)) return self;
        int idx = (!a.empty() && IsSize64(a[0]) && a.size() > 1) ? 1 : 0;
        uint64_t u = (idx < (int)a.size()) ? ToSize64Uint(a[idx]) : 0;
        SetSize64Val(self, u);
        return self;
    });

    t->map[L"greaterThan"] = NF([](vector<Value>& a) -> Value {
        Value self = Interp::GetCurrentOwner();
        int idx = 0;
        if ((!IsSize64(self) || a.size() >= 2) && !a.empty() && IsSize64(a[0])) { self = a[0]; idx = 1; }
        if (!IsSize64(self)) return Value::Bool_(false);
        uint64_t target = (idx < (int)a.size()) ? ToSize64Uint(a[idx]) : 0;
        return Value::Bool_(GetSize64Val(self) > target);
    });

    t->map[L"rshift"] = NF([](vector<Value>& a) -> Value {
        Value self = Interp::GetCurrentOwner();
        int idx = 0;
        if ((!IsSize64(self) || a.size() >= 2) && !a.empty() && IsSize64(a[0])) { self = a[0]; idx = 1; }
        if (!IsSize64(self)) return self;
        int shift = (idx < (int)a.size()) ? (int)BNum(a[idx]) : 0;
        SetSize64Val(self, GetSize64Val(self) >> shift);
        return self;
    });

    t->map[L"lshift"] = NF([](vector<Value>& a) -> Value {
        Value self = Interp::GetCurrentOwner();
        int idx = 0;
        if ((!IsSize64(self) || a.size() >= 2) && !a.empty() && IsSize64(a[0])) { self = a[0]; idx = 1; }
        if (!IsSize64(self)) return self;
        int shift = (idx < (int)a.size()) ? (int)BNum(a[idx]) : 0;
        SetSize64Val(self, GetSize64Val(self) << shift);
        return self;
    });

    if (!g_size64_meta)
    {
        g_size64_meta = std::make_shared<Table>();
        GcAddRootTable(g_size64_meta);   // permanent root: static holder
        g_size64_meta->map[L"_tostring"] = NF([](vector<Value>& a) -> Value {
            Value self = (!a.empty() && IsSize64(a[0])) ? a[0] : Interp::GetCurrentOwner();
            uint64_t u = GetSize64Val(self);
            wchar_t buf[32];
            swprintf_s(buf, 32, L"%llu", u);
            return Value::Str_(buf);
        });
        g_size64_meta->map[L"_tonumber"] = NF([](vector<Value>& a) -> Value {
            Value self = (!a.empty() && IsSize64(a[0])) ? a[0] : Interp::GetCurrentOwner();
            return Value::Num_((double)GetSize64Val(self));
        });
        g_size64_meta->map[L"_topointer"] = NF([](vector<Value>& a) -> Value {
            Value self = (!a.empty() && IsSize64(a[0])) ? a[0] : Interp::GetCurrentOwner();
            return Value::Pointer_((void*)(INT_PTR)GetSize64Val(self));
        });
        g_size64_meta->map[L"_eq"] = NF([](vector<Value>& a) -> Value {
            Value self = Interp::GetCurrentOwner();
            Value other = a.empty() ? Value() : a[0];
            if (!IsSize64(self) && a.size() >= 2) {
                self = a[0];
                other = a[1];
            }
            return Value::Bool_(ToSize64Uint(self) == ToSize64Uint(other));
        });
        g_size64_meta->map[L"_add"] = NF([](vector<Value>& a) -> Value {
            if (a.empty()) return Value();
            Value self = Interp::GetCurrentOwner();
            Value other = a[0];
            if (!IsSize64(self) && a.size() >= 2) { self = a[0]; other = a[1]; }
            return CreateSize64(ToSize64Uint(self) + ToSize64Uint(other));
        });
        g_size64_meta->map[L"_sub"] = NF([](vector<Value>& a) -> Value {
            if (a.empty()) return Value();
            Value self = Interp::GetCurrentOwner();
            Value other = a[0];
            if (!IsSize64(self) && a.size() >= 2) { self = a[0]; other = a[1]; }
            return CreateSize64(ToSize64Uint(self) - ToSize64Uint(other));
        });
        g_size64_meta->map[L"_mul"] = NF([](vector<Value>& a) -> Value {
            if (a.empty()) return Value();
            Value self = Interp::GetCurrentOwner();
            Value other = a[0];
            if (!IsSize64(self) && a.size() >= 2) { self = a[0]; other = a[1]; }
            return CreateSize64(ToSize64Uint(self) * ToSize64Uint(other));
        });
        g_size64_meta->map[L"_div"] = NF([](vector<Value>& a) -> Value {
            if (a.empty()) return Value();
            Value self = Interp::GetCurrentOwner();
            Value other = a[0];
            if (!IsSize64(self) && a.size() >= 2) { self = a[0]; other = a[1]; }
            uint64_t d = ToSize64Uint(other);
            return CreateSize64(d ? ToSize64Uint(self) / d : 0);
        });
        g_size64_meta->map[L"_mod"] = NF([](vector<Value>& a) -> Value {
            if (a.empty()) return Value();
            Value self = Interp::GetCurrentOwner();
            Value other = a[0];
            if (!IsSize64(self) && a.size() >= 2) { self = a[0]; other = a[1]; }
            uint64_t d = ToSize64Uint(other);
            return CreateSize64(d ? ToSize64Uint(self) % d : 0);
        });
        g_size64_meta->map[L"_concat"] = NF([](vector<Value>& a) -> Value {
            Value self = Interp::GetCurrentOwner();
            Value other = a.empty() ? Value() : a[0];
            if (!IsSize64(self) && a.size() >= 2) { self = a[0]; other = a[1]; }
            wstring s1 = self.ToString();
            if (IsSize64(self))
            {
                wchar_t buf[32];
                swprintf_s(buf, 32, L"%llu", GetSize64Val(self));
                s1 = buf;
            }
            wstring s2 = other.ToString();
            if (IsSize64(other))
            {
                wchar_t buf[32];
                swprintf_s(buf, 32, L"%llu", GetSize64Val(other));
                s2 = buf;
            }
            return Value::Str_(s1 + s2);
        });
    }
    t->map[L"_meta"] = Value::Table_(g_size64_meta);
    return v;
}

    void* DynAlloc(size_t n, void* pOld)
    {
        if (n == 0)
        {
            if (pOld)
            {
                if (IsDynBuffer(pOld))
                {
                    DynBufHeader* h = GetDynBufHeader(pOld);
                    h->magic = 0;
                    UnregisterDynBuf(pOld);
                    HeapFree(GetProcessHeap(), 0, h);
                }
                else
                {
                    HeapFree(GetProcessHeap(), 0, pOld);
                }
            }
            return nullptr;
        }

        if (pOld && IsDynBuffer(pOld))
        {
            DynBufHeader* hOld = GetDynBufHeader(pOld);
            size_t oldLen = hOld->length;
            size_t allocSz = sizeof(DynBufHeader) + n + 16;
            DynBufHeader* hNew = (DynBufHeader*)HeapReAlloc(
                GetProcessHeap(), HEAP_ZERO_MEMORY, hOld, allocSz);
            if (!hNew) return nullptr;
            hNew->magic = DYN_BUF_MAGIC;
            hNew->capacity = (uint32_t)n;
            hNew->length = (uint32_t)min((size_t)hNew->capacity, oldLen);
            char* pData = (char*)hNew + sizeof(DynBufHeader);
            pData[n] = 0;
            // the data pointer moved (realloc) — the registry must follow
            UnregisterDynBuf(pOld);
            RegisterDynBuf(pData);
            return pData;
        }
        else if (pOld)
        {
            size_t allocSz = sizeof(DynBufHeader) + n + 16;
            DynBufHeader* hNew = (DynBufHeader*)HeapAlloc(
                GetProcessHeap(), HEAP_ZERO_MEMORY, allocSz);
            if (!hNew) return nullptr;
            hNew->magic = DYN_BUF_MAGIC;
            hNew->capacity = (uint32_t)n;
            hNew->length = 0;
            char* pData = (char*)hNew + sizeof(DynBufHeader);
            memcpy(pData, pOld, n);
            // the old plain buffer is gone — drop its registry entry too
            // (a stale entry would report a size for a freed pointer)
            UnregisterBuffer(pOld);
            HeapFree(GetProcessHeap(), 0, pOld);
            pData[n] = 0;
            RegisterDynBuf(pData);
            return pData;
        }
        else
        {
            size_t allocSz = sizeof(DynBufHeader) + n + 16;
            DynBufHeader* hNew = (DynBufHeader*)HeapAlloc(
                GetProcessHeap(), HEAP_ZERO_MEMORY, allocSz);
            if (!hNew) return nullptr;
            hNew->magic = DYN_BUF_MAGIC;
            hNew->capacity = (uint32_t)n;
            hNew->length = 0;
            char* pData = (char*)hNew + sizeof(DynBufHeader);
            pData[n] = 0;
            RegisterDynBuf(pData);
            return pData;
        }
    }

// ---- string.split 系列的公共切分逻辑（第二十三轮）----
//
// 官方契约（官方 lib/builtin/string.aardio 的成员声明 + 官方库实际调用形态）：
//   split(str)                       按 UTF-8 逐个拆分为字符数组
//   split(str, anyBytes, maxCount)   anyBytes = 多个**单字节**分隔符（集合语义）
//   split(str, '<sep>', maxCount)    <> 内的整词作为分隔符（等价 splitLiteral）
//   splitLiteral(str, sep, maxCount) 按 sep 整词拆分
//   maxCount 省略或 0 = 拆分全部；指定 = 结果元素数上限，剩余部分并入最后一个元素
//
// 引擎里的字符串是 **UTF-8 字节串**（一个 wchar 存一个字节），
// 所以「逐字符」必须按 UTF-8 码点边界切，不能按 wchar 切。
//
// sepMode: 0 = 整词分隔符；1 = 单字节分隔符集合
static void stringSplitRun(const std::shared_ptr<Table>& out, const wstring& s,
    const wstring& sep, int sepMode, long long maxCount)
{
    if (sepMode == 0 && sep.empty())
    {
        // 空分隔符：官方未定义，退化为整体返回（避免死循环）
        out->arr.push_back(Value::Str_(s));
        return;
    }
    size_t pos = 0;
    for (;;)
    {
        if (maxCount > 0 && (long long)out->arr.size() + 1 >= maxCount)
        {
            // 官方：指定 maxCount 时「保留剩余」——剩余部分并入最后一个元素
            out->arr.push_back(Value::Str_(s.substr(pos)));
            return;
        }
        size_t hit = wstring::npos;
        if (sepMode == 0)
        {
            hit = s.find(sep, pos);
        }
        else
        {
            for (size_t i = pos; i < s.size(); i++)
            {
                if (sep.find(s[i]) != wstring::npos) { hit = i; break; }
            }
        }
        if (hit == wstring::npos)
        {
            out->arr.push_back(Value::Str_(s.substr(pos)));
            return;
        }
        out->arr.push_back(Value::Str_(s.substr(pos, hit - pos)));
        pos = hit + (sepMode == 0 ? sep.size() : 1);
        if (pos > s.size()) pos = s.size();
    }
}

void Interp::InitBuiltins()
        {
            // ---- global call() builtin: call(fn, owner, ...)
            // returns ok, ret (metaProperty machinery depends on it)
            globals.VarAt(NameId(L"call")) =
                NF([this](vector<Value>& a) -> Value {
                auto finish = [this](bool ok, Value r) -> Value {
                    Value vOk = Value::Bool_(ok);
                    auto extras = std::make_shared<vector<Value>>();
                    extras->push_back(r);
                    if (r.mrets)
                    {
                        for (auto& item : *r.mrets)
                            extras->push_back(item);
                    }
                    vOk.mrets = extras;
                    return vOk;
                };
                if (a.empty() || (a[0].t != Value::Func &&
                                  a[0].t != Value::Class &&
                                  a[0].t != Value::TableT))
                    return finish(false, Value());
                try
                {
                    vector<Value> args;
                    for (size_t k = 2; k < a.size(); k++)
                        args.push_back(a[k]);
                    Value r = CallOwner(a[0],
                        a.size() > 1 ? a[1] : Value(), args);
                    if (err && !err->empty())
                    {
                        wstring e = *err;
                        err->clear();
                        return finish(false, Value::Str_(e));
                    }
                    return finish(true, r);
                }
                catch (...)
                {
                    return finish(false, Value());
                }
            });

            // ---- string (1-based index) ----
            {
                auto tb = GetRoot(L"string").tab;
                // string.str(v) — kernel primitive: coerce a
                // pointer / byte buffer / number to a text value
                // (win.version csdVersion, D3D info fields etc.)
                tb->map[L"str"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty()) return Value();
                    if (a[0].t == Value::Str) return a[0];
                    return Value::Str_(BStr(a[0])); });
                tb->map[L"charAt"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty() || a[0].t == Value::Null) return Value();
                    wstring t0;
                    const wstring& s = BStrRef(a[0], t0);
                    long long pos = a.size() > 1 ? (long long)BNum(a[1]) : 1;
                    if (pos < 0) pos = (long long)s.size() + pos + 1;
                    if (pos < 1 || (size_t)pos > s.size()) return Value();
                    return Value::Str_(wstring(1, s[(size_t)(pos - 1)]));
                });
                // string.repeat(s,n) — repeat the string n times
                tb->map[L"repeat"] = NF([](vector<Value>& a) -> Value {
                    wstring t0;
                    const wstring& s = BStrRef(a[0], t0);
                    long long n = a.size() > 1 ? (long long)BNum(a[1]) : 1;
                    wstring out;
                    if (n > 0 && !s.empty())
                    {
                        // single allocation instead of log-growth reallocs
                        unsigned long long total =
                            (unsigned long long)n * s.size();
                        if (total < (unsigned long long)out.max_size())
                            out.reserve((size_t)total);
                    }
                    for (long long i = 0; i < n; i++)
                        out += s;
                    return Value::Str_(out); });
                // string.indexAny(str, bytes [, startPos]) — position (1-based,
                // in bytes) of the first byte of `str` at or after startPos
                // that also occurs in `bytes`; 0 when none matches.
                // The second argument may name a byte code numerically.
                tb->map[L"indexAny"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty())
                        return Value::Num_(0.0);
                    std::string s = WToU8(BStr(a[0]));
                    std::string set;
                    if (a.size() > 1)
                    {
                        if (a[1].t == Value::Num)
                        {
                            // a bare byte code instead of a byte string
                            long long code = (long long)BNum(a[1]);
                            if (code >= 0 && code <= 0xFF)
                                set.push_back((char)code);
                        }
                        else
                            set = WToU8(BStr(a[1]));
                    }
                    size_t start = 0;
                    if (a.size() > 2)
                    {
                        long long p = (long long)BNum(a[2]);
                        start = p < 1 ? 0 : (size_t)(p - 1);
                    }
                    if (start >= s.size())
                        return Value::Num_(0.0);
                    for (size_t i = start; i < s.size(); i++)
                    {
                        for (char c : set)
                        {
                            if (s[i] == c)
                                return Value::Num_((double)(i + 1));
                        }
                    }
                    return Value::Num_(0.0); });
                // string.concat(v [, ...]) — join the tostring of every
                // argument, ignoring nulls (the ++ operator throws on
                // null); no args or all-null returns null.
                // lasterr() builds its error text here.
                tb->map[L"concat"] = NF([](vector<Value>& a) -> Value {
                    bool bAny = false;
                    wstring out;
                    for (auto& v : a)
                    {
                        if (v.t == Value::Null) continue;
                        bAny = true;
                        // ToString() copies the whole string for Str args;
                        // append the storage directly instead
                        if (v.t == Value::Str) out += v.s;
                        else out += v.ToString();
                    }
                    return bAny ? Value::Str_(out) : Value(); });
                // string.sub(s,i,j,chars=false) / string.slice(s,i,j,chars=false)
                // official contract: 1-based index, negative = from end.
                // chars = true: count in Unicode characters (codepoints); false: count in UTF-8 bytes.
                tb->map[L"sub"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty()) return Value::Str_(L"");
                    wstring t0;
                    const wstring& s = BStrRef(a[0], t0);
                    bool bChars = false;
                    long long st = 1;
                    long long en = -1;
                    bool hasEn = false;

                    if (a.size() > 1) st = (long long)BNum(a[1]);
                    if (a.size() > 2)
                    {
                        if (a[2].t == Value::Bool)
                        {
                            bChars = a[2].b;
                        }
                        else if (a[2].t != Value::Null)
                        {
                            en = (long long)BNum(a[2]);
                            hasEn = true;
                        }
                    }
                    if (a.size() > 3)
                    {
                        if (a[3].t == Value::Bool) bChars = a[3].b;
                        else if (a[3].t == Value::Num) bChars = (a[3].n != 0);
                    }

                    if (bChars)
                    {
                        // fast path: both bounds positive → one fused walk
                        // replaces CharLenOf + two CharPosToWcharIdx passes
                        if (st >= 1 && (!hasEn || en >= 1))
                        {
                            long long enc = en;
                            if (hasEn && enc > (long long)s.size())
                                enc = (long long)s.size(); // cast guard; still > real codepoint count
                            size_t wa, wb;
                            CharTwoPosToWchar(s, (size_t)st,
                                              hasEn ? (size_t)(enc + 1)
                                                    : (size_t)-1, wa, wb);
                            if (wa > s.size()) return Value::Str_(L""); // st past codepoint count
                            if (wb <= wa) return Value::Str_(L"");      // en < st
                            if (wb > s.size() + 1) wb = s.size() + 1;
                            return Value::Str_(s.substr(wa - 1, wb - wa));
                        }
                        size_t nc = CharLenOf(s);
                        long long n = (long long)nc;
                        if (!hasEn) en = n;
                        if (st < 0) st = n + st + 1;
                        if (en < 0) en = n + en + 1;
                        if (st < 1) st = 1;
                        if (en > n) en = n;
                        if (en < st || (size_t)st > nc) return Value::Str_(L"");
                        size_t wi = CharPosToWcharIdx(s, (size_t)st);
                        size_t wj = CharPosToWcharIdx(s, (size_t)en + 1);
                        if (wj > s.size() + 1) wj = s.size() + 1;
                        if (wj <= wi) return Value::Str_(L"");
                        return Value::Str_(s.substr(wi - 1, wj - wi));
                    }
                    else
                    {
                        // fast path: both bounds positive → no negative-end
                        // normalization, so one fused walk replaces the
                        // Utf8LenOf + two Utf8PosToWcharIdx passes
                        if (st >= 1 && (!hasEn || en >= 1))
                        {
                            long long enb = en;
                            if (hasEn && enb > (long long)s.size() * 4)
                                enb = (long long)s.size() * 4; // cast guard; still > real byte len
                            size_t wa, wb;
                            Utf8TwoPosToWchar(s, (size_t)st,
                                              hasEn ? (size_t)(enb + 1)
                                                    : (size_t)-1, wa, wb);
                            if (wa > s.size()) return Value::Str_(L"");   // st past byte end
                            if (wb <= wa) return Value::Str_(L"");        // en < st or same codepoint
                            if (wb > s.size() + 1) wb = s.size() + 1;
                            return Value::Str_(s.substr(wa - 1, wb - wa));
                        }
                        size_t nb = Utf8LenOf(s);
                        long long n = (long long)nb;
                        if (!hasEn) en = n;
                        if (st < 0) st = n + st + 1;
                        if (en < 0) en = n + en + 1;
                        if (st < 1) st = 1;
                        if (en > n) en = n;
                        if (en < st || (size_t)st > nb) return Value::Str_(L"");
                        size_t wi = Utf8PosToWcharIdx(s, (size_t)st);
                        size_t wj = Utf8PosToWcharIdx(s, (size_t)en + 1);
                        if (wj > s.size() + 1) wj = s.size() + 1;
                        if (wj <= wi) return Value::Str_(L"");
                        return Value::Str_(s.substr(wi - 1, wj - wi));
                    }
                });
                tb->map[L"slice"] = tb->map[L"sub"];
                // string.reverse(s) — per Unicode codepoint: surrogate
                // pairs must stay intact (a raw wchar reverse would split
                // every astral character into lone surrogates)
                tb->map[L"reverse"] = NF([](vector<Value>& a) -> Value {
                    wstring s = BStr(a[0]);
                    wstring r;
                    r.reserve(s.size());
                    size_t i = s.size();
                    while (i > 0)
                    {
                        if (i >= 2 && s[i - 1] >= 0xDC00 && s[i - 1] <= 0xDFFF &&
                            s[i - 2] >= 0xD800 && s[i - 2] <= 0xDBFF)
                        {
                            r.push_back(s[i - 2]);
                            r.push_back(s[i - 1]);
                            i -= 2;
                        }
                        else
                        {
                            r.push_back(s[i - 1]);
                            i--;
                        }
                    }
                    return Value::Str_(r); });
                tb->map[L"len"] = NF([](vector<Value>& a) {
                    if (a.empty()) return Value::Num_(0);
                    wstring t0;
                    const wstring& s = BStrRef(a[0], t0);
                    bool bChars = true;
                    if (a.size() > 1)
                    {
                        if (a[1].t == Value::Bool) bChars = a[1].b;
                        else if (a[1].t == Value::Num) bChars = (a[1].n != 0);
                    }
                    if (bChars)
                        return Value::Num_((double)CharLenOf(s));
                    // byte mode: short strings arithmetic, long strings
                    // SIMD kernel (same hybrid as the # operator)
                    if (s.size() <= 1024)
                        return Value::Num_((double)Utf8LenOf(s));
                    {
                        int cb = WideCharToMultiByte(CP_UTF8, 0, s.c_str(),
                                                     (int)s.size(), nullptr, 0,
                                                     nullptr, nullptr);
                        return Value::Num_((double)max(cb, 0));
                    } });
                // string.charCodeAt(str, pos = 1) -> unicode code point
                tb->map[L"charCodeAt"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty() || a[0].t == Value::Null) return Value();
                    wstring t0;
                    const wstring& s = BStrRef(a[0], t0);
                    long long pos = a.size() > 1 ? (long long)BNum(a[1]) : 1;
                    if (pos < 1) return Value();
                    long long curChar = 1;
                    for (size_t i = 0; i < s.size(); i++)
                    {
                        wchar_t c = s[i];
                        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF)
                        {
                            if (curChar == pos)
                            {
                                unsigned int cp = 0x10000 + (((unsigned int)c - 0xD800) << 10) + ((unsigned int)s[i + 1] - 0xDC00);
                                return Value::Num_((double)cp);
                            }
                            i++;
                        }
                        else
                        {
                            if (curChar == pos)
                            {
                                return Value::Num_((double)(unsigned short)c);
                            }
                        }
                        curChar++;
                    }
                    return Value();
                });
                tb->map[L"left"] = NF([](vector<Value>& a) {
                    if (a.empty()) return Value::Str_(L"");
                    wstring t0;
                    const wstring& s = BStrRef(a[0], t0);
                    double nK = a.size() > 1 ? BNum(a[1]) : 1;
                    bool bChars = false;
                    if (a.size() > 2)
                    {
                        if (a[2].t == Value::Bool) bChars = a[2].b;
                        else if (a[2].t == Value::Num) bChars = (a[2].n != 0);
                    }
                    if (bChars)
                    {
                        // fast path: no CharLenOf pass needed — "c2 not
                        // found by the fused walk" is exactly k >= count
                        if (nK >= 0)
                        {
                            size_t k = (size_t)nK;
                            size_t wa, wb;
                            CharTwoPosToWchar(s, 1, k + 1, wa, wb);
                            if (wb > s.size()) return Value::Str_(s);
                            return Value::Str_(s.substr(0, wb - 1));
                        }
                        size_t nc = CharLenOf(s);
                        long long drop = (long long)(-nK) - 1;
                        if (drop < 0) drop = 0;
                        if ((size_t)drop >= nc) return Value::Str_(L"");
                        size_t wj = CharPosToWcharIdx(s, nc - (size_t)drop + 1);
                        return Value::Str_(s.substr(0, wj - 1));
                    }
                    else
                    {
                        // fast path: no Utf8LenOf pass needed — "b2 not
                        // found by the fused walk" is exactly k >= byte len
                        if (nK >= 0)
                        {
                            size_t k = (size_t)nK;
                            size_t wa, wb;
                            Utf8TwoPosToWchar(s, 1, k + 1, wa, wb);
                            if (wb > s.size()) return Value::Str_(s);
                            return Value::Str_(s.substr(0, wb - 1));
                        }
                        size_t nb = Utf8LenOf(s);
                        long long drop = (long long)(-nK) - 1;
                        if (drop < 0) drop = 0;
                        if ((size_t)drop >= nb) return Value::Str_(L"");
                        size_t wj = Utf8PosToWcharIdx(s, nb - (size_t)drop + 1);
                        return Value::Str_(s.substr(0, wj - 1));
                    }
                });
                tb->map[L"right"] = NF([](vector<Value>& a) {
                    if (a.empty()) return Value::Str_(L"");
                    wstring t0;
                    const wstring& s = BStrRef(a[0], t0);
                    double nK = a.size() > 1 ? BNum(a[1]) : 1;
                    bool bChars = false;
                    if (a.size() > 2)
                    {
                        if (a[2].t == Value::Bool) bChars = a[2].b;
                        else if (a[2].t == Value::Num) bChars = (a[2].n != 0);
                    }
                    if (bChars)
                    {
                        size_t nc = CharLenOf(s);
                        if (nK >= 0)
                        {
                            size_t k = (size_t)nK;
                            if (k >= nc) return Value::Str_(s);
                            size_t wi = CharPosToWcharIdx(s, nc - k + 1);
                            return Value::Str_(s.substr(wi - 1));
                        }
                        long long start = (long long)(-nK) - 1;
                        if (start < 0) start = 0;
                        if ((size_t)start >= nc) return Value::Str_(L"");
                        size_t wi = CharPosToWcharIdx(s, (size_t)start + 1);
                        return Value::Str_(s.substr(wi - 1));
                    }
                    else
                    {
                        // reverse walk: accumulate byte counts from the tail
                        // until k bytes are covered — replaces Utf8LenOf +
                        // Utf8PosToWcharIdx (2 full passes) with an O(k)
                        // walk, and "never reaches k" means k >= byte len.
                        if (nK >= 0)
                        {
                            size_t k = (size_t)nK;
                            if (k == 0) return Value::Str_(L"");
                            size_t acc = 0;
                            size_t i = s.size();
                            while (i > 0)
                            {
                                size_t wstart = i - 1;
                                wchar_t c = s[wstart];
                                size_t n;
                                if (c < 0x80) n = 1;
                                else if (c < 0x800) n = 2;
                                else n = 3;
                                // scanning backwards, a pair is detected by
                                // the LOW surrogate looking at its HIGH
                                // predecessor (mirror of the forward walk)
                                if (c >= 0xDC00 && c <= 0xDFFF && wstart > 0 &&
                                    s[wstart - 1] >= 0xD800 &&
                                    s[wstart - 1] <= 0xDBFF)
                                { n = 4; wstart--; }
                                acc += n;
                                if (acc >= k)
                                    return Value::Str_(s.substr(wstart));
                                i = wstart;
                            }
                            return Value::Str_(s);
                        }
                        // negative: drop |k|-1 bytes from the head; the
                        // walk's past-the-end outcome (wi > size) is exactly
                        // start >= byte length, so no Utf8LenOf pass either
                        long long start = (long long)(-nK) - 1;
                        if (start < 0) start = 0;
                        size_t wi = Utf8PosToWcharIdx(s, (size_t)start + 1);
                        if (wi > s.size()) return Value::Str_(L"");
                        return Value::Str_(s.substr(wi - 1));
                    }
                });
                tb->map[L"mid"] = NF([](vector<Value>& a) {
                    if (a.empty()) return Value::Str_(L"");
                    wstring t0;
                    const wstring& s = BStrRef(a[0], t0);
                    long long st = (long long)BNum(a[1]); if (st < 1) st = 1;
                    long long ct = a.size() > 2 ? (long long)BNum(a[2]) : -1;
                    // fused single walk replaces Utf8LenOf + two
                    // Utf8PosToWcharIdx passes (mid has no negative bounds)
                    long long ub = (long long)s.size() * 4;
                    if (st > ub) st = ub + 1; // beyond any byte length
                    size_t b2;
                    if (ct < 0 || ct > ub - st)
                        b2 = (size_t)-1; // to the end of the string
                    else
                        b2 = (size_t)(st + ct);
                    size_t wa, wb;
                    Utf8TwoPosToWchar(s, (size_t)st, b2, wa, wb);
                    if (wa > s.size()) return Value::Str_(L"");
                    if (wb <= wa) return Value::Str_(L"");
                    if (wb > s.size() + 1) wb = s.size() + 1;
                    return Value::Str_(s.substr(wa - 1, wb - wa)); });
                tb->map[L"upper"] = NF([](vector<Value>& a) {
                    wstring s = BStr(a[0]);
                    for (auto& c : s) c = (wchar_t)towupper(c);
                    return Value::Str_(s); });
                tb->map[L"lower"] = NF([](vector<Value>& a) {
                    wstring s = BStr(a[0]);
                    for (auto& c : s) c = (wchar_t)towlower(c);
                    return Value::Str_(s); });
                tb->map[L"trim"] = NF([](vector<Value>& a) {
                    wstring s = BStr(a[0]);
                    size_t b = s.find_first_not_of(L" \t\r\n");
                    if (b == wstring::npos) return Value::Str_(L"");
                    size_t e = s.find_last_not_of(L" \t\r\n");
                    return Value::Str_(s.substr(b, e - b + 1)); });
                // string.trimLeft / trimRight(s, set?): strip leading /
                // trailing chars from `set`; the default set is whitespace.
                // The library depends on the default (py2 execUtf8 chain)
                // and on set forms like "0", "\\0", ": ", "\\/", "/".
                wstring defTrimSet = L" \t\r\n\v\f";
                tb->map[L"trimLeft"] = NF([defTrimSet](vector<Value>& a)
                    -> Value {
                    wstring s = BStr(a[0]);
                    wstring set = a.size() > 1 && a[1].t != Value::Null
                        ? BStr(a[1]) : defTrimSet;
                    size_t b = s.find_first_not_of(set);
                    if (b == wstring::npos) return Value::Str_(L"");
                    return Value::Str_(s.substr(b)); });
                tb->map[L"trimRight"] = NF([defTrimSet](vector<Value>& a)
                    -> Value {
                    wstring s = BStr(a[0]);
                    wstring set = a.size() > 1 && a[1].t != Value::Null
                        ? BStr(a[1]) : defTrimSet;
                    if (s.empty()) return Value::Str_(L"");
                    size_t e = s.find_last_not_of(set);
                    if (e == wstring::npos) return Value::Str_(L"");
                    return Value::Str_(s.substr(0, e + 1)); });
                // string.escape(str, jsonMode, unicodeEscape)
                tb->map[L"escape"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty() || a[0].t == Value::Null) return Value();
                    wstring s = BStr(a[0]);
                    bool bJson = a.size() > 1 && a[1].t == Value::Bool && a[1].b;
                    bool bUni = a.size() > 2 && a[2].t == Value::Bool && a[2].b;
                    bool bNeeded = false;
                    wstring out;
                    out.reserve(s.size() + 16);
                    for (size_t i = 0; i < s.size(); i++)
                    {
                        wchar_t c = s[i];
                        if (c == L'\"') { out += L"\\\""; bNeeded = true; }
                        else if (c == L'\\') { out += L"\\\\"; bNeeded = true; }
                        else if (c == L'\b') { out += L"\\b"; bNeeded = true; }
                        else if (c == L'\f') { out += L"\\f"; bNeeded = true; }
                        else if (c == L'\n') { out += L"\\n"; bNeeded = true; }
                        else if (c == L'\r') { out += L"\\r"; bNeeded = true; }
                        else if (c == L'\t') { out += L"\\t"; bNeeded = true; }
                        else if (c < 0x20 || c == 0x7F)
                        {
                            bNeeded = true;
                            wchar_t buf[8];
                            swprintf_s(buf, L"\\u%04x", (unsigned int)c);
                            out += buf;
                        }
                        else if (bUni && c > 0x7F)
                        {
                            bNeeded = true;
                            wchar_t buf[8];
                            swprintf_s(buf, L"\\u%04x", (unsigned int)c);
                            out += buf;
                        }
                        else
                        {
                            if (!bJson && c == L'\'') bNeeded = true;
                            out += c;
                        }
                    }
                    if (!bJson && !bNeeded) return Value();
                    return Value::Str_(out);
                });

                // string.unescape(str)
                tb->map[L"unescape"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty() || a[0].t == Value::Null) return Value::Str_(L"");
                    wstring s = BStr(a[0]);
                    wstring out;
                    out.reserve(s.size());
                    for (size_t i = 0; i < s.size(); i++)
                    {
                        if (s[i] == L'\\' && i + 1 < s.size())
                        {
                            wchar_t nxt = s[++i];
                            if (nxt == L'\"') out += L'\"';
                            else if (nxt == L'\'') out += L'\'';
                            else if (nxt == L'\\') out += L'\\';
                            else if (nxt == L'/') out += L'/';
                            else if (nxt == L'b') out += L'\b';
                            else if (nxt == L'f') out += L'\f';
                            else if (nxt == L'n') out += L'\n';
                            else if (nxt == L'r') out += L'\r';
                            else if (nxt == L't') out += L'\t';
                            else if (nxt == L'0') out += L'\0';
                            else if ((nxt == L'u' || nxt == L'U') && i + 4 <= s.size())
                            {
                                int hexLen = (nxt == L'U' && i + 8 <= s.size()) ? 8 : 4;
                                wstring hexStr = s.substr(i + 1, hexLen);
                                wchar_t* endp = nullptr;
                                unsigned long code = wcstoul(hexStr.c_str(), &endp, 16);
                                if (endp && endp != hexStr.c_str())
                                {
                                    i += (size_t)(endp - hexStr.c_str());
                                    if (code <= 0xFFFF)
                                    {
                                        out += (wchar_t)code;
                                    }
                                    else if (code <= 0x10FFFF)
                                    {
                                        code -= 0x10000;
                                        out += (wchar_t)(0xD800 + (code >> 10));
                                        out += (wchar_t)(0xDC00 + (code & 0x3FF));
                                    }
                                }
                                else out += nxt;
                            }
                            else if (nxt == L'x' && i + 2 <= s.size())
                            {
                                wstring hexStr = s.substr(i + 1, 2);
                                wchar_t* endp = nullptr;
                                unsigned long code = wcstoul(hexStr.c_str(), &endp, 16);
                                if (endp && endp != hexStr.c_str())
                                {
                                    i += (size_t)(endp - hexStr.c_str());
                                    out += (wchar_t)code;
                                }
                                else out += nxt;
                            }
                            else
                            {
                                out += nxt;
                            }
                        }
                        else
                        {
                            out += s[i];
                        }
                    }
                    return Value::Str_(out);
                });
                // string.crc32(s) — kernel primitive (official engine):
                // CRC-32 IEEE over the string's bytes, returned as an
                // unsigned 32-bit number. fsys.update/io.tmpName and ~90
                // examples depend on it.
                tb->map[L"crc32"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty() || a[0].t != Value::Str)
                        return Value::Num_(0);
                    std::string bytes = BUtf8(a[0]);
                    unsigned int crc = 0xFFFFFFFFu;
                    for (unsigned char c : bytes)
                    {
                        crc ^= c;
                        for (int k = 0; k < 8; k++)
                            crc = (crc >> 1) ^
                                  (0xEDB88320u & (0u - (crc & 1u)));
                    }
                    return Value::Num_((double)(crc ^ 0xFFFFFFFFu));
                });
                // string.lastIndexOf(s, sub, from?) — last 1-based UTF-8
                // BYTE position or null; plain search (no pattern).
                // `from` is a 1-based inclusive byte position bounding the
                // match start (matches starting after it are ignored);
                // out-of-range `from` clamps to the string end.
                tb->map[L"lastIndexOf"] =
                    NF([](vector<Value>& a) -> Value {
                    wstring t0, t1;
                    const wstring& s = BStrRef(a[0], t0);
                    const wstring& sub = BStrRef(a[1], t1);
                    size_t maxStartW = wstring::npos;
                    if (a.size() > 2 && a[2].t != Value::Null)
                    {
                        size_t from = (size_t)BNum(a[2]);
                        if (from < 1) from = 1;
                        if (from > Utf8LenOf(s) + 1)
                            from = Utf8LenOf(s) + 1;
                        maxStartW = Utf8PosToWcharIdx(s, from) - 1;
                    }
                    size_t pos = maxStartW == wstring::npos
                        ? s.rfind(sub)
                        : s.rfind(sub, maxStartW);
                    if (pos == wstring::npos) return Value();
                    return Value::Num_((double)WcharToUtf8Pos(s, pos + 1)); });
                // string.startsWith / endsWith(s, prefix/suffix, ci?)
                tb->map[L"startsWith"] =
                    NF([](vector<Value>& a) -> Value {
                    wstring s = BStr(a[0]), p = BStr(a[1]);
                    if (a.size() > 2 && BNum(a[2]))
                    {
                        wstring l = s, r = p;
                        for (auto& c : l) c = (wchar_t)towlower(c);
                        for (auto& c : r) c = (wchar_t)towlower(c);
                        return Value::Bool_(
                            l.size() >= r.size() &&
                            l.compare(0, r.size(), r) == 0);
                    }
                    return Value::Bool_(s.size() >= p.size() &&
                                        s.compare(0, p.size(), p) == 0);
                });
                tb->map[L"endsWith"] =
                    NF([](vector<Value>& a) -> Value {
                    wstring s = BStr(a[0]), p = BStr(a[1]);
                    if (a.size() > 2 && BNum(a[2]))
                    {
                        wstring l = s, r = p;
                        for (auto& c : l) c = (wchar_t)towlower(c);
                        for (auto& c : r) c = (wchar_t)towlower(c);
                        return Value::Bool_(
                            l.size() >= r.size() &&
                            l.compare(l.size() - r.size(), r.size(), r)
                                == 0);
                    }
                    return Value::Bool_(s.size() >= p.size() &&
                        s.compare(s.size() - p.size(), p.size(), p) == 0);
                });
                // string.cmp(a,b) — 0 when equal (the lib's cmpMatch
                // contract); otherwise -1/1. Case-sensitive; the lib's
                // own cmpMatch lowercases before calling.
                tb->map[L"cmp"] = NF([](vector<Value>& a) -> Value {
                    wstring x = BStr(a[0]), y = BStr(a[1]);
                    if (x == y) return Value::Num_(0);
                    return Value::Num_(x < y ? -1 : 1); });
                // string.join(t, delim?) — concatenate array members
                tb->map[L"join"] = NF([](vector<Value>& a) -> Value {
                    wstring d = a.size() > 1 ? BStr(a[1]) : L"";
                    wstring out;
                    if (a.empty()) return Value::Str_(L"");
                    auto addV = [&](const Value& v) {
                        if (!out.empty()) out += d;
                        out += BStr(v); };
                    if (a[0].t == Value::TableT && a[0].tab)
                    {
                        for (auto& v : a[0].tab->arr) addV(v);
                    }
                    else for (auto& v : a) addV(v);
                    return Value::Str_(out); });
                // string.random(n, set?) — n chars from the set
                // (default alphanumerics)
                tb->map[L"random"] = NF([](vector<Value>& a) -> Value {
                    static const wchar_t* defSet =
                        L"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                        L"abcdefghijklmnopqrstuvwxyz";
                    wstring set = a.size() > 1 && a[1].t != Value::Null
                        ? BStr(a[1]) : defSet;
                    long long n = a.empty() ? 1 : (long long)BNum(a[0]);
                    if (n < 0) n = 0;
                    wstring out;
                    for (long long i = 0; i < n; i++)
                        out += set[rand() % set.size()];
                    return Value::Str_(out); });
                // string.gmatch(s, pat) — iterator over pattern matches
                // (each iteration: the match, then capture groups)
                tb->map[L"gmatch"] = NF([this](vector<Value>& a)
                    -> Value {
                    wstring s = a.empty() ? L"" : BStr(a[0]);
                    wstring pat = a.size() > 1 ? BStr(a[1]) : L".";
                    size_t* pPos = new size_t(0);
                    return Value::Native_([this, s, pat, pPos](
                        vector<Value>&) mutable -> Value {
                        vector<vector<RE>> br; bool anchored = false;
                        int nGroups = 0; wstring perr;
                        if (!RECompile(pat, br, &anchored, &nGroups,
                                       &perr))
                        {
                            delete pPos;
                            return Value::Native_([](vector<Value>&) {
                                return Value(); });
                        }
                        vector<wstring> caps;
                        size_t end = 0;
                        while (*pPos <= s.size())
                        {
                            size_t p0 = *pPos;
                            if (RETry(br, s, p0, false, caps, &end))
                            {
                                if (end == p0) (*pPos)++;
                                else *pPos = end;
                                auto outs = std::make_shared
                                    <vector<Value>>();
                                if (nGroups == 0)
                                    outs->push_back(Value::Str_(
                                        s.substr(p0, end - p0)));
                                else
                                {
                                    for (size_t g = 1; g <= (size_t)nGroups; g++)
                                        outs->push_back(Value::Str_(
                                            g < caps.size() ? caps[g] : L""));
                                }
                                // the primary return must be non-null or
                                // the for-in loop reads it as termination;
                                // match string first, captures as extras
                                Value f = outs->front();
                                if (outs->size() > 1)
                                {
                                    outs->erase(outs->begin());
                                    f.mrets = outs;
                                }
                                return f;
                            }
                            if (anchored) break;
                            (*pPos)++;
                        }
                        delete pPos;
                        return Value(); });
                });
                // encoding-flag layer: the engine normalizes all strings
                // to wide text, so the official per-string flag collapses.
                // getUtf reports 8 (UTF-8) — or 16 for even-length byte
                // pairs containing a NUL, which is how toUtf16 output
                // looks; setUtf is a no-op returning the string.
                auto strLooksUtf16 = [](const wstring& s) -> bool {
                    if (s.size() < 2 || s.size() % 2) return false;
                    for (size_t i = 1; i < s.size(); i += 2)
                        if (s[i] == 0) return true;
                    return false; };
                tb->map[L"isUtf16"] =
                    NF([strLooksUtf16](vector<Value>& a) -> Value {
                    if (a.empty() || a[0].t != Value::Str)
                        return Value();
                    return Value::Bool_(strLooksUtf16(a[0].s)); });
                tb->map[L"getUtf"] =
                    NF([strLooksUtf16](vector<Value>& a) -> Value {
                    if (a.empty() || a[0].t != Value::Str ||
                        a[0].s.empty())
                        return Value();
                    return Value::Num_(strLooksUtf16(a[0].s) ? 16 : 8);
                });
                tb->map[L"setUtf"] = NF([](vector<Value>& a) -> Value {
                    return a.empty() ? Value() : a[0]; });
                tb->map[L"index"] = NF([](vector<Value>& a) {
                    // official contract: indexOf(s, sub, from?) — 1-based
                    // UTF-8 BYTE position or null; plain search (no
                    // pattern). `from` is a 1-based inclusive byte
                    // position, consistent with string.find/match and the
                    // byte mode of string.sub/left/right.
                    wstring t0, t1;
                    const wstring& s = BStrRef(a[0], t0);
                    const wstring& sub = BStrRef(a[1], t1);
                    size_t from = a.size() > 2 ? (size_t)BNum(a[2]) : 1;
                    // skip the O(n) bounds/convert walks entirely for the
                    // default from==1 shape (the common hot path)
                    size_t startW = 0;
                    if (from > 1)
                    {
                        if (from > Utf8LenOf(s) + 1) return Value();
                        startW = Utf8PosToWcharIdx(s, from) - 1;
                    }
                    size_t pos = s.find(sub, startW);
                    if (pos == wstring::npos) return Value();
                    return Value::Num_((double)WcharToUtf8Pos(s, pos + 1)); });
                tb->map[L"indexOf"] = tb->map[L"index"];
                tb->map[L"find"] = NF([](vector<Value>& a) {
                    // official contract: find(s,p,init) -> start,end,
                    // capture1..N as multiple returns (`matchStart,
                    // matchEnd = string.find(...)` in the shipped lib);
                    // no match -> null. Positions are UTF-8 BYTES.
                    wstring t0, t1;
                    const wstring& s = BStrRef(a[0], t0);
                    const wstring& pat = BStrRef(a[1], t1);
                    size_t startByte = a.size() > 2 ? (size_t)BNum(a[2]) : 1;
                    // skip the O(n) bounds/convert walks for from==1
                    size_t start = 1;
                    if (startByte > 1)
                    {
                        if (startByte > Utf8LenOf(s) + 1) return Value();
                        start = Utf8PosToWcharIdx(s, startByte);
                    }
                    vector<vector<RE>> br; bool anchored = false;
                    int nGroups = 0; wstring perr;
                    if (!RECompile(pat, br, &anchored, &nGroups, &perr))
                        return Value();
                    // literal fast path: no metacharacters → plain
                    // substring search (memchr speed); the per-position
                    // matcher measured ~24ns/position (33k-char tail
                    // scan: 795us/call)
                    if (nGroups == 0 && !anchored && REIsPlainLiteral(pat))
                    {
                        size_t pos = s.find(pat, start - 1);
                        if (pos == wstring::npos) return Value();
                        Value first = Value::Num_(
                            (double)WcharToUtf8Pos(s, pos + 1));
                        auto outs = std::make_shared<vector<Value>>();
                        outs->push_back(Value::Num_((double)(
                            WcharToUtf8Pos(s, pos + 1)
                                + Utf8ByteLenOf(pat) - 1)));
                        first.mrets = outs;
                        return first;
                    }
                    vector<wstring> caps; size_t end = 0;
                    for (size_t p0 = start - 1; p0 <= s.size(); p0++)
                    {
                        if (RETry(br, s, p0, false, caps, &end))
                        {
                            Value first = Value::Num_(
                                (double)WcharToUtf8Pos(s, p0 + 1));
                            auto outs = std::make_shared<vector<Value>>();
                            outs->push_back(Value::Num_(
                                (double)WcharToUtf8Pos(s, end + 1) - 1));
                            for (size_t g = 1;
                                 g <= (size_t)nGroups && g < caps.size();
                                 g++)
                                outs->push_back(Value::Str_(caps[g]));
                            first.mrets = outs;
                            return first;
                        }
                        if (anchored) break;
                    }
                    return Value(); });
                tb->map[L"match"] = NF([](vector<Value>& a) {
                    // official contract: match(s,p) returns the match (or
                    // capture 1) as the primary return and further
                    // captures as extra returns — `fence,lang,title =
                    // string.match(...)`; no match -> null.
                    // The @pos argument is a UTF-8 byte position.
                    wstring t0, t1;
                    const wstring& s = BStrRef(a[0], t0);
                    const wstring& pat = BStrRef(a[1], t1);
                    size_t startByte = a.size() > 2 ? (size_t)BNum(a[2]) : 1;
                    // skip the O(n) bounds/convert walks for from==1
                    size_t start = 1;
                    if (startByte > 1)
                    {
                        if (startByte > Utf8LenOf(s) + 1) return Value();
                        start = Utf8PosToWcharIdx(s, startByte);
                    }
                    vector<vector<RE>> br; bool anchored = false;
                    int nGroups = 0; wstring perr;
                    if (!RECompile(pat, br, &anchored, &nGroups, &perr))
                        return Value();
                    // literal fast path (see string.find): plain substring
                    // match, zero position conversions
                    if (nGroups == 0 && !anchored && REIsPlainLiteral(pat))
                    {
                        size_t pos = s.find(pat, start - 1);
                        if (pos == wstring::npos) return Value();
                        return Value::Str_(s.substr(pos, pat.size()));
                    }
                    vector<wstring> caps; size_t end = 0;
                    for (size_t p0 = start - 1; p0 <= s.size(); p0++)
                    {
                        if (RETry(br, s, p0, false, caps, &end))
                        {
                            Value first;
                            auto outs = std::make_shared<vector<Value>>();
                            if (nGroups == 0)
                                first = Value::Str_(
                                    s.substr(p0, end - p0));
                            else
                            {
                                for (size_t g = 1;
                                     g <= (size_t)nGroups; g++)
                                    outs->push_back(Value::Str_(
                                        g < caps.size() ? caps[g]
                                                        : L""));
                                first = outs->front();
                                outs->erase(outs->begin());
                            }
                            if (!outs->empty()) first.mrets = outs;
                            return first;
                        }
                        if (anchored) break;
                    }
                    return Value(); });
                auto doReplace = [this](vector<Value>& a) -> Value {
                    if (a.empty()) return Value::Str_(L"");
                    wstring s = BStr(a[0]);
                    if (a.size() < 2)
                    {
                        Value vOut = Value::Str_(s);
                        auto ex = std::make_shared<vector<Value>>();
                        ex->push_back(Value::Num_(0));
                        vOut.mrets = ex;
                        return vOut;
                    }
                    wstring pat = BStr(a[1]);
                    if (pat.empty())
                    {
                        Value vOut = Value::Str_(s);
                        auto ex = std::make_shared<vector<Value>>();
                        ex->push_back(Value::Num_(0));
                        vOut.mrets = ex;
                        return vOut;
                    }
                    Value repl = a.size() > 2 ? a[2] : Value();
                    int limit = a.size() > 3 && a[3].t == Value::Num ? (int)a[3].n : -1;
                    if (limit == 0) limit = -1;

                    bool bCallable = (repl.t == Value::Func || repl.t == Value::Class);
                    if (repl.t == Value::TableT && repl.tab)
                    {
                        if (repl.tab->map.count(L"_call") || repl.tab->map.count(L"_meta"))
                            bCallable = true;
                    }

                    bool bLiteral = false;
                    wstring litPat = pat;
                    if (!pat.empty() && pat[0] == L'@')
                    {
                        bLiteral = true;
                        litPat = pat.substr(1);
                    }

                    wstring out;
                    long long count = 0;

                    if (bLiteral)
                    {
                        if (litPat.empty())
                        {
                            Value vOut = Value::Str_(s);
                            auto ex = std::make_shared<vector<Value>>();
                            ex->push_back(Value::Num_(0));
                            vOut.mrets = ex;
                            return vOut;
                        }
                        size_t pos = 0, cp = 0;
                        while ((pos = s.find(litPat, cp)) != wstring::npos && (limit < 0 || count < limit))
                        {
                            out += s.substr(cp, pos - cp);
                            wstring matched = litPat;
                            wstring rStr;
                            if (bCallable)
                            {
                                vector<Value> args{ Value::Str_(matched) };
                                Value rVal = this->CallOwner(repl, Value(), args);
                                if (rVal.t == Value::Null) rStr = matched;
                                else rStr = rVal.ToString();
                            }
                            else if (repl.t == Value::TableT && repl.tab)
                            {
                                auto itM = repl.tab->map.find(matched);
                                if (itM != repl.tab->map.end()) rStr = itM->second.ToString();
                                else rStr = matched;
                            }
                            else
                            {
                                rStr = repl.ToString();
                            }
                            out += rStr;
                            count++;
                            cp = pos + litPat.size();
                        }
                        out += s.substr(cp);
                    }
                    else
                    {
                        vector<vector<RE>> br; bool anchored = false;
                        int nGroups = 0; wstring perr;
                        if (!RECompile(pat, br, &anchored, &nGroups, &perr))
                        {
                            // regex compilation error: fallback to literal match
                            size_t pos = 0, cp = 0;
                            while ((pos = s.find(pat, cp)) != wstring::npos && (limit < 0 || count < limit))
                            {
                                out += s.substr(cp, pos - cp);
                                out += repl.ToString();
                                count++;
                                cp = pos + pat.size();
                            }
                            out += s.substr(cp);
                        }
                        else
                        {
                            size_t cp = 0, p0 = 0;
                            while (p0 <= s.size() && (limit < 0 || count < limit))
                            {
                                vector<wstring> caps; size_t end = 0;
                                if (RETry(br, s, p0, false, caps, &end))
                                {
                                    out += s.substr(cp, p0 - cp);
                                    wstring matched = s.substr(p0, end - p0);
                                    wstring rStr;
                                    if (bCallable)
                                    {
                                        vector<Value> args;
                                        if (nGroups > 0)
                                        {
                                            for (size_t g = 1; g <= (size_t)nGroups; g++)
                                                args.push_back(Value::Str_(g < caps.size() ? caps[g] : L""));
                                        }
                                        else
                                        {
                                            args.push_back(Value::Str_(matched));
                                        }
                                        Value rVal = this->CallOwner(repl, Value(), args);
                                        if (rVal.t == Value::Null) rStr = matched;
                                        else rStr = rVal.ToString();
                                    }
                                    else if (repl.t == Value::TableT && repl.tab)
                                    {
                                        wstring key = (nGroups > 0 && caps.size() > 1) ? caps[1] : matched;
                                        auto itM = repl.tab->map.find(key);
                                        if (itM != repl.tab->map.end()) rStr = itM->second.ToString();
                                        else rStr = matched;
                                    }
                                    else
                                    {
                                        // String template expansion (\0..\9, $0..$9)
                                        wstring tpl = repl.ToString();
                                        for (size_t r = 0; r < tpl.size(); r++)
                                        {
                                            if ((tpl[r] == L'$' || tpl[r] == L'\\') &&
                                                r + 1 < tpl.size() && iswdigit(tpl[r + 1]))
                                            {
                                                int g = tpl[r + 1] - L'0';
                                                if (g == 0) out += matched;
                                                else if ((size_t)g < caps.size()) out += caps[(size_t)g];
                                                r++;
                                            }
                                            else
                                                out += tpl[r];
                                        }
                                        goto replaced_str;
                                    }
                                    out += rStr;
                                replaced_str:
                                    count++;
                                    if (end == p0)
                                    {
                                        if (p0 < s.size()) out += s[p0];
                                        p0++;
                                    }
                                    else
                                        p0 = end;
                                    cp = p0;
                                    if (anchored) break;
                                    continue;
                                }
                                if (p0 >= s.size() || anchored) break;
                                p0++;
                            }
                            out += s.substr(cp);
                        }
                    }

                    Value vOut = Value::Str_(out);
                    auto ex = std::make_shared<vector<Value>>();
                    ex->push_back(Value::Num_((double)count));
                    vOut.mrets = ex;
                    return vOut;
                };
                tb->map[L"gsub"] = NF(doReplace);
                tb->map[L"replace"] = NF(doReplace);
                tb->map[L"replaceLiteral"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty()) return Value::Str_(L"");
                    wstring s = BStr(a[0]);
                    if (a.size() < 2) return Value::Str_(s);

                    auto replaceOnce = [](const wstring& src, const wstring& f, const wstring& r, int limit, int& totalCount) -> wstring {
                        if (limit == 0) return src;
                        if (f.empty()) {
                            wstring out;
                            out += r;
                            for (size_t i = 0; i < src.size(); i++) {
                                out += src[i];
                                out += r;
                                totalCount++;
                                if (limit > 0 && totalCount >= limit) {
                                    out += src.substr(i + 1);
                                    break;
                                }
                            }
                            return out;
                        }
                        wstring out;
                        size_t pos = 0;
                        int c = 0;
                        while (pos < src.size()) {
                            if (limit > 0 && c >= limit) {
                                out += src.substr(pos);
                                break;
                            }
                            size_t found = src.find(f, pos);
                            if (found == wstring::npos) {
                                out += src.substr(pos);
                                break;
                            }
                            out += src.substr(pos, found - pos);
                            out += r;
                            pos = found + f.size();
                            c++;
                        }
                        totalCount += c;
                        return out;
                    };

                    int count = 0;
                    if (a[1].t == Value::TableT && a[1].tab) {
                        int tableLimit = (a.size() > 2 && a[2].t == Value::Num) ? (int)a[2].n : -1;
                        if (a.size() > 2 && tableLimit <= 0) {
                            Value vOut = Value::Str_(s);
                            auto ex = std::make_shared<vector<Value>>();
                            ex->push_back(Value::Num_(0));
                            vOut.mrets = ex;
                            return vOut;
                        }
                        wstring cur = s;
                        for (auto& kv : a[1].tab->map) {
                            int subCount = 0;
                            cur = replaceOnce(cur, kv.first, BStr(kv.second), tableLimit, subCount);
                            count += subCount;
                        }
                        Value vOut = Value::Str_(cur);
                        auto ex = std::make_shared<vector<Value>>();
                        ex->push_back(Value::Num_((double)count));
                        vOut.mrets = ex;
                        return vOut;
                    }

                    int limit = (a.size() > 3 && a[3].t == Value::Num) ? (int)a[3].n : -1;
                    if (a.size() > 3 && limit <= 0) {
                        Value vOut = Value::Str_(s);
                        auto ex = std::make_shared<vector<Value>>();
                        ex->push_back(Value::Num_(0));
                        vOut.mrets = ex;
                        return vOut;
                    }

                    wstring f = BStr(a[1]);
                    wstring r = a.size() > 2 ? BStr(a[2]) : L"";
                    wstring out = replaceOnce(s, f, r, limit, count);
                    Value vOut = Value::Str_(out);
                    auto ex = std::make_shared<vector<Value>>();
                    ex->push_back(Value::Num_((double)count));
                    vOut.mrets = ex;
                    return vOut;
                });
                // string.split / string.splitLiteral
                //
                // 官方契约（lib/builtin/string.aardio 声明 + 官方库调用形态）：
                //   split(str)                       -> 按 UTF-8 逐个拆分为字符数组
                //   split(str, anyBytes, maxCount)   -> anyBytes = 多个**单字节**分隔符
                //   split(str, '<sep>', maxCount)    -> <> 内的整词作为分隔符
                //   splitLiteral(str, sep, maxCount) -> 按 sep 整词拆分
                //   maxCount 省略或 0 = 拆分全部；指定 = 结果元素数上限，剩余并入最后一项
                //
                // 旧实现把第 2 参当成多字节字面量（官方语义是「单字节集合」），
                // 1 参更是默认按 "," 拆 —— 官方库里 `string.split(s)`（逐字符）、
                // `string.split(name,'/\\')`（'/' 或 '\'）这类调用全是坏的。
                tb->map[L"split"] = NF([](vector<Value>& a) -> Value {
                    wstring s = a.empty() ? wstring() : BStr(a[0]);
                    auto tb2 = std::make_shared<Table>();
                    if (s.empty()) return Value::Table_(tb2); // 空串 -> 长度 0 的数组
                    // 第 2 参省略或为 null -> 逐字符拆分（Unicode 码点，
                    // 引擎字符串为 UTF-16：代理对保持完整，不能按 UTF-8
                    // 首字节猜长度 —— 旧逻辑会把 π/ÿ/Emoji 错拆）
                    if (a.size() < 2 || a[1].t == Value::Null)
                    {
                        size_t i = 0;
                        while (i < s.size())
                        {
                            size_t n = 1;
                            wchar_t c = s[i];
                            if (c >= 0xD800 && c <= 0xDBFF &&
                                i + 1 < s.size() &&
                                s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF)
                                n = 2;
                            tb2->arr.push_back(Value::Str_(s.substr(i, n)));
                            i += n;
                        }
                        return Value::Table_(tb2);
                    }
                    wstring d = BStr(a[1]);
                    long long maxCount = 0;
                    if (a.size() > 2 && a[2].t != Value::Null)
                        maxCount = (long long)BNum(a[2]);
                    // '<整词>' 形式：外层串值的首尾尖括号去掉，中间原样作分隔符
                    // （官方源码里写成 '<\r\n>' / '<**>' 这种，转义已由外层字面量处理）
                    if (d.size() >= 2 && d.front() == L'<' && d.back() == L'>')
                    {
                        stringSplitRun(tb2, s, d.substr(1, d.size() - 2), 0, maxCount);
                        return Value::Table_(tb2);
                    }
                    if (d.empty())
                    {
                        tb2->arr.push_back(Value::Str_(s));
                        return Value::Table_(tb2);
                    }
                    // 2 参形式：多个单字节分隔符（集合）
                    stringSplitRun(tb2, s, d, 1, maxCount);
                    return Value::Table_(tb2); });
                tb->map[L"splitLiteral"] = NF([](vector<Value>& a) -> Value {
                    wstring s = a.empty() ? wstring() : BStr(a[0]);
                    auto tb2 = std::make_shared<Table>();
                    if (s.empty()) return Value::Table_(tb2);
                    wstring d = a.size() > 1 && a[1].t != Value::Null ? BStr(a[1]) : wstring();
                    long long maxCount = 0;
                    if (a.size() > 2 && a[2].t != Value::Null)
                        maxCount = (long long)BNum(a[2]);
                    stringSplitRun(tb2, s, d, 0, maxCount);
                    return Value::Table_(tb2); });
                tb->map[L"count"] = NF([](vector<Value>& a) {
                    wstring s = BStr(a[0]), f = BStr(a[1]);
                    if (f.empty()) return Value::Num_(0);
                    vector<vector<RE>> br; bool anchored = false;
                    int nGroups = 0; wstring perr;
                    if (!RECompile(f, br, &anchored, &nGroups, &perr))
                        return Value::Num_(0);
                    // literal fast path: non-overlapping memchr-speed
                    // counting (same rationale as string.find)
                    if (nGroups == 0 && !anchored && REIsPlainLiteral(f))
                    {
                        long long cLit = 0;
                        for (size_t p1 = s.find(f); p1 != wstring::npos;
                             p1 = s.find(f, p1 + f.size()))
                            cLit++;
                        return Value::Num_((double)cLit);
                    }
                    long long c = 0;
                    size_t p0 = 0;
                    while (p0 <= s.size())
                    {
                        vector<wstring> caps; size_t end = 0;
                        if (RETry(br, s, p0, false, caps, &end))
                        {
                            c++;
                            p0 = end > p0 ? end : p0 + 1;
                        }
                        else
                        {
                            if (p0 >= s.size()) break;
                            p0++;
                        }
                    }
                    return Value::Num_((double)c); });
                tb->map[L"rep"] = NF([](vector<Value>& a) {
                    wstring s = BStr(a[0]); long long k = (long long)BNum(a[1]);
                    wstring r;
                    // single allocation instead of geometric regrowth
                    if (k > 0 && !s.empty() &&
                        (unsigned long long)k * s.size() < (1ull << 30))
                        r.reserve((size_t)((unsigned long long)k * s.size()));
                    for (long long i = 0; i < k; i++) r += s;
                    return Value::Str_(r); });
                tb->map[L"toUtf16"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty()) return Value::Str_(L"");
                    wstring w = BStr(a[0]);
                    // official contract: UTF-16 ENCODED byte string — the
                    // raw little-endian bytes, so #result is the byte
                    // length (clipboard GlobalAlloc sizing relies on it)
                    wstring out;
                    out.reserve(w.size() * 2);
                    for (wchar_t ch : w)
                    {
                        out += (wchar_t)(unsigned char)(ch & 0xFF);
                        out += (wchar_t)(unsigned char)((ch >> 8) & 0xFF);
                    }
                    Value ret = Value::Str_(out);
                    ret.bRawBytes = true;
                    return ret; });
                tb->map[L"fromUtf16"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty()) return Value();
                    int reqLen = -1;
                    if (a.size() > 2)
                    {
                        if (a[2].t == Value::Num) reqLen = (int)BNum(a[2]);
                        else if (a[2].b) reqLen = -1;
                    }
                    else if (a.size() > 1)
                    {
                        if (a[1].t == Value::Num) reqLen = (int)BNum(a[1]);
                        else if (a[1].b) reqLen = -1;
                    }

                    if (a[0].t == Value::Str)
                    {
                        // decode UTF-16 LE bytes back to text
                        const wstring& s = a[0].s;
                        wstring w;
                        w.reserve(s.size() / 2);
                        for (size_t i = 0; i + 1 < s.size(); i += 2)
                        {
                            wchar_t ch = (wchar_t)(unsigned char)s[i] |
                                         ((wchar_t)(unsigned char)s[i + 1] << 8);
                            if (ch == 0 && reqLen == -1)
                                break;
                            w += ch;
                        }
                        if (reqLen >= 0 && (size_t)reqLen < w.size())
                            w.resize((size_t)reqLen);
                        return Value::Str_(w);
                    }
                    if (a[0].t == Value::Pointer || a[0].t == Value::Num)
                    {
                        const wchar_t* p = (const wchar_t*)(INT_PTR)(long long)BNum(a[0]);
                        if (!p) return Value();
                        if (reqLen >= 0)
                        {
                            return Value::Str_(wstring(p, (size_t)reqLen));
                        }
                        return Value::Str_(wstring(p));
                    }
                    return Value(); });
                tb->map[L"fromUtf16If"] = NF([](vector<Value>& a) -> Value {
                    // official contract: decode only when the input looks
                    // like UTF-16 LE (heuristic — a plain ASCII/UTF-8
                    // string with a NUL every other byte is UTF-16)
                    if (a.empty()) return Value();
                    if (a[0].t != Value::Str) return a[0];
                    const wstring& s = a[0].s;
                    if (s.size() < 2) return a[0];
                    bool bLooksUtf16 = true;
                    for (size_t i = 1; i + 1 < s.size(); i += 2)
                        if (s[i] != (wchar_t)0) { bLooksUtf16 = false; break; }
                    if (!bLooksUtf16) return a[0];
                    wstring w;
                    w.reserve(s.size() / 2);
                    for (size_t i = 0; i + 1 < s.size(); i += 2)
                        w += (wchar_t)(unsigned char)s[i] |
                             ((wchar_t)(unsigned char)s[i + 1] << 8);
                    return Value::Str_(w); });
                // string.sliceUtf16(s, i = 1, j = -1) -> 1-based character slice of UTF-16 text
                tb->map[L"sliceUtf16"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty() || a[0].t == Value::Null) return Value::Str_(L"");
                    wstring s = BStr(a[0]);
                    long long n = (long long)s.size();
                    long long st = a.size() > 1 && a[1].t != Value::Null ? (long long)BNum(a[1]) : 1;
                    long long en = a.size() > 2 && a[2].t != Value::Null ? (long long)BNum(a[2]) : n;
                    if (st < 0) st = n + st + 1;
                    if (en < 0) en = n + en + 1;
                    if (st < 1) st = 1;
                    if (en > n) en = n;
                    if (en < st) return Value::Str_(L"");
                    return Value::Str_(s.substr((size_t)(st - 1), (size_t)(en - st + 1)));
                });
                tb->map[L"pack"] = NF([](vector<Value>& a) -> Value {
                    wstring out;
                    std::function<void(const Value&)> addVal = [&](const Value& v) {
                        if (v.t == Value::Num)
                        {
                            out += (wchar_t)(unsigned char)(unsigned int)(long long)v.n;
                        }
                        else if (v.t == Value::Str)
                        {
                            out += v.s;
                        }
                        else if (v.t == Value::Pointer && v.ptr && IsBuffer(v.ptr))
                        {
                            size_t bsz = GetBufferSize(v.ptr);
                            const unsigned char* pData = (const unsigned char*)v.ptr;
                            for (size_t i = 0; i < bsz; i++)
                                out += (wchar_t)pData[i];
                        }
                        else if (v.t == Value::TableT && v.tab)
                        {
                            if (!v.tab->arr.empty())
                            {
                                for (auto& item : v.tab->arr)
                                    addVal(item);
                            }
                            else
                            {
                                for (size_t i = 1;; i++)
                                {
                                    auto it = v.tab->map.find(std::to_wstring(i));
                                    if (it != v.tab->map.end())
                                        addVal(it->second);
                                    else
                                        break;
                                }
                            }
                        }
                    };
                    for (auto& arg : a)
                        addVal(arg);
                    Value ret = Value::Str_(out);
                    ret.bRawBytes = true;
                    return ret; });
                tb->map[L"unpack"] = NF([](vector<Value>& a) -> Value {
                    auto t = std::make_shared<Table>();
                    if (a.empty()) return Value::Table_(t);
                    wstring s = BStr(a[0]);
                    int len = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
                    std::string u8(len, 0);
                    if (len > 0)
                        WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &u8[0], len, nullptr, nullptr);
                    for (unsigned char c : u8)
                        t->arr.push_back(Value::Num_((double)c));
                    return Value::Table_(t); });
                tb->map[L"format"] = NF([](vector<Value>& a) {
                    wstring fmt = BStr(a[0]);
                    wstring out;
                    size_t ai = 1;
                    for (size_t i = 0; i < fmt.size(); i++)
                    {
                        if (fmt[i] != L'%' || i + 1 >= fmt.size())
                        {
                            out += fmt[i];
                            continue;
                        }
                        wchar_t c = fmt[++i];
                        if (c == L'%') { out += L'%'; continue; }
                        wstring spec = L"%";
                        while (i < fmt.size() && (wcschr(L".0123456789", fmt[i]) ||
                               fmt[i] == L'l'))
                            spec += fmt[i++];
                        if (i >= fmt.size()) break;
                        spec += fmt[i];
                        Value arg = ai < a.size() ? a[ai++] : Value();
                        wchar_t buf[128];
                        switch (fmt[i])
                        {
                        case L's': out += BStr(arg); break;
                        case L'd': case L'i': case L'u':
                            swprintf(buf, 128, spec.c_str(), (long long)BNum(arg));
                            out += buf; break;
                        case L'f':
                            swprintf(buf, 128, spec.c_str(), BNum(arg));
                            out += buf; break;
                        case L'x': case L'X':
                            swprintf(buf, 128, spec.c_str(), (unsigned)BNum(arg));
                            out += buf; break;
                        case L'c': out += (wchar_t)BNum(arg); break;
                        default: out += arg.ToString(); break;
                        }
                    }
                    return Value::Str_(out); });

                auto ResolveScriptPathInternal = [](const wstring& rel) -> wstring {
                    if (rel.empty()) return rel;
                    if (rel[0] == L'~')
                        return ResolveTildeDisk(rel);
                    wstring appRoot;
                    if (g_interpCurrent && !g_interpCurrent->baseDir.empty())
                        appRoot = g_interpCurrent->baseDir;
                    if (appRoot.empty())
                    {
                        wchar_t szExe[MAX_PATH] = L"";
                        GetModuleFileNameW(nullptr, szExe, MAX_PATH);
                        wchar_t* pSep = wcsrchr(szExe, L'\\');
                        if (pSep) *pSep = 0;
                        appRoot = szExe;
                    }
                    if (rel.size() >= 2 &&
                        (rel[0] == L'/' || rel[0] == L'\\') &&
                        (rel[1] == L'/' || rel[1] == L'\\'))
                    {
                        if (rel[0] == L'\\')
                            return L"";
                        return rel.substr(1);
                    }
                    else if (!rel.empty() && (rel[0] == L'/' || rel[0] == L'\\'))
                    {
                        wstring rest = rel.substr(1);
                        while (!rest.empty() && (rest[0] == L'/' || rest[0] == L'\\'))
                            rest = rest.substr(1);
                        if (rest.empty())
                            return appRoot + L"\\";
                        return appRoot + L"\\" + rest;
                    }
                    wchar_t szFull[MAX_PATH * 2] = L"";
                    DWORD nFull = GetFullPathNameW(rel.c_str(), MAX_PATH * 2, szFull, nullptr);
                    if (nFull > 0 && nFull < MAX_PATH * 2)
                        return szFull;
                    return rel;
                };

                tb->map[L"load"] = NF([ResolveScriptPathInternal](vector<Value>& a) {
                    if (a.empty()) return Value();
                    wstring resolved = ResolveScriptPathInternal(BStr(a[0]));
                    FILE* pf = nullptr;
                    if (_wfopen_s(&pf, resolved.c_str(), L"rb") != 0 || !pf)
                        return Value();
                    std::string raw2;
                    char buf2[4096]; size_t rd;
                    while ((rd = fread(buf2, 1, sizeof(buf2), pf)) > 0)
                        raw2.append(buf2, rd);
                    fclose(pf);
                    // Binary sniff: NUL byte in the first 4KB or invalid UTF-8
                    // -> binary payload, store 1:1 byte-per-wchar (byte string model)
                    bool bBinary = false;
                    size_t nSniff = min(raw2.size(), (size_t)4096);
                    for (size_t k = 0; k < nSniff; k++)
                        if (raw2[k] == 0) { bBinary = true; break; }
                    if (bBinary)
                    {
                        wstring w;
                        w.resize(raw2.size());
                        for (size_t k = 0; k < raw2.size(); k++)
                            w[k] = (wchar_t)(unsigned char)raw2[k];
                        return Value::Str_(w);
                    }
                    int off2 = (raw2.size() >= 3 && (BYTE)raw2[0] == 0xEF &&
                                (BYTE)raw2[1] == 0xBB && (BYTE)raw2[2] == 0xBF) ? 3 : 0;
                    int w2 = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw2.data() + off2,
                                                 (int)(raw2.size() - off2), nullptr, 0);
                    if (w2 <= 0)
                    {
                        wstring w;
                        w.resize(raw2.size());
                        for (size_t k = 0; k < raw2.size(); k++)
                            w[k] = (wchar_t)(unsigned char)raw2[k];
                        return Value::Str_(w);
                    }
                    wstring s2((size_t)w2, 0);
                    MultiByteToWideChar(CP_UTF8, 0, raw2.data() + off2,
                                        (int)(raw2.size() - off2), &s2[0], w2);
                    return Value::Str_(s2); });
                tb->map[L"loadBuffer"] = NF([ResolveScriptPathInternal](vector<Value>& a) -> Value {
                    if (a.empty()) return Value();
                    if (a[0].t == Value::Pointer && a[0].ptr) return a[0];
                    wstring path = BStr(a[0]);
                    if (path.empty()) return Value();

                    std::string rawData;
                    bool bLoaded = false;
                    if (g_interpCurrent)
                    {
                        wstring key = path;
                        if (!key.empty() && key[0] == L'~') key = key.substr(1);
                        if (!key.empty() && (key[0] == L'\\' || key[0] == L'/')) key = key.substr(1);
                        auto eit = g_interpCurrent->embedded.find(key);
                        if (eit == g_interpCurrent->embedded.end() && key != path)
                            eit = g_interpCurrent->embedded.find(path);
                        if (eit != g_interpCurrent->embedded.end())
                        {
                            rawData.resize(eit->second.size());
                            for (size_t k = 0; k < eit->second.size(); k++)
                                rawData[k] = (char)(unsigned char)eit->second[k];
                            bLoaded = true;
                        }
                    }
                    if (!bLoaded)
                    {
                        wstring resolved = ResolveScriptPathInternal(path);
                        FILE* pf = nullptr;
                        if (_wfopen_s(&pf, resolved.c_str(), L"rb") == 0 && pf)
                        {
                            char buf[4096];
                            size_t rd;
                            while ((rd = fread(buf, 1, sizeof(buf), pf)) > 0)
                                rawData.append(buf, rd);
                            fclose(pf);
                            bLoaded = true;
                        }
                    }
                    if (!bLoaded || rawData.empty()) return Value();
                    void* p = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, rawData.size());
                    if (!p) return Value();
                    memcpy(p, rawData.data(), rawData.size());
                    RegisterBuffer(p, rawData.size());
                    return Value::Pointer_(p);
                });
                tb->map[L"hex"] = NF([](vector<Value>& a) {
                    if (a.empty()) return Value::Str_(L"");
                    wstring prefix = a.size() > 1 ? BStr(a[1]) : L"";
                    static const wchar_t* kHex = L"0123456789ABCDEF";
                    wstring out;
                    if (a[0].t == Value::Pointer && a[0].ptr && IsBuffer(a[0].ptr))
                    {
                        size_t bsz = GetBufferSize(a[0].ptr);
                        const unsigned char* pData = (const unsigned char*)a[0].ptr;
                        for (size_t i = 0; i < bsz; i++)
                        {
                            if (!prefix.empty()) out += prefix;
                            out += kHex[pData[i] >> 4];
                            out += kHex[pData[i] & 0xF];
                        }
                        return Value::Str_(out);
                    }
                    wstring str = BStr(a[0]);
                    if (a[0].t == Value::Str && a[0].bRawBytes)
                    {
                        // Byte string / binary data: read raw bytes directly without UTF-8 expansion
                        for (wchar_t c : str)
                        {
                            unsigned char b = (unsigned char)c;
                            if (!prefix.empty()) out += prefix;
                            out += kHex[b >> 4];
                            out += kHex[b & 0xF];
                        }
                    }
                    else
                    {
                        int n = WideCharToMultiByte(CP_UTF8, 0, str.c_str(),
                            (int)str.size(), nullptr, 0, nullptr, nullptr);
                        std::string raw((size_t)n, 0);
                        WideCharToMultiByte(CP_UTF8, 0, str.c_str(),
                            (int)str.size(), &raw[0], n, nullptr, nullptr);
                        for (unsigned char c : raw)
                        {
                            if (!prefix.empty()) out += prefix;
                            out += kHex[c >> 4];
                            out += kHex[c & 0xF];
                        }
                    }
                    return Value::Str_(out); });
                tb->map[L"unhex"] = NF([](vector<Value>& a) {
                    if (a.empty()) return Value::Str_(L"");
                    wstring h = BStr(a[0]);
                    wstring prefix = a.size() > 1 ? BStr(a[1]) : L"";
                    std::string raw;
                    auto hexVal = [](wchar_t c) -> int {
                        if (c >= L'0' && c <= L'9') return c - L'0';
                        if (c >= L'a' && c <= L'f') return c - L'a' + 10;
                        if (c >= L'A' && c <= L'F') return c - L'A' + 10;
                        return -1;
                    };
                    size_t i = 0;
                    if (!prefix.empty())
                    {
                        while (i < h.size())
                        {
                            if (h.compare(i, prefix.size(), prefix) == 0 &&
                                i + prefix.size() + 1 < h.size())
                            {
                                int h1 = hexVal(h[i + prefix.size()]);
                                int h2 = hexVal(h[i + prefix.size() + 1]);
                                if (h1 >= 0 && h2 >= 0)
                                {
                                    raw.push_back((char)((h1 << 4) | h2));
                                    i += prefix.size() + 2;
                                    continue;
                                }
                            }
                            wchar_t ch = h[i++];
                            if ((unsigned int)ch < 0x80)
                            {
                                raw.push_back((char)ch);
                            }
                            else
                            {
                                char mb[8] = {0};
                                int nb = WideCharToMultiByte(CP_UTF8, 0, &ch, 1, mb, 8, nullptr, nullptr);
                                for (int k = 0; k < nb; k++) raw.push_back(mb[k]);
                            }
                        }
                    }
                    else
                    {
                        while (i < h.size())
                        {
                            if (i + 1 < h.size())
                            {
                                int h1 = hexVal(h[i]);
                                int h2 = hexVal(h[i + 1]);
                                if (h1 >= 0 && h2 >= 0)
                                {
                                    raw.push_back((char)((h1 << 4) | h2));
                                    i += 2;
                                    continue;
                                }
                            }
                            wchar_t ch = h[i++];
                            if ((unsigned int)ch < 0x80)
                            {
                                raw.push_back((char)ch);
                            }
                            else
                            {
                                char mb[8] = {0};
                                int nb = WideCharToMultiByte(CP_UTF8, 0, &ch, 1, mb, 8, nullptr, nullptr);
                                for (int k = 0; k < nb; k++) raw.push_back(mb[k]);
                            }
                        }
                    }

                    int w = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw.data(),
                                                (int)raw.size(), nullptr, 0);
                    if (w > 0)
                    {
                        wstring out((size_t)w, 0);
                        MultiByteToWideChar(CP_UTF8, 0, raw.data(),
                                            (int)raw.size(), &out[0], w);
                        return Value::Str_(out);
                    }
                    wstring out((size_t)raw.size(), 0);
                    for (size_t k = 0; k < raw.size(); k++)
                        out[k] = (wchar_t)(unsigned char)raw[k];
                    return Value::Str_(out); });
                tb->map[L"base64"] = NF([](vector<Value>& a) {
                    wstring str = BStr(a[0]);
                    int n = WideCharToMultiByte(CP_UTF8, 0, str.c_str(),
                        (int)str.size(), nullptr, 0, nullptr, nullptr);
                    std::string raw((size_t)n, 0);
                    WideCharToMultiByte(CP_UTF8, 0, str.c_str(),
                        (int)str.size(), &raw[0], n, nullptr, nullptr);
                    static const wchar_t* kB64 =
                        L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                    wstring out;
                    size_t j = 0;
                    while (j < raw.size())
                    {
                        unsigned b0 = (unsigned char)raw[j];
                        unsigned b1 = j + 1 < raw.size() ? (unsigned char)raw[j + 1] : 0;
                        unsigned b2 = j + 2 < raw.size() ? (unsigned char)raw[j + 2] : 0;
                        out += kB64[b0 >> 2];
                        out += kB64[((b0 & 3) << 4) | (b1 >> 4)];
                        out += j + 1 < raw.size() ? kB64[((b1 & 15) << 2) | (b2 >> 6)] : L'=';
                        out += j + 2 < raw.size() ? kB64[b2 & 63] : L'=';
                        j += 3;
                    }
                    return Value::Str_(out); });
                tb->map[L"save"] = NF([ResolveScriptPathInternal](vector<Value>& a) {
                    if (a.empty()) return Value::Bool_(false);
                    wstring resolved = ResolveScriptPathInternal(BStr(a[0]));
                    size_t lastSlash = resolved.find_last_of(L"\\/");
                    if (lastSlash != wstring::npos && lastSlash > 2)
                    {
                        wstring dir = resolved.substr(0, lastSlash);
                        for (size_t i = 3; i <= dir.size(); i++)
                        {
                            if (i == dir.size() || dir[i] == L'\\' || dir[i] == L'/')
                            {
                                wstring part = dir.substr(0, i);
                                if (!part.empty() && part[part.size() - 1] != L':')
                                    CreateDirectoryW(part.c_str(), nullptr);
                            }
                        }
                    }
                    FILE* pf = nullptr;
                    if (_wfopen_s(&pf, resolved.c_str(), L"wb") != 0 || !pf)
                        return Value::Bool_(false);
                    if (a.size() > 1 && a[1].t == Value::Pointer && a[1].ptr && IsBuffer(a[1].ptr))
                    {
                        size_t bsz = GetBufferSize(a[1].ptr);
                        if (bsz > 0)
                            fwrite(a[1].ptr, 1, bsz, pf);
                        fclose(pf);
                        return Value::Bool_(true);
                    }
                    wstring s2 = BStr(a[1]);
                    bool bTextIn = false;
                    for (wchar_t c : s2)
                    {
                        if ((unsigned short)c > 0xFF) { bTextIn = true; break; }
                    }
                    if (!bTextIn)
                    {
                        // Byte string / binary data: write raw bytes directly without UTF-8 re-encoding
                        std::string raw;
                        raw.resize(s2.size());
                        for (size_t i = 0; i < s2.size(); i++)
                            raw[i] = (char)(unsigned char)s2[i];
                        fwrite(raw.data(), 1, raw.size(), pf);
                    }
                    else
                    {
                        int n2 = WideCharToMultiByte(CP_UTF8, 0, s2.c_str(),
                                                     (int)s2.size(), nullptr, 0,
                                                     nullptr, nullptr);
                        std::string out2((size_t)n2, 0);
                        if (n2 > 0)
                            WideCharToMultiByte(CP_UTF8, 0, s2.c_str(),
                                                (int)s2.size(), &out2[0], n2,
                                                nullptr, nullptr);
                        fwrite(out2.data(), 1, out2.size(), pf);
                    }
                    fclose(pf);
                    return Value::Bool_(true); });
                // round 43: fromto(s, from, to) — transcoding. Strings in
                // the VM are wide text; a "byte string" is a wide string
                // with every char <= 0xFF (one char = one raw byte).
                // Input with any char > 0xFF is TEXT (encoded with `from`
                // to get its bytes); otherwise input is a byte string.
                // Output: `to` empty/omitted -> decoded TEXT; otherwise
                // a byte string. from/to accept names or numeric
                // codepages (see CodepageOf); "" = UTF-8.
                tb->map[L"fromto"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty()) return Value();
                    wstring s = BStr(a[0]);
                    UINT cpFrom = CP_UTF8;
                    UINT cpTo = CP_ACP;
                    bool bToText = false;
                    if (a.size() == 1)
                    {
                        // 1 arg: string.fromto(s) -> converts UTF-8 to ANSI (CP_ACP)
                        cpFrom = CP_UTF8;
                        cpTo = CP_ACP;
                        bToText = false;
                    }
                    else if (a.size() == 2)
                    {
                        // 2 args: string.fromto(s, fromCp) -> converts fromCp to UTF-8
                        cpFrom = CodepageOf(a[1], CP_UTF8);
                        cpTo = CP_UTF8;
                        bToText = true;
                    }
                    else
                    {
                        // 3+ args: string.fromto(s, fromCp, toCp)
                        cpFrom = a[1].t == Value::Null ? CP_UTF8 : CodepageOf(a[1], CP_UTF8);
                        if (a[2].t == Value::Null || (a[2].t == Value::Str && a[2].s.empty()))
                        {
                            cpTo = CP_UTF8;
                            bToText = true;
                        }
                        else
                        {
                            cpTo = CodepageOf(a[2], CP_UTF8);
                            bToText = (cpTo == CP_UTF8);
                        }
                    }
                    bool bTextIn = false;
                    for (wchar_t c : s)
                        if (c > 0xFF) { bTextIn = true; break; }
                    std::string raw;
                    if (bTextIn)
                    {
                        int nF = WideCharToMultiByte(cpFrom, 0, s.c_str(),
                            (int)s.size(), nullptr, 0, nullptr, nullptr);
                        if (nF < 0) return Value();
                        raw.resize((size_t)nF);
                    }
                    else
                    {
                        raw.resize(s.size());
                        for (size_t k = 0; k < s.size(); k++)
                            raw[k] = (char)(unsigned char)s[k];
                    }
                    if (bTextIn)
                        WideCharToMultiByte(cpFrom, 0, s.c_str(),
                            (int)s.size(), &raw[0], (int)raw.size(),
                            nullptr, nullptr);
                    int nWide = MultiByteToWideChar(cpFrom, 0, raw.data(),
                        (int)raw.size(), nullptr, 0);
                    if (nWide < 0) return Value();
                    wstring mid((size_t)nWide, 0);
                    if (nWide > 0)
                        MultiByteToWideChar(cpFrom, 0, raw.data(),
                            (int)raw.size(), &mid[0], nWide);
                    if (bToText)
                        return Value::Str_(mid);   // decoded text
                    int nTo = WideCharToMultiByte(cpTo, 0, mid.c_str(),
                        (int)mid.size(), nullptr, 0, nullptr, nullptr);
                    if (nTo < 0) return Value();
                    std::string out((size_t)nTo, 0);
                    if (nTo > 0)
                        WideCharToMultiByte(cpTo, 0, mid.c_str(),
                            (int)mid.size(), &out[0], nTo, nullptr,
                            nullptr);
                    wstring res((size_t)nTo, 0);
                    for (int k = 0; k < nTo; k++)
                        res[(size_t)k] =
                            (wchar_t)(unsigned char)out[(size_t)k];
                    Value ret = Value::Str_(res);
                    ret.bRawBytes = true;
                    return ret; });
            }
            // ---- math ----
            {
                auto tb = GetRoot(L"math").tab;
                tb->map[L"PI"] = Value::Num_(3.14159265358979323846);
                tb->map[L"pi"] = tb->map[L"PI"];
                tb->map[L"E"] = Value::Num_(2.71828182845904523536);
                tb->map[L"e"] = tb->map[L"E"];
                tb->map[L"abs"] = NF([](vector<Value>& a) {
                    return Value::Num_(fabs(BNum(a[0]))); });
                tb->map[L"ceil"] = NF([](vector<Value>& a) {
                    return Value::Num_(ceil(BNum(a[0]))); });
                tb->map[L"floor"] = NF([](vector<Value>& a) {
                    return Value::Num_(floor(BNum(a[0]))); });
                tb->map[L"sqrt"] = NF([](vector<Value>& a) {
                    return Value::Num_(sqrt(BNum(a[0]))); });
                tb->map[L"pow"] = NF([](vector<Value>& a) {
                    return Value::Num_(pow(BNum(a[0]), BNum(a[1]))); });
                tb->map[L"log"] = NF([](vector<Value>& a) {
                    return Value::Num_(log(BNum(a[0]))); });
                tb->map[L"log10"] = NF([](vector<Value>& a) {
                    return Value::Num_(log10(BNum(a[0]))); });
                tb->map[L"exp"] = NF([](vector<Value>& a) {
                    return Value::Num_(exp(BNum(a[0]))); });
                tb->map[L"sin"] = NF([](vector<Value>& a) {
                    return Value::Num_(sin(BNum(a[0]))); });
                tb->map[L"cos"] = NF([](vector<Value>& a) {
                    return Value::Num_(cos(BNum(a[0]))); });
                tb->map[L"tan"] = NF([](vector<Value>& a) {
                    return Value::Num_(tan(BNum(a[0]))); });
                tb->map[L"asin"] = NF([](vector<Value>& a) {
                    return Value::Num_(asin(BNum(a[0]))); });
                tb->map[L"acos"] = NF([](vector<Value>& a) {
                    return Value::Num_(acos(BNum(a[0]))); });
                tb->map[L"atan"] = NF([](vector<Value>& a) {
                    return Value::Num_(atan(BNum(a[0]))); });
                tb->map[L"atan2"] = NF([](vector<Value>& a) {
                    return Value::Num_(atan2(BNum(a[0]), a.size() > 1 ? BNum(a[1]) : 0)); });
                tb->map[L"max"] = NF([](vector<Value>& a) {
                    double m = a.empty() ? 0 : BNum(a[0]);
                    for (size_t i = 1; i < a.size(); i++)
                        m = max(m, BNum(a[i]));
                    return Value::Num_(m); });
                tb->map[L"min"] = NF([](vector<Value>& a) {
                    double m = a.empty() ? 0 : BNum(a[0]);
                    for (size_t i = 1; i < a.size(); i++)
                        m = min(m, BNum(a[i]));
                    return Value::Num_(m); });
                tb->map[L"clamp"] = NF([](vector<Value>& a) {
                    double v = a.empty() ? 0 : BNum(a[0]);
                    double lo = a.size() > 1 ? BNum(a[1]) : 0;
                    double hi = a.size() > 2 ? BNum(a[2]) : 255;
                    if (lo > hi) { double t = lo; lo = hi; hi = t; }
                    return Value::Num_(v < lo ? lo : (v > hi ? hi : v)); });
                // math.fmod(x,y) —— 官方声明（lib/builtin/math.aardio）：
                //   计算并返回浮点数除法 x/y 的余数，返回的余数**符号与被除数 x 相同**
                //   （基于向零取整）。注意 aardio 的 `x % y` 余数符号与 **y** 相同
                //   （基于向下取整）—— 两者语义不同，这里是 C 的 fmod。
                tb->map[L"fmod"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty()) return Value::Num_(0);
                    double x = BNum(a[0]);
                    double y = a.size() > 1 ? BNum(a[1]) : 0;
                    if (y == 0) return Value::Num_(0);
                    return Value::Num_(std::fmod(x, y)); });
                // math.lerp(fromValue,toValue,t) —— 官方声明：
                //   线性插值，返回 `fromValue + (toValue-fromValue) * t`。
                //   参数 t 不会被限制在 0 到 1 之间，允许外插。
                tb->map[L"lerp"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty()) return Value::Num_(0);
                    double from = BNum(a[0]);
                    double to = a.size() > 1 ? BNum(a[1]) : 0;
                    double t = a.size() > 2 ? BNum(a[2]) : 0;
                    return Value::Num_(from + (to - from) * t); });
                tb->map[L"dist"] = NF([](vector<Value>& a) {
                    if (a.empty()) return Value::Num_(0);
                    if (a.size() >= 6)
                    {
                        double dx = BNum(a[0]) - BNum(a[3]);
                        double dy = BNum(a[1]) - BNum(a[4]);
                        double dz = BNum(a[2]) - BNum(a[5]);
                        return Value::Num_(sqrt(dx * dx + dy * dy + dz * dz));
                    }
                    if (a.size() >= 4)
                    {
                        double dx = BNum(a[0]) - BNum(a[2]);
                        double dy = BNum(a[1]) - BNum(a[3]);
                        return Value::Num_(_hypot(dx, dy));
                    }
                    if (a.size() == 3)
                    {
                        double x = BNum(a[0]), y = BNum(a[1]), z = BNum(a[2]);
                        return Value::Num_(sqrt(x * x + y * y + z * z));
                    }
                    if (a.size() == 2)
                    {
                        return Value::Num_(_hypot(BNum(a[0]), BNum(a[1])));
                    }
                    return Value::Num_(fabs(BNum(a[0])));
                });
                tb->map[L"validateRange"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty()) return Value();
                    long long len = a[0].t != Value::Null ? (long long)BNum(a[0]) : 0;
                    long long st = a.size() > 1 && a[1].t != Value::Null ? (long long)BNum(a[1]) : 1;
                    long long en = a.size() > 2 && a[2].t != Value::Null ? (long long)BNum(a[2]) : len;
                    if (st < 0 && len > 0) st = len + st + 1;
                    if (en < 0 && len > 0) en = len + en + 1;
                    if (st < 1) st = 1;
                    if (len > 0 && en > len) en = len;
                    if (st > en) return Value();
                    Value r = Value::Num_((double)st);
                    auto ex = std::make_shared<vector<Value>>();
                    ex->push_back(Value::Num_((double)en));
                    r.mrets = ex;
                    return r;
                });
                tb->map[L"round"] = NF([](vector<Value>& a) {
                    if (a.empty()) return Value::Num_(0);
                    return Value::Num_(round(BNum(a[0]))); });
                tb->map[L"modf"] = NF([](vector<Value>& a) {
                    if (a.empty()) return Value();
                    double intPart = 0;
                    double frac = modf(BNum(a[0]), &intPart);
                    Value r = Value::Num_(intPart);
                    auto ex = std::make_shared<vector<Value>>();
                    ex->push_back(Value::Num_(frac));
                    r.mrets = ex;
                    return r; });
                tb->map[L"mulDiv"] = NF([](vector<Value>& a) {
                    if (a.size() < 3) return Value::Num_(0);
                    int r = MulDiv((int)(long long)BNum(a[0]),
                                   (int)(long long)BNum(a[1]),
                                   (int)(long long)BNum(a[2]));
                    return Value::Num_(r); });
                tb->map[L"random"] = NF([](vector<Value>& a) {
                    static bool seeded = false;
                    if (!seeded) { srand((unsigned)time(nullptr)); seeded = true; }
                    if (a.empty())
                        return Value::Num_((double)rand() / RAND_MAX);
                    long long lo = (long long)BNum(a[0]);
                    if (a.size() == 1)
                        return Value::Num_((double)(1 + rand() % lo));
                    long long hi = (long long)BNum(a[1]);
                    if (hi < lo) return Value::Num_((double)lo);
                    return Value::Num_((double)(lo + rand() % (hi - lo + 1))); });
                tb->map[L"size64"] = NF([](vector<Value>& a) -> Value {
                    uint64_t val = 0;
                    if (a.empty() || a[0].t == Value::Null)
                    {
                        val = 0;
                    }
                    else if (a.size() >= 2 && a[1].t != Value::Null)
                    {
                        uint32_t lo = (uint32_t)(unsigned long long)BNum(a[0]);
                        uint32_t hi = (uint32_t)(unsigned long long)BNum(a[1]);
                        val = ((uint64_t)hi << 32) | lo;
                    }
                    else
                    {
                        val = ToSize64Uint(a[0]);
                    }
                    return CreateSize64(val);
                });
                tb->map[L"isSize64"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty() || a[0].t != Value::TableT || !a[0].tab)
                        return Value::Bool_(false);
                    return Value::Bool_(IsSize64(a[0]));
                });
                CreateSize64(0);
                if (g_size64_meta)
                    tb->map[L"_size64_meta"] = Value::Table_(g_size64_meta);
            }
            // ---- io ----
            {
                auto tb = GetRoot(L"io").tab;
                // io.joinpath(...) — join path parts with '\'
                tb->map[L"joinpath"] = NF([](vector<Value>& a) -> Value {
                    wstring out;
                    for (size_t i = 0; i < a.size(); i++)
                    {
                        if (a[i].t != Value::Str)
                            continue;
                        wstring p = BStr(a[i]);
                        if (p.empty())
                            continue;
                        if (!out.empty() && out.back() != L'\\' &&
                            out.back() != L'/')
                            out += L'\\';
                        else if (out.empty())
                        {
                            out = p;
                            continue;
                        }
                        if (!p.empty() && (p[0] == L'\\' || p[0] == L'/'))
                            p = p.substr(1);
                        out += p;
                    }
                    return Value::Str_(out); });
                // io.splitpath(path) — returns a table of path parts:
                // drive, dir, file, name, ext, path
                tb->map[L"splitpath"] = NF([](vector<Value>& a) -> Value {
                    wstring p = BStr(a[0]);
                    auto out = std::make_shared<Table>();
                    wstring drive = L"";
                    wstring rest = p;
                    if (rest.size() >= 2 && rest[1] == L':')
                    {
                        drive = rest.substr(0, 2);
                        rest = rest.substr(2);
                    }
                    size_t slash = rest.find_last_of(L"\\/");
                    wstring dirPart = L"";
                    wstring filePart = L"";
                    if (slash != wstring::npos)
                    {
                        dirPart = drive + rest.substr(0, slash + 1);
                        filePart = rest.substr(slash + 1);
                    }
                    else
                    {
                        dirPart = drive;
                        filePart = rest;
                    }
                    size_t dot = filePart.find_last_of(L'.');
                    wstring namePart = (dot == wstring::npos) ? filePart : filePart.substr(0, dot);
                    wstring extPart = (dot == wstring::npos) ? L"" : filePart.substr(dot); // includes '.' in official contract
                    out->map[L"drive"] = Value::Str_(drive);
                    out->map[L"dir"] = Value::Str_(dirPart);
                    out->map[L"file"] = Value::Str_(filePart);
                    out->map[L"name"] = Value::Str_(namePart);
                    out->map[L"ext"] = Value::Str_(extPart);
                    out->map[L"path"] = Value::Str_(p);
                    return Value::Table_(out); });

                // Built-in io properties: _exepath, _exedir, _exefile
                wchar_t szModExe[MAX_PATH] = L"";
                GetModuleFileNameW(nullptr, szModExe, MAX_PATH);
                wstring exePathStr = szModExe;
                size_t exeSlash = exePathStr.find_last_of(L"\\/");
                wstring exeDirStr = (exeSlash == wstring::npos) ? L"" : exePathStr.substr(0, exeSlash + 1);
                wstring exeFileStr = (exeSlash == wstring::npos) ? exePathStr : exePathStr.substr(exeSlash + 1);
                tb->map[L"_exepath"] = Value::Str_(exePathStr);
                tb->map[L"_exedir"] = Value::Str_(exeDirStr);
                tb->map[L"_exefile"] = Value::Str_(exeFileStr);
                auto writer = [](vector<Value>& a, const wchar_t* sep,
                                 bool nl) -> Value {
                    // Official aardio strings are byte arrays: writing a
                    // string to the console writes its bytes, so a raw byte
                    // string must not be re-encoded as UTF-8 text.
                    std::string out;
                    std::string sepBytes = Utf8Bytes(sep);
                    for (size_t i = 0; i < a.size(); i++)
                    {
                        if (i) out += sepBytes;
                        out += ValueByteForm(a[i]);
                    }
                    if (nl) out += "\n";
                    fwrite(out.data(), 1, out.size(), stdout);
                    fflush(stdout);
                    return Value();
                };
                tb->map[L"print"] = NF([writer](vector<Value>& a) {
                    return writer(a, L"", false); });
                tb->map[L"println"] = NF([writer](vector<Value>& a) {
                    return writer(a, L"", true); });
                // ---- io.file / io.open — kernel file class ----
                // io.file is a kernel class; io.open is its
                // alias. The ctor takes a path (text) or an OS
                // handle (pointer/num), plus a CRT mode string. Byte
                // contract (engine convention, see string.fromto): file
                // content flows as byte strings — one wchar per raw byte,
                // every char <= 0xFF. TEXT values written are encoded
                // UTF-8; a byte string is written as-is.
                // A file handle: a disk FILE* or a published-payload
                // resource buffer (payload resources have no disk file).
                auto ioFileBytesToValue = [](const std::string& raw) -> Value {
                    wstring s((size_t)raw.size(), 0);
                    for (size_t k = 0; k < raw.size(); k++)
                        s[k] = (wchar_t)(unsigned char)raw[k];
                    Value ret = Value::Str_(s);
                    ret.bRawBytes = true;
                    return ret;
                };
                auto ioFileArgBytes = [](const Value& v,
                                         std::string& out) -> bool {
                    // appends to out (write() loops over several args)
                    std::string piece;
                    if (v.t == Value::TableT || v.t == Value::Class)
                    {
                        if (!v.tab) return false;
                        int total = 0;
                        void* p = StructSerialize(v, &total);
                        if (!p) return false;
                        piece.assign((const char*)p, (size_t)total);
                        HeapFree(GetProcessHeap(), 0, p);
                        out += piece;
                        return true;
                    }
                    wstring s = v.ToString();
                    for (wchar_t c : s)
                    {
                        if (c > 0xFF)
                        {
                            // text: encode UTF-8
                            int n = WideCharToMultiByte(CP_UTF8, 0,
                                s.c_str(), (int)s.size(), nullptr, 0,
                                nullptr, nullptr);
                            piece.resize(n > 0 ? (size_t)n : 0);
                            if (n > 0)
                                WideCharToMultiByte(CP_UTF8, 0, s.c_str(),
                                    (int)s.size(), &piece[0], n, nullptr,
                                    nullptr);
                            out += piece;
                            return true;
                        }
                    }
                    // byte string: raw bytes as-is
                    piece.resize(s.size());
                    for (size_t k = 0; k < s.size(); k++)
                        piece[k] = (char)(unsigned char)s[k];
                    out += piece;
                    return true;
                };
                auto ioFileFail = [](const wchar_t* msg, int code) -> Value {
                    Value v;
                    auto ex = std::make_shared<vector<Value>>();
                    ex->push_back(Value::Str_(msg));
                    ex->push_back(Value::Num_((double)code));
                    v.mrets = ex;
                    return v;
                };
                auto ioFileMake = [ioFileBytesToValue, ioFileArgBytes,
                                   ioFileFail](shared_ptr<IoFileState> st) -> Value {
                    auto tb2 = std::make_shared<Table>();
                    // 隐藏成员：io.lines 需要拿到文件对象的内部状态
                    tb2->map[L"\x01state"] = ioFileStateBox(st);
                    // read(...) — no arg: one line (bytes, newline kept);
                    // n > 0: n bytes; n == -1: all remaining; struct arg:
                    // sizeof(struct) bytes deserialized into a copy;
                    // 0 bytes at EOF -> null
                    tb2->map[L"read"] = NF([st, ioFileBytesToValue,
                                            ioFileFail](
                            vector<Value>& a) -> Value {
                        if (!ioFileOpen(st)) return ioFileFail(L"文件已关闭", 9);
                        // Nodio extension: an encoding name reads the WHOLE
                        // file and converts it ("raw" keeps the bytes). The
                        // official io.readFile contract is a byte count or
                        // "%s" — official code never passes an encoding, so
                        // this branch is purely additive.
                        if (!a.empty() && a[0].t == Value::Str)
                        {
                            wstring encLow = a[0].s;
                            for (auto& c : encLow) c = towlower(c);
                            if (encLow == L"raw" || encLow == L"utf8" ||
                                encLow == L"utf-8" || encLow == L"gbk" ||
                                encLow == L"gb2312" || encLow == L"gb18030" ||
                                encLow == L"ansi" || encLow == L"acp")
                            {
                                std::string all;
                                if (st->bMem)
                                {
                                    all.assign(st->mem.data() + st->memPos,
                                               st->mem.size() - st->memPos);
                                    st->memPos = st->mem.size();
                                }
                                else
                                {
                                    char buf[8192]; size_t got;
                                    while ((got = fread(buf, 1, sizeof(buf),
                                                        st->f)) > 0)
                                        all.append(buf, got);
                                }
                                if (all.empty()) return Value();
                                if (encLow == L"raw")
                                {
                                    wstring raw((size_t)all.size(), 0);
                                    for (size_t k = 0; k < all.size(); k++)
                                        raw[k] = (wchar_t)(unsigned char)all[k];
                                    Value ret = Value::Str_(raw);
                                    ret.bRawBytes = true;
                                    return ret;
                                }
                                UINT cp = CodepageOf(a[0], CP_UTF8);
                                size_t off = 0;
                                if (cp == CP_UTF8 && all.size() >= 3 &&
                                    (unsigned char)all[0] == 0xEF &&
                                    (unsigned char)all[1] == 0xBB &&
                                    (unsigned char)all[2] == 0xBF)
                                    off = 3;   // strip the UTF-8 BOM
                                int w = MultiByteToWideChar(cp, 0,
                                    all.data() + off,
                                    (int)(all.size() - off), nullptr, 0);
                                wstring dec((size_t)(w > 0 ? w : 0), 0);
                                if (w > 0)
                                    MultiByteToWideChar(cp, 0, all.data() + off,
                                        (int)(all.size() - off), &dec[0], w);
                                return Value::Str_(dec);
                            }
                        }
                        if (st->bMem)
                        {
                            // payload resource: the same contract as the
                            // disk path (no arg = one line, n>0 = n bytes,
                            // -1 = all remaining, struct = struct read)
                            std::string raw;
                            bool bLine = a.empty() || a[0].t == Value::Null;
                            if (!a.empty() && (a[0].t == Value::TableT ||
                                               a[0].t == Value::Class))
                            {
                                int total =
                                    a[0].tab ? CalcStructTotalSize(a[0]) : 0;
                                if (total <= 0) return Value();
                                size_t avail = st->mem.size() - st->memPos;
                                size_t got = min((size_t)total, avail);
                                if (got == 0) return Value();
                                raw.assign(st->mem.data() + st->memPos, got);
                                st->memPos += got;
                                auto cp = std::make_shared<Table>();
                                cp->map = a[0].tab->map;
                                Value vCp = Value::Table_(cp);
                                StructDeserialize(vCp, raw.data());
                                return vCp;
                            }
                            long long want = -1;
                            if (!bLine) want = (long long)BNum(a[0]);
                            size_t avail = st->mem.size() - st->memPos;
                            if (bLine)
                            {
                                size_t e = st->memPos;
                                while (e < st->mem.size() &&
                                       st->mem[e] != '\n' &&
                                       !(st->bText &&
                                         (st->mem[e] == '\0' ||
                                          st->mem[e] == '\x1A')))
                                    e++;
                                bool bStopTerm =
                                    (st->bText && e < st->mem.size() &&
                                     (st->mem[e] == '\0' ||
                                      st->mem[e] == '\x1A'));
                                if (!bStopTerm && e < st->mem.size())
                                    e++;   // keep the newline
                                if (e > st->memPos)
                                {
                                    raw.assign(st->mem.data() + st->memPos,
                                               e - st->memPos);
                                    st->memPos = e;
                                }
                            }
                            else if (want == -1)
                            {
                                raw.assign(st->mem.data() + st->memPos, avail);
                                st->memPos = st->mem.size();
                            }
                            else if (want > 0)
                            {
                                size_t got = min((size_t)want, avail);
                                raw.assign(st->mem.data() + st->memPos, got);
                                st->memPos += got;
                            }
                            if (raw.empty()) return Value();
                            if (st->bText)
                            {
                                std::string out;
                                size_t keep = ioFileTextKeep(raw, out);
                                if (keep < raw.size())
                                    st->memPos -= (raw.size() - keep);
                                if (out.empty()) return Value();
                                return ioFileBytesToValue(out);
                            }
                            return ioFileBytesToValue(raw);
                        }
                        std::string raw;
                        bool bLine = a.empty() || a[0].t == Value::Null;
                        if (!a.empty() && (a[0].t == Value::TableT ||
                                           a[0].t == Value::Class))
                        {
                            int total =
                                a[0].tab ? CalcStructTotalSize(a[0]) : 0;
                            if (total <= 0) return Value();
                            raw.resize((size_t)total);
                            size_t got =
                                fread(&raw[0], 1, (size_t)total, st->f);
                            raw.resize(got);
                            if (raw.empty()) return Value();
                            auto cp = std::make_shared<Table>();
                            // shallow copy: nested struct members stay
                            // shared with the template
                            cp->map = a[0].tab->map;
                            Value vCp = Value::Table_(cp);
                            StructDeserialize(vCp, raw.data());
                            return vCp;
                        }
                        long long want = -1;
                        if (!bLine)
                            want = (long long)BNum(a[0]);
                        if (bLine)
                        {
                            // fgetc blocks on _open_osfhandle'd pipe fds
                            // even when data is buffered (read(3) via
                            // fread on the same fd returns instantly), so
                            // the line loop reads through fread as well
                            unsigned char ch = 0;
                            while (fread(&ch, 1, 1, st->f) == 1)
                            {
                                if (st->bText &&
                                    (ch == '\0' || ch == '\x1A'))
                                {
                                    // text-mode terminator: push it back so
                                    // this and later reads stay stopped here
                                    // (a pipe seek fails and the byte is
                                    // swallowed — best effort there)
                                    _fseeki64(st->f, -1, SEEK_CUR);
                                    break;
                                }
                                raw.push_back((char)ch);
                                if (ch == '\n') break;
                            }
                        }
                        else if (want == -1)
                        {
                            char buf[8192];
                            size_t got;
                            while ((got = fread(buf, 1, sizeof(buf),
                                                st->f)) > 0)
                                raw.append(buf, got);
                        }
                        else if (want > 0)
                        {
                            raw.resize((size_t)want);
                            size_t got =
                                fread(&raw[0], 1, (size_t)want, st->f);
                            raw.resize(got);
                        }
                        if (raw.empty()) return Value();
                        if (st->bText)
                        {
                            std::string out;
                            size_t keep = ioFileTextKeep(raw, out);
                            if (keep < raw.size())
                                _fseeki64(st->f,
                                          -(long long)(raw.size() - keep),
                                          SEEK_CUR);
                            if (out.empty()) return Value();
                            return ioFileBytesToValue(out);
                        }
                        return ioFileBytesToValue(raw);
                    });
                    tb2->map[L"readAll"] = NF([st, ioFileBytesToValue,
                                               ioFileFail](
                            vector<Value>&) -> Value {
                        if (!ioFileOpen(st)) return ioFileFail(L"文件已关闭", 9);
                        if (st->bMem)
                        {
                            std::string raw(st->mem.data() + st->memPos,
                                            st->mem.size() - st->memPos);
                            if (st->bText)
                            {
                                std::string out;
                                size_t keep = ioFileTextKeep(raw, out);
                                st->memPos = st->mem.size() -
                                             (raw.size() - keep);
                                if (out.empty()) return Value();
                                return ioFileBytesToValue(out);
                            }
                            st->memPos = st->mem.size();
                            if (raw.empty()) return Value();
                            return ioFileBytesToValue(raw);
                        }
                        std::string raw;
                        char buf[8192];
                        size_t got;
                        while ((got = fread(buf, 1, sizeof(buf), st->f)) > 0)
                            raw.append(buf, got);
                        if (raw.empty()) return Value();
                        if (st->bText)
                        {
                            std::string out;
                            size_t keep = ioFileTextKeep(raw, out);
                            if (keep < raw.size())
                                _fseeki64(st->f,
                                          -(long long)(raw.size() - keep),
                                          SEEK_CUR);
                            if (out.empty()) return Value();
                            return ioFileBytesToValue(out);
                        }
                        return ioFileBytesToValue(raw);
                    });
                    tb2->map[L"readBuffer"] = NF([st](vector<Value>& a) -> Value {
                        if (!ioFileOpen(st) || a.empty()) return Value();
                        void* pBuf = nullptr;
                        size_t bsz = 0;
                        if (a[0].t == Value::Pointer)
                        {
                            pBuf = a[0].ptr;
                            bsz = GetBufferSize(a[0].ptr);
                        }
                        else if (a[0].t == Value::Num)
                        {
                            pBuf = (void*)(INT_PTR)(long long)a[0].n;
                        }
                        else if (a[0].t == Value::TableT && a[0].tab)
                        {
                            auto iby = a[0].tab->map.find(L"_bytes");
                            if (iby != a[0].tab->map.end() && iby->second.t == Value::Str)
                            {
                                pBuf = (void*)iby->second.s.data();
                                bsz = iby->second.s.size() * sizeof(wchar_t);
                            }
                        }
                        if (!pBuf) return Value();
                        size_t want = bsz;
                        if (a.size() > 1 && a[1].t == Value::Num)
                            want = (size_t)(long long)a[1].n;
                        if (want == 0) return Value();
                        if (st->bMem)
                        {
                            size_t avail = st->mem.size() - st->memPos;
                            size_t got = min(want, avail);
                            if (got == 0) return Value();
                            memcpy(pBuf, st->mem.data() + st->memPos, got);
                            st->memPos += got;
                            return Value::Num_((double)got);
                        }
                        size_t got = fread(pBuf, 1, want, st->f);
                        if (got == 0) return Value();
                        return Value::Num_((double)got);
                    });
                    tb2->map[L"writeBuffer"] = NF([st, ioFileFail](vector<Value>& a) -> Value {
                        if (!ioFileOpen(st) || a.empty()) return Value();
                        if (st->bMem) return ioFileFail(L"载荷资源为只读", 5);
                        const void* pBuf = nullptr;
                        size_t bsz = 0;
                        if (a[0].t == Value::Pointer)
                        {
                            pBuf = a[0].ptr;
                            bsz = GetBufferSize(a[0].ptr);
                        }
                        else if (a[0].t == Value::Num)
                        {
                            pBuf = (const void*)(INT_PTR)(long long)a[0].n;
                        }
                        else if (a[0].t == Value::Str)
                        {
                            pBuf = a[0].s.data();
                            bsz = a[0].s.size();
                        }
                        if (!pBuf) return Value();
                        size_t want = bsz;
                        if (a.size() > 1 && a[1].t == Value::Num)
                            want = (size_t)(long long)a[1].n;
                        if (want == 0) return Value::Num_(0);
                        size_t written = fwrite(pBuf, 1, want, st->f);
                        return Value::Num_((double)written);
                    });
                    tb2->map[L"readLine"] = tb2->map[L"read"];
                    // readback([bytes]) — read backward from current position.
                    // If bytes is omitted or null: read the preceding line up to previous newline;
                    // If bytes > 0: read preceding `bytes` bytes and move position backward;
                    // If bytes < 0: seek to absolute position -bytes and read to original position.
                    tb2->map[L"readback"] = NF([st, ioFileBytesToValue, ioFileFail](vector<Value>& a) -> Value {
                        if (!ioFileOpen(st)) return ioFileFail(L"文件已关闭", 9);
                        if (st->bMem)
                        {
                            long long cur = (long long)st->memPos;
                            if (cur <= 0) return Value();
                            long long startPos = 0;
                            if (a.empty() || a[0].t == Value::Null)
                            {
                                long long p = cur;
                                if (p > 0 && st->mem[p - 1] == '\n')
                                {
                                    p--;
                                    if (p > 0 && st->mem[p - 1] == '\r') p--;
                                }
                                while (p > 0 && st->mem[p - 1] != '\n') p--;
                                startPos = p;
                            }
                            else if (a[0].t == Value::Num)
                            {
                                long long n = (long long)BNum(a[0]);
                                if (n > 0) startPos = (cur > n) ? (cur - n) : 0;
                                else if (n < 0) startPos = -n;
                                if (startPos >= cur) return Value();
                            }
                            else return Value();
                            size_t want = (size_t)(cur - startPos);
                            if (want == 0) return Value();
                            std::string raw(st->mem.data() + startPos, want);
                            st->memPos = (size_t)startPos;
                            return ioFileBytesToValue(raw);
                        }
                        long long cur = _ftelli64(st->f);
                        if (cur <= 0) return Value();
                        long long startPos = 0;
                        if (a.empty() || a[0].t == Value::Null)
                        {
                            // line backward: scan backward for newline
                            long long p = cur;
                            // if immediately preceded by \r\n or \n, skip trailing newline
                            if (p > 0)
                            {
                                unsigned char buf[2] = {0, 0};
                                _fseeki64(st->f, p - 1, SEEK_SET);
                                if (fread(&buf[0], 1, 1, st->f) == 1 && buf[0] == '\n')
                                {
                                    p--;
                                    if (p > 0)
                                    {
                                        _fseeki64(st->f, p - 1, SEEK_SET);
                                        if (fread(&buf[1], 1, 1, st->f) == 1 && buf[1] == '\r')
                                            p--;
                                    }
                                }
                            }
                            // now find previous \n or beginning of file
                            long long lineEnd = p;
                            while (p > 0)
                            {
                                p--;
                                _fseeki64(st->f, p, SEEK_SET);
                                unsigned char ch = 0;
                                if (fread(&ch, 1, 1, st->f) == 1 && ch == '\n')
                                {
                                    p++; // line starts after newline
                                    break;
                                }
                            }
                            startPos = p;
                            size_t want = (size_t)(lineEnd - startPos);
                            std::string raw;
                            if (want > 0)
                            {
                                raw.resize(want);
                                _fseeki64(st->f, startPos, SEEK_SET);
                                fread(&raw[0], 1, want, st->f);
                            }
                            _fseeki64(st->f, startPos, SEEK_SET);
                            return ioFileBytesToValue(raw);
                        }
                        else if (a[0].t == Value::Num)
                        {
                            long long n = (long long)BNum(a[0]);
                            if (n > 0)
                            {
                                startPos = (cur > n) ? (cur - n) : 0;
                                size_t want = (size_t)(cur - startPos);
                                std::string raw(want, 0);
                                _fseeki64(st->f, startPos, SEEK_SET);
                                fread(&raw[0], 1, want, st->f);
                                _fseeki64(st->f, startPos, SEEK_SET);
                                return ioFileBytesToValue(raw);
                            }
                            else if (n < 0)
                            {
                                startPos = -n;
                                if (startPos >= cur)
                                {
                                    _fseeki64(st->f, cur, SEEK_SET);
                                    return Value();
                                }
                                size_t want = (size_t)(cur - startPos);
                                std::string raw(want, 0);
                                _fseeki64(st->f, startPos, SEEK_SET);
                                fread(&raw[0], 1, want, st->f);
                                _fseeki64(st->f, startPos, SEEK_SET);
                                return ioFileBytesToValue(raw);
                            }
                        }
                        return Value();
                    });
                    // write(...) — text encodes UTF-8; byte strings and
                    // structs write raw. Returns true, or null,errmsg,code.
                    tb2->map[L"write"] = NF([st, ioFileArgBytes, ioFileFail](
                            vector<Value>& a) -> Value {
                        if (!ioFileOpen(st)) return ioFileFail(L"文件已关闭", 9);
                        if (st->bMem) return ioFileFail(L"载荷资源为只读", 5);
                        std::string out;
                        for (auto& v : a)
                            if (!ioFileArgBytes(v, out))
                                return ioFileFail(L"参数无法序列化", 87);
                        if (st->bText) ioFileWriteTranslate(out);
                        if (!out.empty() &&
                            fwrite(out.data(), 1, out.size(), st->f) !=
                                out.size())
                            return ioFileFail(L"写入失败", errno);
                        return Value::Bool_(true);
                    });
                    tb2->map[L"writeLine"] =
                        NF([st, ioFileArgBytes, ioFileFail](
                               vector<Value>& a) -> Value {
                            if (!ioFileOpen(st)) return ioFileFail(L"文件已关闭", 9);
                            if (st->bMem) return ioFileFail(L"载荷资源为只读", 5);
                            std::string out;
                            for (auto& v : a)
                                if (!ioFileArgBytes(v, out))
                                    return ioFileFail(L"参数无法序列化", 87);
                            if (st->bText) ioFileWriteTranslate(out);
                            out += "\r\n";
                            if (fwrite(out.data(), 1, out.size(), st->f) !=
                                out.size())
                                return ioFileFail(L"写入失败", errno);
                            return Value::Bool_(true);
                        });
                    tb2->map[L"flush"] = NF([st, ioFileFail](
                            vector<Value>&) -> Value {
                        if (!ioFileOpen(st)) return ioFileFail(L"文件已关闭", 9);
                        if (st->bMem) return Value::Bool_(true);
                        return fflush(st->f) == 0
                            ? Value::Bool_(true)
                            : ioFileFail(L"刷新失败", errno);
                    });
                    tb2->map[L"close"] = NF([st](vector<Value>&) -> Value {
                        if (st->f) { fclose(st->f); st->f = nullptr; }
                        st->bClosed = true;
                        return Value::Bool_(true);
                    });
                    tb2->map[L"tell"] = NF([st](vector<Value>&) -> Value {
                        long long p = ioFilePos(st);
                        return p >= 0 ? Value::Num_((double)p) : Value();
                    });
                    // seek(whence, offset) — "set"/"cur"/"end" or 0/1/2;
                    // returns the new position
                    tb2->map[L"seek"] = NF([st](vector<Value>& a) -> Value {
                        int origin = SEEK_SET;
                        if (!a.empty() && a[0].t == Value::Str)
                        {
                            wstring w = a[0].s;
                            for (auto& c : w) c = towlower(c);
                            if (w == L"cur") origin = SEEK_CUR;
                            else if (w == L"end") origin = SEEK_END;
                        }
                        else if (!a.empty() && a[0].t == Value::Num)
                        {
                            long long w = (long long)BNum(a[0]);
                            origin = w == 1 ? SEEK_CUR
                                            : (w == 2 ? SEEK_END : SEEK_SET);
                        }
                        long long off =
                            a.size() > 1 ? (long long)BNum(a[1]) : 0;
                        if (st->bMem)
                        {
                            long long base = (origin == SEEK_CUR)
                                ? ioFilePos(st)
                                : (origin == SEEK_END
                                        ? (long long)st->mem.size() : 0);
                            long long p = base + off;
                            if (p < 0) return Value();
                            st->memPos = (size_t)p;
                            return Value::Num_((double)p);
                        }
                        if (!st->f) return Value();
                        if (_fseeki64(st->f, off, origin) != 0) return Value();
                        long long p = _ftelli64(st->f);
                        return p >= 0 ? Value::Num_((double)p) : Value();
                    });
                    tb2->map[L"size"] = NF([st](vector<Value>&) -> Value {
                        if (st->bMem) return Value::Num_((double)st->mem.size());
                        if (!st->f) return Value();
                        long long cur = _ftelli64(st->f);
                        _fseeki64(st->f, 0, SEEK_END);
                        long long end = _ftelli64(st->f);
                        _fseeki64(st->f, cur, SEEK_SET);
                        return Value::Num_((double)end);
                    });
                    tb2->map[L"eof"] = NF([st](vector<Value>&) -> Value {
                        if (!ioFileOpen(st)) return Value::Bool_(true);
                        if (st->bMem)
                            return Value::Bool_(st->memPos >= st->mem.size());
                        return Value::Bool_(feof(st->f) != 0);
                    });
                    // mode(flags) — _O_TEXT(0x4000) / _O_BINARY(0x8000)
                    tb2->map[L"mode"] = NF([st](vector<Value>& a) -> Value {
                        if (!st->f) return Value();
                        int flags = a.empty() ? 0 : (int)BNum(a[0]);
                        int prev = _setmode(_fileno(st->f), flags);
                        return prev >= 0 ? Value::Num_((double)prev)
                                         : Value();
                    });
                    return Value::Table_(tb2);
                };
                // ctor: path or OS handle + CRT mode string. io.open() with
                // no args stays the console library's "is a console
                // attached" query.
                auto ioFileCtor = NF([ioFileMake](vector<Value>& a) -> Value {
                    if (a.size() < 2 || a[0].t == Value::Null)
                        return Value::Bool_(GetConsoleWindow() != nullptr);
                    wstring mode =
                        (a[1].t == Value::Str && !a[1].s.empty()) ? a[1].s
                                                                  : L"r";
                    FILE* pf = nullptr;
                    void* hOs = nullptr;
                    if (a[0].t == Value::Pointer)
                        hOs = a[0].ptr;
                    else if (a[0].t == Value::Num)
                        hOs = (void*)(INT_PTR)(long long)BNum(a[0]);
                    // every fd is binary — sanitize the mode string first:
                    // drop 't', append 'b' when absent (_fdopen's mode
                    // string overrides the _open_osfhandle text/binary
                    // flag, so "r" would silently re-enable text mode and
                    // block short reads on live pipes)
                    wstring m2 = mode;
                    if (m2.find(L'b') == wstring::npos)
                    {
                        wstring m3;
                        for (wchar_t c : m2)
                            if (c != L't') m3.push_back(c);
                        m3.push_back(L'b');
                        m2 = m3;
                    }
                    // Official io.file default is TEXT mode ('b' opts out):
                    // the CRT fd stays binary (pipes), translation is done
                    // per-call in the byte layer (ioFileTextKeep /
                    // ioFileWriteTranslate). Text mode applies to DISK
                    // files and payload resources; files opened from a raw
                    // OS handle (process.popen pipes, console streams) stay
                    // byte-exact — JSON-RPC framing (LSP/DAP over popen)
                    // depends on Content-Length counting raw bytes.
                    bool bTextMode = (mode.find(L'b') == wstring::npos) &&
                                     !hOs;
                    if (hOs)
                    {
                        // wrap an OS handle: the CRT fd takes ownership
                        // (closing the file closes the handle) — matches
                        // io.file semantics for popen pipes.
                        // CRT text mode is fatal on live pipes: a short
                        // translated read loops on ReadFile until EOF, so
                        // every fd stays binary at the CRT level; the
                        // official io.file text-mode translation (default
                        // 't') is done per-call in the byte layer above —
                        // popen pipes opened through io.file keep exact
                        // byte semantics by opening with 'b'.
                        int access = _O_RDONLY;
                        if (m2.find(L'+') != wstring::npos)
                            access = _O_RDWR;
                        else if (m2.find(L'w') != wstring::npos ||
                                 m2.find(L'a') != wstring::npos)
                            access = _O_WRONLY;
                        int fd = _open_osfhandle((intptr_t)hOs, _O_BINARY |
                                                 _O_NOINHERIT | access);
                        if (fd < 0) return Value();
                        std::string mA;
                        for (wchar_t c : m2) mA.push_back((char)c);
                        pf = _fdopen(fd, mA.c_str());
                        if (!pf)
                        {
                            _close(fd);
                            return Value();
                        }
                    }
                    else
                    {
                        // published payload: the embedded map IS the exe's
                        // virtual file system — a resource shipped inside
                        // the payload has no disk file, so it is opened from
                        // its bytes. Payload resources are read-only.
                        wstring srcPath = a[0].ToString();
                        wstring key = EmbeddedPayloadKey(srcPath);
                        if (!key.empty() && m2.find(L'w') == wstring::npos &&
                            m2.find(L'a') == wstring::npos &&
                            m2.find(L'+') == wstring::npos)
                        {
                            auto st = std::make_shared<IoFileState>();
                            auto eit = g_interpCurrent->embedded.find(key);
                            const wstring& bytes = eit->second;
                            st->mem.resize(bytes.size());
                            for (size_t k = 0; k < bytes.size(); k++)
                                st->mem[k] = (char)(unsigned char)bytes[k];
                            st->bMem = true;
                            st->bText = bTextMode;
                            return ioFileMake(st);
                        }
                        wstring path = ResolveTildeDisk(srcPath);
                        if (_wfopen_s(&pf, path.c_str(), m2.c_str()) != 0)
                            pf = nullptr;
                        if (!pf) return Value();
                    }
                    auto st = std::make_shared<IoFileState>();
                    st->f = pf;
                    st->bText = bTextMode;
                    return ioFileMake(st);
                });
                auto fileCls = std::make_shared<Table>();
                fileCls->map[L"ctor"] = ioFileCtor;
                Value vFileClass;
                vFileClass.t = Value::Class;
                vFileClass.fnData = std::make_shared<FuncData>();
                vFileClass.tab = fileCls;
                tb->map[L"file"] = vFileClass;
                tb->map[L"open"] = vFileClass; // legacy alias, same class
                // std streams — binary dups of fd 0/1/2; we own the dup,
                // the process keeps the original
                auto ioStdWrap = [ioFileMake](int nFd, const char* m) -> Value {
                    auto st = std::make_shared<IoFileState>();
                    int fd = _dup(nFd);
                    if (fd >= 0)
                    {
                        st->f = _fdopen(fd, m);
                        if (!st->f) _close(fd);
                    }
                    return ioFileMake(st);
                };
                tb->map[L"stdout"] = ioStdWrap(1, "wb");
                tb->map[L"stderr"] = ioStdWrap(2, "wb");
                Value vStdin = ioStdWrap(0, "rb");
                tb->map[L"stdin"] = vStdin;
                // io.lines(pathOrFile = io.stdin)
                //
                // 官方契约（docs/library-guide/builtin/io/lines.md）：
                //   自动打开 @pathOrFile 指定的文件（文本模式，'\0'/'\x1A' 为终止符），
                //   创建一个**迭代器**，在泛型 for 循环中逐行读取文件，
                //   在读取完毕以后**自动关闭**文件对象。
                //   @pathOrFile 也可以是 io.file 打开的文件对象；省略则用 io.stdin。
                //
                // 官方库的实际用法（lib/fsys/update/app.aardio、lib/debug/_.aardio、
                // lib/fsys/cookies.aardio 等）都是 `for path in io.lines(文件路径)`，
                // 每行是一个不含行尾的路径字符串。
                // io.libpath(模块名) -> 库文件路径, 所在目录
                //
                // 官方用法（lib/autos/skills/_.aardio、lib/py2/.build/main.aardio、
                // lib/ide/_.aardio、lib/ide/doc/library.aardio 等）：
                //   var path,dir = ..io.libpath(libName);
                //   path = 库文件的完整路径；dir = 该文件所在目录
                //   官方也用 `if(..io.libpath(x))` 判存在 —— 找不到时返回 null。
                //
                // 解析走的是**与 import 完全同一套**查找顺序
                // （Interp::ResolveModulePath），不另造一套。
                tb->map[L"libpath"] = NF([](vector<Value>& a) -> Value {
                    wstring name = a.empty() ? wstring() : BStr(a[0]);
                    if (name.empty()) return Value();
                    // 模块名按 '.' 拆成名字段
                    vector<wstring> names;
                    size_t pos = 0;
                    for (;;)
                    {
                        size_t dot = name.find(L'.', pos);
                        if (dot == wstring::npos)
                        {
                            names.push_back(name.substr(pos));
                            break;
                        }
                        names.push_back(name.substr(pos, dot - pos));
                        pos = dot + 1;
                    }
                    Interp* it = g_interpCurrent;
                    if (!it) return Value();
                    wstring file;
                    if (!it->ResolveModulePath(names, &file)) return Value();
                    // 目录 = 去掉最后的文件名
                    wstring dir = file;
                    size_t sl = dir.find_last_of(L"\\/");
                    if (sl != wstring::npos) dir = dir.substr(0, sl);
                    Value v = Value::Str_(file);
                    auto ex = std::make_shared<vector<Value>>();
                    ex->push_back(Value::Str_(dir));
                    v.mrets = ex;
                    return v; });
                tb->map[L"lines"] = NF([ioFileCtor, vStdin](vector<Value>& a) -> Value {
                    Value fileObj;
                    if (a.empty() || a[0].t == Value::Null)
                    {
                        fileObj = vStdin;
                    }
                    else if (a[0].t == Value::TableT)
                    {
                        fileObj = a[0];
                    }
                    else
                    {
                        vector<Value> ca;
                        ca.push_back(a[0]);
                        ca.push_back(Value::Str_(L"rb"));
                        fileObj = ioFileCtor.native(ioFileCtor.nativeCtx.get(), ca);
                        if (fileObj.t != Value::TableT || !fileObj.tab)
                        {
                            Interp* it = g_interpCurrent;
                            if (it) *it->err = L"运行错误: 无法打开文件 " + BStr(a[0]);
                            return Value();
                        }
                    }
                    if (!ioFileStateOf(fileObj))
                    {
                        Interp* it = g_interpCurrent;
                        if (it) *it->err = L"运行错误: io.lines 需要文件路径或文件对象";
                        return Value();
                    }
                    // 迭代器：每次返回一行（不含行尾 CR/LF），读完返回 null 结束 for
                    return Value::Native_([fileObj](vector<Value>&) -> Value {
                        IoFileState* st = ioFileStateOf(fileObj);
                        wstring line;
                        if (!ioFileReadLineRaw(st, line))
                        {
                            ioFileCloseRaw(st); // 官方：读取完毕以后自动关闭
                            return Value();
                        }
                        return Value::Str_(line);
                    });
                });
                // io.exist / io.remove keep their official contracts and
                // are registered further down (exist returns the absolute
                // path; remove deletes); not duplicated here.
                // io.readFile(path) — convenience wrapper
                // round 43: optional encoding arg — "utf8" (default, with
                // BOM tolerance), "gbk"/"ansi"/numeric codepage, or "raw"
                // (byte string: one char per raw byte)
                tb->map[L"readFile"] = NF([](vector<Value>& a) {
                    wstring s3;
                    wstring enc3 = a.size() > 1 && a[1].t != Value::Null
                        ? BStr(a[1]) : L"utf8";
                    // published payload — the embedded map IS the exe's
                    // virtual file system (see EmbeddedPayloadKey)
                    bool bHit = false;
                    if (g_interpCurrent && a[0].t == Value::Str)
                    {
                        wstring key = EmbeddedPayloadKey(BStr(a[0]));
                        if (!key.empty())
                        {
                            bHit = true;
                            // The embedded map already stores payload bytes
                            // one-per-wchar (the engine's byte-string
                            // convention), so "raw" is a straight copy —
                            // re-encoding through UTF-8 would inflate
                            // multi-byte sequences and corrupt binaries.
                            s3 = g_interpCurrent->embedded.find(key)->second;
                            if (enc3 == L"raw")
                            {
                                Value ret = Value::Str_(s3);
                                ret.bRawBytes = true;
                                return ret;
                            }
                        }
                    }
                    if (!bHit)
                    {
                        // "~" = the install root (parent of
                        // NODIO_LIBDIR when set, else the exe's directory)
                        wstring strPath = ResolveTildeDisk(BStr(a[0]));
                        FILE* pf = nullptr;
                        if (_wfopen_s(&pf, strPath.c_str(), L"rb") != 0 ||
                            !pf)
                            return Value();
                        std::string raw3;
                        char buf3[4096]; size_t rd3;
                        while ((rd3 = fread(buf3, 1, sizeof(buf3), pf)) > 0)
                            raw3.append(buf3, rd3);
                        fclose(pf);
                        if (enc3 == L"raw")
                        {
                            s3.resize(raw3.size());
                            for (size_t k = 0; k < raw3.size(); k++)
                                s3[k] =
                                    (wchar_t)(unsigned char)raw3[k];
                            Value ret = Value::Str_(s3);
                            ret.bRawBytes = true;
                            return ret;
                        }
                        UINT cp3 = CodepageOf(a.size() > 1 ? a[1] : Value());
                        size_t off3 = 0;
                        if (enc3 == L"utf8" && raw3.size() >= 3 &&
                            (unsigned char)raw3[0] == 0xEF &&
                            (unsigned char)raw3[1] == 0xBB &&
                            (unsigned char)raw3[2] == 0xBF)
                            off3 = 3;    // strip UTF-8 BOM before decoding
                        int w3 = MultiByteToWideChar(cp3, 0,
                            raw3.data() + off3,
                            (int)(raw3.size() - off3), nullptr, 0);
                        s3.resize((size_t)(w3 > 0 ? w3 : 0));
                        if (w3 > 0)
                            MultiByteToWideChar(cp3, 0, raw3.data() + off3,
                                (int)(raw3.size() - off3), &s3[0], w3);
                    }
                    return Value::Str_(s3); });
                // io.fullpath(path) — convert a relative path to an
                // absolute system path:
                //   '~…'      → EXE/installation root
                //   '/…' '\…' → 应用程序根目录 (the running project's dir)
                //   '//…'     → the first slash is dropped (drive root)
                //   '\\…'     → UNC, returned as null
                //   otherwise → resolved against the current directory
                tb->map[L"fullpath"] = NF([](vector<Value>& a) -> Value {
                    wstring rel = a.empty() ? L"" : BStr(a[0]);
                    if (!rel.empty() && rel[0] == L'~')
                    {
                        wstring resolved = ResolveTildeDisk(rel);
                        wchar_t szFull[MAX_PATH * 2] = L"";
                        DWORD nFull = GetFullPathNameW(resolved.c_str(), MAX_PATH * 2,
                                                       szFull, nullptr);
                        if (nFull > 0 && nFull < MAX_PATH * 2)
                            return Value::Str_(szFull);
                        return Value::Str_(resolved);
                    }
                    // app root: the running script's directory (a published
                    // exe falls back to its own directory)
                    wstring appRoot;
                    if (g_interpCurrent && !g_interpCurrent->baseDir.empty())
                        appRoot = g_interpCurrent->baseDir;
                    if (appRoot.empty())
                    {
                        wchar_t szExe[MAX_PATH] = L"";
                        GetModuleFileNameW(nullptr, szExe, MAX_PATH);
                        wchar_t* pSep = wcsrchr(szExe, L'\\');
                        if (pSep) *pSep = 0;
                        appRoot = szExe;
                    }
                    if (rel.size() >= 2 &&
                        (rel[0] == L'/' || rel[0] == L'\\') &&
                        (rel[1] == L'/' || rel[1] == L'\\'))
                    {
                        // '\\' (UNC) is not converted; '//' drops one slash
                        if (rel[0] == L'\\')
                            return Value();
                        rel = rel.substr(1);
                    }
                    else if (!rel.empty() &&
                             (rel[0] == L'/' || rel[0] == L'\\'))
                    {
                        wstring rest = rel.substr(1);
                        while (!rest.empty() &&
                               (rest[0] == L'/' || rest[0] == L'\\'))
                            rest = rest.substr(1);
                        if (rest.empty())
                            return Value::Str_(appRoot + L"\\");
                        return Value::Str_(appRoot + L"\\" + rest);
                    }
                    wchar_t szFull[MAX_PATH * 2] = L"";
                    DWORD nFull = GetFullPathNameW(rel.c_str(), MAX_PATH * 2,
                                                   szFull, nullptr);
                    if (nFull > 0 && nFull < MAX_PATH * 2)
                        return Value::Str_(szFull);
                    return Value::Str_(rel); });
                // io.remove(path) — delete a file (an empty directory also
                // works, which makes it forgiving of fsys-level callers)
                tb->map[L"remove"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty() || a[0].t != Value::Str)
                        return Value::Bool_(false);
                    wstring p = ResolveTildeDisk(BStr(a[0]));
                    if (p.empty())
                        return Value::Bool_(false);
                    // a trailing separator would defeat DeleteFileW
                    while (p.size() > 3 &&
                           (p.back() == L'\\' || p.back() == L'/') &&
                           p[p.size() - 2] != L':')
                        p.pop_back();
                    if (DeleteFileW(p.c_str()))
                        return Value::Bool_(true);
                    if (RemoveDirectoryW(p.c_str()))
                        return Value::Bool_(true);
                    return Value::Bool_(false); });
                // io.rename(oldPath, newPath)
                tb->map[L"rename"] = NF([](vector<Value>& a) -> Value {
                    if (a.size() < 2 || a[0].t != Value::Str || a[1].t != Value::Str)
                        return Value::Bool_(false);
                    wstring src = ResolveTildeDisk(BStr(a[0]));
                    wstring dst = ResolveTildeDisk(BStr(a[1]));
                    if (src.empty() || dst.empty())
                        return Value::Bool_(false);
                    if (MoveFileExW(src.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED))
                        return Value::Bool_(true);
                    return Value::Bool_(false); });
                // io.exist(path,mode) — docs/builtin/io/path.md: returns the
                // ABSOLUTE path when the file exists, otherwise null. The
                // return value is a path (not a boolean), which is what the
                // library idiom `io.exist(p) : fallback` relies on
                // (zlib/nodeJs/golang/py2/py3 resolve their bundled DLL this
                // way). mode: 2 = writable, 4 = readable, 6 = both; a
                // directory only satisfies the default mode.
                // io.localpath(url) — a file:/// URL (or file:// URL)
                // becomes the local filesystem path with '/' -> '\' and
                // %XX escapes decoded; anything that is not a file URL or
                // an existing plain path form passes through as-is when it
                // is relative, http(s):// and other schemes return null
                // io.localpath(path) — path specification:
                // Only converts path starting with single '\', '/', or '~' to absolute path.
                // - Starts with '\\': returns null
                // - Starts with '//': removes first slash
                // - Starts with single '\' or '/': resolves relative to application base directory
                // - Starts with '~/' or '~\' (or single '~'): resolves relative to EXE / root directory
                // - Other formats do not convert, returns null.
                tb->map[L"localpath"] = NF([](vector<Value>& a) -> Value {
                    wstring s = a.empty() ? L"" : BStr(a[0]);
                    if (s.empty()) return Value();

                    auto ieq = [](const wchar_t* p, const wchar_t* pre) {
                        while (*pre)
                            if (towlower(*p++) != towlower(*pre++))
                                return false;
                        return true; };
                    bool bFileUrl = false;
                    if (ieq(s.c_str(), L"file:///"))
                    {
                        s = s.substr(8);
                        bFileUrl = true;
                    }
                    else if (ieq(s.c_str(), L"file://"))
                    {
                        s = s.substr(7);
                        bFileUrl = true;
                    }
                    if (bFileUrl)
                    {
                        for (auto& ch : s)
                            if (ch == L'/') ch = L'\\';
                        return Value::Str_(s);
                    }

                    if (s.size() >= 2 && s[0] == L'\\' && s[1] == L'\\')
                        return Value(); // UNC or \\ starts with double backslash -> null
                    if (s.size() >= 2 && s[0] == L'/' && s[1] == L'/')
                        return Value::Str_(s.substr(1));

                    if (!s.empty() && s[0] == L'~')
                        return Value::Str_(ResolveTildeDisk(s));

                    if (!s.empty() && (s[0] == L'/' || s[0] == L'\\'))
                    {
                        wstring appRoot;
                        if (g_interpCurrent && !g_interpCurrent->baseDir.empty())
                            appRoot = g_interpCurrent->baseDir;
                        if (appRoot.empty())
                        {
                            wchar_t szExe[MAX_PATH] = L"";
                            GetModuleFileNameW(nullptr, szExe, MAX_PATH);
                            wchar_t* pSep = wcsrchr(szExe, L'\\');
                            if (pSep) *pSep = 0;
                            appRoot = szExe;
                        }
                        wstring rest = s.substr(1);
                        while (!rest.empty() && (rest[0] == L'/' || rest[0] == L'\\'))
                            rest = rest.substr(1);
                        if (rest.empty())
                            return Value::Str_(appRoot + L"\\");
                        return Value::Str_(appRoot + L"\\" + rest);
                    }

                    return Value(); });
                tb->map[L"exist"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty() || a[0].t != Value::Str)
                        return Value();      // null
                    wstring strPath = BStr(a[0]);
                    if (strPath.empty())
                        return Value();
                    int mode = (a.size() > 1 && a[1].t == Value::Num)
                                   ? (int)a[1].n : 0;
                    // embedded payload of a published exe: it ships as data
                    if (g_interpCurrent)
                    {
                        wstring key = strPath;
                        if (!key.empty() && key[0] == L'~')
                            key = key.substr(1);
                        if (!key.empty() &&
                            (key[0] == L'\\' || key[0] == L'/'))
                            key = key.substr(1);
                        if (mode == 0 &&
                            (g_interpCurrent->embedded.count(key) ||
                             (key != strPath &&
                              g_interpCurrent->embedded.count(strPath))))
                            return Value::Str_(ResolveTildeDisk(strPath));
                    }
                    wstring full;
                    if (!strPath.empty() && strPath[0] == L'~')
                    {
                        full = ResolveTildeDisk(strPath);
                        wchar_t szFull[MAX_PATH * 2] = L"";
                        DWORD nFull = GetFullPathNameW(full.c_str(),
                                                      MAX_PATH * 2, szFull,
                                                      nullptr);
                        if (nFull > 0 && nFull < MAX_PATH * 2)
                            full = szFull;
                    }
                    else if (!strPath.empty() && (strPath[0] == L'/' || strPath[0] == L'\\') &&
                             !(strPath.size() >= 2 && strPath[0] == L'\\' && strPath[1] == L'\\'))
                    {
                        wstring appRoot;
                        if (g_interpCurrent && !g_interpCurrent->baseDir.empty())
                            appRoot = g_interpCurrent->baseDir;
                        if (appRoot.empty())
                        {
                            wchar_t szExe[MAX_PATH] = L"";
                            GetModuleFileNameW(nullptr, szExe, MAX_PATH);
                            wchar_t* pSep = wcsrchr(szExe, L'\\');
                            if (pSep) *pSep = 0;
                            appRoot = szExe;
                        }
                        wstring rest = strPath.substr(1);
                        while (!rest.empty() && (rest[0] == L'/' || rest[0] == L'\\'))
                            rest = rest.substr(1);
                        full = rest.empty() ? (appRoot + L"\\") : (appRoot + L"\\" + rest);
                    }
                    else
                    {
                        full = ResolveTildeDisk(strPath);
                        wchar_t szFull[MAX_PATH * 2] = L"";
                        DWORD nFull = GetFullPathNameW(full.c_str(),
                                                      MAX_PATH * 2, szFull,
                                                      nullptr);
                        if (nFull > 0 && nFull < MAX_PATH * 2)
                            full = szFull;
                    }
                    int acc = 0;
                    if (mode == 2 || mode == 6)
                        acc |= 2;
                    if (mode == 4 || mode == 6)
                        acc |= 4;
                    if (_waccess(full.c_str(), acc) != 0)
                        return Value();      // null
                    DWORD attr = GetFileAttributesW(full.c_str());
                    if (attr != INVALID_FILE_ATTRIBUTES &&
                        (attr & FILE_ATTRIBUTE_DIRECTORY) && mode != 0)
                        return Value();      // a directory is not a file
                    return Value::Str_(full); });

            // ---- io.listFiles(path, mask) — directory listing ----
            // Returns a table carrying an array of {name; path; isDir; size}.
            // mask defaults to "*.*"; "." and ".." are skipped.
            tb->map[L"listFiles"] = NF([](vector<Value>& a) -> Value {
                if (a.empty() || a[0].t != Value::Str) return Value();
                wstring dir = BStr(a[0]);
                if (dir.empty()) return Value();
                if (dir.back() != L'\\' && dir.back() != L'/') dir += L"\\";
                wstring mask = (a.size() > 1 && a[1].t == Value::Str)
                                   ? BStr(a[1]) : L"*.*";
                if (mask.empty()) mask = L"*.*";
                WIN32_FIND_DATAW fd = { 0 };
                HANDLE h = FindFirstFileW((dir + mask).c_str(), &fd);
                if (h == INVALID_HANDLE_VALUE) return Value();
                auto out = std::make_shared<Table>();
                do
                {
                    wstring name = fd.cFileName;
                    if (name == L"." || name == L"..") continue;
                    auto it = std::make_shared<Table>();
                    it->map[L"name"] = Value::Str_(name);
                    it->map[L"path"] = Value::Str_(dir + name);
                    it->map[L"isDir"] = Value::Bool_(
                        (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
                    it->map[L"size"] = Value::Num_(
                        (double)(((LONGLONG)fd.nFileSizeHigh << 32) |
                                  (LONGLONG)fd.nFileSizeLow));
                    out->arr.push_back(Value::Table_(it));
                } while (FindNextFileW(h, &fd));
                FindClose(h);
                return Value::Table_(out); });

            // ---- io.getSize(path) — file size in bytes ----
            tb->map[L"getSize"] = NF([](vector<Value>& a) -> Value {
                if (a.empty() || a[0].t != Value::Str) return Value();
                wstring path = BStr(a[0]);
                if (path.empty()) return Value();
                WIN32_FILE_ATTRIBUTE_DATA fad = { 0 };
                if (!GetFileAttributesExW(path.c_str(),
                                          GetFileExInfoStandard, &fad))
                    return Value();
                LONGLONG nSize = ((LONGLONG)fad.nFileSizeHigh << 32) |
                                 (LONGLONG)fad.nFileSizeLow;
                return Value::Num_((double)nSize); });
                tb->map[L"writeFile"] = NF([](vector<Value>& a) {
                    FILE* pf = nullptr;
                    if (_wfopen_s(&pf, BStr(a[0]).c_str(), L"wb") != 0 || !pf)
                        return Value::Bool_(false);
                    wstring s3 = BStr(a[1]);
                    wstring enc3 = a.size() > 2 && a[2].t != Value::Null
                        ? BStr(a[2]) : L"utf8";
                    std::string out3;
                    if (enc3 == L"raw")
                    {
                        out3.resize(s3.size());
                        for (size_t k = 0; k < s3.size(); k++)
                            out3[k] = (char)(unsigned char)s3[k];
                    }
                    else
                    {
                        UINT cp3 = CodepageOf(a.size() > 2 ? a[2]
                                                           : Value());
                        int n3 = WideCharToMultiByte(cp3, 0, s3.c_str(),
                                                     (int)s3.size(), nullptr,
                                                     0, nullptr, nullptr);
                        out3.resize((size_t)(n3 > 0 ? n3 : 0));
                        if (n3 > 0)
                            WideCharToMultiByte(cp3, 0, s3.c_str(),
                                                (int)s3.size(), &out3[0],
                                                n3, nullptr, nullptr);
                    }
                    fwrite(out3.data(), 1, out3.size(), pf);
                    fclose(pf);
                    return Value::Bool_(true); });
                tb->map[L"getText"] = NF([](vector<Value>& a) -> Value {
                    int maxLen = (a.size() > 0 && a[0].t == Value::Num) ? (int)a[0].n : 1024;
                    if (maxLen <= 0) maxLen = 1024;
                    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
                    DWORD mode = 0;
                    if (hIn && GetConsoleMode(hIn, &mode))
                    {
                        std::vector<wchar_t> buf(maxLen + 1, 0);
                        DWORD charsRead = 0;
                        if (ReadConsoleW(hIn, buf.data(), (DWORD)maxLen, &charsRead, nullptr) && charsRead > 0)
                        {
                            wstring s(buf.data(), charsRead);
                            while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n'))
                                s.pop_back();
                            return Value::Str_(s);
                        }
                    }
                    std::vector<wchar_t> buf(maxLen + 1, 0);
                    if (fgetws(buf.data(), maxLen, stdin))
                    {
                        wstring s(buf.data());
                        while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n'))
                            s.pop_back();
                        return Value::Str_(s);
                    }
                    return Value::Str_(L"");
                });
            }
            // ---- console ----
            {
                auto tb = GetRoot(L"console").tab;
                auto writer = [](vector<Value>& a, bool nl) -> Value {
                    // Strings are byte arrays in official aardio: console
                    // output writes the bytes verbatim.
                    std::string out;
                    for (size_t i = 0; i < a.size(); i++)
                    {
                        if (i) out += " ";
                        out += ValueByteForm(a[i]);
                    }
                    if (nl) out += "\n";
                    fwrite(out.data(), 1, out.size(), stdout);
                    fflush(stdout);
                    return Value();
                };
                tb->map[L"open"] = NF([](vector<Value>&) { return Value::Bool_(true); });
                tb->map[L"showLoading"] = NF([](vector<Value>&) { return Value(); });
                tb->map[L"close"] = NF([](vector<Value>&) { return Value(); });
                tb->map[L"log"] = NF([writer](vector<Value>& a) {
                    return writer(a, true); });
                tb->map[L"println"] = tb->map[L"log"];
                tb->map[L"print"] = NF([writer](vector<Value>& a) {
                    return writer(a, false); });
                tb->map[L"dump"] = NF([writer](vector<Value>& a) {
                    for (auto& v : a)
                    {
                        std::string out;
                        if (v.t == Value::TableT && v.tab)
                        {
                            out = "{ ";
                            for (auto& kv : v.tab->map)
                            {
                                out += Utf8Bytes(kv.first);
                                out += "=";
                                out += ValueByteForm(kv.second);
                                out += "; ";
                            }
                            out += "}";
                        }
                        else
                            out = ValueByteForm(v);
                        out += "\n";
                        fwrite(out.data(), 1, out.size(), stdout);
                    }
                    fflush(stdout);
                    return Value(); });
                tb->map[L"pause"] = NF([](vector<Value>&) {
                    WriteU8(stdout, L"请按任意键继续 . . .\n");
                    fflush(stdout);
                    if (fgetwc(stdin) == WEOF)
                    {
                        // stdin closed (pipe/headless): nothing to wait on
                    }
                    return Value(); });
                tb->map[L"open"] = NF([](vector<Value>&) { return Value(); });
                tb->map[L"close"] = NF([](vector<Value>&) { return Value(); });
            }
            // ---- time (round 30) ----
            {
                auto tb = GetRoot(L"time").tab;

                // ---- ParseTimeText ----
                // Official aardio parses a time from text with the SAME
                // format specifiers used to format it
                // (docs/library-guide/builtin/time/time.md):
                //   - the format's markers (%Y %m %d %H %M %S ...) pick the
                //     values out of the text, other characters are matched
                //     loosely;
                //   - a format that requires a date part the text does not
                //     provide makes the parse FAIL -> time() returns null;
                //   - a format that requires a time part the text does not
                //     provide also fails, unless lax was passed;
                //   - with no explicit format the default is
                //     "%Y/%m/%d %H:%M:%S" but the time part may be omitted.
                // Returns false when the text cannot supply the required parts.
                auto parseTimeText = [](const wstring& text, const wstring& fmtIn,
                                        int& y, int& mo, int& d,
                                        int& h, int& mi, int& sec,
                                        wstring& endstr) -> bool {
                    wstring fmt = fmtIn;
                    if (!fmt.empty() && fmt[0] == L'!') fmt = fmt.substr(1);
                    struct Marker { wchar_t code; int* val; int maxDigits; };
                    std::vector<Marker> markers;
                    for (size_t i = 0; i < fmt.size(); i++)
                    {
                        if (fmt[i] != L'%') continue;
                        if (i + 1 >= fmt.size()) break;
                        wchar_t c = fmt[++i];
                        if (c == L'%') continue;
                        switch (c)
                        {
                        case L'Y': markers.push_back({ c, &y, 4 }); break;
                        case L'y': markers.push_back({ c, &y, 2 }); break;
                        case L'm': markers.push_back({ c, &mo, 2 }); break;
                        case L'd': markers.push_back({ c, &d, 2 }); break;
                        case L'H': markers.push_back({ c, &h, 2 }); break;
                        case L'I': markers.push_back({ c, &h, 2 }); break;
                        case L'M': markers.push_back({ c, &mi, 2 }); break;
                        case L'S': markers.push_back({ c, &sec, 2 }); break;
                        case L'b': markers.push_back({ c, &mo, 0 }); break;
                        case L'B': markers.push_back({ c, &mo, 0 }); break;
                        default: break;   // %a %A %b %B %p %Z are optional
                        }
                    }
                    size_t pos = 0;
                    std::vector<char> done(markers.size(), 0);
                    for (size_t k = 0; k < markers.size(); k++)
                    {
                        if (done[k]) continue;
                        // ISO8601 compatibility: the date may be written
                        // without separators even when the format string
                        // spells the separators out ("20170822" with
                        // "%Y/%m/%d"). The date must be 8 digits (yyyymmdd)
                        // or 6 digits (yymmdd), and the month/day markers
                        // must follow.
                        if (markers[k].code == L'Y' &&
                            k + 2 < markers.size() &&
                            markers[k + 1].code == L'm' &&
                            markers[k + 2].code == L'd')
                        {
                            size_t n = 0;
                            while (pos + n < text.size() && n < 8 &&
                                   iswdigit(text[pos + n])) n++;
                            if (n == 8)
                            {
                                y = _wtoi(text.substr(pos, 4).c_str());
                                mo = _wtoi(text.substr(pos + 4, 2).c_str());
                                d = _wtoi(text.substr(pos + 6, 2).c_str());
                                pos += 8;
                                done[k + 1] = 1;
                                done[k + 2] = 1;
                                continue;
                            }
                            if (n == 6)
                            {
                                int yy = _wtoi(text.substr(pos, 2).c_str());
                                y = (yy < 70) ? 2000 + yy : 1900 + yy;
                                mo = _wtoi(text.substr(pos + 2, 2).c_str());
                                d = _wtoi(text.substr(pos + 4, 2).c_str());
                                pos += 6;
                                done[k + 1] = 1;
                                done[k + 2] = 1;
                                continue;
                            }
                        }
                        // a month name: %b (abbreviated) / %B (full)
                        if (markers[k].code == L'b' || markers[k].code == L'B')
                        {
                            static const wchar_t* kMon[12] = {
                                L"january", L"february", L"march", L"april",
                                L"may", L"june", L"july", L"august",
                                L"september", L"october", L"november",
                                L"december" };
                            while (pos < text.size() && !iswalpha(text[pos])) pos++;
                            size_t ns = pos;
                            while (ns < text.size() && iswalpha(text[ns])) ns++;
                            wstring low = text.substr(pos, ns - pos);
                            for (auto& c : low) c = towlower(c);
                            int mnum = 0;
                            for (int mi2 = 0; mi2 < 12; mi2++)
                            {
                                wstring full = kMon[mi2];
                                if (low.size() >= 3 && low.size() <= full.size() &&
                                    full.compare(0, low.size(), low) == 0)
                                {
                                    mnum = mi2 + 1;
                                    break;
                                }
                            }
                            if (!mnum) return false;
                            mo = mnum;
                            pos = ns;
                            continue;
                        }
                        // separators are matched loosely
                        while (pos < text.size() && !iswdigit(text[pos])) pos++;
                        if (pos >= text.size()) return false;
                        int v = 0, n = 0;
                        while (pos < text.size() && iswdigit(text[pos]) &&
                               n < markers[k].maxDigits)
                        {
                            v = v * 10 + (text[pos] - L'0');
                            pos++; n++;
                        }
                        if (n == 0) return false;
                        if (markers[k].code == L'y' && n <= 2)
                            v += (v < 70) ? 2000 : 1900;
                        *(markers[k].val) = v;
                    }
                    // official: skip leading blanks, then keep the run of
                    // non-blank characters that follows the last marker
                    while (pos < text.size() && iswspace(text[pos])) pos++;
                    size_t endPos = pos;
                    while (endPos < text.size() && !iswspace(text[endPos])) endPos++;
                    endstr = text.substr(pos, endPos - pos);
                    if (mo < 1 || mo > 12) return false;
                    if (d < 1 || d > 31) return false;
                    if (h < 0 || h > 23) return false;
                    if (mi < 0 || mi > 59) return false;
                    if (sec < 0 || sec > 59) return false;
                    return true;
                };
                // Candidate formats for one text: the format itself, then
                // the same format with the trailing time markers dropped
                // one by one ("%Y/%m/%d %H:%M:%S" -> "%Y/%m/%d %H:%M" ->
                // "%Y/%m/%d %H" -> "%Y/%m/%d"). Official: with lax the
                // trailing time parts may be omitted, and with no explicit
                // format the time part may always be omitted.
                auto timeFormatCandidates = [](const wstring& f)
                {
                    std::vector<wstring> out;
                    out.push_back(f);
                    std::vector<size_t> tpos;
                    for (size_t i = 0; i + 1 < f.size(); i++)
                    {
                        if (f[i] != L'%') continue;
                        wchar_t c = f[i + 1];
                        if (c == L'H' || c == L'I' || c == L'M' || c == L'S')
                            tpos.push_back(i);
                        i++;
                    }
                    for (size_t k = tpos.size(); k-- > 0; )
                        out.push_back(f.substr(0, tpos[k]));
                    return out;
                };

                auto timeMeta = std::make_shared<Table>();
                auto getSelf = [](const vector<Value>& a) -> Value {
                    Value self = Interp::GetCurrentOwner();
                    if (self.t == Value::TableT && self.tab) return self;
                    if (!a.empty() && a[0].t == Value::TableT && a[0].tab) return a[0];
                    return Value();
                };
                auto formatFunc = NF([getSelf](vector<Value>& a) -> Value {
                    Value self = getSelf(a);
                    if (!self.tab) return Value::Str_(L"");
                    wstring fmt = L"%Y/%m/%d %H:%M:%S";
                    if (!a.empty() && a[0].t == Value::Str)
                        fmt = a[0].s;
                    else if (a.size() > 1 && a[1].t == Value::Str)
                        fmt = a[1].s;
                    else
                    {
                        auto ifmt = self.tab->map.find(L"format");
                        if (ifmt != self.tab->map.end() && ifmt->second.t == Value::Str)
                            fmt = ifmt->second.s;
                    }
                    bool bUtc = false;
                    if (!fmt.empty() && fmt[0] == L'!')
                    {
                        bUtc = true;
                        fmt = fmt.substr(1);
                    }
                    if (fmt.find(L'%') == wstring::npos)
                    {
                        wstring converted;
                        for (size_t fi = 0; fi < fmt.size(); )
                        {
                            if (fmt.compare(fi, 4, L"yyyy") == 0) { converted += L"%Y"; fi += 4; }
                            else if (fmt.compare(fi, 2, L"yy") == 0) { converted += L"%y"; fi += 2; }
                            else if (fmt.compare(fi, 2, L"MM") == 0) { converted += L"%m"; fi += 2; }
                            else if (fmt.compare(fi, 2, L"dd") == 0) { converted += L"%d"; fi += 2; }
                            else if (fmt.compare(fi, 2, L"HH") == 0) { converted += L"%H"; fi += 2; }
                            else if (fmt.compare(fi, 2, L"mm") == 0) { converted += L"%M"; fi += 2; }
                            else if (fmt.compare(fi, 2, L"ss") == 0) { converted += L"%S"; fi += 2; }
                            else { converted += fmt[fi++]; }
                        }
                        fmt = converted;
                    }
                    tm tmb{};
                    tmb.tm_year = (int)(long long)BNum(self.tab->map[L"year"]) - 1900;
                    tmb.tm_mon = (int)(long long)BNum(self.tab->map[L"month"]) - 1;
                    tmb.tm_mday = (int)(long long)BNum(self.tab->map[L"day"]);
                    tmb.tm_hour = (int)(long long)BNum(self.tab->map[L"hour"]);
                    tmb.tm_min = (int)(long long)BNum(self.tab->map[L"minute"]);
                    tmb.tm_sec = (int)(long long)BNum(self.tab->map[L"second"]);
                    wchar_t buf[256];
                    wcsftime(buf, 256, fmt.c_str(), &tmb);
                    return Value::Str_(buf);
                });
                timeMeta->map[L"_tostring"] = formatFunc;

                auto addSecondsFunc = NF([getSelf](vector<Value>& a) -> Value {
                    Value self = getSelf(a);
                    if (!self.tab) return self;
                    double sec = 0;
                    if (!a.empty()) {
                        if (a[0].t == Value::Num) sec = BNum(a[0]);
                        else if (a.size() > 1 && a[1].t == Value::Num) sec = BNum(a[1]);
                    }
                    tm tmb{};
                    tmb.tm_year = (int)(long long)BNum(self.tab->map[L"year"]) - 1900;
                    tmb.tm_mon = (int)(long long)BNum(self.tab->map[L"month"]) - 1;
                    tmb.tm_mday = (int)(long long)BNum(self.tab->map[L"day"]);
                    tmb.tm_hour = (int)(long long)BNum(self.tab->map[L"hour"]);
                    tmb.tm_min = (int)(long long)BNum(self.tab->map[L"minute"]);
                    tmb.tm_sec = (int)(long long)BNum(self.tab->map[L"second"]);
                    time_t t = mktime(&tmb);
                    t += (time_t)sec;
                    localtime_s(&tmb, &t);
                    self.tab->map[L"year"] = Value::Num_(tmb.tm_year + 1900);
                    self.tab->map[L"month"] = Value::Num_(tmb.tm_mon + 1);
                    self.tab->map[L"day"] = Value::Num_(tmb.tm_mday);
                    self.tab->map[L"dayofweek"] = Value::Num_(tmb.tm_wday);
                    self.tab->map[L"hour"] = Value::Num_(tmb.tm_hour);
                    self.tab->map[L"minute"] = Value::Num_(tmb.tm_min);
                    self.tab->map[L"second"] = Value::Num_(tmb.tm_sec);
                    return self;
                });
                timeMeta->map[L"addSeconds"] = addSecondsFunc;
                timeMeta->map[L"addMinutes"] = NF([addSecondsFunc](vector<Value>& a) -> Value {
                    vector<Value> na = a;
                    if (!na.empty() && na[0].t == Value::Num) na[0] = Value::Num_(BNum(na[0]) * 60);
                    else if (na.size() > 1 && na[1].t == Value::Num) na[1] = Value::Num_(BNum(na[1]) * 60);
                    return addSecondsFunc.native(addSecondsFunc.nativeCtx.get(), na);
                });
                timeMeta->map[L"addHours"] = NF([addSecondsFunc](vector<Value>& a) -> Value {
                    vector<Value> na = a;
                    if (!na.empty() && na[0].t == Value::Num) na[0] = Value::Num_(BNum(na[0]) * 3600);
                    else if (na.size() > 1 && na[1].t == Value::Num) na[1] = Value::Num_(BNum(na[1]) * 3600);
                    return addSecondsFunc.native(addSecondsFunc.nativeCtx.get(), na);
                });
                timeMeta->map[L"addDays"] = NF([addSecondsFunc](vector<Value>& a) -> Value {
                    vector<Value> na = a;
                    if (!na.empty() && na[0].t == Value::Num) na[0] = Value::Num_(BNum(na[0]) * 86400);
                    else if (na.size() > 1 && na[1].t == Value::Num) na[1] = Value::Num_(BNum(na[1]) * 86400);
                    return addSecondsFunc.native(addSecondsFunc.nativeCtx.get(), na);
                });
                auto addMonthsFunc = NF([getSelf](vector<Value>& a) -> Value {
                    Value self = getSelf(a);
                    if (!self.tab) return self;
                    long long n = 0;
                    if (!a.empty()) {
                        if (a[0].t == Value::Num) n = (long long)BNum(a[0]);
                        else if (a.size() > 1 && a[1].t == Value::Num) n = (long long)BNum(a[1]);
                    }
                    long long curY = (long long)BNum(self.tab->map[L"year"]);
                    long long curM = (long long)BNum(self.tab->map[L"month"]);
                    long long curD = (long long)BNum(self.tab->map[L"day"]);
                    long long totalM = curY * 12 + (curM - 1) + n;
                    long long newY = totalM / 12;
                    long long newM = totalM % 12 + 1;
                    if (newM <= 0) {
                        newM += 12;
                        newY--;
                    }
                    int daysInMonth[] = { 0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
                    if ((newY % 4 == 0 && newY % 100 != 0) || (newY % 400 == 0))
                        daysInMonth[2] = 29;
                    if (curD > daysInMonth[newM]) curD = daysInMonth[newM];
                    self.tab->map[L"year"] = Value::Num_((double)newY);
                    self.tab->map[L"month"] = Value::Num_((double)newM);
                    self.tab->map[L"day"] = Value::Num_((double)curD);
                    tm tmb{};
                    tmb.tm_year = (int)newY - 1900;
                    tmb.tm_mon = (int)newM - 1;
                    tmb.tm_mday = (int)curD;
                    tmb.tm_hour = (int)(long long)BNum(self.tab->map[L"hour"]);
                    tmb.tm_min = (int)(long long)BNum(self.tab->map[L"minute"]);
                    tmb.tm_sec = (int)(long long)BNum(self.tab->map[L"second"]);
                    mktime(&tmb);
                    self.tab->map[L"dayofweek"] = Value::Num_(tmb.tm_wday);
                    return self;
                });
                timeMeta->map[L"addMonths"] = addMonthsFunc;
                timeMeta->map[L"diffMonths"] = NF([getSelf](vector<Value>& a) -> Value {
                    Value self = getSelf(a);
                    Value other;
                    if (!a.empty()) {
                        if (a[0].t == Value::TableT && a[0].tab != self.tab) other = a[0];
                        else if (a.size() > 1 && a[1].t == Value::TableT) other = a[1];
                    }
                    if (!self.tab || !other.tab) return Value::Num_(0);
                    long long y1 = (long long)BNum(self.tab->map[L"year"]);
                    long long m1 = (long long)BNum(self.tab->map[L"month"]);
                    long long y2 = (long long)BNum(other.tab->map[L"year"]);
                    long long m2 = (long long)BNum(other.tab->map[L"month"]);
                    return Value::Num_((double)((y1 - y2) * 12 + (m1 - m2)));
                });
                timeMeta->map[L"diffSeconds"] = NF([getSelf](vector<Value>& a) -> Value {
                    Value self = getSelf(a);
                    Value other;
                    if (!a.empty()) {
                        if (a[0].t == Value::TableT && a[0].tab != self.tab) other = a[0];
                        else if (a.size() > 1 && a[1].t == Value::TableT) other = a[1];
                    }
                    if (!self.tab || !other.tab) return Value::Num_(0);
                    auto toT = [](Table* tab) -> time_t {
                        tm tmb{};
                        tmb.tm_year = (int)(long long)BNum(tab->map[L"year"]) - 1900;
                        tmb.tm_mon = (int)(long long)BNum(tab->map[L"month"]) - 1;
                        tmb.tm_mday = (int)(long long)BNum(tab->map[L"day"]);
                        tmb.tm_hour = (int)(long long)BNum(tab->map[L"hour"]);
                        tmb.tm_min = (int)(long long)BNum(tab->map[L"minute"]);
                        tmb.tm_sec = (int)(long long)BNum(tab->map[L"second"]);
                        return mktime(&tmb);
                    };
                    return Value::Num_((double)(toT(self.tab.get()) - toT(other.tab.get())));
                });
                timeMeta->map[L"diffMinutes"] = NF([timeMeta](vector<Value>& a) -> Value {
                    Value v = timeMeta->map[L"diffSeconds"].native(timeMeta->map[L"diffSeconds"].nativeCtx.get(), a);
                    return Value::Num_((double)(long long)(BNum(v) / 60.0));
                });
                timeMeta->map[L"diffHours"] = NF([timeMeta](vector<Value>& a) -> Value {
                    Value v = timeMeta->map[L"diffSeconds"].native(timeMeta->map[L"diffSeconds"].nativeCtx.get(), a);
                    return Value::Num_(BNum(v) / 3600.0);
                });
                timeMeta->map[L"diffDays"] = NF([timeMeta](vector<Value>& a) -> Value {
                    Value v = timeMeta->map[L"diffSeconds"].native(timeMeta->map[L"diffSeconds"].nativeCtx.get(), a);
                    return Value::Num_(BNum(v) / 86400.0);
                });
                timeMeta->map[L"diffYears"] = NF([timeMeta](vector<Value>& a) -> Value {
                    Value v = timeMeta->map[L"diffMonths"].native(timeMeta->map[L"diffMonths"].nativeCtx.get(), a);
                    return Value::Num_(BNum(v) / 12.0);
                });
                timeMeta->map[L"diffday"] = timeMeta->map[L"diffDays"];
                timeMeta->map[L"diffmonth"] = timeMeta->map[L"diffMonths"];
                timeMeta->map[L"diffsecond"] = timeMeta->map[L"diffSeconds"];
                timeMeta->map[L"format"] = formatFunc;
                timeMeta->map[L"_get"] = NF([formatFunc](vector<Value>& a) -> Value {
                    if (a.empty() || a[0].t != Value::Str) return Value();
                    const wstring& k = a[0].s;
                    bool bOwnerCall = (a.size() > 1 && a[1].t == Value::Bool && a[1].b);
                    Value self = Interp::GetCurrentOwner();
                    if (!self.tab && a.size() > 2 && a[2].t == Value::TableT && a[2].tab)
                        self = a[2];
                    if (k == L"format")
                    {
                        if (bOwnerCall) return formatFunc;
                        if (self.tab)
                        {
                            auto itF = self.tab->map.find(L"format");
                            if (itF != self.tab->map.end()) return itF->second;
                        }
                        return Value::Str_(L"%Y/%m/%d %H:%M:%S");
                    }
                    return Value();
                });
                timeMeta->map[L"_set"] = NF([](vector<Value>& a) -> Value {
                    if (a.size() < 2 || a[0].t != Value::Str) return Value();
                    const wstring& k = a[0].s;
                    Value self = Interp::GetCurrentOwner();
                    if (!self.tab && a.size() > 3 && a[3].t == Value::TableT && a[3].tab)
                        self = a[3];
                    if (k == L"format" && self.tab)
                    {
                        self.tab->map[L"format"] = a[1];
                    }
                    return Value();
                });
                timeMeta->map[L"local"] = NF([getSelf](vector<Value>& a) -> Value {
                    Value self = getSelf(a);
                    if (!self.tab) return self;
                    bool copy = false;
                    for (auto& arg : a) {
                        if (arg.t == Value::Bool && arg.b) copy = true;
                    }
                    if (copy)
                    {
                        auto res = std::make_shared<Table>();
                        res->map = self.tab->map;
                        res->arr = self.tab->arr;
                        return Value::Table_(res);
                    }
                    return self;
                });
                timeMeta->map[L"utc"] = NF([getSelf](vector<Value>& a) -> Value {
                    Value self = getSelf(a);
                    if (!self.tab) return self;
                    bool copy = false;
                    for (auto& arg : a) {
                        if (arg.t == Value::Bool && arg.b) copy = true;
                    }
                    if (copy)
                    {
                        auto res = std::make_shared<Table>();
                        res->map = self.tab->map;
                        res->arr = self.tab->arr;
                        return Value::Table_(res);
                    }
                    return self;
                });
                timeMeta->map[L"_call"] = NF([timeMeta](vector<Value>& a) -> Value {
                    // When invoked through timeMeta._call, a[0] is the owner (time table).
                    // The actual constructor arguments start at a[1].
                    vector<Value> ctorArgs;
                    for (size_t i = 1; i < a.size(); i++)
                        ctorArgs.push_back(a[i]);
                    auto ictor = timeMeta->map.find(L"_timeCtor");
                    if (ictor != timeMeta->map.end() && ictor->second.native)
                        return ictor->second.native(ictor->second.nativeCtx.get(), ctorArgs);
                    return Value();
                });

                auto timeCtor = NF([timeMeta, parseTimeText, timeFormatCandidates](vector<Value>& a) -> Value {
                    SYSTEMTIME st;
                    GetLocalTime(&st);
                    wstring fmt = L"%Y/%m/%d %H:%M:%S";
                    // text left over after the last parsed time value: the
                    // official time object keeps it in its endstr property
                    wstring strEndStr;
                    if (!a.empty())
                    {
                        if (a.size() >= 3 && a[0].t == Value::Num && a[1].t == Value::Num && a[2].t == Value::Num)
                        {
                            st.wYear = (WORD)BNum(a[0]);
                            st.wMonth = (WORD)BNum(a[1]);
                            st.wDay = (WORD)BNum(a[2]);
                            st.wHour = (WORD)(a.size() > 3 ? BNum(a[3]) : 0);
                            st.wMinute = (WORD)(a.size() > 4 ? BNum(a[4]) : 0);
                            st.wSecond = (WORD)(a.size() > 5 ? BNum(a[5]) : 0);
                            st.wMilliseconds = 0;
                            tm tmb{};
                            tmb.tm_year = st.wYear - 1900;
                            tmb.tm_mon = st.wMonth - 1;
                            tmb.tm_mday = st.wDay;
                            mktime(&tmb);
                            st.wDayOfWeek = (WORD)tmb.tm_wday;
                        }
                        else if (a[0].t == Value::Num)
                        {
                            time_t t = (time_t)BNum(a[0]);
                            tm tmb{};
                            localtime_s(&tmb, &t);
                            st.wYear = (WORD)(tmb.tm_year + 1900);
                            st.wMonth = (WORD)(tmb.tm_mon + 1);
                            st.wDay = (WORD)tmb.tm_mday;
                            st.wDayOfWeek = (WORD)tmb.tm_wday;
                            st.wHour = (WORD)tmb.tm_hour;
                            st.wMinute = (WORD)tmb.tm_min;
                            st.wSecond = (WORD)tmb.tm_sec;
                            st.wMilliseconds = 0;
                        }
                        else if (a[0].t == Value::Str)
                        {
                            // Official: the text is parsed with the same
                            // format specifiers used to format a time, and a
                            // text that cannot supply the required parts
                            // makes time() return null.
                            bool bHasFmt = a.size() > 1 && a[1].t == Value::Str;
                            if (bHasFmt) fmt = a[1].s;
                            bool bLax = a.size() > 3 && a[3].Truthy();
                            int y = 0, m = 1, d = 1, h = 0, mi = 0, s = 0;
                            bool bOk = false;
                            // Candidate formats: the requested one, then the
                            // same one with the trailing time markers
                            // dropped one by one ("%Y/%m/%d %H:%M:%S" ->
                            // "%Y/%m/%d %H:%M" -> "%Y/%m/%d %H" ->
                            // "%Y/%m/%d"). Official: with lax the trailing
                            // time parts may be omitted; with no explicit
                            // format the time part may always be omitted,
                            // while the date part is always required.
                            std::vector<wstring> cands;
                            if (bHasFmt)
                            {
                                cands = timeFormatCandidates(fmt);
                                if (!bLax) cands.resize(1);
                            }
                            else
                            {
                                cands = timeFormatCandidates(L"%Y/%m/%d %H:%M:%S");
                            }
                            for (const auto& cf : cands)
                            {
                                int ty = 0, tmo = 1, td = 1, th = 0, tmi = 0, ts = 0;
                                wstring tend;
                                if (!parseTimeText(a[0].s, cf, ty, tmo, td, th, tmi, ts, tend))
                                    continue;
                                y = ty; m = tmo; d = td; h = th; mi = tmi; s = ts;
                                strEndStr = tend;
                                bOk = true;
                                break;
                            }
                            if (!bOk) return Value();   // official returns null
                            st.wYear = (WORD)y;
                            st.wMonth = (WORD)m;
                            st.wDay = (WORD)d;
                            st.wHour = (WORD)h;
                            st.wMinute = (WORD)mi;
                            st.wSecond = (WORD)s;
                            st.wMilliseconds = 0;
                            tm tmb{};
                            tmb.tm_year = y - 1900;
                            tmb.tm_mon = m - 1;
                            tmb.tm_mday = d;
                            tmb.tm_hour = h;
                            tmb.tm_min = mi;
                            tmb.tm_sec = s;
                            mktime(&tmb);
                            st.wDayOfWeek = (WORD)tmb.tm_wday;
                        }
                        else if (a[0].t == Value::TableT && a[0].tab)
                        {
                            auto getF = [&](const wchar_t* k, WORD def) -> WORD {
                                auto it = a[0].tab->map.find(k);
                                return it != a[0].tab->map.end() ? (WORD)BNum(it->second) : def;
                            };
                            st.wYear = getF(L"year", st.wYear);
                            st.wMonth = getF(L"month", st.wMonth);
                            st.wDay = getF(L"day", st.wDay);
                            st.wHour = getF(L"hour", 0);
                            st.wMinute = getF(L"minute", 0);
                            st.wSecond = getF(L"second", 0);
                            st.wMilliseconds = 0;
                            if (a.size() > 1 && a[1].t == Value::Str) fmt = a[1].s;
                        }
                    }
                    if (a.size() > 1 && a[1].t == Value::Str) fmt = a[1].s;

                    auto inst = std::make_shared<Table>();
                    inst->map[L"_struct"] = Value::Str_(L"WORD year;WORD month;WORD dayofweek;WORD day;WORD hour;WORD minute;WORD second;WORD milliseconds;");
                    inst->map[L"year"] = Value::Num_(st.wYear);
                    inst->map[L"month"] = Value::Num_(st.wMonth);
                    inst->map[L"dayofweek"] = Value::Num_(st.wDayOfWeek);
                    inst->map[L"day"] = Value::Num_(st.wDay);
                    inst->map[L"hour"] = Value::Num_(st.wHour);
                    inst->map[L"minute"] = Value::Num_(st.wMinute);
                    inst->map[L"second"] = Value::Num_(st.wSecond);
                    inst->map[L"milliseconds"] = Value::Num_(st.wMilliseconds);
                    inst->map[L"format"] = Value::Str_(fmt);
                    // official: the text after the last parsed value is
                    // kept in the endstr property (used by time.iso8601 to
                    // read the trailing time zone)
                    if (!strEndStr.empty())
                        inst->map[L"endstr"] = Value::Str_(strEndStr);
                    inst->map[L"_meta"] = Value::Table_(timeMeta);
                    return Value::Table_(inst);
                });

                timeMeta->map[L"_timeCtor"] = timeCtor;
                tb->map[L"_call"] = timeCtor;
                tb->map[L"_meta"] = Value::Table_(timeMeta);
                globals.VarAt(NameId(L"time")) = Value::Table_(tb);
                roots[L"time"] = Value::Table_(tb);

                tb->map[L"is"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty() || a[0].t != Value::TableT || !a[0].tab)
                        return Value::Bool_(false);
                    auto is = a[0].tab->map.find(L"_struct");
                    if (is != a[0].tab->map.end() && is->second.t == Value::Str)
                    {
                        if (is->second.s.find(L"WORD year;") != wstring::npos)
                            return Value::Bool_(true);
                    }
                    return Value::Bool_(false);
                });
                tb->map[L"now"] = timeCtor;
                tb->map[L"tick"] = NF([](vector<Value>&) {
                    return Value::Num_((double)GetTickCount()); });
                tb->map[L"date"] = NF([timeCtor](vector<Value>& a) -> Value {
                    if (!a.empty() && a[0].t == Value::Str && !a[0].s.empty() && a[0].s[0] == L'%')
                    {
                        wstring fmt = a[0].s;
                        time_t t = a.size() > 1 ? (time_t)BNum(a[1]) : time(nullptr);
                        tm tmb{};
                        localtime_s(&tmb, &t);
                        wchar_t buf[256];
                        wcsftime(buf, 256, fmt.c_str(), &tmb);
                        return Value::Str_(buf);
                    }
                    vector<Value> args = a;
                    if (args.empty())
                    {
                        args.push_back(Value());
                        args.push_back(Value::Str_(L"%Y-%m-%d"));
                    }
                    else if (args.size() == 1 && args[0].t == Value::Str)
                    {
                        args.push_back(Value::Str_(L"%Y-%m-%d"));
                    }
                    Value ret = timeCtor.native(timeCtor.nativeCtx.get(), args);
                    if (ret.t == Value::TableT && ret.tab)
                    {
                        auto itFmt = ret.tab->map.find(L"format");
                        if (itFmt != ret.tab->map.end() && itFmt->second.s == L"%Y/%m/%d %H:%M:%S")
                        {
                            ret.tab->map[L"format"] = Value::Str_(L"%Y-%m-%d");
                        }
                    }
                    return ret;
                });
                // time.stamp() — seconds since the epoch, as a number
                tb->map[L"stamp"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty())
                        return Value::Num_((double)time(nullptr));
                    wstring fmt = BStr(a[0]);
                    time_t t = a.size() > 1 ? (time_t)BNum(a[1]) : time(nullptr);
                    tm tmb;
                    localtime_s(&tmb, &t);
                    wchar_t buf[256];
                    wcsftime(buf, 256, fmt.c_str(), &tmb);
                    return Value::Str_(buf); });
                // time.ole() — OLE automation date (days since 1899-12-30)
                tb->map[L"ole"] = NF([](vector<Value>& a) -> Value {
                    SYSTEMTIME st;
                    if (a.empty())
                        GetLocalTime(&st);
                    else
                    {
                        FILETIME ft;
                        SYSTEMTIME stIn;
                        stIn.wYear = (WORD)BNum(a[0]);
                        stIn.wMonth = (WORD)(a.size() > 1 ? BNum(a[1]) : 1);
                        stIn.wDay = (WORD)(a.size() > 2 ? BNum(a[2]) : 1);
                        stIn.wHour = (WORD)(a.size() > 3 ? BNum(a[3]) : 0);
                        stIn.wMinute = (WORD)(a.size() > 4 ? BNum(a[4]) : 0);
                        stIn.wSecond = (WORD)(a.size() > 5 ? BNum(a[5]) : 0);
                        stIn.wMilliseconds = 0;
                        if (a.empty())
                            st = stIn;
                        else if (!SystemTimeToTzSpecificLocalTime(nullptr,
                                                                 &stIn, &st))
                            st = stIn;
                        (void)ft;
                    }
                    // days from 1899-12-30 plus the fraction of the day
                    double dSerial = 0;
                    {
                        FILETIME ft;
                        SYSTEMTIME base;
                        memset(&base, 0, sizeof(base));
                        base.wYear = 1899; base.wMonth = 12; base.wDay = 30;
                        ULARGE_INTEGER ul, ulBase;
                        SystemTimeToFileTime(&st, &ft);
                        ul.HighPart = ft.dwHighDateTime;
                        ul.LowPart = ft.dwLowDateTime;
                        SystemTimeToFileTime(&base, &ft);
                        ulBase.HighPart = ft.dwHighDateTime;
                        ulBase.LowPart = ft.dwLowDateTime;
                        dSerial = (double)(long long)
                            ((ul.QuadPart - ulBase.QuadPart) / 10000000) /
                            86400.0;
                    }
                    return Value::Num_(dSerial); });
            }
            // ---- json (round 30) ----
            {
                auto tb = GetRoot(L"json").tab;
                tb->map[L"parse"] = NF([](vector<Value>& a) {
                    JParser jp{ BStr(a[0]), 0, nullptr };
                    wstring err2;
                    jp.err = &err2;
                    Value v = jp.Parse();
                    if (!err2.empty()) return Value();
                    return v; });
                tb->map[L"stringify"] = NF([](vector<Value>& a) {
                    bool pretty = a.size() > 1 && a[1].Truthy();
                    return Value::Str_(JWrite(a[0], pretty ? 0 : -1, 0)); });
            }
            // ---- win (round 29 minimal): msgbox + clipboard ----
            {
                auto tb = GetRoot(L"win").tab;
                auto clip = std::make_shared<Table>();
                tb->map[L"clip"] = Value::Table_(clip);
                // win.getMessagePos(lParam) — the mouse position packed into
                // an lParam (WM_MOUSEMOVE & friends), returned as x,y.
                // Called with no argument it returns the current cursor
                // position (official contract — win.ui.tracker falls back to
                // it when the lParam is -1, i.e. the menu key / Shift+F10).
                tb->map[L"getMessagePos"] = NF([](vector<Value>& a) -> Value {
                    short x = 0, y = 0;
                    if (a.empty())
                    {
                        POINT pt = { 0, 0 };
                        GetCursorPos(&pt);
                        x = pt.x; y = pt.y;
                    }
                    else
                    {
                        long long lp = (long long)BNum(a[0]);
                        x = (short)(lp & 0xFFFF);
                        y = (short)((lp >> 16) & 0xFFFF);
                    }
                    Value vx = Value::Num_((double)x);
                    auto ex = std::make_shared<vector<Value>>();
                    ex->push_back(Value::Num_((double)y));
                    vx.mrets = ex;
                    return vx; });
                tb->map[L"msgbox"] = NF([](vector<Value>& a) {
                    wstring text = a.empty() ? L"" : a[0].ToString();
                    wstring title = a.size() > 1 ? a[1].ToString() : GetNodioConfig().defaultTitle;
                    UINT flag = a.size() > 2 ? (UINT)BNum(a[2]) : 0u;
                    int rc = MessageBoxW(nullptr, text.c_str(), title.c_str(),
                                         MB_OK | flag);
                    return Value::Num_((double)rc); });
                tb->map[L"msgboxTest"] = NF([](vector<Value>& a) {
                    wstring text = a.empty() ? L"" : a[0].ToString();
                    wstring title = a.size() > 1 ? a[1].ToString() : GetNodioConfig().defaultTitle;
                    return Value::Bool_(MessageBoxW(nullptr, text.c_str(),
                        title.c_str(), MB_OKCANCEL | MB_ICONQUESTION) == IDOK); });
                // ---- win.clip: 官方 API 面（E-17） ----
                // 官方 win/clip/_.aardio（namespace win.clip;）定义 12 个成员：
                //   readUtf16 / read / write / writeBitmap / readBitmap /
                //   clear / test / formats / formatNames / getFormatName /
                //   eachFormat / readUnicode
                // 此前引擎内置兜底只有 read/write（2/12），其余成员缺失；而
                // `import win.clip;` 会让官方包整体覆盖内置实现，于是同一条
                // API 在「导入 / 未导入」两条路径上行为不一致（IDE 的
                // editor.nodio 用的是未导入的内置实现，官方文档却要求显式
                // `import win.clip;`）。这里按官方语义把内置实现补齐到官方
                // API 面，两条路径的 API 面一致：
                //   · write(str, rep)：rep 控制复制前是否清空剪贴板（官方：
                //     rep 为 null 或 true 时先 EmptyClipboard），成功返回
                //     剪贴板数据句柄（官方契约，不是 bool）。
                //   · read() / readUtf16()：剪贴板里没有 CF_UNICODETEXT 时
                //     返回 null（官方语义），而不是空串——官方 aardio 里空串
                //     是真值，返回 null 才能让 `if(s)` 正确地跳过粘贴。
                //   · 官方包的 read() 返回 UTF-8 字节串（官方 aardio 的字符串
                //     是 UTF-8 字节串）；Nodio 的字符串是宽文本，内置 read()
                //     返回宽文本——剪贴板载荷（CF_UNICODETEXT）一致，仅编码
                //     不同；richedit 的 text/selText 两种形态都接受（已实测）。
                {
                    // 打开剪贴板（官方语义：OpenClipboard 失败即静默失败）。
                    // 无捕获 lambda → 可按值复制进长期存活的 native 闭包。
                    auto clipOpen = [](bool bEmpty) -> bool
                    {
                        for (int retry = 0; retry < 10; retry++)
                        {
                            if (OpenClipboard(nullptr))
                            {
                                if (bEmpty) EmptyClipboard();
                                return true;
                            }
                            Sleep(10);
                        }
                        return false;
                    };
                    // 读 CF_UNICODETEXT：pOk=剪贴板可用，pHas=该格式存在
                    auto clipReadText = [clipOpen](bool* pOk, bool* pHas) -> wstring
                    {
                        *pOk = false; *pHas = false;
                        wstring out;
                        if (!clipOpen(false)) return out;
                        HANDLE h = GetClipboardData(CF_UNICODETEXT);
                        if (h)
                        {
                            *pHas = true;
                            const wchar_t* q = (const wchar_t*)GlobalLock(h);
                            if (q) { out = q; GlobalUnlock(h); }
                        }
                        CloseClipboard();
                        *pOk = true;
                        return out;
                    };
                    // 列出剪贴板格式 ID（官方 formats()）
                    auto clipFormatIds = [clipOpen](bool* pOk) -> std::vector<UINT>
                    {
                        std::vector<UINT> ids;
                        *pOk = false;
                        if (!clipOpen(false)) return ids;
                        UINT f = 0;
                        while ((f = EnumClipboardFormats(f)) != 0) ids.push_back(f);
                        CloseClipboard();
                        *pOk = true;
                        return ids;
                    };
                    // 官方 formatNames：系统预定义剪贴板格式的友好名称
                    auto tbFmtNames = std::make_shared<Table>();
                    {
                        struct FmtName { UINT id; const wchar_t* name; };
                        static const FmtName kFmtNames[] = {
                            { 1, L"Text" }, { 2, L"Bitmap" }, { 3, L"Metafilepict" },
                            { 4, L"Sylk" }, { 5, L"DIF" }, { 6, L"TIFF" },
                            { 7, L"OEM Text" }, { 8, L"DIB" }, { 9, L"Palette" },
                            { 10, L"Pendata" }, { 11, L"RIFF" }, { 12, L"Wave" },
                            { 13, L"Unicode Text" }, { 14, L"Enhmetafile" },
                            { 17, L"DIBV5" }, { 0x0080, L"Owner Display" },
                            { 0x0081, L"DSP Text" }, { 0x0082, L"DSP Bitmap" },
                            { 0x0083, L"DSP Metafilepict" }, { 0x008E, L"DSP Enhmetafile" },
                            { 0x0200, L"Private First" }, { 0x02FF, L"Private Last" },
                            { 0x0300, L"GDI Object First" }, { 0x03FF, L"GDI Object Last" }
                        };
                        for (const auto& k : kFmtNames)
                            tbFmtNames->map[std::to_wstring(k.id)] = Value::Str_(k.name);
                    }
                    clip->map[L"formatNames"] = Value::Table_(tbFmtNames);

                    clip->map[L"write"] = NF([clipOpen](vector<Value>& a) -> Value {
                        // 官方 write(str, rep)：rep 为 null 或 true 时先清空
                        // 剪贴板；成功返回剪贴板数据句柄。
                        wstring text = a.empty() ? L"" : a[0].ToString();
                        bool rep = a.size() < 2 || a[1].t == Value::Null || a[1].Truthy();
                        if (text.empty()) return Value();
                        if (!clipOpen(rep)) return Value();
                        // CF_UNICODETEXT：NUL 结尾的 UTF-16LE
                        size_t bytes = text.size() * 2;
                        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes + 2);
                        if (!h) { CloseClipboard(); return Value(); }
                        wchar_t* q = (wchar_t*)GlobalLock(h);
                        if (!q)
                        {
                            // AUDIT-06: GlobalLock can fail (discarded
                            // movable block under memory pressure) — a raw
                            // memcpy through NULL was a guaranteed AV.
                            GlobalFree(h);
                            CloseClipboard();
                            return Value();
                        }
                        memcpy(q, text.c_str(), bytes);
                        q[text.size()] = 0;
                        GlobalUnlock(h);
                        HANDLE hdl = SetClipboardData(CF_UNICODETEXT, h);
                        CloseClipboard();
                        if (!hdl) return Value();
                        Sleep(30);
                        return Value::Num_((double)(INT_PTR)hdl); });
                clip->map[L"read"] = NF([clipReadText](vector<Value>&) -> Value {
                    bool ok = false, has = false;
                    wstring s = clipReadText(&ok, &has);
                    if (!ok || !has) return Value();   // 官方：无文本返回 null
                    return Value::Str_(s); });
                clip->map[L"readUtf16"] = NF([clipReadText](vector<Value>&) -> Value {
                    bool ok = false, has = false;
                    wstring s = clipReadText(&ok, &has);
                    if (!ok || !has) return Value();
                    return Value::Str_(s); });
                // 官方 readUnicode 是 readUtf16 的别名（已废弃）
                clip->map[L"readUnicode"] = clip->map[L"readUtf16"];
                clip->map[L"clear"] = NF([clipOpen](vector<Value>&) -> Value {
                    if (!clipOpen(true)) return Value();
                    CloseClipboard();
                    return Value::Bool_(true); });
                clip->map[L"formats"] = NF([clipFormatIds](vector<Value>&) -> Value {
                    bool ok = false;
                    auto ids = clipFormatIds(&ok);
                    if (!ok) return Value();     // 官方：OpenClipboard 失败返回 null
                    auto tb = std::make_shared<Table>();
                    for (UINT id : ids) tb->arr.push_back(Value::Num_((double)id));
                    return Value::Table_(tb); });
                clip->map[L"test"] = NF([clipFormatIds](vector<Value>& a) -> Value {
                    // 官方 test(f, ...)：参数是格式 ID（数值）或它们的数组，
                    // 返回剪贴板里是否存在其中任意一种格式。
                    std::vector<UINT> want;
                    auto addOne = [&want](const Value& v)
                    {
                        if (v.t == Value::TableT && v.tab)
                        {
                            for (auto& e : v.tab->arr)
                                want.push_back((UINT)BNum(e));
                            for (auto& kv : v.tab->map)
                            {
                                bool bNum = !kv.first.empty();
                                for (wchar_t c : kv.first)
                                    if (c < L'0' || c > L'9') { bNum = false; break; }
                                if (bNum) want.push_back((UINT)BNum(kv.second));
                            }
                        }
                        else if (v.t != Value::Null)
                        {
                            want.push_back((UINT)BNum(v));
                        }
                    };
                    for (auto& v : a) addOne(v);
                    if (want.empty()) return Value::Bool_(false);
                    bool ok = false;
                    auto ids = clipFormatIds(&ok);
                    if (!ok) return Value();
                    for (UINT id : ids)
                        for (UINT w : want)
                            if (id == w) return Value::Bool_(true);
                    return Value::Bool_(false); });
                clip->map[L"getFormatName"] = NF([tbFmtNames](vector<Value>& a) -> Value {
                    UINT id = a.empty() ? 0 : (UINT)(INT_PTR)(long long)BNum(a[0]);
                    bool friendly = a.size() > 1 && a[1].Truthy();
                    wchar_t buf[80] = L"";
                    if (GetClipboardFormatNameW(id, buf, 80))
                        return Value::Str_(buf);
                    if (friendly)
                    {
                        auto it = tbFmtNames->map.find(std::to_wstring(id));
                        if (it != tbFmtNames->map.end()) return it->second;
                        return Value::Str_(L"Unknown");
                    }
                    return Value(); });
                clip->map[L"eachFormat"] = NF([clipFormatIds, tbFmtNames](vector<Value>&) -> Value {
                    // 官方 eachFormat() 始终返回一个迭代器（剪贴板不可用时
                    // 迭代器为空）：var ids = formats(); return function(){...}
                    bool ok = false;
                    auto ids = clipFormatIds(&ok);
                    auto state = std::make_shared<std::vector<UINT>>(ids);
                    auto idx = std::make_shared<size_t>(0);
                    return NF([state, idx, tbFmtNames](vector<Value>&) -> Value {
                        if (*idx >= state->size()) return Value();
                        UINT id = (*state)[(*idx)++];
                        wstring name;
                        wchar_t buf[80] = L"";
                        if (GetClipboardFormatNameW(id, buf, 80)) name = buf;
                        else
                        {
                            auto it = tbFmtNames->map.find(std::to_wstring(id));
                            if (it != tbFmtNames->map.end()) name = it->second.s;
                            else name = L"Unknown";
                        }
                        Value v = Value::Num_((double)id);
                        auto ex = std::make_shared<vector<Value>>();
                        ex->push_back(Value::Str_(name));
                        v.mrets = ex;
                        return v; }); });
                clip->map[L"writeBitmap"] = NF([clipOpen](vector<Value>& a) -> Value {
                    if (a.empty() || a[0].t == Value::Null) return Value();
                    HBITMAP hbmp = (HBITMAP)(INT_PTR)(long long)BNum(a[0]);
                    bool own = a.size() > 1 && a[1].Truthy();
                    bool rep = a.size() < 3 || a[2].t == Value::Null || a[2].Truthy();
                    if (!clipOpen(rep)) return Value();
                    HBITMAP hSafe = (HBITMAP)CopyImage(hbmp, IMAGE_BITMAP, 0, 0,
                                                      LR_COPYRETURNORG);
                    HANDLE hdl = 0;
                    if (hSafe) hdl = SetClipboardData(CF_BITMAP, hSafe);
                    CloseClipboard();
                    if (own && hbmp) DeleteObject(hbmp);
                    if (!hdl) return Value();
                    return Value::Num_((double)(INT_PTR)hdl); });
                clip->map[L"readBitmap"] = NF([clipOpen](vector<Value>&) -> Value {
                    if (!clipOpen(false)) return Value();
                    HBITMAP hbmp = (HBITMAP)GetClipboardData(CF_BITMAP);
                    HBITMAP hCopy = 0;
                    if (hbmp) hCopy = (HBITMAP)CopyImage(hbmp, IMAGE_BITMAP, 0, 0,
                                                         LR_DEFAULTSIZE);
                    CloseClipboard();
                    if (!hCopy) return Value();
                    return Value::Num_((double)(INT_PTR)hCopy); });
                }
                // round 42: raw window access + common dialogs
                tb->map[L"findWindow"] = NF([](vector<Value>& a) -> Value {
                    wstring s1 = a.size() > 0 ? BStr(a[0]) : L"";
                    wstring s2 = a.size() > 1 ? BStr(a[1]) : L"";
                    HWND h = FindWindowW(
                        a.size() > 0 && a[0].t != Value::Null && !s1.empty()
                            ? s1.c_str() : nullptr,
                        a.size() > 1 && a[1].t != Value::Null && !s2.empty()
                            ? s2.c_str() : nullptr);
                    return h ? Value::Num_((double)(INT_PTR)h) : Value(); });
                tb->map[L"postMessage"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty() || a[0].t != Value::Num)
                        return Value::Bool_(false);
                    HWND h = (HWND)(INT_PTR)(long long)BNum(a[0]);
                    UINT m = a.size() > 1 ? (UINT)BNum(a[1]) : 0;
                    WPARAM w = a.size() > 2
                        ? (WPARAM)(INT_PTR)(long long)BNum(a[2]) : 0;
                    LPARAM l = a.size() > 3
                        ? (LPARAM)(INT_PTR)(long long)BNum(a[3]) : 0;
                    return Value::Bool_(PostMessageW(h, m, w, l) != 0); });
                auto HwndOf = [](const Value& v) -> HWND {
                    if (v.t == Value::Num)
                        return (HWND)(INT_PTR)(long long)v.n;
                    if (v.t == Value::TableT && v.tab)
                    {
                        auto ih = v.tab->map.find(L"hwnd");
                        if (ih != v.tab->map.end() &&
                            ih->second.t == Value::Num)
                            return (HWND)(INT_PTR)(long long)ih->second.n;
                    }
                    return nullptr; };
                auto dlg = std::make_shared<Table>();
                tb->map[L"dlg"] = Value::Table_(dlg);
                // open(formOrHwnd, title, filter, initDir, initFile) -> path
                // filter uses pipe form: "文本|*.txt|所有|*.*"
                auto dlgImpl = [HwndOf](vector<Value>& a, bool bSave)
                    -> Value {
                    HWND hOwner = a.empty() ? nullptr : HwndOf(a[0]);
                    wstring sTitle = a.size() > 1 ? BStr(a[1]) : L"";
                    wstring sFilter = a.size() > 2 ? BStr(a[2]) : L"";
                    wstring sDir = a.size() > 3 ? BStr(a[3]) : L"";
                    wstring sFile = a.size() > 4 ? BStr(a[4]) : L"";
                    for (auto& c : sFilter)
                        if (c == L'|') c = L'\0';
                    if (!sFilter.empty()) sFilter.push_back(L'\0');
                    wchar_t buf[MAX_PATH * 2] = L"";
                    wcsncpy_s(buf, sFile.c_str(), _TRUNCATE);
                    OPENFILENAMEW ofn = { sizeof(ofn) };
                    ofn.hwndOwner = hOwner;
                    ofn.lpstrFilter = sFilter.empty()
                                          ? L"所有文件\0*.*\0"
                                          : sFilter.c_str();
                    ofn.lpstrFile = buf;
                    ofn.nMaxFile = MAX_PATH * 2;
                    ofn.lpstrTitle =
                        sTitle.empty() ? nullptr : sTitle.c_str();
                    ofn.lpstrInitialDir =
                        sDir.empty() ? nullptr : sDir.c_str();
                    ofn.Flags = OFN_EXPLORER | OFN_HIDEREADONLY;
                    if (bSave)
                        ofn.Flags |= OFN_OVERWRITEPROMPT;
                    else
                        ofn.Flags |= OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
                    BOOL ok = bSave ? GetSaveFileNameW(&ofn)
                                    : GetOpenFileNameW(&ofn);
                    if (!ok)
                        return Value();     // cancel -> null
                    return Value::Str_(buf); };
                dlg->map[L"open"] = NF([dlgImpl](vector<Value>& a)
                    -> Value {
                    return dlgImpl(a, false); });
                dlg->map[L"save"] = NF([dlgImpl](vector<Value>& a)
                    -> Value {
                    return dlgImpl(a, true); });
            }
            // ---- round 44: raw — P/Invoke + raw memory ----
            {
                auto tb = GetRoot(L"raw").tab;
                // raw.str(ptr, len, cp) — kernel contract: read a C
                // string from memory. len omitted → ANSI to '\0';
                // len===true → UTF-16 to '\0\0' (clipboard text);
                // numeric len → that many bytes decoded per cp
                // (0=ANSI, 65001=UTF-8)
                tb->map[L"str"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty()) return Value();
                    const unsigned char* p = nullptr;
                    void* pSerialized = nullptr;
                    int totalSz = 0;
                    if (a[0].t == Value::Pointer)
                        p = (const unsigned char*)a[0].ptr;
                    else if (a[0].t == Value::Num)
                        p = (const unsigned char*)(INT_PTR)(long long)
                            BNum(a[0]);
                    else if ((a[0].t == Value::TableT || a[0].t == Value::Class) && a[0].tab)
                    {
                        auto iby = a[0].tab->map.find(L"_bytes");
                        if (iby != a[0].tab->map.end() && iby->second.t == Value::Str)
                        {
                            p = (const unsigned char*)iby->second.s.data();
                            totalSz = (int)(iby->second.s.size() * sizeof(wchar_t));
                        }
                        else
                        {
                            pSerialized = StructSerialize(a[0], &totalSz);
                            p = (const unsigned char*)pSerialized;
                        }
                    }
                    if (!p) return Value();
                    Value retVal;
                    if (a.size() > 1 && a[1].t == Value::Bool && a[1].b)
                    {
                        const wchar_t* pw = (const wchar_t*)p;
                        wstring w;
                        if (totalSz > 0)
                        {
                            size_t maxChars = (size_t)totalSz / sizeof(wchar_t);
                            for (size_t i = 0; i < maxChars && pw[i]; i++)
                                w += pw[i];
                        }
                        else
                        {
                            while (*pw) w += *pw++;
                        }
                        retVal = Value::Str_(w, /*isUtf16=*/true);
                    }
                    else if (a.size() > 1 && a[1].t == Value::Num)
                    {
                        size_t len = (size_t)BNum(a[1]);
                        UINT cp = (a.size() > 2 && a[2].t == Value::Num)
                                      ? (UINT)(long long)BNum(a[2])
                                      : CP_ACP;
                        int wl = MultiByteToWideChar(cp, 0, (const char*)p,
                            (int)len, nullptr, 0);
                        wstring w;
                        if (wl > 0)
                        {
                            w.resize(wl);
                            MultiByteToWideChar(cp, 0, (const char*)p,
                                (int)len, &w[0], wl);
                        }
                        retVal = Value::Str_(w);
                    }
                    else
                    {
                        // ANSI, NUL-terminated
                        const char* pa = (const char*)p;
                        wstring w;
                        if (totalSz > 0)
                        {
                            for (int i = 0; i < totalSz && pa[i]; i++)
                                w += (wchar_t)(unsigned char)pa[i];
                        }
                        else
                        {
                            while (*pa) w += (wchar_t)(unsigned char)*pa++;
                        }
                        retVal = Value::Str_(w);
                    }
                    if (pSerialized)
                    {
                        HeapFree(GetProcessHeap(), 0, pSerialized);
                    }
                    return retVal;
                });
                tb->map[L"array"] = NF([](vector<Value>& a) -> Value {
                    // raw.array(arrayOrLength, rawTypename, fieldName)
                    // creates a struct { <rawTypename> <fieldName>[len] = ... }
                    if (a.size() < 3) return Value();
                    int len = 0;
                    auto arrTab = std::make_shared<Table>();
                    if (a[0].t == Value::Num)
                    {
                        len = (int)BNum(a[0]);
                        if (len < 0) len = 0;
                        arrTab->arr.resize(len);
                    }
                    else if (a[0].t == Value::TableT && a[0].tab)
                    {
                        len = (int)a[0].tab->arr.size();
                        arrTab->arr = a[0].tab->arr;
                    }
                    wstring typeName = BStr(a[1]);
                    wstring fieldName = BStr(a[2]);
                    wstring structDef = typeName + L" " + fieldName + L"[" + std::to_wstring(len) + L"];";

                    auto res = std::make_shared<Table>();
                    res->map[L"_struct"] = Value::Str_(structDef);
                    res->map[fieldName] = Value::Table_(arrTab);
                    return Value::Table_(res);
                });
                // 第五十四轮/第五十六轮：richedit 打印——EM_FORMATRANGE
                // 全链下沉 C++ 侧（结构体编组在 aardio FFI 不可靠：实测
                // 动态 ::User32.SendMessageW 连 EM_GETTEXTLENGTH 都回 0，
                // 显式 pointer 形参也收不到 class 实例的地址）。
                // raw.printRichedit(hwndEdit, hdc, render)：
                //   render=false → 仅测量（返回页数）
                //   render=true  → StartPage/EndPage 逐页渲染打印
                // 返回页数；hdc/hwndEdit 无效返回 0。收尾释放 richedit
                // 缓存的设备信息（EM_FORMATRANGE,0,0）。
                tb->map[L"printRichedit"] = NF([this](vector<Value>& a)
                    -> Value {
                    if (a.size() < 3) return Value::Num_(0);
                    // aardio 的 hwnd/hdc：Pointer（官方控件的 hwnd 带
                    // _meta._topointer 元方法——ptr 可能为 0，真句柄在
                    // 元方法里）或纯数值。与 raw FFI 的 case-5 解引用同款。
                    auto resolveH = [this](const Value& v) -> void* {
                        if (v.t == Value::Pointer)
                        {
                            if (v.ptr) return (void*)v.ptr;
                            if (v.tab)
                            {
                                auto im = v.tab->map.find(L"_meta");
                                if (im != v.tab->map.end() &&
                                    (im->second.t == Value::TableT ||
                                     im->second.t == Value::Class) &&
                                    im->second.tab)
                                {
                                    auto itp = im->second.tab->map.find(L"_topointer");
                                    if (itp != im->second.tab->map.end())
                                    {
                                        if (itp->second.t == Value::Pointer)
                                            return (void*)itp->second.ptr;
                                        if (itp->second.t == Value::Func ||
                                            itp->second.native)
                                        {
                                            vector<Value> noArgs;
                                            Value r = CallOwner(itp->second, v, noArgs);
                                            if (r.t == Value::Pointer)
                                                return (void*)r.ptr;
                                            return (void*)(INT_PTR)BNum(r);
                                        }
                                    }
                                }
                            }
                            return nullptr;
                        }
                        return (void*)(INT_PTR)BNum(v);
                    };
                    HWND hwndEdit = (HWND)resolveH(a[0]);
                    HDC hdc = (HDC)resolveH(a[1]);
                    bool bRender = BNum(a[2]) != 0;
                    if (!hwndEdit || !hdc) return Value::Num_(0);
                    // richedit.h 不在本编译单元：FORMATRANGE/CHARRANGE 内联定义
                    struct CHARRANGE_T { LONG cpMin; LONG cpMax; };
                    struct FORMATRANGE_T
                    {
                        HDC hdc;
                        HDC hdcTarget;
                        RECT rc;
                        RECT rcPage;
                        CHARRANGE_T chrg;
                    };
                    LONG last = 0;
                    int pages = 0;
                    while (pages < 9999)
                    {
                        FORMATRANGE_T fr = {};
                        fr.hdc = hdc;
                        fr.hdcTarget = hdc;
                        RECT rc = {0, 0,
                            GetDeviceCaps(hdc, 8 /*HORZRES*/),
                            GetDeviceCaps(hdc, 10 /*VERTRES*/)};
                        fr.rc = rc;
                        fr.rcPage = rc;
                        fr.chrg.cpMin = last;
                        fr.chrg.cpMax = -1;
                        LONG next;
                        if (bRender)
                        {
                            StartPage(hdc);
                            next = (LONG)SendMessageW(hwndEdit,
                                0x439 /*EM_FORMATRANGE*/, 1, (LPARAM)&fr);
                            EndPage(hdc);
                        }
                        else
                        {
                            next = (LONG)SendMessageW(hwndEdit,
                                0x439 /*EM_FORMATRANGE*/, 0, (LPARAM)&fr);
                        }
                        if (next <= last) break;
                        pages++;
                        last = next;
                    }
                    SendMessageW(hwndEdit, 0x439, 0, 0);
                    // 诊断：pages==0 时回传 -hwnd（非 0 = 句柄已到位、消息真返回 0）
                    if (pages == 0) return Value::Num_(-(double)(INT_PTR)hwndEdit);
                    return Value::Num_(pages);
                });
                // 第五十九轮续：打印补完（对话框 + 预览 + 页码范围）。
                // 与 printRichedit 同一条 EM_FORMATRANGE 链的三个变体：
                //
                // printPageCount2(hwndEdit, hdc, hdcTarget, w, h)：
                //   **渲染 DC 与排版目标分离**的页数测量（FORMATRANGE 的
                //   hdc/hdcTarget 本来就是两个字段——官方 WordPad 打印预览
                //   即此用法：dc=屏幕、hdcTarget=打印机，richedit 按
                //   目标设备排版、按渲染 DC 缩放出图）。w/h 是渲染 DC 上的
                //   页面像素（预览窗里就是缩小的页）。返回页数。
                //
                // printPagePaint(hwndEdit, hwndDst, hdcPrinter, pageNo, mode)：
                //   把第 pageNo 页渲染进 hwndDst 的客户区（打印预览画面）。
                //   mode 0 = 屏幕 DC + 96dpi 缩小页（hdcTarget=打印机）；
                //   mode 1 = 打印机 DC 兼容内存 DC 满分辨率渲染再拉伸贴回
                //   （两种都保留，探针定胜负，实测哪条 WYSIWYG 用哪条）。
                //   内部 GetDC + 白底 + StretchBlt(HALFTONE) + ValidateRect，
                //   不经过 BeginPaint（PAINTSTRUCT 编组绕开）。
                //
                // printRange(hwndEdit, hdcPrinter, from, to)：
                //   真打印，带页码范围（1 基闭区间；先测量总页数钳边界，
                //   范围外只推进不 StartPage——不出空白页）。返回实打印页数。
                // 打印预览：按**打印机版面**排版、按**预览尺寸**出图。
                //
                // ��� 条踩过的坑（都是实测，不是推测）：
                //  ① EM_FORMATRANGE 是 **0x439**，不是 0x43D（0x43D 是
                //     EM_GETPARAFORMAT）——消息发错 → 永远返回 0 → 页数恒 0、
                //     预览恒空白。官方 win/#DEFINE.aardio:1428 就是 0x439。
                //  ② 预览 DC 必须是**打印机兼容 DC**（CreateCompatibleDC
                //     (printerDC)）：它的 LOGPIXELS 与打印机一致，排版才
                //     与真打印一致；再叠 MM_ANISOTROPIC（窗口范围=打印页面
                //     像素、视口范围=预览像素），600dpi 版面就缩到了屏幕尺寸。
                //     踩坑记录：拿屏幕 DC 当渲染 DC（hdcTarget=打印机）时，
                //     200 行短文能排出 **147 页**（排版按屏幕宽度折行）。
                //  ③ 预览页码与真打印页码必须一致，否则预览是骗人的——
                //     printPreviewCount 与 printRichedit(false) 的页数
                //     不一致时，调用方应回退到全分辨率模式。
                //
                // printPreviewCount(hwndEdit, hdcPrinter, previewW, previewH)
                //   → 预览版面下的页数。
                // printPreviewPaint(hwndEdit, hwndDst, hdcPrinter, pageNo,
                //                   previewW, previewH, mode)
                //   → 把第 pageNo 页画进 hwndDst 客户区（居中 + 深灰底 +
                //     页边框），返回 1/0。mode 0 = 预览缩放，mode 1 = 全分辨率。
                tb->map[L"printPreviewCount"] = NF([](vector<Value>& a)
                    -> Value {
                    if (a.size() < 4) return Value::Num_(0);
                    auto resolveH = [](const Value& v) -> void* {
                        if (v.t == Value::Pointer)
                        {
                            if (v.ptr) return (void*)v.ptr;
                            return nullptr;
                        }
                        return (void*)(INT_PTR)BNum(v);
                    };
                    HWND hwndEdit = (HWND)resolveH(a[0]);
                    HDC hdcPrinter = (HDC)resolveH(a[1]);
                    int prevW = (int)BNum(a[2]);
                    int prevH = (int)BNum(a[3]);
                    if (!hwndEdit || !hdcPrinter || prevW <= 0 || prevH <= 0)
                        return Value::Num_(0);
                    int pageW = GetDeviceCaps(hdcPrinter, 8 /*HORZRES*/);
                    int pageH = GetDeviceCaps(hdcPrinter, 10 /*VERTRES*/);
                    if (pageW <= 0 || pageH <= 0) return Value::Num_(0);
                    HDC hdcMem = CreateCompatibleDC(hdcPrinter);
                    HBITMAP hbmp = CreateCompatibleBitmap(hdcPrinter,
                        prevW, prevH);
                    if (!hdcMem || !hbmp)
                    {
                        if (hbmp) DeleteObject(hbmp);
                        if (hdcMem) DeleteDC(hdcMem);
                        return Value::Num_(0);
                    }
                    HBITMAP old = (HBITMAP)SelectObject(hdcMem, hbmp);
                    SetMapMode(hdcMem, MM_ANISOTROPIC);
                    SetWindowExtEx(hdcMem, pageW, pageH, nullptr);
                    SetViewportExtEx(hdcMem, prevW, prevH, nullptr);
                    struct CHARRANGE_T { LONG cpMin; LONG cpMax; };
                    struct FORMATRANGE_T
                    {
                        HDC hdc;
                        HDC hdcTarget;
                        RECT rc;
                        RECT rcPage;
                        CHARRANGE_T chrg;
                    };
                    LONG last = 0;
                    int pages = 0;
                    while (pages < 9999)
                    {
                        FORMATRANGE_T fr = {};
                        fr.hdc = hdcMem;
                        fr.hdcTarget = hdcPrinter;
                        fr.rc = {0, 0, pageW, pageH};
                        fr.rcPage = fr.rc;
                        fr.chrg.cpMin = last;
                        fr.chrg.cpMax = -1;
                        LONG next = (LONG)SendMessageW(hwndEdit,
                            0x439 /*EM_FORMATRANGE*/, 0, (LPARAM)&fr);
                        if (next <= last) break;
                        pages++;
                        last = next;
                    }
                    SendMessageW(hwndEdit, 0x439, 0, 0);
                    SetMapMode(hdcMem, MM_TEXT);
                    SelectObject(hdcMem, old);
                    DeleteObject(hbmp);
                    DeleteDC(hdcMem);
                    return Value::Num_(pages);
                });
                tb->map[L"printPreviewPaint"] = NF([](vector<Value>& a)
                    -> Value {
                    if (a.size() < 6) return Value::Num_(0);
                    auto resolveH = [](const Value& v) -> void* {
                        if (v.t == Value::Pointer)
                        {
                            if (v.ptr) return (void*)v.ptr;
                            return nullptr;
                        }
                        return (void*)(INT_PTR)BNum(v);
                    };
                    HWND hwndEdit = (HWND)resolveH(a[0]);
                    HWND hwndDst = (HWND)resolveH(a[1]);
                    HDC hdcPrinter = (HDC)resolveH(a[2]);
                    int pageNo = (int)BNum(a[3]);
                    int prevW = (int)BNum(a[4]);
                    int prevH = (int)BNum(a[5]);
                    int mode = a.size() >= 7 ? (int)BNum(a[6]) : 0;
                    if (!hwndEdit || !hwndDst || !hdcPrinter || pageNo < 1 ||
                        prevW <= 0 || prevH <= 0)
                        return Value::Num_(0);
                    int pageW = GetDeviceCaps(hdcPrinter, 8 /*HORZRES*/);
                    int pageH = GetDeviceCaps(hdcPrinter, 10 /*VERTRES*/);
                    if (pageW <= 0 || pageH <= 0) return Value::Num_(0);
                    HDC hdcDraw = GetDC(hwndDst);
                    if (!hdcDraw) return Value::Num_(0);
                    RECT rcClient = {};
                    GetClientRect(hwndDst, &rcClient);
                    // mode 0 = 预览缩放（打印机版面 → 预览位图）
                    // mode 1 = 全分辨率（打印机版面原尺寸渲染再拉伸）
                    int renderW = (mode == 1) ? pageW : prevW;
                    int renderH = (mode == 1) ? pageH : prevH;
                    HDC hdcMem = CreateCompatibleDC(hdcPrinter);
                    HBITMAP hbmp = CreateCompatibleBitmap(hdcPrinter,
                        renderW, renderH);
                    if (!hdcMem || !hbmp)
                    {
                        if (hbmp) DeleteObject(hbmp);
                        if (hdcMem) DeleteDC(hdcMem);
                        ReleaseDC(hwndDst, hdcDraw);
                        return Value::Num_(0);
                    }
                    HBITMAP old = (HBITMAP)SelectObject(hdcMem, hbmp);
                    if (mode != 1)
                    {
                        SetMapMode(hdcMem, MM_ANISOTROPIC);
                        SetWindowExtEx(hdcMem, pageW, pageH, nullptr);
                        SetViewportExtEx(hdcMem, renderW, renderH, nullptr);
                    }
                    RECT rcAll = {0, 0, renderW, renderH};
                    FillRect(hdcMem, &rcAll,
                        (HBRUSH)GetStockObject(WHITE_BRUSH));
                    struct CHARRANGE_T { LONG cpMin; LONG cpMax; };
                    struct FORMATRANGE_T
                    {
                        HDC hdc;
                        HDC hdcTarget;
                        RECT rc;
                        RECT rcPage;
                        CHARRANGE_T chrg;
                    };
                    LONG last = 0;
                    bool rendered = false;
                    for (int i = 1; i <= pageNo && i < 9999; i++)
                    {
                        FORMATRANGE_T fr = {};
                        fr.hdc = hdcMem;
                        fr.hdcTarget = hdcPrinter;
                        // ⚠ rc 用**打印页面像素**（渲染 DC 的逻辑单位），
                        //   不是预览位图尺寸：MM_ANISOTROPIC 下 logical
                        //   4760×6814 → device 560×801，缩放由映射模式做。
                        //   写错成预览尺寸 → 页面被缩到 1/8、文字小到看不见
                        //   （实测：整页纯白、0 个深色像素）。
                        fr.rc = {0, 0, pageW, pageH};
                        fr.rcPage = fr.rc;
                        fr.chrg.cpMin = last;
                        fr.chrg.cpMax = -1;
                        LONG next = (LONG)SendMessageW(hwndEdit,
                            0x439 /*EM_FORMATRANGE*/,
                            i == pageNo ? 1 : 0, (LPARAM)&fr);
                        if (next <= last) break;
                        last = next;
                        if (i == pageNo) rendered = true;
                    }
                    SendMessageW(hwndEdit, 0x439, 0, 0);
                    int ink = 0;
                    if (rendered)
                    {
                        // 着墨像素统计（抽样）：页面真的是空的就返回 0，
                        // 调用方（预览窗/冒烟）据此判「预览有内容」。
                        for (int yy = 0; yy < renderH; yy += 4)
                        {
                            for (int xx = 0; xx < renderW; xx += 4)
                            {
                                COLORREF c = GetPixel(hdcMem, xx, yy);
                                if (c != RGB(0xFF, 0xFF, 0xFF)) ink++;
                            }
                        }
                        HBRUSH bg = CreateSolidBrush(RGB(0x3a, 0x3e, 0x42));
                        FillRect(hdcDraw, &rcClient, bg);
                        DeleteObject(bg);
                        double scW = rcClient.right / (double)renderW;
                        double scH = rcClient.bottom / (double)renderH;
                        double sc = scW < scH ? scW : scH;
                        if (sc > 1.0) sc = 1.0;
                        int dw = (int)(renderW * sc), dh = (int)(renderH * sc);
                        int dx = (rcClient.right - dw) / 2;
                        int dy = (rcClient.bottom - dh) / 2;
                        SetStretchBltMode(hdcDraw, HALFTONE);
                        SetBrushOrgEx(hdcDraw, 0, 0, nullptr);
                        StretchBlt(hdcDraw, dx, dy, dw, dh,
                            hdcMem, 0, 0, renderW, renderH, SRCCOPY);
                        RECT rcFrame = {dx, dy, dx + dw, dy + dh};
                        HBRUSH fb = CreateSolidBrush(RGB(0x1c, 0x1e, 0x22));
                        FrameRect(hdcDraw, &rcFrame, fb);
                        DeleteObject(fb);
                        ValidateRect(hwndDst, nullptr);
                    }
                    SetMapMode(hdcMem, MM_TEXT);
                    SelectObject(hdcMem, old);
                    DeleteObject(hbmp);
                    DeleteDC(hdcMem);
                    ReleaseDC(hwndDst, hdcDraw);
                    return Value::Num_(rendered ? ink : -1);
                });
                tb->map[L"printRange"] = NF([](vector<Value>& a)
                    -> Value {
                    if (a.size() < 4) return Value::Num_(0);
                    auto resolveH = [](const Value& v) -> void* {
                        if (v.t == Value::Pointer)
                        {
                            if (v.ptr) return (void*)v.ptr;
                            return nullptr;
                        }
                        return (void*)(INT_PTR)BNum(v);
                    };
                    HWND hwndEdit = (HWND)resolveH(a[0]);
                    HDC hdc = (HDC)resolveH(a[1]);
                    int from = (int)BNum(a[2]);
                    int to = (int)BNum(a[3]);
                    if (!hwndEdit || !hdc) return Value::Num_(0);
                    struct CHARRANGE_T { LONG cpMin; LONG cpMax; };
                    struct FORMATRANGE_T
                    {
                        HDC hdc;
                        HDC hdcTarget;
                        RECT rc;
                        RECT rcPage;
                        CHARRANGE_T chrg;
                    };
                    RECT rcFull = {0, 0,
                        GetDeviceCaps(hdc, 8 /*HORZRES*/),
                        GetDeviceCaps(hdc, 10 /*VERTRES*/)};
                    // 先量总页数（钳 from/to 边界，防越界请求）
                    LONG last = 0;
                    int total = 0;
                    while (total < 9999)
                    {
                        FORMATRANGE_T fr = {};
                        fr.hdc = hdc;
                        fr.hdcTarget = hdc;
                        fr.rc = rcFull;
                        fr.rcPage = rcFull;
                        fr.chrg.cpMin = last;
                        fr.chrg.cpMax = -1;
                        LONG next = (LONG)SendMessageW(hwndEdit,
                            0x439, 0, (LPARAM)&fr);
                        if (next <= last) break;
                        total++;
                        last = next;
                    }
                    SendMessageW(hwndEdit, 0x439, 0, 0);
                    if (from < 1) from = 1;
                    if (to > total || to <= 0) to = total;
                    if (from > to) return Value::Num_(0);
                    // 再渲染：范围内 StartPage/EndPage，范围外只推进
                    int printed = 0;
                    last = 0;
                    for (int p = 1; p <= to && p < 9999; p++)
                    {
                        FORMATRANGE_T fr = {};
                        fr.hdc = hdc;
                        fr.hdcTarget = hdc;
                        fr.rc = rcFull;
                        fr.rcPage = rcFull;
                        fr.chrg.cpMin = last;
                        fr.chrg.cpMax = -1;
                        bool inRange = (p >= from);
                        if (inRange) StartPage(hdc);
                        LONG next = (LONG)SendMessageW(hwndEdit,
                            0x439, inRange ? 1 : 0, (LPARAM)&fr);
                        if (inRange) EndPage(hdc);
                        if (next <= last) break;
                        last = next;
                        if (inRange) printed++;
                        if (p >= to) break;
                    }
                    SendMessageW(hwndEdit, 0x439, 0, 0);
                    return Value::Num_(printed);
                });
                tb->map[L"loadDll"] = NF([this](vector<Value>& a)
                    -> Value {
                    HMODULE hMod = nullptr;
                    bool bIsMainMod = false;
                    // A memory DLL keeps its mapped image alive for the
                    // lifetime of the dll object; free() releases it.
                    auto memDll = std::make_shared<MemDll>();
                    if (a.empty() || a[0].t == Value::Null ||
                        (a[0].t == Value::Str && a[0].s.empty()))
                    {
                        hMod = GetModuleHandleW(nullptr);
                        bIsMainMod = true;
                    }
                    else if (a[0].t == Value::Str)
                    {
                        wstring strArg = BStr(a[0]);
                        // A payload-embedded DLL: the published exe carries
                        // res\ under its embedded FS, and $"~\path" only
                        // resolves at publish time (EmbedRoot needs
                        // NODIO_LIBDIR). Resolve it here from the embedded
                        // map, the same way io.file does, so a DLL
                        // shipped in a payload loads without a disk file.
                        {
                            wstring key = EmbeddedPayloadKey(strArg);
                            if (!key.empty())
                                strArg = g_interpCurrent->embedded.find(key)->second;
                        }
                        // Order matters: LoadLibraryW first, because a bare
                        // name like "user32.dll" must go through the normal
                        // DLL search path (GetFileAttributesW would reject
                        // it when the CWD is elsewhere). Only when that
                        // fails — or the argument is clearly not a path —
                        // treat the string as mapped image bytes.
                        // A mapped image starts with 'MZ' and is full of
                        // NULs in its headers; a path is short and NUL-free.
                        bool bLooksMapped =
                            strArg.size() >= 2 && strArg[0] == L'M' &&
                            strArg[1] == L'Z' &&
                            strArg.find(L'\0') != wstring::npos;
                        if (!strArg.empty())
                        {
                            // Try the disk path first so bare names
                            // ("user32.dll") go through the normal DLL search
                            // path, then fall back to mapping.
                            if (!bLooksMapped)
                                hMod = LoadLibraryW(
                                    ResolveTildeDisk(strArg).c_str());
                            if (!hMod && bLooksMapped)
                            {
                                void* pMemMod = nullptr;
                                if (MemDllLoad(strArg, &pMemMod,
                                               memDll.get()))
                                    hMod = (HMODULE)pMemMod;
                            }
                        }
                        if (!hMod)
                        {
                            // The argument may be a whole DLL image rather
                            // than a path; dumping the binary into the
                            // message buries the real cause.
                            wstring detail;
                            if (bLooksMapped)
                                detail = L"(内嵌的内存映像)";
                            else
                            {
                                wstring shown = strArg;
                                if (shown.size() > 120)
                                    shown = shown.substr(0, 120) + L"...";
                                detail = shown;
                            }
                            (*err) = L"运行错误: 无法加载 DLL: " + detail;
                            return Value();
                        }
                    }
                    else
                    {
                        (*err) = L"运行错误: raw.loadDll(路径或内存数据)";
                        return Value();
                    }
                    auto dll = std::make_shared<Table>();
                    dll->map[L"_h"] =
                        Value::Pointer_(hMod);
                    dll->map[L"_memdll"] =
                        Value::Pointer_(memDll->bOk ? memDll.get() : nullptr);
                    dll->map[L"gethandle"] = NF([hMod](vector<Value>&) {
                        return Value::Pointer_(hMod);
                    });
                    dll->map[L"free"] = NF([hMod, bIsMainMod, memDll](
                        vector<Value>&) -> Value {
                        if (bIsMainMod) return Value();
                        if (memDll->bOk) MemDllUnload(memDll.get());
                        else FreeLibrary(hMod);
                        return Value(); });
                    bool bDllCdecl = false;
                    bool bDllUni = false;
                    for (size_t ai = 1; ai < a.size(); ai++)
                    {
                        wstring conv = BStr(a[ai]);
                        for (auto& c : conv) c = towlower(c);
                        if (conv.find(L"cdecl") != wstring::npos) bDllCdecl = true;
                        if (conv.find(L"unicode") != wstring::npos) bDllUni = true;
                    }
                    dll->map[L"_cdecl"] = Value::Bool_(bDllCdecl);
                    dll->map[L"_unicode"] = Value::Bool_(bDllUni);
                    // api(name, signature) -> callable function value
                    dll->map[L"api"] = NF([this, hMod, memDll, bDllCdecl](
                        vector<Value>& a2) -> Value {
                        if (a2.empty())
                        {
                            (*err) = L"运行错误: dll.api(name, sig)";
                            return Value();
                        }
                        FARPROC fp = nullptr;
                        wstring strName;
                        if (a2[0].t == Value::Pointer)
                            fp = (FARPROC)a2[0].ptr;
                        else if (a2[0].t == Value::Num)
                        {
                            if (hMod && a2[0].n > 0 && a2[0].n < 65536)
                                fp = ::GetProcAddress(hMod, MAKEINTRESOURCEA((WORD)(INT_PTR)a2[0].n));
                            else
                                fp = (FARPROC)(INT_PTR)a2[0].n;
                        }
                        else
                        {
                            strName = BStr(a2[0]);
                        }
                        ApiSig sig;
                        wstring sigStr = a2.size() > 1 ? BStr(a2[1]) : L"void()";
                        if (!ParseApiSig(sigStr, &sig))
                        {
                            (*err) = L"运行错误: API 签名无效: " + sigStr;
                            return Value();
                        }
                        if (bDllCdecl) sig.bCdecl = true;
                        if (a2.size() > 2)
                        {
                            wstring cc = BStr(a2[2]);
                            for (auto& c : cc) c = towlower(c);
                            if (cc.find(L"cdecl") != wstring::npos) sig.bCdecl = true;
                            else if (cc.find(L"stdcall") != wstring::npos) sig.bCdecl = false;
                        }
                        bool bNameW = false;   // final export name ends in W
#if defined(_WIN64)
                        if (strName.rfind(L"FindFirstFile", 0) == 0)
                        {
                            if (sig.ret == ARInt) sig.ret = ARAddr;
                        }
                        else if (strName.rfind(L"FindNextFile", 0) == 0)
                        {
                            if (!sig.params.empty() && (sig.params[0] & 0xFF) == 0)
                                sig.params[0] = 5 | (sig.params[0] & 0x100);
                        }
                        else if (strName.rfind(L"FindClose", 0) == 0)
                        {
                            if (!sig.params.empty() && (sig.params[0] & 0xFF) == 0)
                                sig.params[0] = 5 | (sig.params[0] & 0x100);
                        }
                        else if (strName.rfind(L"CallWindowProc", 0) == 0 ||
                                 strName.rfind(L"DefWindowProc", 0) == 0)
                        {
                            // 官方 win/api.aardio:11 把这两个 API 声明成 int
                            // 返回（32 位进程的写法）。64 位下 LRESULT 是
                            // 指针宽的，int 会把窗口过程回传的指针句柄截成
                            // 低 32 位 —— 官方 treeview 的 HTREEITEM 就是
                            // 这么坏的：装上官方 wndproc（win.ui.tracker）
                            // 之后 SendMessage 只拿回半个句柄，插入子节点
                            // 直接失败。
                            if (sig.ret == ARInt) sig.ret = ARI64;
                        }
#endif
                        if (!fp && !strName.empty())
                        {
                            std::string strNameA((size_t)
                                WideCharToMultiByte(CP_ACP, 0,
                                    strName.c_str(), (int)strName.size(),
                                    nullptr, 0, nullptr, nullptr), 0);
                            if (!strNameA.empty())
                                WideCharToMultiByte(CP_ACP, 0,
                                    strName.c_str(), (int)strName.size(),
                                    &strNameA[0], (int)strNameA.size(),
                                    nullptr, nullptr);
                            char suffix = 0;
                            std::string bareName = strNameA;
                            if (strNameA.size() > 1 && strNameA.find('@') == std::string::npos)
                            {
                                char last = strNameA.back();
                                if (last == 'W' || last == 'A' || last == 'L' || last == 'P' ||
                                    last == 'D' || last == 'F' || last == 'B')
                                {
                                    suffix = last;
                                    bareName = strNameA.substr(0, strNameA.size() - 1);
                                }
                            }

                            if (memDll->bOk)
                            {
                                fp = MemDllGetProc(memDll->pImage, strNameA.c_str());
                                if (!fp && suffix)
                                {
                                    fp = MemDllGetProc(memDll->pImage, bareName.c_str());
                                    if (fp)
                                    {
                                        if (suffix == 'W') bNameW = true;
                                        else if (suffix == 'A') bNameW = false;
                                    }
                                }
                                if (!fp && strNameA.find('@') == std::string::npos)
                                {
                                    std::string w = (suffix ? bareName : strNameA) + "W";
                                    fp = MemDllGetProc(memDll->pImage, w.c_str());
                                    if (fp) bNameW = true;
                                }
                                if (!fp && strNameA.find('@') == std::string::npos)
                                {
                                    std::string an = (suffix ? bareName : strNameA) + "A";
                                    fp = MemDllGetProc(memDll->pImage, an.c_str());
                                    if (fp) bNameW = false;
                                }
                            }
                            else
                            {
                                // 1. Exact export name
                                fp = ::GetProcAddress(hMod, strNameA.c_str());

                                // 2. Bare export name without suffix
                                if (!fp && suffix)
                                {
                                    fp = ::GetProcAddress(hMod, bareName.c_str());
                                    if (fp)
                                    {
                                        if (suffix == 'W') bNameW = true;
                                        else if (suffix == 'A') bNameW = false;
                                    }
                                }

                                // 3. Unicode variant (appending 'W')
                                if (!fp && strNameA.find('@') == std::string::npos)
                                {
                                    std::string w = (suffix ? bareName : strNameA) + "W";
                                    fp = ::GetProcAddress(hMod, w.c_str());
                                    if (fp) bNameW = true;
                                }

                                // 4. ANSI variant (appending 'A')
                                if (!fp && strNameA.find('@') == std::string::npos)
                                {
                                    std::string an = (suffix ? bareName : strNameA) + "A";
                                    fp = ::GetProcAddress(hMod, an.c_str());
                                    if (fp) bNameW = false;
                                }
                            }
                            if (!bNameW && suffix == 'W')
                                bNameW = true;
                            if (fp)
                                fp = ResolveLongPtrAlias(hMod, fp, strNameA);
                        }
                        if (!fp)
                        {
                            // api() on a missing
                            // export returns null (callers guard with
                            // try/catch)
                            if (a2.size() <= 1 || a2[1].t == Value::Null)
                                return Value();
                            (*err) = L"运行错误: 找不到 API: " + strName;
                            return Value();
                        }
                        if (a2.size() <= 1 || a2[1].t == Value::Null)
                        {
                            // dll.api("symbol") with no signature
                            // returns the exported symbol address as a pointer
                            // (e.g. PyBool_Type = ::Python3.api("PyBool_Type"), _Py_NoneStruct,
                            // or feature probe: if (::Kernel32.api("SomeFunc")))
                            return Value::Pointer_((void*)fp);
                        }
                        void* fn = (void*)fp;
                        // raw.interface / COM vtable: app.api(fnPtr, proto,
                        // cc, thisPtr) — the 4th arg binds an implicit this
                        // prepended to every call's argument list
                        void* pThisBound = nullptr;
                        if (a2.size() > 3 && (a2[3].t == Value::Pointer))
                            pThisBound = a2[3].ptr;
                        else if (a2.size() > 3 && a2[3].t == Value::Num)
                            pThisBound = (void*)(INT_PTR)(long long)a2[3].n;
#if defined(_WIN64)
                        // one executable call stub per bound signature —
                        // carries float/double args into XMM0..3
                        // bound this: the stub must treat the this
                        // pointer as parameter slot 0 (RCX) — the marshal
                        // path below already packs aBound (this + args),
                        // so the stub needs the synthetic leading ptr arg
                        ApiSig sigStub = sig;
                        if (pThisBound)
                            sigStub.params.insert(sigStub.params.begin(), 5);
                        void* stub = EmitX64CallStub(sigStub);
#else
                        void* stub = nullptr;
#endif
                        Value retFn = NF([this, fn, sig, stub, bNameW,
                                          pThisBound](vector<Value>& a3)
                            -> Value {
                            // box arguments into integer slots
                            vector<SlotT> slots;
                            std::vector<std::string> tmpA;
                            std::vector<wstring> tmpW;
                            vector<Value> aBound;
                            if (pThisBound)
                            {
                                aBound.push_back(
                                    Value::Pointer_(pThisBound));
                                for (auto& v : a3)
                                    aBound.push_back(v);
                            }
                            vector<Value>& argsEff =
                                pThisBound ? aBound : a3;
                            size_t nA = 0, nW = 0;
                            // aBound[0] is the bound this — declared
                            // parameters start at index 1
                            size_t nOff = pThisBound ? 1 : 0;
                            for (size_t i = 0; i < sig.params.size();
                                 i++)
                            {
                                Value v = (i + nOff) < argsEff.size()
                                    ? argsEff[i + nOff] : Value();
                                if ((sig.params[i] == 6 || sig.params[i] == 11) && v.t == Value::Str)
                                    nA++;
                                if (sig.params[i] == 7 && v.t == Value::Str)
                                    nW++;
                            }
                            tmpA.reserve(nA);
                            tmpW.reserve(nW);
                            struct ApiArgRef
                            {
                                size_t paramIdx = 0;
                                int paramType = 0;
                                void* pBuffer = nullptr;
                                bool bStruct = false;
                                Value origVal;
                                bool bNoFree = false;   // caller-owned buffer
                                size_t bufLen = 0;
                            };
                            std::vector<ApiArgRef> tmpRefs;
                            // bound this (COM vtable): RCX must be the
                            // interface pointer — put it in slot 0 ahead
                            // of the declared parameters
                            if (pThisBound)
                                slots.push_back((SlotT)(INT_PTR)pThisBound);
                            for (size_t i = 0; i < sig.params.size();
                                 i++)
                            {
                                Value v = (i + nOff) < argsEff.size()
                                    ? argsEff[i + nOff] : Value();
                                int pt = sig.params[i] & 0xFF;
                                // a W-suffixed export takes
                                // `str` params as UTF-16 (string.getenv with
                                // GetEnvironmentVariable passes "str name" to
                                // the auto-W'd GetEnvironmentVariableW)
                                if (bNameW && pt == 6) pt = 7;
                                bool bByRef =
                                    (sig.params[i] & 0x100) != 0;
                                // & out-params: allocate scratch memory, copy initial value if passed
                                if (bByRef && (pt == 0 || pt == 1 || pt == 2 ||
                                               pt == 3 || pt == 4 || pt == 5 ||
                                               pt == 9 || pt == 10 || pt == 12))
                                {
                                    void* pRef = HeapAlloc(
                                        GetProcessHeap(),
                                        HEAP_ZERO_MEMORY, 8);
                                    if (pt == 0 || pt == 4) *(int*)pRef = (int)(unsigned int)(long long)BNum(v);
                                    else if (pt == 1) *(__int64*)pRef = (__int64)BNum(v);
                                    else if (pt == 2) *(unsigned short*)pRef = (unsigned short)(unsigned int)(long long)BNum(v);
                                    else if (pt == 3) *(unsigned char*)pRef = (unsigned char)(unsigned int)(long long)BNum(v);
                                    else if (pt == 5 || pt == 12) *(INT_PTR*)pRef = (INT_PTR)(v.t == Value::Pointer ? (INT_PTR)v.ptr : (INT_PTR)(long long)BNum(v));
                                    else if (pt == 9) { float f = (float)BNum(v); memcpy(pRef, &f, sizeof(float)); }
                                    else if (pt == 10) { double d = BNum(v); memcpy(pRef, &d, sizeof(double)); }
                                    tmpRefs.push_back({ i, pt, pRef, false, v });
                                    slots.push_back((SlotT)(INT_PTR)pRef);
                                    continue;
                                }
                                if (bByRef && (pt == 6 || pt == 7 || pt == 11))
                                {
                                    // string-by-ref contract: a NUMBER is the
                                    // buffer length — bytes for string &,
                                    // wchars for ustring & — zeroed, plus
                                    // a 2-byte null tail; 0 passes a null
                                    // pointer. A STRING is seeded into a
                                    // temp buffer the API may overwrite;
                                    // a Pointer (raw.buffer) is passed
                                    // as-is. The final content always
                                    // comes back as an extra string
                                    // return value. Passing the raw
                                    // number as the pointer (the old
                                    // behaviour) let FormatMessage write
                                    // through address 0x100 and crash.
                                    if (v.t == Value::Num)
                                    {
                                        long long nU = (long long)BNum(v);
                                        if (nU <= 0)
                                        {
                                            tmpRefs.push_back(
                                                { i, pt, nullptr, false, Value(), true, 0 });
                                            slots.push_back((SlotT)0);
                                            continue;
                                        }
                                        size_t cb =
                                            (size_t)nU * (pt == 7 ? 2 : 1) +
                                            2;
                                        void* pRef = HeapAlloc(
                                            GetProcessHeap(),
                                            HEAP_ZERO_MEMORY, cb);
                                        tmpRefs.push_back(
                                            { i, pt, pRef, false, Value(), false, (size_t)nU });
                                        slots.push_back((SlotT)(INT_PTR)pRef);
                                        continue;
                                    }
                                    if (v.t == Value::Str)
                                    {
                                        void* pRef;
                                        if (pt == 7)
                                        {
                                            wstring w = BStr(v);
                                            size_t cb = (w.size() + 1) * 2;
                                            pRef = HeapAlloc(GetProcessHeap(),
                                                             0, cb);
                                            memcpy(pRef, w.c_str(), cb - 2);
                                            ((wchar_t*)pRef)[w.size()] = 0;
                                            tmpRefs.push_back(
                                                { i, pt, pRef, false, v, false, w.size() * 2 });
                                        }
                                        else if (pt == 11)
                                        {
                                            wstring w = BStr(v);
                                            std::string sA;
                                            sA.reserve(w.size());
                                            for (wchar_t ch : w)
                                                sA.push_back((char)(unsigned char)ch);
                                            sA.push_back('\0');
                                            pRef = HeapAlloc(GetProcessHeap(),
                                                             0, sA.size());
                                            memcpy(pRef, sA.c_str(),
                                                   sA.size());
                                            tmpRefs.push_back(
                                                { i, pt, pRef, false, v, false, w.size() });
                                        }
                                        else
                                        {
                                            wstring w = BStr(v);
                                            std::string sA((size_t)
                                                WideCharToMultiByte(CP_ACP,
                                                    0, w.c_str(),
                                                    (int)w.size(), nullptr, 0,
                                                    nullptr, nullptr), 0);
                                            if (!sA.empty())
                                                WideCharToMultiByte(CP_ACP,
                                                    0, w.c_str(),
                                                    (int)w.size(), &sA[0],
                                                    (int)sA.size(), nullptr,
                                                    nullptr);
                                            sA.push_back('\0');
                                            pRef = HeapAlloc(GetProcessHeap(),
                                                             0, sA.size());
                                            memcpy(pRef, sA.c_str(),
                                                   sA.size());
                                            tmpRefs.push_back(
                                                { i, pt, pRef, false, v, false, sA.size() - 1 });
                                        }
                                        slots.push_back((SlotT)(INT_PTR)pRef);
                                        continue;
                                    }
                                    if (v.t == Value::Pointer && v.ptr)
                                    {
                                        // caller-owned raw buffer: the
                                        // content is read back but the
                                        // memory is never freed here
                                        size_t bsz = GetBufferSize(v.ptr);
                                        tmpRefs.push_back(
                                            { i, pt, v.ptr, false, v, true, bsz });
                                        slots.push_back((SlotT)(INT_PTR)v.ptr);
                                        continue;
                                    }
                                    tmpRefs.push_back(
                                        { i, pt, nullptr, false, Value(), true, 0 });
                                    slots.push_back((SlotT)0);
                                    continue;
                                }
                                if (pt == 8) // struct
                                {
                                    if (v.t == Value::TableT || v.t == Value::Class)
                                    {
                                        int total = 0;
                                        // zeroed slack: the callee may use a
                                        // wider platform layout than the
                                        // declared one (x64 ULONG_PTR vs the
                                        // library's `int`) and would read
                                        // past the end of the buffer.
                                        void* pBuf = StructSerialize(
                                            v, &total, StructOverreadSlack(v));
                                        if (pBuf)
                                        {
                                            // auto-maintains cbSize:
                                            // a struct whose first member is
                                            // cbSize gets it stamped with the
                                            // real size on every API call
                                            if (total >= 4 && v.tab &&
                                                v.tab->map.find(L"cbSize") !=
                                                    v.tab->map.end())
                                                *(int*)pBuf = total;
                                            slots.push_back((SlotT)(INT_PTR)pBuf);
                                            tmpRefs.push_back({ i, pt, pBuf, true, v });
                                            continue;
                                        }
                                    }
                                    if (v.t == Value::Null)
                                    {
                                        slots.push_back((SlotT)0);
                                        continue;
                                    }
                                    if (v.t == Value::Pointer)
                                    {
                                        slots.push_back((SlotT)(INT_PTR)v.ptr);
                                        continue;
                                    }
                                    slots.push_back((SlotT)(INT_PTR)(long long)BNum(v));
                                    continue;
                                }
                                switch (pt)
                                {
                                case 0:   // int
                                    // wrap modulo 2^32 (numbers:
                                    // 0xFFFFFFF0 must arrive as -16, not
                                    // the MSVC saturate value)
                                    slots.push_back((SlotT)(
                                        INT_PTR)(int)(unsigned int)
                                            (long long)BNum(v));
                                    break;
                                case 1:   // int64
#if defined(_M_IX86)
                                {
                                    long long v64 =
                                        (long long)BNum(v);
                                    slots.push_back((SlotT)
                                        (v64 & 0xFFFFFFFF));
                                    slots.push_back((SlotT)
                                        ((unsigned long long)v64
                                         >> 32));
                                    break;
                                }
#else
                                    slots.push_back((SlotT)
                                        (unsigned long long)
                                            (long long)BNum(v));
                                    break;
#endif
                                case 2:   // word
                                    slots.push_back((SlotT)
                                        (unsigned short)(unsigned int)
                                            (long long)BNum(v));
                                    break;
                                case 3:   // byte
                                    slots.push_back((SlotT)
                                        (unsigned char)(unsigned int)
                                            (long long)BNum(v));
                                    break;
                                case 4:   // bool
                                    slots.push_back(
                                        BNum(v) != 0 ? 1 : 0);
                                    break;
                                case 12:  // addr / hwnd / handle
                                    if (v.t == Value::Pointer)
                                        slots.push_back((SlotT)(INT_PTR)v.ptr);
                                    else
                                        slots.push_back((SlotT)(INT_PTR)(long long)BNum(v));
                                    break;
                                case 5:   // pointer (hwnd / buffer)
                                    if (v.t == Value::Pointer)
                                    {
                                        if (v.tab)
                                        {
                                            auto im = v.tab->map.find(L"_meta");
                                            if (im != v.tab->map.end() && (im->second.t == Value::TableT || im->second.t == Value::Class) && im->second.tab)
                                            {
                                                auto itp = im->second.tab->map.find(L"_topointer");
                                                if (itp != im->second.tab->map.end())
                                                {
                                                    if (itp->second.t == Value::Pointer)
                                                    {
                                                        slots.push_back((SlotT)(INT_PTR)itp->second.ptr);
                                                        break;
                                                    }
                                                    else if (itp->second.t == Value::Func || itp->second.native)
                                                    {
                                                        vector<Value> noArgs;
                                                        Value r = CallOwner(itp->second, v, noArgs);
                                                        slots.push_back(r.t == Value::Pointer
                                                            ? (SlotT)(INT_PTR)r.ptr
                                                            : (SlotT)(INT_PTR)(long long)BNum(r));
                                                        break;
                                                    }
                                                }
                                            }
                                        }
                                        slots.push_back((SlotT)(INT_PTR)v.ptr);
                                    }
                                    else if (v.t == Value::TableT && v.tab)
                                    {
                                        // an object with a _topointer meta
                                        // method (gdip.graphics/font/bitmap)
                                        // marshals as its native pointer —
                                        // the official FFI contract
                                        Value vp;
                                        bool bE = false; wstring se;
                                        bool bGot = MetaGet(v, L"_topointer",
                                                            &vp, &bE, &se,
                                                            true);
                                        if (bGot && vp.t == Value::Func)
                                        {
                                            vector<Value> noArgs;
                                            Value r = vp.native
                                                ? vp.native(vp.nativeCtx.get(), noArgs)
                                                : CallVmFuncStatic(
                                                      v, vp, noArgs, &bE,
                                                      &se);
                                            slots.push_back(
                                                r.t == Value::Pointer
                                                    ? (SlotT)(INT_PTR)r.ptr
                                                    : (SlotT)(INT_PTR)
                                                          (long long)BNum(r));
                                        }
                                        else
                                            slots.push_back((SlotT)0);
                                    }
                                    else if (v.t == Value::Str)
                                    {
                                        std::string sA = RawBytesOf(v);
                                        sA.push_back('\0');
                                        tmpA.push_back(std::move(sA));
                                        slots.push_back((SlotT)(INT_PTR)tmpA.back().data());
                                    }
                                    else
                                        slots.push_back(v.t == Value::Null
                                            ? (SlotT)0
                                            : (SlotT)(INT_PTR)(long long)
                                                  BNum(v));
                                    break;
                                case 6:   // str (ANSI)
                                case 11:  // string (binary)
                                    if (v.t == Value::Null)
                                        slots.push_back((SlotT)0);
                                    else if (v.t == Value::Pointer)
                                        slots.push_back((SlotT)(INT_PTR)v.ptr);
                                    else if (v.t == Value::Num)
                                        slots.push_back((SlotT)
                                            (INT_PTR)(long long)
                                                BNum(v));
                                    else
                                    {
                                        wstring w = BStr(v);
                                        std::string sA;
                                        if (pt == 11 || v.bRawBytes)
                                        {
                                            sA = RawBytesOf(v);
                                        }
                                        else
                                        {
                                            sA.resize((size_t)WideCharToMultiByte(
                                                CP_ACP, 0, w.c_str(),
                                                (int)w.size(), nullptr,
                                                0, nullptr, nullptr));
                                            if (!sA.empty())
                                                WideCharToMultiByte(CP_ACP,
                                                    0, w.c_str(),
                                                    (int)w.size(), &sA[0],
                                                    (int)sA.size(), nullptr,
                                                    nullptr);
                                        }
                                        sA.push_back('\0');
                                        tmpA.push_back(std::move(sA));
                                        slots.push_back((SlotT)
                                            (INT_PTR)
                                                tmpA.back().c_str());
                                    }
                                    break;
                                case 7:   // wstr (UTF-16)
                                    if (v.t == Value::Null)
                                        slots.push_back((SlotT)0);
                                    else if (v.t == Value::Pointer)
                                        slots.push_back((SlotT)(INT_PTR)v.ptr);
                                    else if (v.t == Value::Num)
                                        slots.push_back((SlotT)
                                            (INT_PTR)(long long)
                                                BNum(v));
                                    else
                                    {
                                        tmpW.push_back(BStr(v));
                                        slots.push_back((SlotT)
                                            (INT_PTR)
                                                tmpW.back().c_str());
                                    }
                                    break;
                                case 9:   // float
                                {
                                    float f = (float)BNum(v);
                                    SlotT s = 0;
                                    memcpy(&s, &f, sizeof(float));
                                    slots.push_back(s);
                                    break;
                                }
                                case 10:  // double
                                {
#if defined(_M_IX86)
                                    double d = BNum(v);
                                    unsigned __int64 u64 = 0;
                                    memcpy(&u64, &d, sizeof(double));
                                    slots.push_back((SlotT)(u64 & 0xFFFFFFFF));
                                    slots.push_back((SlotT)(u64 >> 32));
#else
                                    double d = BNum(v);
                                    SlotT s = 0;
                                    memcpy(&s, &d, sizeof(double));
                                    slots.push_back(s);
#endif
                                    break;
                                }
                                default:
                                    slots.push_back(0);
                                    break;
                                }
                            }
                            if (slots.size() > 32)
                            {
                                (*err) =
                                    L"运行错误: API 参数超过 32 个";
                                return Value();
                            }
                            // invoke, then interpret the result.
                            // x64: the per-signature call stub routes
                            // float/double params through XMM0..3 per the
                            // ABI; a float/double return arrives bit-cast
                            // in RAX (stub epilogue) and is re-decoded.
#if defined(_WIN64)
                            // x64 只有一套调用约定（MS x64 ABI）：编译器忽略
                            // __cdecl / __stdcall 的区别，float/double 实参必须
                            // 走 XMM0..3。而 InvokeCdl / InvokeStd 的定型指针
                            // 把**每个**实参都当成整数类（RCX/RDX/R8/R9），所以
                            // 带 "cdecl" 的调用会让被调方读到垃圾而不是那个
                            // double —— 实测：msvcrt pow(2.0,10.0) 返回 1、
                            // av.audio.dsp 的 a_fill 写不进缓冲区。
                            // => x64 一律走 ABI 正确的调用桩。
                            bool bStub = (stub != nullptr);
#else
                            bool bStub = false;
#endif
                            if (bStub && sig.ret == ARVoid)
                            {
#if defined(_WIN64)
                                InvokeX64Stub(stub, fn, slots);
#endif
                                return Value();
                            }
                            if (sig.ret == ARDbl)
                            {
                                double d;
#if defined(_WIN64)
                                if (bStub)
                                {
                                    __int64 rb =
                                        InvokeX64Stub(stub, fn, slots);
                                    memcpy(&d, &rb, 8);
                                }
                                else
#endif
                                    d = sig.bCdecl
                                        ? InvokeCdl<double>(fn, slots)
                                        : InvokeStd<double>(fn, slots);
                                return Value::Num_(d);
                            }
                            __int64 r;
#if defined(_WIN64)
                            if (bStub)
                                r = InvokeX64Stub(stub, fn, slots);
                            else
#endif
                                r = sig.bCdecl
                                    ? InvokeCdl<__int64>(fn, slots)
                                    : InvokeStd<__int64>(fn, slots);
                            // decode the primary return
                            Value vRet;
                            // a W-suffixed export returning "str" hands back
                            // UTF-16 even when the sig spelled it str
                            int retW = sig.ret == ARStrA && bNameW ? ARStrW
                                                                   : sig.ret;
                            switch (retW)
                            {
                            case ARInt:
                                vRet = Value::Num_((double)(int)r);
                                break;
                            case ARWord:
                                vRet = Value::Num_(
                                    (double)(unsigned short)r);
                                break;
                            case ARByte:
                                vRet = Value::Num_(
                                    (double)(unsigned char)r);
                                break;
                            case ARBool:
                                vRet = Value::Bool_((int)r != 0);
                                break;
                            case ARPtr:
                                // pointer returns typed as pointer;
                                // type(arg)=="pointer" drives lib dispatch
                                vRet = (INT_PTR)r
                                    ? Value::Pointer_((void*)(INT_PTR)r)
                                    : Value();
                                break;
                            case ARAddr:
                                // `addr` returns are plain numbers
                                // (feeds ::SendMessage results straight back
                                // into length params)
                                vRet = Value::Num_((double)(INT_PTR)r);
                                break;
                            case ARStrA:
                                vRet = Value::Str_(
                                    ReadStrA((const char*)(INT_PTR)r));
                                break;
                            case ARStrW:
                                vRet = Value::Str_(
                                    ReadStrW((const wchar_t*)
                                             (INT_PTR)r));
                                break;
                            case ARI64:
                            default:
                                vRet = Value::Num_((double)r);
                                break;
                            }
                            // by-ref out-params come back as extra
                            // return values (var len,str = formatMessage(..))
                            if (!tmpRefs.empty())
                            {
                                auto extras =
                                    std::make_shared<vector<Value>>();
                                for (auto& ref : tmpRefs)
                                {
                                    if (ref.bStruct)
                                    {
                                        bool bRef = (sig.params[ref.paramIdx] & 0x100) != 0;
                                        if (ref.origVal.t == Value::TableT || ref.origVal.t == Value::Class)
                                        {
                                            StructDeserialize(ref.origVal, ref.pBuffer);
                                        }
                                        if (bRef)
                                            extras->push_back(ref.origVal);
                                        HeapFree(GetProcessHeap(), 0, ref.pBuffer);
                                    }
                                    else
                                    {
                                        Value outVal;
                                        if (!ref.pBuffer)
                                        {
                                            extras->push_back(outVal);
                                            continue;
                                        }
                                        switch (ref.paramType)
                                        {
                                        case 0:
                                            outVal = Value::Num_((double)*(int*)ref.pBuffer);
                                            break;
                                        case 1:
                                            outVal = Value::Num_((double)*(__int64*)ref.pBuffer);
                                            break;
                                        case 2:
                                            outVal = Value::Num_((double)*(unsigned short*)ref.pBuffer);
                                            break;
                                        case 3:
                                            outVal = Value::Num_((double)*(unsigned char*)ref.pBuffer);
                                            break;
                                        case 4:
                                            outVal = Value::Bool_(*(int*)ref.pBuffer != 0);
                                            break;
                                        case 5:
                                            // match the return-side contract:
                                            // pointer out-params come back as
                                            // Pointer values (official cdata)
                                            outVal = *(INT_PTR*)ref.pBuffer
                                                ? Value::Pointer_(
                                                      (void*)(INT_PTR)
                                                          *(INT_PTR*)
                                                              ref.pBuffer)
                                                : Value();
                                            break;
                                        case 12:
                                            outVal = Value::Num_((double)*(INT_PTR*)ref.pBuffer);
                                            break;
                                        case 6:
                                            outVal = Value::Str_(
                                                bNameW ? ReadStrW((const wchar_t*)ref.pBuffer)
                                                       : ReadStrA((const char*)ref.pBuffer));
                                            break;
                                        case 7:
                                            outVal = Value::Str_(
                                                ReadStrW((const wchar_t*)
                                                    ref.pBuffer));
                                            break;
                                        case 11:
                                        {
                                            // binary string &: pack raw bytes into wstring
                                            wstring ws;
                                            size_t len = ref.bufLen;
                                            if (len == 0 && ref.pBuffer)
                                                len = strlen((const char*)ref.pBuffer);
                                            ws.reserve(len);
                                            const unsigned char* pb = (const unsigned char*)ref.pBuffer;
                                            for (size_t k = 0; k < len; k++)
                                                ws += (wchar_t)pb[k];
                                            outVal = Value::Str_(ws);
                                            outVal.bRawBytes = true;
                                            break;
                                        }
                                        case 9:
                                        {
                                            float f = 0;
                                            memcpy(&f, ref.pBuffer, sizeof(float));
                                            outVal = Value::Num_(f);
                                            break;
                                        }
                                        case 10:
                                        {
                                            double d = 0;
                                            memcpy(&d, ref.pBuffer, sizeof(double));
                                            outVal = Value::Num_(d);
                                            break;
                                        }
                                        default:
                                            outVal = Value::Num_((double)*(int*)ref.pBuffer);
                                            break;
                                        }
                                        extras->push_back(outVal);
                                        if (!ref.bNoFree)
                                            HeapFree(GetProcessHeap(), 0,
                                                     ref.pBuffer);
                                    }
                                }
                                if (!extras->empty())
                                    vRet.mrets = extras;
                            }
                            return vRet;
                            });
                            retFn.ptr = fn;
                            return retFn;
                        });

                    auto fnApi = dll->map[L"api"];
                    auto dmeta = std::make_shared<Table>();
                    dmeta->map[L"_get"] = NF([this, fnApi, hMod, memDll, bDllCdecl, bDllUni, dllWeak = std::weak_ptr<Table>(dll)](vector<Value>& a) -> Value {
                        if (a.empty() || a[0].t != Value::Str) return Value();
                        wstring name = a[0].s;
                        if (name.empty()) return Value();

                        auto pDll = dllWeak.lock();
                        if (pDll)
                        {
                            auto it = pDll->map.find(name);
                            if (it != pDll->map.end()) return it->second;
                        }

                        // Determine return type and bare export name from API suffix
                        // Suffix rule: preceded by lowercase letter
                        wchar_t suffix = 0;
                        wstring bareName = name;
                        if (name.size() >= 2 && iswlower(name[name.size() - 2]))
                        {
                            wchar_t last = name.back();
                            if (last == L'P' || last == L'L' || last == L'D' ||
                                last == L'F' || last == L'B' || last == L'W' || last == L'A')
                            {
                                suffix = last;
                                bareName = name.substr(0, name.size() - 1);
                            }
                        }

                        std::string bareNameA = RawBytesOf(Value::Str_(bareName));
                        std::string nameA = RawBytesOf(Value::Str_(name));
                        FARPROC fp = nullptr;
                        if (memDll->bOk)
                        {
                            fp = MemDllGetProc(memDll->pImage, nameA.c_str());
                            if (!fp && suffix)
                                fp = MemDllGetProc(memDll->pImage, bareNameA.c_str());
                            if (!fp && nameA.find('@') == std::string::npos)
                            {
                                std::string w = (suffix ? bareNameA : nameA) + "W";
                                fp = MemDllGetProc(memDll->pImage, w.c_str());
                            }
                            if (!fp && nameA.find('@') == std::string::npos)
                            {
                                std::string an = (suffix ? bareNameA : nameA) + "A";
                                fp = MemDllGetProc(memDll->pImage, an.c_str());
                            }
                        }
                        else if (hMod)
                        {
                            fp = ::GetProcAddress(hMod, nameA.c_str());
                            if (!fp && suffix)
                                fp = ::GetProcAddress(hMod, bareNameA.c_str());
                            if (!fp && nameA.find('@') == std::string::npos)
                            {
                                std::string w = (suffix ? bareNameA : nameA) + "W";
                                fp = ::GetProcAddress(hMod, w.c_str());
                            }
                            if (!fp && nameA.find('@') == std::string::npos)
                            {
                                std::string an = (suffix ? bareNameA : nameA) + "A";
                                fp = ::GetProcAddress(hMod, an.c_str());
                            }
                        }
                        if (!fp)
                            return Value();

                        // Create a dynamic invoker function that synthesizes signature on first call
                        Value dynFn = NF([this, fnApi, name, bareName, suffix, bDllCdecl, bDllUni, dllWeak](vector<Value>& args) -> Value {
                            wstring retType = L"addr";
                            if (suffix == L'P') retType = L"pointer";
                            else if (suffix == L'L') retType = L"long";
                            else if (suffix == L'D') retType = L"double";
                            else if (suffix == L'F') retType = L"float";
                            else if (suffix == L'B') retType = L"bool";

                            bool isUnicode = (suffix == L'W') || bDllUni;

                            wstring sig = retType + L"(";
                            for (size_t i = 0; i < args.size(); ++i)
                            {
                                if (i > 0) sig += L",";
                                const auto& arg = args[i];
                                if (arg.t == Value::Str)
                                {
                                    sig += isUnicode ? L"ustring" : L"string";
                                }
                                else if (arg.t == Value::TableT || arg.t == Value::Class)
                                {
                                    sig += L"struct&";
                                }
                                else if (arg.t == Value::Pointer)
                                {
                                    sig += L"pointer";
                                }
                                else if (arg.t == Value::Bool)
                                {
                                    sig += L"bool";
                                }
                                else if (arg.t == Value::Num)
                                {
                                    sig += L"int";
                                }
                                else
                                {
                                    sig += L"pointer";
                                }
                            }
                            if (bDllCdecl) sig += (args.empty() ? L"cdecl" : L",cdecl");
                            sig += L")";

                            vector<Value> apiArgs{ Value::Str_(bareName), Value::Str_(sig) };
                            Value boundApi = fnApi.native(fnApi.nativeCtx.get(), apiArgs);
                            if (boundApi.t != Value::Func)
                            {
                                if (bareName != name)
                                {
                                    vector<Value> apiArgs2{ Value::Str_(name), Value::Str_(sig) };
                                    boundApi = fnApi.native(fnApi.nativeCtx.get(), apiArgs2);
                                }
                            }
                            if (boundApi.t == Value::Func)
                            {
                                auto pDll = dllWeak.lock();
                                if (pDll)
                                    pDll->map[name] = boundApi;
                                return this->CallVmFunc(boundApi, args);
                            }
                            return Value();
                        });
                        return dynFn;
                    });
                    dll->map[L"_meta"] = Value::Table_(dmeta);
                    dll->bExplicitMeta = true;

                    return Value::Table_(dll); });
                // raw memory: a raw.buffer is a distinct pointer value
                // (buffer type). Numbers still work as addresses for
                // backwards compatibility of older scripts.
                auto BufPtr = [](const Value& v) -> void* {
                    if (v.t == Value::Pointer) return v.ptr;
                    if (v.t == Value::Num)
                        return (void*)(INT_PTR)(long long)v.n;
                    return nullptr;
                };
                tb->map[L"buffer"] = NF([this, BufPtr](vector<Value>& a) -> Value {
                    if (a.empty()) return Value();
                    // raw.buffer(structTable) — serialize a struct instance
                    if (a[0].t == Value::TableT || a[0].t == Value::Class)
                    {
                        int total = 0;
                        void* pS = StructSerialize(a[0], &total);
                        if (pS)
                        {
                            RegisterBuffer(pS, (size_t)total);
                            return Value::Pointer_(pS);
                        }
                        return Value();
                    }
                    if (a.size() == 1 && a[0].t == Value::Str)
                    {
                        // raw.buffer("initialContent") — allocate UTF-8 bytes of string
                        wstring s = a[0].s;
                        if (s.empty()) return Value();
                        int cb = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
                        if (cb <= 0) return Value();
                        void* p = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size_t)cb);
                        if (!p) return Value();
                        WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), (char*)p, cb, nullptr, nullptr);
                        RegisterBuffer(p, (size_t)cb);
                        return Value::Pointer_(p);
                    }
                    size_t n = 0;
                    if (a[0].t == Value::Num)
                        n = (size_t)(long long)a[0].n;
                    if (n == 0) return Value();
                    void* p = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n);
                    if (!p) return Value();
                    if (a.size() > 1)
                    {
                        if (a[1].t == Value::Str)
                        {
                            if (a[1].bRawBytes)
                            {
                                for (size_t k = 0; k < a[1].s.size() && k < n; k++)
                                    ((char*)p)[k] = (char)(unsigned char)a[1].s[k];
                            }
                            else
                            {
                                bool allByte = true;
                                for (wchar_t c : a[1].s)
                                {
                                    if ((unsigned short)c > 0xFF) { allByte = false; break; }
                                }
                                if (allByte)
                                {
                                    for (size_t k = 0; k < a[1].s.size() && k < n; k++)
                                        ((char*)p)[k] = (char)(unsigned char)a[1].s[k];
                                }
                                else
                                {
                                    WideCharToMultiByte(CP_UTF8, 0, a[1].s.c_str(), (int)a[1].s.size(), (char*)p, (int)n, nullptr, nullptr);
                                }
                            }
                        }
                        else if (a[1].t == Value::TableT || a[1].t == Value::Class)
                        {
                            int total = 0;
                            void* pS = StructSerialize(a[1], &total);
                            if (pS)
                            {
                                memcpy(p, pS, (size_t)total < n ? (size_t)total : n);
                                HeapFree(GetProcessHeap(), 0, pS);
                            }
                        }
                        else if (a[1].t == Value::Pointer && a[1].ptr)
                        {
                            size_t srcSz = GetBufferSize(a[1].ptr);
                            if (srcSz > 0)
                            {
                                memcpy(p, a[1].ptr, srcSz < n ? srcSz : n);
                            }
                            else
                            {
                                memcpy(p, a[1].ptr, n);
                            }
                        }
                        else if (a[1].t == Value::Num && a[1].n != 0)
                        {
                            memcpy(p, (void*)(INT_PTR)(long long)a[1].n, n);
                        }
                    }
                    RegisterBuffer(p, n);
                    return Value::Pointer_(p);
                });
                tb->map[L"free"] = NF([BufPtr](vector<Value>& a) -> Value {
                    if (a.empty()) return Value::Bool_(false);
                    void* p = BufPtr(a[0]);
                    if (!p) return Value::Bool_(true);
                    // AUDIT-11: a dynbuf's script-visible pointer is the DATA
                    // area; freeing it directly is an interior-pointer free
                    // (heap corruption — Windows' own heap tolerates it, so
                    // no ASAN report, but the block header is left dangling).
                    // Route it through DynAlloc(0, p), which frees the whole
                    // block (header + data) and drops the registry entry.
                    if (IsDynBuffer(p))
                    {
                        DynAlloc(0, p);
                        return Value::Bool_(true);
                    }
                    UnregisterBuffer(p);
                    return Value::Bool_(
                        HeapFree(GetProcessHeap(), 0, p) != FALSE); });
                // write(ptr, s): byte string -> raw bytes; text -> UTF-8
                tb->map[L"write"] = NF([BufPtr](vector<Value>& a) -> Value {
                    if (a.size() < 2) return Value::Num_(0);
                    char* p = (char*)BufPtr(a[0]);
                    if (!p) return Value::Num_(0);
                    wstring s = BStr(a[1]);
                    bool bText = false;
                    for (wchar_t c : s)
                        if (c > 0xFF) { bText = true; break; }
                    if (!bText)
                    {
                        for (size_t k = 0; k < s.size(); k++)
                            p[k] = (char)(unsigned char)s[k];
                        return Value::Num_((double)s.size());
                    }
                    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(),
                        (int)s.size(), nullptr, 0, nullptr, nullptr);
                    if (n <= 0) return Value::Num_(0);
                    WideCharToMultiByte(CP_UTF8, 0, s.c_str(),
                        (int)s.size(), p, n, nullptr, nullptr);
                    return Value::Num_((double)n); });
                // read(ptr, n) -> byte string (one char per raw byte)
                tb->map[L"read"] = NF([BufPtr](vector<Value>& a) -> Value {
                    if (a.size() < 2) return Value::Str_(L"");
                    const char* p = (const char*)BufPtr(a[0]);
                    long long n = (long long)BNum(a[1]);
                    if (!p || n <= 0) return Value::Str_(L"");
                    wstring s((size_t)n, 0);
                    for (long long k = 0; k < n; k++)
                        s[(size_t)k] =
                            (wchar_t)(unsigned char)p[k];
                    return Value::Str_(s); });
                // readW(ptr, nChars) -> wide text from a UTF-16 buffer
                tb->map[L"readW"] = NF([BufPtr](vector<Value>& a) -> Value {
                    if (a.size() < 2) return Value::Str_(L"");
                    const wchar_t* p = (const wchar_t*)BufPtr(a[0]);
                    long long n = (long long)BNum(a[1]);
                    if (!p || n <= 0) return Value::Str_(L"");
                    return Value::Str_(wstring((size_t)n, L'\0')
                        .assign(p, p + n)); });
                // ---- M3-b: struct layout serialize/deserialize ----
                // serialize: table fields -> heap buffer; returns pointer
                tb->map[L"structPtr"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty()) return Value::Num_(0);
                    void* p = StructSerialize(a[0]);
                    return Value::Num_((double)(INT_PTR)p);
                });
                // deserialize: buffer -> table fields (in place)
                tb->map[L"structFrom"] = NF([](vector<Value>& a) -> Value {
                    if (a.size() < 2 || (a[0].t != Value::TableT && a[0].t != Value::Class))
                        return a.empty() ? Value() : a[0];
                    Value vTab = a[0];
                    void* p = a[1].t == Value::Pointer ? a[1].ptr
                        : (void*)(INT_PTR)(long long)BNum(a[1]);
                    StructDeserialize(vTab, p);
                    return vTab;
                });
                // byte size of a struct table or dynamic pointer
                tb->map[L"sizeOf"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty()) return Value::Num_(0);
                    if (a[0].t == Value::Pointer && a[0].ptr)
                    {
                        if (IsDynBuffer(a[0].ptr))
                        {
                            const DynBufHeader* h = GetDynBufHeader(a[0].ptr);
                            Value v = Value::Num_((double)h->length);
                            auto extras = std::make_shared<vector<Value>>();
                            extras->push_back(Value::Num_((double)h->capacity));
                            v.mrets = extras;
                            return v;
                        }
                    }
                    if (a[0].t == Value::Str)
                        return Value::Num_((double)GetStructFieldSize(a[0].s, 1));
                    return Value::Num_((double)CalcStructTotalSize(a[0]));
                });
                tb->map[L"sizeof"] = tb->map[L"sizeOf"];
                // raw.len: string byte length, or the struct's total size.
                // raw library relies on it for equal()/typeOfArray.
                tb->map[L"len"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty() || a[0].t == Value::Null) return Value();
                    if (a[0].t == Value::Str)
                        return Value::Num_((double)BStr(a[0]).size());
                    if (a[0].t == Value::TableT || a[0].t == Value::Class)
                    {
                        auto is = a[0].tab ? a[0].tab->map.find(L"_struct")
                                           : a[0].tab->map.end();
                        if (is != a[0].tab->map.end() && is->second.t == Value::Str)
                            return Value::Num_(
                                (double)CalcStructTotalSize(a[0]));
                    }
                    return Value(); });
                // raw.swap(v [, type]) — reverse byte order. Numbers swap
                // the width named by the API type (default INT — the lib
                // calls raw.swap on INT/WORD struct fields); a struct table
                // swaps its integer fields in place per its _struct
                // metadata. wsock/tcp, bt, fastcgi, crypt/otp and bencode
                // all depend on this.
                tb->map[L"swap"] = NF([](vector<Value>& a) -> Value {
                    auto swapNum = [](double n, int size) -> Value {
                        if (size != 2 && size != 4 && size != 8)
                            return Value::Num_(n);
                        unsigned long long v;
                        if (size == 8)
                            v = (unsigned long long)(long long)n;
                        else if (size == 2)
                            v = (unsigned short)(long long)n;
                        else
                            v = (unsigned int)(long long)n;
                        unsigned long long r = 0;
                        for (int i = 0; i < size; i++)
                            r = (r << 8) | ((v >> (i * 8)) & 0xFFull);
                        double d;
                        if (size == 8) d = (double)(long long)r;
                        else if (size == 2) d = (double)(unsigned short)r;
                        else d = (double)(int)r;
                        return Value::Num_(d);
                    };
                    auto isIntType = [](const wstring& ty) {
                        wstring t = ty;
                        for (auto& c : t) c = towlower(c);
                        return t == L"byte" || t == L"uchar" || t == L"char" ||
                               t == L"int8" || t == L"word" || t == L"ushort" ||
                               t == L"short" || t == L"wchar" || t == L"int" ||
                               t == L"uint" || t == L"long" || t == L"dword" ||
                               t == L"bool" || t == L"int64" || t == L"uint64" ||
                               t == L"long64" || t == L"i64" || t == L"qword";
                    };
                    if (a.empty()) return Value();
                    if (a[0].t == Value::Num)
                    {
                        int size = 4;
                        if (a.size() > 1 && a[1].t == Value::Str)
                            size = GetStructFieldSize(a[1].s, 1);
                        return swapNum(a[0].n, size);
                    }
                    if ((a[0].t == Value::TableT || a[0].t == Value::Class) &&
                        a[0].tab)
                    {
                        auto is = a[0].tab->map.find(L"_struct");
                        if (is == a[0].tab->map.end() ||
                            is->second.t != Value::Str)
                            return a[0];
                        for (auto& f : ParseStructDef(a[0]))
                        {
                            if (f.bIsArray || !isIntType(f.type))
                                continue;
                            auto iv = a[0].tab->map.find(f.name);
                            if (iv == a[0].tab->map.end() ||
                                iv->second.t != Value::Num)
                                continue;
                            iv->second = swapNum(iv->second.n,
                                                 GetStructFieldSize(f.type, 1));
                        }
                        return a[0];
                    }
                    return a[0];
                });
                // raw.uint / raw.int / raw.word / raw.byte — read a little-
                // endian integer of the given width from a pointer/string.
                // Used as raw.uint(,true) to allocate an out-parameter.
                auto mkReader = [](int nWidth) {
                    return [nWidth](vector<Value>& a) -> Value {
                        bool bZero = !a.empty() &&
                                     (a[0].t == Value::Null ||
                                      (a[0].t == Value::Num && a[0].n == 0));
                        if (bZero)
                        {
                            // raw.uint(,true): allocate an out-parameter
                            // buffer of this width and return a table
                            // wrapper exposing .value
                            auto tb = std::make_shared<Table>();
                            std::vector<char> z((size_t)nWidth, 0);
                            tb->map[L"_bytes"] = Value::Str_(
                                wstring(z.begin(), z.end()));
                            tb->map[L"value"] = Value::Num_(0);
                            return Value::Table_(tb);
                        }
                        const char* p = nullptr;
                        std::string held;
                        if (!a.empty() && a[0].t == Value::Str)
                        {
                            held = RawBytesOf(BStr(a[0]));
                            p = held.data();
                        }
                        else if (!a.empty() && a[0].t == Value::Pointer)
                            p = (const char*)a[0].ptr;
                        else if (!a.empty() && a[0].t == Value::Num)
                            p = (const char*)(INT_PTR)(long long)a[0].n;
                        if (!p) return Value::Num_(0);
                        unsigned long long v = 0;
                        for (int k = 0; k < nWidth; k++)
                            v |= ((unsigned long long)(unsigned char)p[k]) << (8 * k);
                        return Value::Num_((double)v);
                    };
                };
                tb->map[L"uint"] = NF(mkReader(4));
                tb->map[L"int"] = NF(mkReader(4));
                tb->map[L"word"] = NF(mkReader(2));
                tb->map[L"byte"] = NF(mkReader(1));
                tb->map[L"ushort"] = NF(mkReader(2));
                tb->map[L"short"] = NF(mkReader(2));
                tb->map[L"ulong"] = NF(mkReader(4));
                tb->map[L"long"] = NF(mkReader(4));
                // raw.topointer / raw.tonumber — pointer <-> integer
                tb->map[L"topointer"] = NF([this](vector<Value>& a) -> Value {
                    if (a.empty() || a[0].t == Value::Null)
                        return Value::Pointer_(nullptr);
                    INT_PTR offset = a.size() > 1 ? (INT_PTR)(long long)BNum(a[1]) : 0;
                    if (a[0].t == Value::Pointer)
                        return Value::Pointer_((char*)a[0].ptr + offset);
                    if (a[0].t == Value::TableT || a[0].t == Value::Class)
                    {
                        Value vp = GetMember(a[0], L"_topointer");
                        if (vp.t == Value::Func || vp.native)
                        {
                            vector<Value> noArgs;
                            Value r = CallOwner(vp, a[0], noArgs);
                            if (r.t == Value::Pointer)
                                return Value::Pointer_((char*)r.ptr + offset);
                            return Value::Pointer_((char*)(INT_PTR)(long long)BNum(r) + offset);
                        }
                    }
                    if (a[0].t == Value::Num)
                        return Value::Pointer_((char*)(INT_PTR)(long long)a[0].n + offset);
                    if (a[0].t == Value::Str)
                        return Value::Pointer_((char*)a[0].s.c_str() + offset);
                    return Value::Pointer_(nullptr); });
                tb->map[L"tonumber"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty()) return Value::Num_(0);
                    if (a[0].t == Value::Pointer)
                        return Value::Num_((double)(INT_PTR)a[0].ptr);
                    return Value::Num_(BNum(a[0])); });
                // raw.tostring(ptr, start=1, stop=-1) — read bytes from pointer/buffer/struct
                tb->map[L"tostring"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty()) return Value::Str_(L"");
                    const char* p = nullptr;
                    void* pAllocated = nullptr;
                    size_t knownSize = 0;
                    std::string heldStr;
                    if (a[0].t == Value::Pointer)
                    {
                        p = (const char*)a[0].ptr;
                        if (a[0].ptr && IsBuffer(a[0].ptr))
                            knownSize = GetBufferSize(a[0].ptr);
                        else if (a[0].ptr && IsDynBuffer(a[0].ptr))
                            knownSize = GetDynBufHeader(a[0].ptr)->length;
                    }
                    else if (a[0].t == Value::Num)
                    {
                        p = (const char*)(INT_PTR)(long long)a[0].n;
                    }
                    else if (a[0].t == Value::Str)
                    {
                        heldStr = RawBytesOf(BStr(a[0]));
                        p = heldStr.data();
                        knownSize = heldStr.size();
                    }
                    else if (a[0].t == Value::TableT && a[0].tab)
                    {
                        int outTotal = 0;
                        pAllocated = StructSerialize(a[0], &outTotal);
                        if (pAllocated && outTotal > 0)
                        {
                            p = (const char*)pAllocated;
                            knownSize = (size_t)outTotal;
                        }
                    }

                    if (!p)
                    {
                        if (pAllocated) HeapFree(GetProcessHeap(), 0, pAllocated);
                        return Value::Str_(L"");
                    }

                    long long start = 1;
                    if (a.size() > 1 && a[1].t == Value::Num)
                        start = (long long)a[1].n;
                    if (start < 1) start = 1;

                    size_t n = 0;
                    if (a.size() > 2 && a[2].t == Value::Num)
                    {
                        long long stop = (long long)a[2].n;
                        if (stop < start)
                        {
                            if (pAllocated) HeapFree(GetProcessHeap(), 0, pAllocated);
                            return Value::Str_(L"");
                        }
                        n = (size_t)(stop - start + 1);
                    }
                    else if (knownSize > 0)
                    {
                        if ((size_t)start <= knownSize)
                            n = knownSize - (size_t)start + 1;
                        else
                            n = 0;
                    }
                    else
                    {
                        const char* scan = p + (start - 1);
                        while (n < (1u << 25) && scan[n]) n++;
                    }

                    p += (start - 1);
                    wstring w;
                    w.resize(n);
                    for (size_t k = 0; k < n; k++)
                        w[k] = (wchar_t)(unsigned char)p[k];

                    if (pAllocated)
                        HeapFree(GetProcessHeap(), 0, pAllocated);

                    Value ret = Value::Str_(w);
                    ret.bRawBytes = true;
                    return ret;
                });
                // raw.copy(dst,src,size) — raw memory copy, returns dst
                tb->map[L"copy"] = NF([](vector<Value>& a) -> Value {
                    if (a.size() < 2) return a.empty() ? Value() : a[0];
                    void* d = nullptr; const char* s = nullptr;
                    std::string held;
                    void* pAllocStruct = nullptr;
                    if (a[0].t == Value::Pointer) d = a[0].ptr;
                    else if (a[0].t == Value::Num)
                        d = (void*)(INT_PTR)(long long)a[0].n;
                    else if (a[0].t == Value::TableT || a[0].t == Value::Class)
                    {
                        if (a[1].t == Value::Pointer && a[1].ptr)
                        {
                            StructDeserialize(a[0], a[1].ptr);
                            return a[0];
                        }
                        else if (a[1].t == Value::Str)
                        {
                            std::string bytes = RawBytesOf(BStr(a[1]));
                            StructDeserialize(a[0], bytes.data());
                            return a[0];
                        }
                    }
                    size_t n = a.size() > 2 && a[2].t != Value::Null ? (size_t)BNum(a[2]) : 0;
                    if (a[1].t == Value::Str) { held = RawBytesOf(BStr(a[1])); s = held.data(); if (!n) n = held.size(); }
                    else if (a[1].t == Value::Pointer) { s = (const char*)a[1].ptr; if (!n) n = GetBufferSize(a[1].ptr); }
                    else if (a[1].t == Value::Num)
                        s = (const char*)(INT_PTR)(long long)a[1].n;
                    else if (a[1].t == Value::TableT || a[1].t == Value::Class)
                    {
                        int total = 0;
                        pAllocStruct = StructSerialize(a[1], &total);
                        s = (const char*)pAllocStruct;
                        if (!n) n = (size_t)total;
                    }
                    if (d && s && n) memcpy(d, s, n);
                    if (pAllocStruct) HeapFree(GetProcessHeap(), 0, pAllocStruct);
                    return a[0]; });
                // raw.concat(dst,src,len) — append src bytes to a buffer / dynamic pointer
                auto fnConcat = NF([](vector<Value>& a) -> Value {
                    if (a.size() < 2) return a.empty() ? Value() : a[0];
                    std::string held;
                    void* pAllocStruct = nullptr;
                    const char* s = nullptr; size_t n = 0;
                    // AUDIT-10: an explicit len used to pass straight
                    // through as `n`; a negative or huge value (e.g. -1,
                    // 1e30) made n enormous — an OOB read on the source
                    // and, combined with a failed DynAlloc, a huge memcpy
                    // through the old block. Clamp n to the source's real
                    // extent wherever the extent is known; legal inputs
                    // (len <= real size) are unaffected.
                    if (a[1].t == Value::Str)
                    {
                        held = RawBytesOf(BStr(a[1]));
                        s = held.data();
                        n = a.size() > 2 && a[2].t != Value::Null ? (size_t)BNum(a[2]) : held.size();
                        n = min(n, held.size());
                    }
                    else if (a[1].t == Value::Pointer)
                    {
                        s = (const char*)a[1].ptr;
                        n = a.size() > 2 && a[2].t != Value::Null ? (size_t)BNum(a[2]) : 0;
                        // bare pointers carry no size metadata — clamp only
                        // when the extent is knowable (raw semantics kept:
                        // script-owned pointers stay the caller's word)
                        if (s && a[1].ptr)
                        {
                            size_t known = 0;
                            if (IsBuffer(a[1].ptr)) known = GetBufferSize(a[1].ptr);
                            else if (IsDynBuffer(a[1].ptr)) known = GetDynBufHeader(a[1].ptr)->length;
                            if (known && n > known) n = known;
                        }
                    }
                    else if (a[1].t == Value::TableT || a[1].t == Value::Class)
                    {
                        int total = 0;
                        pAllocStruct = StructSerialize(a[1], &total);
                        s = (const char*)pAllocStruct;
                        n = a.size() > 2 && a[2].t != Value::Null ? (size_t)BNum(a[2]) : (size_t)total;
                        n = min(n, (size_t)total);
                    }
                    else if (a[1].t == Value::Num)
                    {
                        held = std::to_string((long long)a[1].n);
                        s = held.data();
                        n = a.size() > 2 && a[2].t != Value::Null ? (size_t)BNum(a[2]) : held.size();
                        n = min(n, held.size());
                    }

                    if (!s || n == 0)
                    {
                        if (pAllocStruct) HeapFree(GetProcessHeap(), 0, pAllocStruct);
                        return a[0];
                    }

                    if (a[0].t == Value::Pointer && a[0].ptr && IsDynBuffer(a[0].ptr))
                    {
                        DynBufHeader* h = GetDynBufHeader(a[0].ptr);
                        if (h->length + n >= h->capacity)
                        {
                            size_t newCap = max((size_t)h->capacity * 2, (size_t)h->length + n + 1024);
                            void* pNew = DynAlloc(newCap, a[0].ptr);
                            if (pNew)
                            {
                                a[0].ptr = pNew;
                                h = GetDynBufHeader(pNew);
                            }
                            else
                            {
                                // AUDIT-10: allocation failed (huge n) — the
                                // old code fell through and memcpy'd n bytes
                                // through the undersized old block. Give up
                                // this append; the dynbuf stays intact.
                                if (pAllocStruct) HeapFree(GetProcessHeap(), 0, pAllocStruct);
                                return a[0];
                            }
                        }
                        memcpy((char*)a[0].ptr + h->length, s, n);
                        h->length += (uint32_t)n;
                        ((char*)a[0].ptr)[h->length] = 0;
                        if (pAllocStruct) HeapFree(GetProcessHeap(), 0, pAllocStruct);
                        return a[0];
                    }

                    if (a[0].t == Value::Pointer || a[0].t == Value::Num)
                    {
                        void* dst = a[0].t == Value::Pointer ? a[0].ptr : (void*)(INT_PTR)(long long)a[0].n;
                        if (dst)
                        {
                            // AUDIT-10 (write side): here the 3rd arg is the
                            // WRITE OFFSET (dst semantics), not a length —
                            // a negative/huge value is UB on the size_t
                            // conversion and memcpy(dst+off,...) crashed
                            // (probe p5 case 1: off=1e30 → 0x8000...). Reject
                            // out-of-domain offsets; for size-known buffers
                            // also reject an overrunning window. Bare unknown
                            // pointers inside the safe domain keep raw
                            // semantics (caller owns the pointer).
                            double dOff = a.size() > 2 ? BNum(a[2]) : 0;
                            if (!(dOff >= 0 && dOff <= 0x7FFFFFFF))
                            {
                                if (pAllocStruct) HeapFree(GetProcessHeap(), 0, pAllocStruct);
                                return a[0];
                            }
                            size_t off = (size_t)dOff;
                            if (a[0].t == Value::Pointer && a[0].ptr)
                            {
                                size_t known = 0;
                                if (IsBuffer(a[0].ptr)) known = GetBufferSize(a[0].ptr);
                                else if (IsDynBuffer(a[0].ptr)) known = GetDynBufHeader(a[0].ptr)->length;
                                if (known && off + n > known)
                                {
                                    if (pAllocStruct) HeapFree(GetProcessHeap(), 0, pAllocStruct);
                                    return a[0];
                                }
                            }
                            memcpy((char*)dst + off, s, n);
                            if (pAllocStruct) HeapFree(GetProcessHeap(), 0, pAllocStruct);
                            return Value::Num_((double)(off + n));
                        }
                    }

                    if (a[0].t == Value::Str)
                    {
                        wstring w = BStr(a[0]);
                        for (size_t k = 0; k < n; k++)
                            w.push_back((wchar_t)(unsigned char)s[k]);
                        if (pAllocStruct) HeapFree(GetProcessHeap(), 0, pAllocStruct);
                        return Value::Str_(w);
                    }

                    if (pAllocStruct) HeapFree(GetProcessHeap(), 0, pAllocStruct);
                    return a[0];
                });
                tb->map[L"concat"] = fnConcat;
                tb->map[L"append"] = fnConcat;
                // raw.closehandle(h) — CloseHandle
                auto rawArrayFunc = NF([](vector<Value>& a) -> Value {
                    // raw.array / raw.toarray(arrayOrLength, rawTypename, fieldName)
                    // creates a struct { <rawTypename> <fieldName>[len] = ... }
                    if (a.size() < 3) return Value();
                    int len = 0;
                    auto arrTab = std::make_shared<Table>();
                    if (a[0].t == Value::Num)
                    {
                        len = (int)BNum(a[0]);
                        if (len < 0) len = 0;
                        arrTab->arr.resize(len);
                    }
                    else if (a[0].t == Value::TableT && a[0].tab)
                    {
                        len = (int)a[0].tab->arr.size();
                        arrTab->arr = a[0].tab->arr;
                    }
                    wstring typeName = BStr(a[1]);
                    wstring fieldName = BStr(a[2]);
                    wstring structDef = typeName + L" " + fieldName + L"[" + std::to_wstring(len) + L"];";

                    auto res = std::make_shared<Table>();
                    res->map[L"_struct"] = Value::Str_(structDef);
                    res->map[fieldName] = Value::Table_(arrTab);
                    return Value::Table_(res);
                });
                tb->map[L"array"] = rawArrayFunc;
                tb->map[L"toarray"] = rawArrayFunc;
                tb->map[L"closehandle"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty()) return Value::Bool_(false);
                    HANDLE h = nullptr;
                    if (a[0].t == Value::Pointer) h = a[0].ptr;
                    else if (a[0].t == Value::Num)
                        h = (HANDLE)(INT_PTR)(long long)a[0].n;
                    if (!h) return Value::Bool_(false);
                    return Value::Bool_(CloseHandle(h) != FALSE); });
                // raw.realloc(size, ptr [, initialData]) — official contract: raw.realloc(size, [ptr])
                // also tolerant to raw.realloc(ptr, size) if someone passes ptr first
                tb->map[L"realloc"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty()) return Value();
                    size_t n = 0;
                    void* p = nullptr;
                    if (a[0].t == Value::Pointer)
                    {
                        p = a[0].ptr;
                        if (a.size() > 1) n = (size_t)BNum(a[1]);
                    }
                    else
                    {
                        n = (size_t)BNum(a[0]);
                        if (a.size() > 1)
                        {
                            if (a[1].t == Value::Pointer) p = a[1].ptr;
                            else if (a[1].t == Value::Num && a[1].n != 0)
                                p = (void*)(INT_PTR)(long long)a[1].n;
                        }
                    }
                    void* q = DynAlloc(n, p);
                    if (q && a.size() > 2 && a[2].t != Value::Null)
                    {
                        DynBufHeader* h = GetDynBufHeader(q);
                        if (a[2].t == Value::Str)
                        {
                            std::string bytes = RawBytesOf(BStr(a[2]));
                            size_t cpy = min((size_t)h->capacity, bytes.size());
                            memcpy(q, bytes.data(), cpy);
                            h->length = (uint32_t)cpy;
                            ((char*)q)[cpy] = 0;
                        }
                        else if (a[2].t == Value::TableT || a[2].t == Value::Class)
                        {
                            int total = 0;
                            void* pS = StructSerialize(a[2], &total);
                            if (pS)
                            {
                                size_t cpy = min((size_t)h->capacity, (size_t)total);
                                memcpy(q, pS, cpy);
                                h->length = (uint32_t)cpy;
                                ((char*)q)[cpy] = 0;
                                HeapFree(GetProcessHeap(), 0, pS);
                            }
                        }
                    }
                    if (!q) return Value();
                    return Value::Pointer_(q); });
                tb->map[L"malloc"] = tb->map[L"realloc"];
                // raw.equal(a,b,size) — byte comparison
                tb->map[L"equal"] = NF([](vector<Value>& a) -> Value {
                    if (a.size() < 2) return Value::Bool_(true);
                    std::string x, y;
                    if (a[0].t == Value::Str) x = RawBytesOf(BStr(a[0]));
                    if (a[1].t == Value::Str) y = RawBytesOf(BStr(a[1]));
                    if (x.empty() || y.empty()) return Value::Bool_(x == y);
                    size_t n = a.size() > 2 ? (size_t)BNum(a[2])
                                            : min(x.size(), y.size());
                    if (n > x.size() || n > y.size()) return Value::Bool_(false);
                    return Value::Bool_(memcmp(x.data(), y.data(), n) == 0); });
                tb->map[L"toPointer"] = NF([this](vector<Value>& a) -> Value {
                    if (a.empty() || a[0].t == Value::Null) return Value::Pointer_(nullptr);
                    if (a[0].t == Value::Pointer) return a[0];
                    if (a[0].t == Value::TableT || a[0].t == Value::Class)
                    {
                        Value vp = GetMember(a[0], L"_topointer");
                        if (vp.t == Value::Func || vp.native)
                        {
                            vector<Value> noArgs;
                            Value r = CallOwner(vp, a[0], noArgs);
                            if (r.t == Value::Pointer) return r;
                            return Value::Pointer_((void*)(INT_PTR)(long long)BNum(r));
                        }
                    }
                    if (a[0].ptr != nullptr) return Value::Pointer_(a[0].ptr);
                    if (a[0].t == Value::Num) return Value::Pointer_((void*)(INT_PTR)(long long)a[0].n);
                    if (a[0].t == Value::Str) return Value::Pointer_((void*)a[0].s.c_str());
                    return Value::Pointer_((void*)(INT_PTR)(long long)BNum(a[0])); });
                // raw.closeHandle(handle) — CloseHandle wrapper used by
                // process.popen to drop the inherited pipe ends it owns
                tb->map[L"closeHandle"] = NF([](vector<Value>& a) -> Value {
                    void* h = nullptr;
                    if (!a.empty() && a[0].t == Value::Pointer) h = a[0].ptr;
                    else if (!a.empty() && a[0].t == Value::Num)
                        h = (void*)(INT_PTR)(long long)BNum(a[0]);
                    if (!h) return Value::Bool_(false);
                    return Value::Bool_(CloseHandle(h) != 0); });
                // raw._release(obj) — forces a reference release.
                // The VM's lifetimes are ARC-managed, so dropping the last
                // reference happens naturally when callers null the field;
                // keep this as an accepted no-op so library code
                // (process.popen.close) runs unchanged.
                tb->map[L"_release"] = NF([](vector<Value>&) -> Value {
                    return Value(); });
                tb->map[L"convert"] =
                    NF([](vector<Value>& a) -> Value {
                    if (a.size() < 2) return a.empty() ? Value() : a[0];
                    const Value& from = a[0];
                    Value to = a[1];
                    long long off = a.size() > 2 ? (long long)BNum(a[2]) : 0;

                    if (to.t == Value::Pointer || to.t == Value::Num)
                    {
                        void* dst = to.t == Value::Pointer ? to.ptr : (void*)(INT_PTR)(long long)to.n;
                        if (dst)
                        {
                            if (from.t == Value::TableT || from.t == Value::Class)
                            {
                                int total = 0;
                                void* pSrc = StructSerialize(from, &total);
                                if (pSrc)
                                {
                                    memcpy((char*)dst + off, pSrc, total);
                                    HeapFree(GetProcessHeap(), 0, pSrc);
                                }
                            }
                            else if (from.t == Value::Str)
                            {
                                std::string bytes = RawBytesOf(BStr(from));
                                memcpy((char*)dst + off, bytes.data(), bytes.size());
                            }
                            else if (from.t == Value::Pointer && from.ptr)
                            {
                                size_t sz = a.size() > 3 ? (size_t)BNum(a[3]) : 0;
                                if (sz > 0) memcpy((char*)dst + off, from.ptr, sz);
                            }
                        }
                        return to;
                    }

                    if (to.t != Value::TableT || !to.tab) return to;

                    if (from.t == Value::TableT || from.t == Value::Class)
                    {
                        void* pSrc = StructSerialize(from);
                        if (pSrc)
                        {
                            StructDeserialize(to, (const char*)pSrc + off);
                            HeapFree(GetProcessHeap(), 0, pSrc);
                        }
                    }
                    else if (from.t == Value::Pointer)
                    {
                        if (from.ptr) StructDeserialize(to, (const char*)from.ptr + off);
                    }
                    else if (from.t == Value::Num)
                    {
                        void* p = (void*)(INT_PTR)(long long)from.n;
                        if (p) StructDeserialize(to, (const char*)p + off);
                    }
                    else if (from.t == Value::Str)
                    {
                        std::string bytes = RawBytesOf(BStr(from));
                        if ((size_t)off < bytes.size())
                            StructDeserialize(to, bytes.data() + off);
                    }
                    return to;
                });
                tb->map[L"find"] = NF([this](vector<Value>& a) -> Value {
                    if (a.size() < 3) return Value();
                    void* p = a[0].t == Value::Pointer ? a[0].ptr : (void*)(INT_PTR)(long long)BNum(a[0]);
                    size_t len = (size_t)BNum(a[1]);
                    if (!p || len == 0) return Value();
                    wstring s; s.resize(len);
                    const unsigned char* up = (const unsigned char*)p;
                    for (size_t k = 0; k < len; k++) s[k] = (wchar_t)up[k];
                    vector<Value> args;
                    args.push_back(Value::Str_(s));
                    args.push_back(a[2]);
                    if (a.size() > 3) args.push_back(a[3]);
                    auto sFind = GetRoot(L"string").tab ? GetRoot(L"string").tab->map[L"find"] : Value();
                    if (sFind.native) return sFind.native(sFind.nativeCtx.get(), args);
                    return Value();
                });
                tb->map[L"match"] = NF([this](vector<Value>& a) -> Value {
                    if (a.size() < 3) return Value();
                    void* p = a[0].t == Value::Pointer ? a[0].ptr : (void*)(INT_PTR)(long long)BNum(a[0]);
                    size_t len = (size_t)BNum(a[1]);
                    if (!p || len == 0) return Value();
                    wstring s; s.resize(len);
                    const unsigned char* up = (const unsigned char*)p;
                    for (size_t k = 0; k < len; k++) s[k] = (wchar_t)up[k];
                    vector<Value> args;
                    args.push_back(Value::Str_(s));
                    args.push_back(a[2]);
                    if (a.size() > 3) args.push_back(a[3]);
                    auto sMatch = GetRoot(L"string").tab ? GetRoot(L"string").tab->map[L"match"] : Value();
                    if (sMatch.native) return sMatch.native(sMatch.nativeCtx.get(), args);
                    return Value();
                });
                // preloaded system DLLs (::User32 / ::Kernel32)
                {
                    auto uDll = tb->map[L"loadDll"];
                    vector<Value> ua{ Value::Str_(L"User32.dll") };
                    if (uDll.native)
                    {
                        globals.VarAt(NameId(L"User32")) = uDll.native(uDll.nativeCtx.get(), ua);
                        roots[L"User32"] = globals.VarAt(NameId(L"User32"));
                    }
                    vector<Value> ka{ Value::Str_(L"Kernel32.dll") };
                    if (uDll.native)
                    {
                        globals.VarAt(NameId(L"Kernel32")) = uDll.native(uDll.nativeCtx.get(), ka);
                        roots[L"Kernel32"] = globals.VarAt(NameId(L"Kernel32"));
                    }
                    // Preload common system library roots (::Shell32 / ::Gdi32 / ::Comctl32)
                    // crtReopen() uses ::Msvcrt to reopen
                    // the CRT streams. Modern MSVCRT no longer exports
                    // __iob_func (removed in VS2015+), so the library's
                    // "stdin + 32" FILE arithmetic would write through a null
                    // pointer and crash the process. Provide the CRT helpers
                    // ourselves so the reopen is a safe no-op.
                    {
                        vector<Value> da{ Value::Str_(L"msvcrt.dll"), Value::Str_(L"cdecl") };
                        Value dv = uDll.native(uDll.nativeCtx.get(), da);
                        if (dv.t == Value::TableT && dv.tab)
                        {
                            dv.tab->map[L"__iob_func"] = NF([](vector<Value>&) -> Value {
                                return Value::Pointer_(stdin); });
                            dv.tab->map[L"freopen"] = NF([](vector<Value>& a) -> Value {
                                wstring path = a.empty() ? L"" : BStr(a[0]);
                                FILE* pf = (path == L"CONIN$") ? stdin : stdout;
                                return Value::Pointer_(pf); });
                            dv.tab->map[L"_open_osfhandle"] = NF([](vector<Value>&) -> Value {
                                return Value::Num_(-1.0); });
                            globals.VarAt(NameId(L"Msvcrt")) = dv;
                            roots[L"Msvcrt"] = dv;
                        }
                    }
                    const wchar_t* DLLS[] = { L"Shell32.dll", L"Gdi32.dll",
                        L"Ole32.dll", L"OleAut32.dll", L"Advapi32.dll",
                        L"Comctl32.dll", L"Comdlg32.dll", L"Uxtheme.dll",
                        L"Version.dll", L"Winmm.dll", L"Msimg32.dll",
                        L"Rpcrt4.dll", L"Ntdll.dll", L"Wininet.dll",
                        L"Shlwapi.dll", L"Secur32.dll", L"Crypt32.dll",
                        L"Imm32.dll", nullptr };
                    for (int di = 0; DLLS[di]; di++)
                    {
                        vector<Value> da{ Value::Str_(DLLS[di]) };
                        Value dv = uDll.native(uDll.nativeCtx.get(), da);
                        wstring nm(DLLS[di]);
                        nm = nm.substr(0, nm.size() - 4);
                        globals.VarAt(NameId(nm)) = dv;
                        roots[nm] = dv;
                    }
                }

                // install the late-bound dispatch used by trampolines
                if (!g_cbDispatch)
                    g_cbDispatch = [](CbCtx* ctx,
                                      vector<Value>& args) -> __int64 {
                        Value r = ctx->interp->CallOwner(
                            ctx->fn, ctx->owner, args);
                        switch (ctx->sig.ret)
                        {
                        case ARDbl: return (__int64)BNum(r);
                        case ARBool: return (int)BNum(r) != 0 ? 1 : 0;
                        case ARPtr: return r.t == Value::Pointer ? (__int64)(INT_PTR)r.ptr : (__int64)BNum(r);
                        case ARStrA:
                        case ARStrW:
                        default: return r.t == Value::Pointer ? (__int64)(INT_PTR)r.ptr : (__int64)BNum(r);
                        }
                    };
                // M3-c: callback(fn, "ret(p1,p2)", owner) -> function pointer
                // that APIs can call.
                //
                // Official contract (docs/library-guide/builtin/raw/callback.md):
                // the result is a "cdata" object - it marshals as a pointer,
                // it can be called from script, and the collector releases
                // the native trampoline plus the script closure once the
                // last script reference disappears. Returning a bare number
                // (the old behaviour) left every callback - and everything
                // its closure captured - pinned for the process lifetime.
                auto makeCallback = [this](bool bForceCdecl) {
                    return NF([this, bForceCdecl](vector<Value>& a) -> Value {
                        if (a.size() < 2 || !(a[0].t == Value::Func))
                        {
                            (*err) = L"运行错误: raw.callback(fn, sig, owner)";
                            return Value();
                        }
                        ApiSig sig;
                        if (!ParseApiSig(BStr(a[1]), &sig))
                        {
                            (*err) = L"运行错误: 回调签名无效: " + BStr(a[1]);
                            return Value();
                        }
                        if (bForceCdecl)
                            sig.bCdecl = true;
                        auto spCtx = std::make_shared<CbCtx>();
                        CbCtx* ctx = spCtx.get();
                        ctx->fn = a[0];
                        if (a.size() >= 3)
                            ctx->owner = a[2];
                        ctx->sig = sig;
                        ctx->interp = this;
                        // the cdata object below carries the script side, so
                        // the collector reaches the closure through it - the
                        // callback must not be a GC root on its own
                        ctx->rooted = false;
                        CallbackTrampoline(ctx);
                        g_cbs.push_back(spCtx);
                        void* pCode = ctx->code;

                        auto spObj = std::make_shared<Table>();
                        auto spMeta = std::make_shared<Table>();
                        // cdata -> pointer (official _topointer meta method)
                        spMeta->map[L"_topointer"] = NF([pCode](vector<Value>&) -> Value {
                            return Value::Pointer_(pCode);
                        });
                        // a native callback can also be called from script:
                        //   func_c(1,2,3)
                        spMeta->map[L"_call"] = NF([spCtx](vector<Value>& a) -> Value {
                            CbCtx* c = spCtx.get();
                            vector<Value> args;
                            for (size_t i = 1; i < a.size(); i++) args.push_back(a[i]);
                            if (c->released || !c->interp || c->fn.t != Value::Func)
                                return Value();
                            return c->interp->CallOwner(c->fn, c->owner, args);
                        });
                        spObj->map[L"_meta"] = Value::Table_(spMeta);
                        // keep the script closure alive while the cdata lives
                        spObj->map[L"_fn"] = a[0];
                        spObj->map[L"_owner"] = ctx->owner;
                        // finalizer: the collector runs it when the cdata
                        // became unreachable - release the native side
                        spObj->map[L"_gc"] = NF([spCtx](vector<Value>&) -> Value {
                            ReleaseCallback(spCtx.get());
                            return Value();
                        });
                        Value v = Value::Pointer_(pCode);
                        v.tab = spObj;
                        return v;
                    });
                };
                auto fnCallback = makeCallback(false);
                auto fnTocdecl = makeCallback(true);
                tb->map[L"callback"] = fnCallback;
                tb->map[L"tostdcall"] = fnCallback;
                tb->map[L"tocdecl"] = fnTocdecl;
                globals.VarAt(NameId(L"tostdcall")) = fnCallback;
                globals.VarAt(NameId(L"tocdecl")) = fnTocdecl;

                auto tbThread = GetRoot(L"thread").tab;
                if (tbThread)
                {
                    tbThread->map[L"callback"] = fnCallback;
                    tbThread->map[L"tostdcall"] = fnCallback;
                    tbThread->map[L"tocdecl"] = fnTocdecl;
                    tbThread->map[L"tofastcall"] = fnCallback;
                }
            }
            // ---- table ----
            {
                auto tb = GetRoot(L"table").tab;
                // table.tostring(t) — kernel primitive:
                // serialize a table to source format (array part
                // separated by ';', named members as k=v;), strings
                // quoted with escapes; scalars pass through
                tb->map[L"tostring"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty()) return Value::Str_(L"");
                    if (a[0].t != Value::TableT)
                        return Value::Str_(BStr(a[0]));
                    // self-recursive serializer: plain function pointers
                    // via explicit stack would be heavy; a Y-combinator
                    // style pair of std::function kept alive by the NF
                    std::function<void(const Value&, wstring&, int)>
                        serTbl;
                    std::function<void(const Value&, wstring&, int)>
                        serVal;
                    serVal = [&serTbl](const Value& v, wstring& out,
                                       int depth)
                    {
                        if (depth > 8) return;
                        switch (v.t)
                        {
                        case Value::Str:
                        {
                            wstring e = L"'";
                            for (wchar_t ch : v.s)
                            {
                                if (ch == 39) e += L"\'";
                                else if (ch == 92) e += L"\\\\";
                                else if (ch == 13) e += L"\r";
                                else if (ch == 10) e += L"\n";
                                else if (ch == 9) e += L"\t";
                                else if (ch == 0) e += L"\0";
                                else e += ch;
                            }
                            out += e + L"'";
                            break;
                        }
                        case Value::Num:
                            out += BStr(v);
                            break;
                        case Value::Bool:
                            out += v.b ? L"true" : L"false";
                            break;
                        case Value::TableT:
                            if (v.tab)
                                serTbl(v, out, depth + 1);
                            else
                                out += L"{}";
                            break;
                        default:
                            break;
                        }
                    };
                    serTbl = [&serVal](const Value& tv, wstring& out,
                                       int depth)
                    {
                        if (!tv.tab || depth > 8)
                        {
                            out += L"{}";
                            return;
                        }
                        bool bFirst = true;
                        wstring s = L"{";
                        for (auto& item : tv.tab->arr)
                        {
                            if (!bFirst) s += L";";
                            bFirst = false;
                            serVal(item, s, depth);
                        }
                        for (auto& kv : tv.tab->map)
                        {
                            if (kv.first.empty()) continue;
                            if (kv.first.rfind(L"_", 0) == 0) continue;
                            wstring vs;
                            serVal(kv.second, vs, depth);
                            if (vs.empty()) continue;
                            if (!bFirst) s += L";";
                            bFirst = false;
                            // A loadable literal must bracket every key that
                            // is not a plain identifier. The official mmap
                            // registry stores numeric hwnd keys and the
                            // reserved [-1] return slot; emitting those bare
                            // re-parses "-1=x" as an array item and the
                            // ide.command registry/return roundtrip silently
                            // loses them (eval("{-1='v'}") yields {1:-1}).
                            bool bIdentAll =
                                iswalpha(kv.first[0]) ||
                                kv.first[0] == L'_';
                            for (size_t c = 1;
                                 c < kv.first.size() && bIdentAll; c++)
                                if (!(iswalnum(kv.first[c]) ||
                                      kv.first[c] == L'_'))
                                    bIdentAll = false;
                            if (bIdentAll)
                            {
                                s += kv.first + L"=" + vs;
                                continue;
                            }
                            wstring num = kv.first;
                            if (!num.empty() && num[0] == L'-')
                                num = num.substr(1);
                            bool bNum = !num.empty();
                            for (wchar_t ch : num)
                                if (!(ch >= L'0' && ch <= L'9'))
                                {
                                    bNum = false;
                                    break;
                                }
                            if (bNum)
                                s += L"[" + kv.first + L"]=" + vs;
                            else
                            {
                                wstring esc;
                                for (wchar_t ch : kv.first)
                                {
                                    if (ch == L'\'') esc += L"\\'";
                                    else if (ch == L'\\') esc += L"\\\\";
                                    else esc += ch;
                                }
                                s += L"['" + esc + L"']=" + vs;
                            }
                        }
                        s += L"}";
                        out += s;
                    };
                    wstring out;
                    serTbl(a[0], out, 0);
                    return Value::Str_(out); });
                // table.slice(array,start,end) — 1-based slice
                tb->map[L"slice"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty() || a[0].t != Value::TableT || !a[0].tab)
                        return Value();
                    auto& src = a[0].tab->arr;
                    auto out = std::make_shared<Table>();
                    long long n = (long long)src.size();
                    long long s = a.size() > 1 ? (long long)BNum(a[1]) : 1;
                    long long e = a.size() > 2 && a[2].t != Value::Null
                                      ? (long long)BNum(a[2]) : n;
                    if (s < 0) s = n + s + 1;
                    if (e < 0) e = n + e + 1;
                    if (s < 1) s = 1;
                    if (e > n) e = n;
                    for (long long i = s; i <= e; i++)
                        out->arr.push_back(src[(size_t)i - 1]);
                    return Value::Table_(out); });
                // table.clone(t) — shallow copy including the array part
                tb->map[L"clone"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty() || a[0].t != Value::TableT || !a[0].tab)
                        return Value();
                    auto out = std::make_shared<Table>();
                    out->map = a[0].tab->map;
                    out->arr = a[0].tab->arr;
                    return Value::Table_(out); });
                // table.assign(target, src...) - copy key-values from
                // each source table into the target (metaProperty ctor)
                tb->map[L"assign"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty() || a[0].t != Value::TableT || !a[0].tab)
                        return Value();
                    for (size_t si = 1; si < a.size(); si++)
                    {
                        if (a[si].t != Value::TableT || !a[si].tab)
                            continue;
                        for (auto& kv : a[si].tab->map)
                        {
                            if (kv.second.t != Value::Null)
                                a[0].tab->map[kv.first] = kv.second;
                            else
                                a[0].tab->map.erase(kv.first);
                        }
                        for (size_t ai = 0; ai < a[si].tab->arr.size(); ai++)
                            a[0].tab->arr.push_back(a[si].tab->arr[ai]);
                    }
                    return a[0];
                });
                tb->map[L"getAttr"] = NF([this](vector<Value>& a) -> Value {
                    if (a.empty()) return Value();
                    if (a.size() == 1) return a[0];
                    return this->GetMember(a[0], a[1], false, 0);
                });
                // table.setAttr(object,key,value) —— 官方声明
                // （lib/builtin/table.aardio）：
                //   调用成员（属性）操作符修改对象的键值。
                //   如果对象实际包含 @key 指定的键值则**直接修改**；
                //   否则调用 _set 元方法修改指定键的值，元方法第 3 个参数
                //   （@ownerAttr）设为 **true**。
                //   （用索引下标 [] 修改键值触发 _set 元方法时 ownerAttr 为 false。）
                //
                // 注意官方 intellisense 声明只列了 (object,key) 两个参数，
                // 但「修改对象的键值」必须给出新值 —— 元方法签名是 (k,v,ownerAttr)，
                // 所以值作为第 3 个参数传入。官方库内零调用点（只影响用户代码）。
                tb->map[L"setAttr"] = NF([this](vector<Value>& a) -> Value {
                    if (a.size() < 3) return Value();
                    Value obj = a[0], keyVal = a[1], val = a[2];
                    if (obj.t != Value::TableT || !obj.tab) return Value();
                    wstring key = ObjectKeyOf(keyVal);
                    // 对象实际包含该键 -> 直接修改
                    if (obj.tab->map.count(key))
                    {
                        obj.tab->map[key] = val;
                        return val;
                    }
                    // 1-based 数字下标命中数组部分 -> 直接修改（与 t[i]=v 一致）
                    long long numIdx = -1;
                    if (keyVal.t == Value::Num && keyVal.n == (long long)keyVal.n && keyVal.n >= 1)
                        numIdx = (long long)keyVal.n;
                    else if (!key.empty() && key.find_first_not_of(L"0123456789") == wstring::npos)
                        numIdx = _wtoi64(key.c_str());
                    if (numIdx >= 1 && (size_t)numIdx <= obj.tab->arr.size())
                    {
                        obj.tab->arr[(size_t)(numIdx - 1)] = val;
                        obj.tab->map.erase(key);
                        return val;
                    }
                    // 否则走 _set 元方法（ownerAttr = true）
                    bool bErr = false;
                    wstring errMsg;
                    if (this->MetaSet(obj, keyVal, val, &bErr, &errMsg, true))
                    {
                        if (bErr)
                        {
                            Interp* it = g_interpCurrent;
                            if (it && !errMsg.empty()) *it->err = errMsg;
                        }
                        return val;
                    }
                    // 没有 _set 元方法 -> 落回普通成员写入
                    // （与 t[i]=v 相同：数组末尾追加，其余写进 map）
                    if (numIdx >= 1 && (size_t)(numIdx - 1) == obj.tab->arr.size() &&
                        val.t != Value::Null)
                    {
                        obj.tab->arr.push_back(val);
                        obj.tab->map.erase(key);
                        return val;
                    }
                    if (val.t == Value::Null)
                        obj.tab->map.erase(key);
                    else
                        obj.tab->map[key] = val;
                    return val;
                });
                tb->map[L"count"] = NF([](vector<Value>& a) {
                    if (a[0].t != Value::TableT || !a[0].tab) return Value::Num_(0);
                    return Value::Num_((double)(a[0].tab->arr.size() +
                                                a[0].tab->map.size())); });
                auto fnIsArray = NF([](vector<Value>& a) -> Value {
                    if (a.empty() || a[0].t != Value::TableT || !a[0].tab)
                        return Value::Bool_(false);
                    return Value::Bool_(!a[0].tab->arr.empty());
                });
                tb->map[L"isArray"] = fnIsArray;
                tb->map[L"isArrayLike"] = fnIsArray;
                globals.VarAt(NameId(L"isArray")) = fnIsArray;
                globals.VarAt(NameId(L"isArrayLike")) = fnIsArray;
                tb->map[L"append"] = NF([](vector<Value>& a) {
                    // official contract (pinned by the official testCase):
                    // table.append(arr, v1, v2, ...) appends EVERY arg;
                    // an array-typed arg is spliced element-by-element
                    // (table.append([1,2],[3]) == [1,2,3])
                    if (a.empty() || a[0].t != Value::TableT || !a[0].tab)
                        return a.empty() ? Value() : a[0];
                    Table* t = a[0].tab.get();
                    for (size_t i = 1; i < a.size(); i++)
                    {
                        if (a[i].t == Value::TableT && a[i].tab &&
                            a[i].tab->map.empty() &&
                            !a[i].tab->arr.empty())
                        {
                            for (auto& item : a[i].tab->arr)
                                t->arr.push_back(item);
                        }
                        else
                            t->arr.push_back(a[i]);
                    }
                    return a[0]; });
                tb->map[L"gc"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty()) return Value();
                    if (a.size() > 1 && a[0].t == Value::TableT && a[0].tab)
                        a[0].tab->map[L"_gc"] = a[1];
                    return a[0]; });
                tb->map[L"add_gc"] = tb->map[L"gc"];
                auto fnPush = NF([](vector<Value>& a) -> Value {
                    if (a.empty() || a[0].t != Value::TableT || !a[0].tab)
                        return Value::Num_(0);
                    Table* t = a[0].tab.get();
                    for (size_t i = 1; i < a.size(); i++)
                    {
                        if (a[i].t != Value::Null)
                            t->arr.push_back(a[i]);
                    }
                    return Value::Num_((double)t->arr.size());
                });
                tb->map[L"push"] = fnPush;
                globals.VarAt(NameId(L"push")) = fnPush;

                // table.unpack(t[, from[, to]]) / table.unpackArgs(t) —
                // expand the array part as extra return values (popen.read,
                // io libraries return `{ file.read(...) }` through these)
                auto fnUnpack = NF([](vector<Value>& a) -> Value {
                    Value vRes;
                    if (a.empty() || a[0].t != Value::TableT || !a[0].tab)
                        return vRes;
                    auto& arr = a[0].tab->arr;
                    long long from = 1, to = (long long)arr.size();
                    if (a.size() > 1 && a[1].t != Value::Null)
                        from = (long long)BNum(a[1]);
                    if (a.size() > 2 && a[2].t != Value::Null)
                        to = (long long)BNum(a[2]);
                    if (from < 1) from = 1;
                    if (to > (long long)arr.size())
                        to = (long long)arr.size();
                    auto ex = std::make_shared<vector<Value>>();
                    for (long long i = from; i <= to; i++)
                        ex->push_back(arr[(size_t)(i - 1)]);
                    if (!ex->empty())
                    {
                        vRes = ex->at(0);
                        ex->erase(ex->begin());
                        if (!ex->empty())
                            vRes.mrets = ex;
                    }
                    return vRes;
                });
                tb->map[L"unpack"] = fnUnpack;
                tb->map[L"unpackArgs"] = fnUnpack;
                tb->map[L"left"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty() || a[0].t != Value::TableT || !a[0].tab)
                        return Value();
                    auto& arr = a[0].tab->arr;
                    long long n = (long long)arr.size();
                    long long len = a.size() > 1 ? (long long)BNum(a[1]) : 1;
                    long long count = 0;
                    if (len >= 0)
                        count = len < n ? len : n;
                    else
                    {
                        long long idx = n + len + 1;
                        count = idx < 0 ? 0 : (idx < n ? idx : n);
                    }
                    if (count <= 0) return Value();
                    auto ex = std::make_shared<vector<Value>>();
                    for (long long i = 0; i < count; i++)
                        ex->push_back(arr[(size_t)i]);
                    Value vRes = ex->at(0);
                    ex->erase(ex->begin());
                    if (!ex->empty()) vRes.mrets = ex;
                    return vRes;
                });
                tb->map[L"right"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty() || a[0].t != Value::TableT || !a[0].tab)
                        return Value();
                    auto& arr = a[0].tab->arr;
                    long long n = (long long)arr.size();
                    long long len = a.size() > 1 ? (long long)BNum(a[1]) : 1;
                    long long start = 0;
                    if (len >= 0)
                        start = len < n ? n - len : 0;
                    else
                    {
                        long long idx = -len;
                        start = idx < n ? idx : n;
                    }
                    if (start >= n) return Value();
                    auto ex = std::make_shared<vector<Value>>();
                    for (long long i = start; i < n; i++)
                        ex->push_back(arr[(size_t)i]);
                    if (ex->empty()) return Value();
                    Value vRes = ex->at(0);
                    ex->erase(ex->begin());
                    if (!ex->empty()) vRes.mrets = ex;
                    return vRes;
                });

                // table.next(t[, key]) — walks tables in insertion
                // order; the array part is ordered, the hash part's order
                // is not recorded yet, so next() covers the array first,
                // then hash members in unordered_map order (pinned in the
                // ledger). Returns (key,value), null at the end.
                tb->map[L"next"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty() ||
                        (a[0].t != Value::TableT && a[0].t != Value::Class) ||
                        !a[0].tab)
                        return Value();
                    Table* t = a[0].tab.get();
                    vector<Value> keys, vals;
                    for (size_t i = 0; i < t->arr.size(); i++)
                    {
                        keys.push_back(Value::Num_((double)(i + 1)));
                        vals.push_back(t->arr[i]);
                    }
                    for (auto& kv : t->map)
                    {
                        // a numeric-string key was written as a number
                        // (form._ctrlHwnd[hwndNumber] = ctrl):
                        // iterates it back as a NUMBER — handing a string
                        // to win.getStyle(string) marshals the string's
                        // raw buffer as the hwnd and misreads the style
                        long long nKey = 0;
                        if (!kv.first.empty() &&
                            kv.first.find_first_not_of(L"0123456789") ==
                                wstring::npos)
                            nKey = _wtoi64(kv.first.c_str());
                        if (nKey > 0)
                            keys.push_back(Value::Num_((double)nKey));
                        else
                            keys.push_back(Value::Str_(kv.first));
                        vals.push_back(kv.second);
                    }
                    size_t start = 0;
                    if (a.size() > 1 && a[1].t != Value::Null)
                    {
                        bool bFound = false;
                        for (size_t i = 0; i < keys.size(); i++)
                        {
                            bool eq = a[1].t == keys[i].t &&
                                      (keys[i].t == Value::Num
                                           ? BNum(a[1]) == keys[i].n
                                           : (keys[i].t == Value::Str &&
                                              a[1].s == keys[i].s));
                            if (eq) { start = i + 1; bFound = true; break; }
                        }
                        if (!bFound) return Value();
                    }
                    if (start >= keys.size()) return Value();
                    Value vK = keys[start];
                    auto ex = std::make_shared<vector<Value>>();
                    ex->push_back(vals[start]);
                    vK.mrets = ex;
                    return vK;
                });
                // table.pop(t[,n]) — remove n from the array tail (1 by
                // default), return the last removed value; null when empty
                tb->map[L"pop"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty() ||
                        (a[0].t != Value::TableT && a[0].t != Value::Class) ||
                        !a[0].tab)
                        return Value();
                    Table* t = a[0].tab.get();
                    long long n = (a.size() > 1 && a[1].t != Value::Null)
                                      ? (long long)BNum(a[1]) : 1;
                    if (n < 1) n = 1;
                    Value vLast;
                    bool bGot = false;
                    while (n-- > 0 && !t->arr.empty())
                    {
                        vLast = t->arr.back();
                        bGot = true;
                        t->arr.pop_back();
                    }
                    return bGot ? vLast : Value();
                });
                // table.shift(t) — remove and return the first element
                tb->map[L"shift"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty() || a[0].t != Value::TableT || !a[0].tab ||
                        a[0].tab->arr.empty())
                        return Value();
                    Value v = a[0].tab->arr.front();
                    a[0].tab->arr.erase(a[0].tab->arr.begin());
                    return v;
                });
                // table.unshift(t,...) — insert at the array front
                tb->map[L"unshift"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty() || a[0].t != Value::TableT || !a[0].tab)
                        return Value::Num_(0);
                    for (size_t i = 1; i < a.size(); i++)
                        a[0].tab->arr.insert(
                            a[0].tab->arr.begin() + (long long)(i - 1), a[i]);
                    return Value::Num_((double)a[0].tab->arr.size());
                });
                // table.eachArgs(t) — iterator over the array part,
                // yielding (index, value); popen converts codepages with it
                tb->map[L"eachArgs"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty() || a[0].t != Value::TableT || !a[0].tab)
                        return Value();
                    auto arr =
                        std::make_shared<vector<Value>>(a[0].tab->arr);
                    auto idx = std::make_shared<size_t>(0);
                    return Value::Native_([arr, idx](vector<Value>&) -> Value {
                        if (*idx >= arr->size()) return Value();
                        Value v = (*arr)[*idx];
                        Value k = Value::Num_((double)(*idx + 1));
                        auto ex = std::make_shared<vector<Value>>();
                        ex->push_back(v);
                        k.mrets = ex;
                        (*idx)++;
                        return k;
                    });
                });
                // table.each(t) — iterator over the array part and then
                // the hash members, yielding (key, value)
                tb->map[L"each"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty() || a[0].t != Value::TableT || !a[0].tab)
                        return Value();
                    auto keys = std::make_shared<vector<Value>>();
                    auto vals = std::make_shared<vector<Value>>();
                    for (size_t i = 0; i < a[0].tab->arr.size(); i++)
                    {
                        keys->push_back(Value::Num_((double)(i + 1)));
                        vals->push_back(a[0].tab->arr[i]);
                    }
                    for (auto& kv : a[0].tab->map)
                    {
                        keys->push_back(Value::Str_(kv.first));
                        vals->push_back(kv.second);
                    }
                    auto idx = std::make_shared<size_t>(0);
                    return Value::Native_([keys, vals, idx](
                            vector<Value>&) -> Value {
                        if (*idx >= keys->size()) return Value();
                        Value k = (*keys)[*idx];
                        auto ex = std::make_shared<vector<Value>>();
                        ex->push_back((*vals)[*idx]);
                        k.mrets = ex;
                        (*idx)++;
                        return k;
                    });
                });
                // table.filter(t,fn) — new array of elements where fn(v)
                // is truthy (JSON.ndParse consumers)
                tb->map[L"filter"] = NF([this](vector<Value>& a) -> Value {
                    auto out = std::make_shared<Table>();
                    if (a.size() < 2 || a[0].t != Value::TableT || !a[0].tab)
                        return Value::Table_(out);
                    for (auto& v : a[0].tab->arr)
                    {
                        vector<Value> oneArg{ v };
                        Value r = CallVmFunc(a[1], oneArg);
                        if (r.Truthy()) out->arr.push_back(v);
                    }
                    return Value::Table_(out);
                });

                // table.range(array) -> min, max — the smallest and the
                // largest numeric index of the array part (sparse-array
                // helper; an ordinary array yields 1, #arr).
                // table.range(a [, b]) with numbers builds the sequence
                // array [a..b] (b defaults to a, so range(n) = 1..n).
                tb->map[L"range"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty()) return Value();
                    if (a[0].t == Value::Num ||
                        (a.size() > 1 && a[1].t == Value::Num))
                    {
                        long long lo = a[0].t == Value::Num
                            ? (long long)BNum(a[0]) : 1;
                        long long hi = a.size() > 1 && a[1].t == Value::Num
                            ? (long long)BNum(a[1]) : lo;
                        auto seq = std::make_shared<Table>();
                        if (lo > hi) std::swap(lo, hi);
                        for (long long v = lo; v <= hi; v++)
                            seq->arr.push_back(Value::Num_((double)v));
                        return Value::Table_(seq);
                    }
                    if (a[0].t != Value::TableT || !a[0].tab)
                        return Value();
                    Table* t = a[0].tab.get();
                    long long nMin = 0, nMax = 0;
                    bool bAny = false;
                    if (!t->arr.empty())
                    {
                        nMin = 1;
                        nMax = (long long)t->arr.size();
                        bAny = true;
                    }
                    for (auto& kv : t->map)
                    {
                        if (kv.first.empty()) continue;
                        bool bNum = true;
                        for (wchar_t c : kv.first)
                            if (c < L'0' || c > L'9') { bNum = false; break; }
                        if (!bNum) continue;
                        long long ix = _wtoi64(kv.first.c_str());
                        if (ix <= 0) continue;
                        if (!bAny || ix < nMin) nMin = ix;
                        if (!bAny || ix > nMax) nMax = ix;
                        bAny = true;
                    }
                    if (!bAny) return Value();
                    Value vMin = Value::Num_((double)nMin);
                    auto extras = std::make_shared<vector<Value>>();
                    extras->push_back(Value::Num_((double)nMax));
                    vMin.mrets = extras;
                    return vMin;
                });

                auto fnPop = NF([](vector<Value>& a) -> Value {
                    if (a.empty() || a[0].t != Value::TableT || !a[0].tab || a[0].tab->arr.empty())
                        return Value();
                    Table* t = a[0].tab.get();
                    Value v = t->arr.back();
                    t->arr.pop_back();
                    return v;
                });
                tb->map[L"pop"] = fnPop;
                globals.VarAt(NameId(L"pop")) = fnPop;

                auto fnSplice = NF([](vector<Value>& a) -> Value {
                    auto res = std::make_shared<Table>();
                    if (a.empty() || a[0].t != Value::TableT || !a[0].tab)
                        return Value::Table_(res);
                    Table* t = a[0].tab.get();
                    long long idx = a.size() > 1 ? (long long)BNum(a[1]) : 1;
                    if (idx < 1) idx = (long long)t->arr.size() + idx + 1;
                    if (idx < 1) idx = 1;
                    size_t start = (size_t)idx - 1;
                    if (start > t->arr.size()) start = t->arr.size();
                    long long cnt = a.size() > 2 ? (long long)BNum(a[2]) : ((long long)t->arr.size() - (long long)start);
                    if (cnt < 0) cnt = 0;
                    size_t count = (size_t)cnt;
                    if (start + count > t->arr.size()) count = t->arr.size() - start;
                    for (size_t k = 0; k < count; k++)
                        res->arr.push_back(t->arr[start + k]);
                    t->arr.erase(t->arr.begin() + start, t->arr.begin() + start + count);
                    for (size_t k = 3; k < a.size(); k++)
                        t->arr.insert(t->arr.begin() + start + (k - 3), a[k]);
                    return Value::Table_(res);
                });
                tb->map[L"splice"] = fnSplice;
                globals.VarAt(NameId(L"splice")) = fnSplice;
                // ---- round 43: array ops ----
                // table.insert(t, val, pos=1) — in official standard library:
                //   table.insert(t, val) -> inserts val at position 1 (head of table)
                //   table.insert(t, val, pos) -> inserts val at 1-based pos (clamped to 1..#t+1)
                //   if pos is null -> appends val to tail (or inserts at pos)
                tb->map[L"insert"] = NF([](vector<Value>& a) -> Value {
                    if (a.size() < 2 || a[0].t != Value::TableT || !a[0].tab)
                        return a.size() > 0 ? a[0] : Value();
                    Table* t = a[0].tab.get();
                    const Value& val = a[1];
                    if (a.size() >= 3)
                    {
                        if (a[2].t == Value::Null)
                        {
                            t->arr.push_back(val);
                        }
                        else
                        {
                            Value insVal = a[1];
                            long long i = (long long)BNum(a[2]);
                            // Support both official (tab, value, pos) and Lua/legacy (tab, pos, value)
                            if (a[1].t == Value::Num && (a[2].t != Value::Num || (long long)a[2].n < 1 || (long long)a[2].n > (long long)t->arr.size() + 1))
                            {
                                i = (long long)BNum(a[1]);
                                insVal = a[2];
                            }
                            if (i < 1) i = 1;
                            if (i > (long long)t->arr.size() + 1)
                                i = (long long)t->arr.size() + 1;
                            t->arr.insert(t->arr.begin() + (i - 1), insVal);
                        }
                    }
                    else
                    {
                        // 2 arguments: table.insert(t, val) -> append at tail
                        t->arr.push_back(val);
                    }
                    return a[0]; });
                tb->map[L"remove"] = NF([](vector<Value>& a) -> Value {
                    if (a.size() < 2 || a[0].t != Value::TableT || !a[0].tab)
                        return Value();
                    Table* t = a[0].tab.get();
                    long long i = (long long)BNum(a[1]);
                    if (i < 1 || i > (long long)t->arr.size())
                        return Value();
                    Value v = t->arr[(size_t)i - 1];
                    t->arr.erase(t->arr.begin() + (i - 1));
                    return v; });
                tb->map[L"reverse"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty() || a[0].t != Value::TableT || !a[0].tab)
                        return a.empty() ? Value() : a[0];
                    std::reverse(a[0].tab->arr.begin(),
                                 a[0].tab->arr.end());
                    return a[0]; });
                tb->map[L"concat"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty() || a[0].t != Value::TableT || !a[0].tab)
                        return Value::Str_(L"");
                    wstring sep = a.size() > 1 ? BStr(a[1]) : L"";
                    wstring out;
                    auto& arr = a[0].tab->arr;
                    for (size_t k = 0; k < arr.size(); k++)
                    {
                        if (k) out += sep;
                        out += arr[k].ToString();
                    }
                    return Value::Str_(out); });
                tb->map[L"find"] = NF([](vector<Value>& a) -> Value {
                    if (a.size() < 2 || a[0].t != Value::TableT || !a[0].tab)
                        return Value();
                    auto& arr = a[0].tab->arr;
                    const Value& want = a[1];
                    for (size_t k = 0; k < arr.size(); k++)
                    {
                        const Value& v = arr[k];
                        bool eq;
                        if (v.t == Value::TableT || want.t == Value::TableT)
                            eq = v.tab == want.tab;   // identity
                        else
                            eq = v.ToString() == want.ToString() &&
                                 v.t == want.t;
                        if (eq) return Value::Num_((double)(k + 1));
                    }
                    return Value(); });
                // sort: default compares numbers numerically, everything
                // else by ToString; a custom comparator (function value)
                // is invoked re-entrantly and must return truthy when the
                // first argument belongs before the second
                tb->map[L"sort"] = NF([this](vector<Value>& a) -> Value {
                    if (a.empty() || a[0].t != Value::TableT || !a[0].tab)
                        return a.empty() ? Value() : a[0];
                    std::vector<Value>* pArr = &a[0].tab->arr;
                    Value cmp = a.size() > 1 ? a[1] : Value();
                    // shell sort — no recursion, stable enough for VM use
                    size_t n = pArr->size();
                    size_t gap = 1;
                    while (gap < n / 3) gap = gap * 3 + 1;
                    auto before = [this, &cmp](const Value& x,
                                               const Value& y) -> bool {
                        if (cmp.t == Value::Func || cmp.native)
                        {
                            vector<Value> args{ x, y };
                            Value r = CallVmFunc(cmp, args);
                            return r.Truthy();
                        }
                        if (x.t == Value::Num && y.t == Value::Num)
                            return x.n < y.n;
                        return x.ToString() < y.ToString(); };
                    while (gap > 0)
                    {
                        for (size_t i = gap; i < n; i++)
                        {
                            Value tmp = (*pArr)[i];
                            size_t j = i;
                            while (j >= gap && before(tmp, (*pArr)[j - gap]))
                            {
                                (*pArr)[j] = (*pArr)[j - gap];
                                j -= gap;
                            }
                            (*pArr)[j] = tmp;
                        }
                        gap /= 3;
                    }
                    return a[0]; });
                tb->map[L"cache"] = NF([](vector<Value>& a) -> Value {
                    auto tbRes = std::make_shared<Table>();
                    auto meta = std::make_shared<Table>();
                    meta->map[L"_weak"] = Value::Str_(L"kv");
                    if (a.size() >= 2 && (a[1].t == Value::Func || a[1].native))
                        meta->map[L"_get"] = a[1];
                    else if (a.size() >= 1 && (a[0].t == Value::Func || a[0].native))
                        meta->map[L"_get"] = a[0];
                    tbRes->map[L"_meta"] = Value::Table_(meta);
                    return Value::Table_(tbRes); });
                tb->map[L"mix"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty()) return Value();
                    Value target = a[0];
                    if (target.t != Value::TableT || !target.tab)
                    {
                        target = Value::Table_(std::make_shared<Table>());
                    }
                    for (size_t i = 1; i < a.size(); i++)
                    {
                        if (a[i].t != Value::TableT || !a[i].tab) continue;
                        for (auto& kv : a[i].tab->map)
                        {
                            if (target.tab->map.find(kv.first) == target.tab->map.end())
                                target.tab->map[kv.first] = kv.second;
                        }
                        for (auto& v : a[i].tab->arr)
                            target.tab->arr.push_back(v);
                    }
                    return target; });
            }
            // ---- round 36: web (WinINet HTTP) ----
            {
                auto tb = GetRoot(L"web").tab;
                tb->map[L"get"] = NF([this](vector<Value>& a) -> Value {
                    wstring strErr2;
                    wstring r = HttpFetch(BStr(a[0]), L"GET", L"", L"",
                                          &strErr2);
                    if (!strErr2.empty())
                    {
                        (*err) = L"运行错误: web.get: " + strErr2;
                        return Value();
                    }
                    return Value::Str_(r); });
                tb->map[L"post"] = NF([this](vector<Value>& a) -> Value {
                    wstring strErr2;
                    wstring strCType = a.size() > 2
                        ? BStr(a[2])
                        : L"application/x-www-form-urlencoded";
                    wstring r = HttpFetch(BStr(a[0]), L"POST",
                                          a.size() > 1 ? BStr(a[1]) : L"",
                                          strCType, &strErr2);
                    if (!strErr2.empty())
                    {
                        (*err) = L"运行错误: web.post: " + strErr2;
                        return Value();
                    }
                    return Value::Str_(r); });
                tb->map[L"download"] = NF([this](vector<Value>& a) -> Value {
                    wstring strErr2;
                    wstring r = HttpFetch(BStr(a[0]), L"GET", L"", L"",
                                          &strErr2);
                    if (!strErr2.empty())
                    {
                        (*err) = L"运行错误: web.download: " + strErr2;
                        return Value::Bool_(false);
                    }
                    wstring strFile = a.size() > 1 ? BStr(a[1]) : L"web.bin";
                    FILE* pf = nullptr;
                    if (_wfopen_s(&pf, strFile.c_str(), L"wb") != 0 || !pf)
                    {
                        (*err) = L"运行错误: web.download: 无法写文件";
                        return Value::Bool_(false);
                    }
                    int nU8 = WideCharToMultiByte(CP_UTF8, 0, r.c_str(),
                                                  (int)r.size(), nullptr, 0,
                                                  nullptr, nullptr);
                    if (nU8 > 0)
                    {
                        std::vector<char> buf8((size_t)nU8);
                        WideCharToMultiByte(CP_UTF8, 0, r.c_str(), (int)r.size(),
                                            buf8.data(), nU8, nullptr,
                                            nullptr);
                        fwrite(buf8.data(), 1, (size_t)nU8, pf);
                    }
                    fclose(pf);
                    return Value::Bool_(true); });
                tb->map[L"encode"] = NF([](vector<Value>& a) -> Value {
                    wstring s = BStr(a[0]);
                    int nU8 = WideCharToMultiByte(CP_UTF8, 0, s.c_str(),
                                                  (int)s.size(), nullptr, 0,
                                                  nullptr, nullptr);
                    std::string str8((size_t)max(nU8, 0), 0);
                    if (nU8 > 0)
                        WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(),
                                            &str8[0], nU8, nullptr, nullptr);
                    wstring out;
                    const wchar_t* hex = L"0123456789ABCDEF";
                    for (unsigned char c : str8)
                    {
                        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                            (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                            c == '.' || c == '~')
                            out += (wchar_t)c;
                        else
                        {
                            out += L'%';
                            out += hex[c >> 4];
                            out += hex[c & 0xF];
                        }
                    }
                    return Value::Str_(out); });
                tb->map[L"decode"] = NF([](vector<Value>& a) -> Value {
                    wstring s = BStr(a[0]);
                    std::string str8;
                    for (size_t i = 0; i < s.size(); i++)
                    {
                        if (s[i] == L'%' && i + 2 < s.size() + 1 &&
                            i + 2 < s.size() + 1)
                        {
                            wchar_t h1 = s[i + 1], h2 = s[i + 2];
                            auto hv = [](wchar_t c) -> int {
                                if (c >= L'0' && c <= L'9') return c - L'0';
                                if (c >= L'A' && c <= L'F') return c - L'A' + 10;
                                if (c >= L'a' && c <= L'f') return c - L'a' + 10;
                                return -1; };
                            int a1 = hv(h1), a2 = hv(h2);
                            if (a1 >= 0 && a2 >= 0)
                            {
                                str8.push_back((char)((a1 << 4) | a2));
                                i += 2;
                                continue;
                            }
                        }
                        if (s[i] == L'+')
                            str8.push_back(' ');   // form-urlencoded space
                        else
                        {
                            int nU8 = WideCharToMultiByte(CP_UTF8, 0, &s[i],
                                                          1, nullptr, 0,
                                                          nullptr, nullptr);
                            std::string one((size_t)max(nU8, 0), 0);
                            if (nU8 > 0)
                                WideCharToMultiByte(CP_UTF8, 0, &s[i], 1,
                                                    &one[0], nU8, nullptr,
                                                    nullptr);
                            str8 += one;
                        }
                    }
                    int w = MultiByteToWideChar(CP_UTF8, 0, str8.data(),
                                                (int)str8.size(), nullptr, 0);
                    if (w <= 0)
                        return Value::Str_(s);
                    wstring out((size_t)w, 0);
                    MultiByteToWideChar(CP_UTF8, 0, str8.data(),
                                        (int)str8.size(), &out[0], w);
                    return Value::Str_(out); });
            }

            

// ---- round 36: thread (worker threads, own VM per thread) ----
            {
                auto tb = GetRoot(L"thread").tab;
                // thread.applyCallWnd(form, wParam, lParam) - forward
                // a WM_THREAD_CALLWND payload to the owning window
                // (VM builtin in the original; residual stub here)
                tb->map[L"applyCallWnd"] = NF([](vector<Value>& a) -> Value {
                    return Value();
                });
                tb->map[L"getId"] = NF([](vector<Value>&) -> Value {
                    return Value::Num_((double)::GetCurrentThreadId());
                });
                // thread.getMainId — id of the thread that started the VM.
                // The ide library gates on getId()!=getMainId() to detect
                // an external launch, and dotNet/clr uses the same test;
                // inside a fiber both official calls distinguish the
                // worker, so a created fiber reports its own id here.
                tb->map[L"getMainId"] =
                    NF([](vector<Value>&) -> Value {
                    VMFiber* rf = CurRunningFiber();
                    if (rf)
                        return Value::Num_(
                            (double)(INT_PTR)(void*)rf);
                    return Value::Num_(
                        (double)::GetCurrentThreadId()); });
                // thread.status(handle) — 0 = still running ("active")
                tb->map[L"status"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty())
                        return Value::Str_(L"active");
                    HANDLE h = (HANDLE)(INT_PTR)(long long)BNum(a[0]);
                    if (!h)
                        return Value::Str_(L"active");
                    DWORD rc = WaitForSingleObject(h, 0);
                    if (rc == WAIT_OBJECT_0)
                    {
                        DWORD code = 0;
                        GetExitCodeThread(h, &code);
                        return Value::Num_((double)code);
                    }
                    return Value::Str_(L"active"); });
                // thread.suspend(handle) / thread.resume(handle)
                tb->map[L"suspend"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty())
                        return Value::Bool_(false);
                    HANDLE h = (HANDLE)(INT_PTR)(long long)BNum(a[0]);
                    if (!h)
                        return Value::Bool_(false);
                    return Value::Bool_(SuspendThread(h) != (DWORD)-1); });
                tb->map[L"resume"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty())
                        return Value::Bool_(false);
                    HANDLE h = (HANDLE)(INT_PTR)(long long)BNum(a[0]);
                    if (!h)
                        return Value::Bool_(false);
                    return Value::Bool_(ResumeThread(h) != (DWORD)-1); });
                // thread.call(funcOrName, owner, ...) - official contract: run fn in
                // the thread-shared area (critical section locked by g_threadSharedMutex).
                // A string first arg names a function in the thread-shared map.
                // Returns every value the thread fn produced.
                tb->map[L"call"] = NF([this](vector<Value>& a) -> Value {
                    if (a.empty())
                        return Value();
                    Value fn = a[0];
                    std::lock_guard<std::recursive_mutex> lk(g_threadSharedMutex);
                    if (fn.t == Value::Str)
                    {
                        auto it = g_threadSharedMap.find(fn.s);
                        if (it != g_threadSharedMap.end())
                            fn = it->second;
                        else
                            fn = Value();
                    }
                    if (fn.t != Value::Func && !fn.native && !(fn.t == Value::TableT && fn.tab))
                    {
                        (*err) = L"\u8fd0\u884c\u9519\u8bef: thread.call \u9700\u8981\u51fd\u6570";
                        return Value();
                    }
                    Value owner = a.size() > 1 ? a[1] : Value();
                    vector<Value> args;
                    for (size_t i = 2; i < a.size(); i++)
                        args.push_back(a[i]);
                    // Thread-shared functions execute in global namespace without enclosing lexical scopes
                    fn.fnData->nsClosure = nullptr;
                    fn.fnData->envUp = nullptr;
                    return this->CallOwner(fn, owner, args); });
                tb->map[L"acquire"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty()) return Value();
                    wstring key = BStr(a[0]);
                    double timeout = a.size() > 1 ? BNum(a[1]) : -1.0;
                    DWORD start = GetTickCount();
                    while (true)
                    {
                        {
                            std::lock_guard<std::recursive_mutex> lock(g_threadSharedMutex);
                            auto it = g_threadSharedMap.find(key);
                            if (it != g_threadSharedMap.end() && it->second.t != Value::Null)
                                return it->second;
                        }
                        if (timeout >= 0 && (GetTickCount() - start) >= (DWORD)timeout)
                            return Value();
                        Sleep(5);
                    }
                });
                tb->map[L"waitOne"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty())
                        return Value();
                    HANDLE h = (HANDLE)(INT_PTR)(long long)BNum(a[0]);
                    DWORD ms = a.size() > 1 ? (DWORD)BNum(a[1]) : INFINITE;
                    if (!h)
                        return Value();
                    DWORD rc = WaitForSingleObject(h, ms);
                    return Value::Bool_(rc == WAIT_OBJECT_0); });
                // thread.getExitCode(handle) — exit code of a finished
                // thread or process handle (process.waitOne reads it after
                // a successful wait; popen's peek path depends on it)
                tb->map[L"getExitCode"] =
                    NF([](vector<Value>& a) -> Value {
                    if (a.empty())
                        return Value();
                    HANDLE h = (HANDLE)(INT_PTR)(long long)BNum(a[0]);
                    if (!h)
                        return Value();
                    DWORD code = 0;
                    if (!GetExitCodeThread(h, &code) &&
                        !GetExitCodeProcess(h, &code))
                        return Value();
                    return Value::Num_(code); });
                // thread.lock(name[,fn,...]) — named inter-thread lock. With
                // a function it runs under the lock and returns its results;
                // without one it acquires the lock until thread.unlock(name).
                static std::mutex g_lockMapMutex;
                static std::map<wstring, std::shared_ptr<std::recursive_mutex>>
                    g_namedLocks;
                tb->map[L"lock"] = NF([this](vector<Value>& a) -> Value {
                    if (a.empty())
                        return Value();
                    wstring nm = BStr(a[0]);
                    std::shared_ptr<std::recursive_mutex> m;
                    {
                        std::lock_guard<std::mutex> g(g_lockMapMutex);
                        auto it = g_namedLocks.find(nm);
                        if (it == g_namedLocks.end())
                        {
                            m = std::make_shared<std::recursive_mutex>();
                            g_namedLocks[nm] = m;
                        }
                        else
                            m = it->second;
                    }
                    if (a.size() > 1 && a[1].t == Value::Func)
                    {
                        std::lock_guard<std::recursive_mutex> g(*m);
                        vector<Value> args;
                        for (size_t i = 2; i < a.size(); i++)
                            args.push_back(a[i]);
                        return this->CallVmFunc(a[1], args);
                    }
                    // Without a function the lock is advisory (used
                    // as "already open?" guard): report whether it can
                    // be taken instead of blocking a single-threaded VM.
                    if (m->try_lock())
                    {
                        m->unlock();
                        return Value::Bool_(true);
                    }
                    return Value::Bool_(false); });
                tb->map[L"unlock"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty())
                        return Value::Bool_(false);
                    std::shared_ptr<std::recursive_mutex> m;
                    {
                        std::lock_guard<std::mutex> g(g_lockMapMutex);
                        auto it = g_namedLocks.find(BStr(a[0]));
                        if (it == g_namedLocks.end())
                            return Value::Bool_(false);
                        m = it->second;
                    }
                    m->unlock();
                    return Value::Bool_(true); });
                tb->map[L"close"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty())
                        return Value::Bool_(false);
                    HANDLE h = (HANDLE)(INT_PTR)(long long)BNum(a[0]);
                    if (!h)
                        return Value::Bool_(false);
                    return Value::Bool_(CloseHandle(h) != 0); });
                tb->map[L"get"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty()) return Value();
                    std::lock_guard<std::recursive_mutex> lock(g_threadSharedMutex);
                    auto it = g_threadSharedMap.find(BStr(a[0]));
                    if (it != g_threadSharedMap.end()) return it->second;
                    return Value();
                });
                tb->map[L"set"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty()) return Value();
                    std::lock_guard<std::recursive_mutex> lock(g_threadSharedMutex);
                    if (a.size() > 1 && a[1].t != Value::Null)
                        g_threadSharedMap[BStr(a[0])] = a[1];
                    else
                        g_threadSharedMap.erase(BStr(a[0]));
                    return Value();
                });
                tb->map[L"getWnd"] = NF([](vector<Value>&) -> Value {
                    return Value();
                });
                tb->map[L"setWnd"] = NF([](vector<Value>&) -> Value {
                    return Value();
                });
                tb->map[L"callWnd"] = NF([](vector<Value>&) -> Value {
                    return Value();
                });
                // __waitForMultipleObjects(timeout, waitall, handles,
                // flags) — kernel global consumed by
                // threadwait. With a QS_* wake
                // mask it must be MsgWaitForMultipleObjects so that
                // "input satisfied" returns WAIT_OBJECT_0+count (the
                // caller then pumps and retries); without one it is a
                // plain WaitForMultipleObjects.
                globals.VarAt(NameId(L"__waitForMultipleObjects")) =
                    NF([](vector<Value>& a) -> Value {
                        if (a.size() < 3)
                            return Value::Num_(0xFFFFFFFF);
                        DWORD ms = (DWORD)(long long)BNum(a[0]);
                        bool bAll = a[1].Truthy();
                        std::vector<HANDLE> hs;
                        if (a[2].t == Value::TableT && a[2].tab)
                        {
                            for (auto& h : a[2].tab->arr)
                                hs.push_back((HANDLE)(INT_PTR)(long long)
                                                 BNum(h));
                        }
                        else if (a[2].t != Value::Null)
                        {
                            hs.push_back((HANDLE)(INT_PTR)(long long)
                                             BNum(a[2]));
                        }
                        if (hs.empty() ||
                            hs.size() > MAXIMUM_WAIT_OBJECTS)
                            return Value::Num_(0xFFFFFFFF);
                        UINT wake = a.size() > 3
                                        ? (UINT)(long long)BNum(a[3])
                                        : 0;
                        DWORD rc = wake
                                       ? MsgWaitForMultipleObjects(
                                             (DWORD)hs.size(), hs.data(),
                                             bAll ? TRUE : FALSE, ms, wake)
                                       : WaitForMultipleObjects(
                                             (DWORD)hs.size(), hs.data(),
                                             bAll ? TRUE : FALSE, ms);
                        return Value::Num_((double)rc);
                    });
                // ---- kernel message-loop primitives
                // injects these as __-prefixed globals
                auto fillMsgTable = [](Value& vMsg, const MSG& m) -> Value {
                    auto tb = (vMsg.t == Value::TableT && vMsg.tab)
                                  ? vMsg.tab
                                  : std::make_shared<Table>();
                    tb->map[L"hwnd"] =
                        Value::Num_((double)(INT_PTR)(UINT_PTR)m.hwnd);
                    tb->map[L"message"] = Value::Num_((double)m.message);
                    tb->map[L"wParam"] =
                        Value::Num_((double)(long long)m.wParam);
                    tb->map[L"lParam"] =
                        Value::Num_((double)(long long)m.lParam);
                    tb->map[L"time"] = Value::Num_((double)m.time);
                    tb->map[L"x"] = Value::Num_((double)m.pt.x);
                    tb->map[L"y"] = Value::Num_((double)m.pt.y);
                    // The message table doubles as a native MSG struct:
                    // official win.parseMessage hands it to
                    // win.isDialogMessage(root,msg), whose signature declares
                    // the 2nd parameter as `struct msg` (passed by value).
                    // The official pump fills a PLAIN table ({}), so it has no
                    // struct metadata of its own — without this the FFI has
                    // no layout to marshal and passes NULL → IsDialogMessageW
                    // reads address 0 and kills the process on the first key
                    // press. This is the x64-native MSG layout (48 bytes).
                    if (tb->map.find(L"_struct") == tb->map.end())
                        tb->map[L"_struct"] = Value::Str_(
                            L"addr hwnd;INT message;ADDR wParam;addr lParam;"
                            L"INT time;int x;int y;");
                    return Value::Table_(tb);
                };
                auto msgHwnd = [](vector<Value>& a) -> HWND {
                    return (a.size() > 1 && a[1].t == Value::Num)
                               ? (HWND)(INT_PTR)(long long)BNum(a[1])
                               : nullptr;
                };
                auto msgRange = [](vector<Value>& a, size_t idx,
                                   UINT def) -> UINT {
                    return (a.size() > idx && a[idx].t == Value::Num)
                               ? (UINT)(long long)BNum(a[idx])
                               : def;
                };
                // peekMessage: non-blocking; option defaults to PM_REMOVE
                // (resetMessage strips a pending WM_QUIT through it);
                // returns the MSG table, false when the queue is empty,
                // null on WM_QUIT (table still filled so callers can read
                // the exit code)
                globals.VarAt(NameId(L"__messagePeek")) =
                    NF([fillMsgTable, msgHwnd, msgRange](
                           vector<Value>& a) -> Value {
                        MSG m;
                        ZeroMemory(&m, sizeof(m));
                        UINT opt = msgRange(a, 4, PM_REMOVE);
                        if (!PeekMessageW(&m, msgHwnd(a), msgRange(a, 2, 0),
                                          msgRange(a, 3, 0), opt))
                            return Value::Bool_(false);
                        Value vMsg =
                            fillMsgTable(a.empty() ? Value() : a[0], m);
                        if (m.message == WM_QUIT) return Value();
                        return vMsg;
                    });
                // getMessage: blocking GetMessageW; an explicit PM_NOREMOVE
                // option switches to a non-blocking PeekMessageW that
                // returns false when no message is queued
                globals.VarAt(NameId(L"__messageGet")) =
                    NF([fillMsgTable, msgHwnd, msgRange](
                           vector<Value>& a) -> Value {
                        MSG m;
                        ZeroMemory(&m, sizeof(m));
                        UINT opt = msgRange(a, 4, PM_REMOVE);
                        if (!(opt & PM_REMOVE))
                        {
                            if (!PeekMessageW(&m, msgHwnd(a),
                                              msgRange(a, 2, 0),
                                              msgRange(a, 3, 0), opt))
                                return Value::Bool_(false);
                        }
                        else
                        {
                            if (GetMessageW(&m, msgHwnd(a), msgRange(a, 2, 0),
                                            msgRange(a, 3, 0)) == -1)
                                return Value::Bool_(false);
                        }
                        Value vMsg =
                            fillMsgTable(a.empty() ? Value() : a[0], m);
                        if (m.message == WM_QUIT) return Value();
                        return vMsg;
                    });
                // translate+dispatch a MSG table fetched by peek/get
                globals.VarAt(NameId(L"__messageTranslateDispatch")) =
                    NF([](vector<Value>& a) -> Value {
                        if (a.empty() || a[0].t != Value::TableT ||
                            !a[0].tab)
                            return Value();
                        auto& mp = a[0].tab->map;
                        auto fld = [&mp](const wchar_t* k) -> long long {
                            auto it = mp.find(k);
                            return it == mp.end()
                                       ? 0
                                       : (long long)BNum(it->second);
                        };
                        MSG m;
                        ZeroMemory(&m, sizeof(m));
                        m.hwnd = (HWND)(INT_PTR)fld(L"hwnd");
                        m.message = (UINT)fld(L"message");
                        m.wParam = (WPARAM)(INT_PTR)fld(L"wParam");
                        m.lParam = (LPARAM)(INT_PTR)fld(L"lParam");
                        m.time = (DWORD)fld(L"time");
                        m.pt.x = (LONG)fld(L"x");
                        m.pt.y = (LONG)fld(L"y");
                        TranslateMessage(&m);
                        DispatchMessageW(&m);
                        return Value();
                    });
                tb->map[L"create"] = NF([this](vector<Value>& a) -> Value {
                    if (a.empty())
                    {
                        (*err) = L"运行错误: thread.create 缺少参数";
                        return Value();
                    }
                    PNode fnBody;
                    vector<wstring> params;
                    vector<Value> args;
                    Value ownerVal;
                    wstring newBaseDir;

                    if (a[0].t == Value::Func && a[0].fnData->fn)
                    {
                        fnBody = a[0].fnData->fn;
                        params = a[0].fnData->params;
                        for (size_t i = 1; i < a.size(); i++)
                            args.push_back(DeepCopy(a[i]));
                    }
                    else if (a[0].t == Value::Str)
                    {
                        wstring scriptOrPath = a[0].s;
                        wstring code;
                        wstring fullPath = ResolveTildeDisk(scriptOrPath);
                        if (GetFileAttributesW(fullPath.c_str()) == INVALID_FILE_ATTRIBUTES && !baseDir.empty())
                        {
                            wstring cand = baseDir + L"\\" + scriptOrPath;
                            if (GetFileAttributesW(cand.c_str()) != INVALID_FILE_ATTRIBUTES)
                                fullPath = cand;
                        }
                        DWORD attr = GetFileAttributesW(fullPath.c_str());
                        if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY))
                        {
                            FILE* pf = nullptr;
                            if (_wfopen_s(&pf, fullPath.c_str(), L"rb") == 0 && pf)
                            {
                                std::string raw;
                                char buf[4096]; size_t rd;
                                while ((rd = fread(buf, 1, sizeof(buf), pf)) > 0)
                                    raw.append(buf, rd);
                                fclose(pf);
                                size_t off = 0;
                                if (raw.size() >= 3 &&
                                    (unsigned char)raw[0] == 0xEF &&
                                    (unsigned char)raw[1] == 0xBB &&
                                    (unsigned char)raw[2] == 0xBF)
                                    off = 3;
                                int w = MultiByteToWideChar(CP_UTF8, 0, raw.data() + off,
                                    (int)(raw.size() - off), nullptr, 0);
                                if (w > 0)
                                {
                                    code.resize((size_t)w);
                                    MultiByteToWideChar(CP_UTF8, 0, raw.data() + off,
                                        (int)(raw.size() - off), &code[0], w);
                                }
                            }
                            size_t slash = fullPath.find_last_of(L"\\/");
                            if (slash != wstring::npos)
                                newBaseDir = fullPath.substr(0, slash);
                            else
                                newBaseDir = fullPath;
                        }
                        else
                        {
                            code = scriptOrPath;
                        }
                        wstring perr;
                        PNode fn = CompileToFunc(code, !newBaseDir.empty() ? &newBaseDir : nullptr, &perr);
                        if (fn)
                        {
                            fnBody = fn->a;
                            params = fn->names;
                        }
                        else
                        {
                            (*err) = L"运行错误: thread.create 解析脚本失败: " + perr;
                            return Value();
                        }
                        if (a.size() > 1)
                            ownerVal = DeepCopy(a[1]);
                        for (size_t i = 1; i < a.size(); i++)
                            args.push_back(DeepCopy(a[i]));
                    }
                    else
                    {
                        (*err) = L"运行错误: thread.create 需要函数字面值或脚本路径";
                        return Value();
                    }

                    auto st = std::make_shared<ThreadState>();
                    HANDLE hTh = CreateThread(
                        nullptr, 0, ThreadWorkerProc,
                        new ThreadJob{ st, fnBody, params, args, ownerVal, newBaseDir }, 0,
                        nullptr);
                    if (!hTh)
                    {
                        (*err) = L"运行错误: thread.create CreateThread 失败";
                        return Value();
                    }
                    {
                        std::lock_guard<std::mutex> lk(g_threadStatesMutex);
                        g_threadStates[hTh] = st;
                    }
                    return Value::Pointer_((void*)hTh); });
            }

            // ---- round 37: fsys (file system) ----
            {
                auto tb = GetRoot(L"fsys").tab;
                tb->map[L"joinpath"] = GetRoot(L"io").tab->map[L"joinpath"];
                tb->map[L"splitpath"] = GetRoot(L"io").tab->map[L"splitpath"];
                tb->map[L"exists"] = NF([](vector<Value>& a) -> Value {
                    return Value::Bool_(
                        GetFileAttributesW(BStr(a[0]).c_str()) !=
                        INVALID_FILE_ATTRIBUTES); });
                tb->map[L"isDir"] = NF([](vector<Value>& a) -> Value {
                    DWORD at = GetFileAttributesW(BStr(a[0]).c_str());
                    return Value::Bool_(at != INVALID_FILE_ATTRIBUTES &&
                                        (at & FILE_ATTRIBUTE_DIRECTORY)); });
                tb->map[L"createDir"] = NF([](vector<Value>& a) -> Value {
                    // recursive: create each path segment in turn
                    wstring strPath = BStr(a[0]);
                    if (strPath.empty())
                        return Value::Bool_(false);
                    DWORD at = GetFileAttributesW(strPath.c_str());
                    if (at != INVALID_FILE_ATTRIBUTES)
                        return Value::Bool_(at & FILE_ATTRIBUTE_DIRECTORY);
                    for (size_t i = 3; i < strPath.size(); i++)
                    {
                        if (strPath[i] != L'\\' && strPath[i] != L'/')
                            continue;
                        wstring part = strPath.substr(0, i);
                        if (!part.empty() && part[part.size() - 1] != L':')
                            CreateDirectoryW(part.c_str(), nullptr);
                    }
                    return Value::Bool_(CreateDirectoryW(strPath.c_str(),
                                                         nullptr) ||
                        GetFileAttributesW(strPath.c_str()) !=
                            INVALID_FILE_ATTRIBUTES); });
                GetRoot(L"io").tab->map[L"createDir"] = tb->map[L"createDir"];
                tb->map[L"delete"] = NF([](vector<Value>& a) -> Value {
                    // recursive delete (files then deepest-first directories)
                    std::function<bool(const wstring&)> del =
                        [&](const wstring& strDir) -> bool {
                        WIN32_FIND_DATAW fd;
                        HANDLE h = FindFirstFileW(
                            (strDir + L"\\*").c_str(), &fd);
                        if (h != INVALID_HANDLE_VALUE)
                        {
                            do
                            {
                                if (!wcscmp(fd.cFileName, L".") ||
                                    !wcscmp(fd.cFileName, L".."))
                                    continue;
                                wstring strChild =
                                    strDir + L"\\" + fd.cFileName;
                                if (fd.dwFileAttributes &
                                    FILE_ATTRIBUTE_DIRECTORY)
                                    del(strChild);
                                else
                                    DeleteFileW(strChild.c_str());
                            } while (FindNextFileW(h, &fd));
                            FindClose(h);
                        }
                        return RemoveDirectoryW(strDir.c_str()) ||
                            DeleteFileW(strDir.c_str()) ||
                            GetFileAttributesW(strDir.c_str()) ==
                                INVALID_FILE_ATTRIBUTES;
                    };
                    return Value::Bool_(del(BStr(a[0]))); });
                tb->map[L"copy"] = NF([](vector<Value>& a) -> Value {
                    return Value::Bool_(CopyFileW(BStr(a[0]).c_str(),
                        BStr(a[1]).c_str(), FALSE)); });
                tb->map[L"move"] = NF([](vector<Value>& a) -> Value {
                    return Value::Bool_(MoveFileW(BStr(a[0]).c_str(),
                        BStr(a[1]).c_str())); });
                tb->map[L"size"] = NF([](vector<Value>& a) -> Value {
                    WIN32_FIND_DATAW fd;
                    HANDLE h = FindFirstFileW(BStr(a[0]).c_str(), &fd);
                    if (h == INVALID_HANDLE_VALUE)
                        return Value::Num_(0);
                    FindClose(h);
                    ULONGLONG n = ((ULONGLONG)fd.nFileSizeHigh << 32) |
                                  fd.nFileSizeLow;
                    return Value::Num_((double)n); });
                tb->map[L"list"] = NF([](vector<Value>& a) -> Value {
                    // array of {name, dir, size}; "." and ".." skipped
                    auto out = std::make_shared<Table>();
                    WIN32_FIND_DATAW fd;
                    HANDLE h = FindFirstFileW(
                        (BStr(a[0]) + L"\\*").c_str(), &fd);
                    if (h == INVALID_HANDLE_VALUE)
                        return Value::Table_(out);
                    do
                    {
                        if (!wcscmp(fd.cFileName, L".") ||
                            !wcscmp(fd.cFileName, L".."))
                            continue;
                        auto it = std::make_shared<Table>();
                        it->map[L"name"] = Value::Str_(fd.cFileName);
                        it->map[L"dir"] = Value::Bool_(
                            fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
                        ULONGLONG n = ((ULONGLONG)fd.nFileSizeHigh << 32) |
                                      fd.nFileSizeLow;
                        it->map[L"size"] = Value::Num_((double)n);
                        out->arr.push_back(Value::Table_(it));
                    } while (FindNextFileW(h, &fd));
                    FindClose(h);
                    return Value::Table_(out); });
                tb->map[L"tmpDir"] = NF([](vector<Value>&) -> Value {
                    wchar_t szPath[MAX_PATH + 1] = L"";
                    DWORD n = GetTempPathW(MAX_PATH, szPath);
                    wstring s(n ? szPath : L"");
                    if (!s.empty() && s[s.size() - 1] != L'\\')
                        s += L"\\";
                    return Value::Str_(s); });
            }

            // ---- round 37: process (launch + run with output) ----
            {
                auto tb = GetRoot(L"process").tab;
                tb->map[L"execute"] = NF([](vector<Value>& a) -> Value {
                    // ShellExecute: launch, do not wait
                    wstring strParams = a.size() > 1 ? BStr(a[1]) : L"";
                    HINSTANCE h = ShellExecuteW(nullptr, L"open",
                        BStr(a[0]).c_str(),
                        strParams.empty() ? nullptr : strParams.c_str(),
                        nullptr, SW_SHOWNORMAL);
                    return Value::Bool_((INT_PTR)h > 32); });
                tb->map[L"run"] = NF([](vector<Value>& a) -> Value {
                    // run a command line, wait, capture merged output
                    auto out = std::make_shared<Table>();
                    out->map[L"out"] = Value::Str_(L"");
                    out->map[L"code"] = Value::Num_(-1);
                    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
                    HANDLE hRead = nullptr, hWrite = nullptr;
                    if (!CreatePipe(&hRead, &hWrite, &sa, 0))
                        return Value::Table_(out);
                    STARTUPINFOW si = { sizeof(si) };
                    si.dwFlags = STARTF_USESTDHANDLES;
                    si.hStdOutput = hWrite;
                    si.hStdError = hWrite;
                    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
                    wstring strCmd = BStr(a[0]);
                    vector<wchar_t> cmd(strCmd.begin(), strCmd.end());
                    cmd.push_back(L'\0');
                    PROCESS_INFORMATION pi = {};
                    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr,
                        nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                        &si, &pi);
                    CloseHandle(hWrite);   // child holds the write end now
                    if (!ok)
                    {
                        CloseHandle(hRead);
                        return Value::Table_(out);
                    }
                    std::string str8;
                    char buf[4096];
                    DWORD n = 0;
                    while (ReadFile(hRead, buf, sizeof(buf), &n, nullptr) &&
                           n > 0)
                        str8.append(buf, n);
                    CloseHandle(hRead);
                    WaitForSingleObject(pi.hProcess, 15000);
                    DWORD code = 0;
                    GetExitCodeProcess(pi.hProcess, &code);
                    CloseHandle(pi.hThread);
                    CloseHandle(pi.hProcess);
                    out->map[L"code"] = Value::Num_((double)code);
                    int w = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                        str8.data(), (int)str8.size(), nullptr, 0);
                    if (w <= 0)
                        w = MultiByteToWideChar(CP_ACP, 0, str8.data(),
                                                (int)str8.size(), nullptr, 0);
                    if (w > 0)
                    {
                        wstring s((size_t)w, 0);
                        MultiByteToWideChar(CP_UTF8, 0, str8.data(),
                                            (int)str8.size(), &s[0], w);
                        out->map[L"out"] = Value::Str_(s);
                    }
                    return Value::Table_(out); });
            }

    // ---- round 38: win.form — minimal windowing ----
            {
                auto tb = GetRoot(L"win").tab;
                // win.setPos / win.getPos — the official win library calls
                // ::SetWindowPos, and the engine's `::` does not resolve
                // bare system APIs, so the geometry helpers the design
                // canvas relies on have to be native.
                auto HwndArg = [](const Value& v) -> HWND {
                    if (v.t == Value::Num)
                        return (HWND)(INT_PTR)(long long)v.n;
                    if (v.t == Value::TableT && v.tab)
                    {
                        auto ih = v.tab->map.find(L"hwnd");
                        if (ih != v.tab->map.end() &&
                            ih->second.t == Value::Num)
                            return (HWND)(INT_PTR)(long long)ih->second.n;
                    }
                    return nullptr; };
                tb->map[L"setPos"] = NF([HwndArg](vector<Value>& a) -> Value {
                    if (a.empty()) return Value();
                    HWND h = HwndArg(a[0]);
                    if (!h) return Value();
                    bool bNoMove = a.size() < 2 || a[1].t == Value::Null;
                    bool bNoSize = a.size() < 4 || a[3].t == Value::Null;
                    int x = bNoMove ? 0 : (int)BNum(a[1]);
                    int y = (!bNoMove && a.size() > 2) ? (int)BNum(a[2]) : 0;
                    int cx = bNoSize ? 0 : (int)BNum(a[3]);
                    int cy = (!bNoSize && a.size() > 4) ? (int)BNum(a[4]) : 0;
                    HWND after = nullptr;
                    // after 参数（hWndInsertAfter）两种写法都要认：
                    //   · 指针型：topointer(1) / topointer(0)
                    //   · 数值型：1 = _HWND_BOTTOM，0 = _HWND_TOP
                    // 以前只认指针型 → 传数值 1 会被当成「没传」，
                    // after 退回 nullptr(=HWND_TOP)，
                    // 「把窗口压到最底」实际变成了「提到最顶」。
                    if (a.size() > 5)
                    {
                        if (a[5].t == Value::Pointer)
                            after = (HWND)a[5].ptr;
                        else if (a[5].t == Value::Num)
                            after = (HWND)(INT_PTR)(long long)BNum(a[5]);
                    }
                    UINT flag = 0;
                    if (a.size() > 6 && a[6].t == Value::Num)
                        flag = (UINT)(long long)BNum(a[6]);
                    else
                        flag = SWP_NOACTIVATE | SWP_NOZORDER;
                    if (bNoMove) flag |= SWP_NOMOVE;
                    if (bNoSize) flag |= SWP_NOSIZE;
                    SetWindowPos(h, after, x, y, cx, cy, flag);
                    return Value(); });
                tb->map[L"getPos"] = NF([HwndArg](vector<Value>& a) -> Value {
                    if (a.empty()) return Value();
                    HWND h = HwndArg(a[0]);
                    if (!h) return Value();
                    RECT rc;
                    if (!GetWindowRect(h, &rc)) return Value();
                    auto out = std::make_shared<Table>();
                    out->map[L"left"] = Value::Num_((double)rc.left);
                    out->map[L"top"] = Value::Num_((double)rc.top);
                    out->map[L"width"] = Value::Num_((double)(rc.right - rc.left));
                    out->map[L"height"] = Value::Num_((double)(rc.bottom - rc.top));
                    out->map[L"right"] = Value::Num_((double)rc.right);
                    out->map[L"bottom"] = Value::Num_((double)rc.bottom);
                    return Value::Table_(out); });
                tb->map[L"form"] = NF([this](vector<Value>& a) -> Value {
                    if (!RegisterFormClass())
                    {
                        (*err) = L"运行错误: win.form 窗口类注册失败";
                        return Value();
                    }
                    wstring strText = a.size() > 0 ? BStr(a[0]) : L"窗体";
                    int nW = a.size() > 1 ? (int)BNum(a[1]) : 600;
                    int nH = a.size() > 2 ? (int)BNum(a[2]) : 400;
                    auto fs = std::make_shared<FormState>();
                    fs->it = this;
                    HWND hwnd = CreateWindowExW(
                        0, GetNodioConfig().windowClass.c_str(), strText.c_str(),
                        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                        nW, nH, nullptr, nullptr, GetModuleHandle(nullptr),
                        nullptr);
                    if (!hwnd)
                    {
                        fs.reset();
                        (*err) = L"运行错误: win.form 窗口创建失败";
                        return Value();
                    }
                    g_forms[hwnd] = fs;

                    auto form = std::make_shared<Table>();

                    HFONT hGui = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
                    SendMessageW(hwnd, WM_SETFONT, (WPARAM)hGui, TRUE);

                    form->map[L"text"] = NF([hwnd](vector<Value>&) -> Value {
                        wchar_t buf[512] = L"";
                        GetWindowTextW(hwnd, buf, 512);
                        return Value::Str_(buf); });
                    form->map[L"setText"] = NF([hwnd](vector<Value>& a2) -> Value {
                        SetWindowTextW(hwnd, BStr(a2[0]).c_str());
                        return Value(); });
                    form->map[L"hwnd"] = Value::Num_((double)(INT_PTR)hwnd);
                    form->map[L"show"] = NF([hwnd](vector<Value>& a) -> Value {
                        // show(flag) — the official API hides the window when
                        // the flag is false (the design canvas uses that)
                        bool bShow = a.empty() ? true : BNum(a[0]) != 0;
                        ShowWindow(hwnd, bShow ? SW_SHOW : SW_HIDE);
                        if (bShow) UpdateWindow(hwnd);
                        return Value(); });
                    form->map[L"doModal"] = NF([fs, hwnd](vector<Value>&)
                        -> Value {
                        ShowWindow(hwnd, SW_SHOW);
                        UpdateWindow(hwnd);
                        MSG msg;
                        while (!fs->bModalDone && IsWindow(hwnd) &&
                               GetMessageW(&msg, nullptr, 0, 0) > 0)
                        {
                            TranslateMessage(&msg);
                            DispatchMessageW(&msg);
                        }
                        return Value::Num_((double)fs->nModalResult); });
                    form->map[L"endModal"] = NF([fs, hwnd](vector<Value>& a2)
                        -> Value {
                        fs->nModalResult = a2.empty() ? 0 : (int)BNum(a2[0]);
                        fs->bModalDone = true;
                        PostMessageW(hwnd, WM_QUIT, 0, 0);
                        return Value(); });
                    form->map[L"close"] = NF([hwnd](vector<Value>&) -> Value {
                        PostMessageW(hwnd, WM_CLOSE, 0, 0);
                        return Value(); });
                    // round 40: paint trigger + timers (ontimer(tid) member)
                    fs->formVal = Value::Table_(form);
                    form->map[L"invalidate"] =
                        NF([fs, hwnd](vector<Value>&) -> Value {
                        InvalidateRect(hwnd, nullptr, TRUE);
                        if (!fs->bInPaint)
                            UpdateWindow(hwnd);   // synchronous repaint
                        return Value(); });
                    form->map[L"setInterval"] =
                        NF([fs, hwnd](vector<Value>& a2) -> Value {
                        int nId = fs->nNextTimer++;
                        UINT ms = a2.empty() ? 1000 : (UINT)BNum(a2[0]);
                        SetTimer(hwnd, (UINT_PTR)nId, ms, nullptr);
                        return Value::Num_((double)nId); });
                    form->map[L"killTimer"] =
                        NF([fs, hwnd](vector<Value>& a2) -> Value {
                        if (!a2.empty())
                            KillTimer(hwnd, (UINT_PTR)(int)BNum(a2[0]));
                        return Value(); });
                    // round 41: menu bar — addMenu(text) -> popup table;
                    // popup.add(text) -> item table with .oncommand slot
                    form->map[L"addMenu"] =
                        NF([fs, hwnd, form, this](vector<Value>& a2)
                            -> Value {
                        wstring strText = a2.empty() ? L"菜单" : BStr(a2[0]);
                        if (!fs->hMenuBar)
                        {
                            fs->hMenuBar = CreateMenu();
                            SetMenu(hwnd, fs->hMenuBar);
                        }
                        HMENU hPopup = CreatePopupMenu();
                        AppendMenuW(fs->hMenuBar, MF_POPUP,
                            (UINT_PTR)hPopup, strText.c_str());
                        DrawMenuBar(hwnd);
                        auto popup = std::make_shared<Table>();
                        // popup.add(text) -> item table
                        popup->map[L"add"] = NF([fs, hPopup, hwnd, this](
                            vector<Value>& a3) -> Value {
                            wstring s = a3.empty() ? L"" : BStr(a3[0]);
                            int nMid = fs->nNextMenuId++;
                            AppendMenuW(hPopup, MF_STRING,
                                (UINT_PTR)nMid, s.c_str());
                            auto item = std::make_shared<Table>();
                            item->map[L"text"] = NF([s](
                                vector<Value>&) -> Value {
                                return Value::Str_(s); });
                            item->map[L"oncommand"] = Value();
                            item->map[L"click"] = NF([hwnd, nMid](
                                vector<Value>&) -> Value {
                                PostMessageW(hwnd, WM_COMMAND,
                                    MAKEWPARAM(nMid, 0), 0);
                                return Value(); });
                            item->map[L"enable"] = NF([fs, nMid](
                                vector<Value>& a4) -> Value {
                                EnableMenuItem(fs->hMenuBar, nMid,
                                    MF_BYCOMMAND |
                                        (!a4.empty() && BNum(a4[0]) != 0
                                             ? MF_ENABLED
                                             : MF_GRAYED));
                                return Value(); });
                            item->map[L"check"] = NF([fs, nMid](
                                vector<Value>& a4) -> Value {
                                CheckMenuItem(fs->hMenuBar, nMid,
                                    MF_BYCOMMAND |
                                        (!a4.empty() && BNum(a4[0]) != 0
                                             ? MF_CHECKED
                                             : MF_UNCHECKED));
                                return Value(); });
                            fs->menuItems[nMid] = Value::Table_(item);
                            fs->menus[nMid] = NF([fs, nMid, this](
                                vector<Value>&) -> Value {
                                auto ii = fs->menuItems.find(nMid);
                                if (ii == fs->menuItems.end() ||
                                    !ii->second.tab)
                                    return Value();
                                auto ib = ii->second.tab->map.find(
                                    L"oncommand");
                                if (ib == ii->second.tab->map.end())
                                    return Value();
                                vector<Value> noArgs;
                                return this->CallVmFunc(ib->second,
                                                        noArgs); });
                            return Value::Table_(item); });
                        // popup.sep() -> separator
                        popup->map[L"sep"] = NF([hPopup](
                            vector<Value>&) -> Value {
                            AppendMenuW(hPopup, MF_SEPARATOR, 0, nullptr);
                            return Value(); });
                        // popup.addMenu(text) -> nested popup
                        popup->map[L"addMenu"] =
                            NF([fs, hPopup, hwnd, this](
                                vector<Value>& a3) -> Value {
                            wstring s = a3.empty() ? L"" : BStr(a3[0]);
                            HMENU hSub = CreatePopupMenu();
                            AppendMenuW(hPopup, MF_POPUP,
                                (UINT_PTR)hSub, s.c_str());
                            // reuse the same item-building closure shape:
                            // return a table with add/sep bound to hSub
                            auto sub = std::make_shared<Table>();
                            sub->map[L"add"] = NF([fs, hSub, hwnd, this](
                                vector<Value>& a4) -> Value {
                                wstring s2 = a4.empty() ? L"" : BStr(a4[0]);
                                int nMid = fs->nNextMenuId++;
                                AppendMenuW(hSub, MF_STRING,
                                    (UINT_PTR)nMid, s2.c_str());
                                auto item2 = std::make_shared<Table>();
                                item2->map[L"text"] = NF([s2](
                                    vector<Value>&) -> Value {
                                    return Value::Str_(s2); });
                                item2->map[L"oncommand"] = Value();
                                item2->map[L"click"] = NF([hwnd, nMid](
                                    vector<Value>&) -> Value {
                                    PostMessageW(hwnd, WM_COMMAND,
                                        MAKEWPARAM(nMid, 0), 0);
                                    return Value(); });
                                fs->menuItems[nMid] = Value::Table_(item2);
                                fs->menus[nMid] = NF([fs, nMid, this](
                                    vector<Value>&) -> Value {
                                    auto ii = fs->menuItems.find(nMid);
                                    if (ii == fs->menuItems.end() ||
                                        !ii->second.tab)
                                        return Value();
                                    auto ib = ii->second.tab->map.find(
                                        L"oncommand");
                                    if (ib == ii->second.tab->map.end())
                                        return Value();
                                    vector<Value> noArgs;
                                    return this->CallVmFunc(ib->second,
                                                            noArgs); });
                                return Value::Table_(item2); });
                            sub->map[L"sep"] = NF([hSub](
                                vector<Value>&) -> Value {
                                AppendMenuW(hSub, MF_SEPARATOR, 0, nullptr);
                                return Value(); });
                            return Value::Table_(sub); });
                        return Value::Table_(popup); });
                    form->map[L"add"] = NF([form, fs, hwnd, this, hGui](
                        vector<Value>& a2) -> Value {
                    // winform.add(name={cls="button";text=..;left=..},
                    // name2={...}) — the named-argument syntax collapses all
                    // of them into ONE table whose keys are the control
                    // names and whose values are the init tables. Accept
                    // that shape, plus the legacy positional form
                    // (name, cls, text, x, y, w, h).
                    struct CtrlInit { wstring strName; Value vInit; };
                    vector<CtrlInit> vCtls;
                    if (a2.size() == 1 && a2[0].t == Value::TableT &&
                        a2[0].tab && !a2[0].tab->map.empty())
                    {
                        for (auto& kv : a2[0].tab->map)
                        {
                            if (kv.second.t != Value::TableT)
                            {
                                (*err) = L"运行错误: form.add 控件参数表无效: "
                                         + kv.first;
                                return Value();
                            }
                            vCtls.push_back({ kv.first, kv.second });
                        }
                    }
                    else if (a2.size() >= 7)
                    {
                        // legacy positional form: name, cls, text, x, y, w, h
                        // — synthesize the init table so one code path
                        // handles both shapes.
                        auto tb = std::make_shared<Table>();
                        tb->map[L"cls"] = Value::Str_(BStr(a2[1]));
                        tb->map[L"text"] = Value::Str_(BStr(a2[2]));
                        tb->map[L"left"] = Value::Num_(BNum(a2[3]));
                        tb->map[L"top"] = Value::Num_(BNum(a2[4]));
                        tb->map[L"width"] = Value::Num_(BNum(a2[5]));
                        tb->map[L"height"] = Value::Num_(BNum(a2[6]));
                        vCtls.push_back({ BStr(a2[0]), Value::Table_(tb) });
                    }
                    else
                    {
                        (*err) = L"运行错误: form.add(name={cls;text;left;top;"
                                 L"right;bottom}, ...)";
                        return Value();
                    }
                    // map iteration order is unordered; keep source order
                    // stable by sorting nothing (names are unique keys)
                    for (auto& ci : vCtls)
                    {
                        wstring strName = ci.strName;
                        auto itCls = ci.vInit.tab
                            ? ci.vInit.tab->map.find(L"cls") : ci.vInit.tab->map.end();
                        wstring strCls = itCls != ci.vInit.tab->map.end() &&
                                         itCls->second.t == Value::Str
                            ? BStr(itCls->second) : L"static";
                        auto GetI = [&](const wchar_t* k, double dDef) -> double {
                            if (!ci.vInit.tab) return dDef;
                            auto it = ci.vInit.tab->map.find(k);
                            return it != ci.vInit.tab->map.end() &&
                                   it->second.t == Value::Num
                                ? it->second.n : dDef; };
                        auto GetS = [&](const wchar_t* k) -> wstring {
                            if (!ci.vInit.tab) return L"";
                            auto it = ci.vInit.tab->map.find(k);
                            return it != ci.vInit.tab->map.end() &&
                                   it->second.t == Value::Str
                                ? BStr(it->second) : L""; };
                        // geometry: left/top/right/bottom in client
                        // coordinates; width/height are derived from them
                        double dL = GetI(L"left", 0), dT = GetI(L"top", 0);
                        double dR = GetI(L"right", 0), dB = GetI(L"bottom", 0);
                        double dW = GetI(L"width", 0), dH = GetI(L"height", 0);
                        if (dW == 0 && dR > 0) dW = dR - dL;
                        if (dH == 0 && dB > 0) dH = dB - dT;
                        const wchar_t* szCls = nullptr;
                        DWORD dwCtlStyle = 0;
                        if (strCls == L"button") szCls = L"BUTTON";
                        else if (strCls == L"edit") szCls = L"EDIT";
                        else if (strCls == L"static") szCls = L"STATIC";
                        else if (strCls == L"checkbox")
                        {
                            szCls = L"BUTTON";
                            dwCtlStyle = BS_AUTOCHECKBOX;
                        }
                        else if (strCls == L"radio")
                        {
                            szCls = L"BUTTON";
                            dwCtlStyle = BS_AUTORADIOBUTTON;
                        }
                        else if (strCls == L"groupbox")
                        {
                            szCls = L"BUTTON";
                            dwCtlStyle = BS_GROUPBOX;
                        }
                        else if (strCls == L"listbox")
                        {
                            szCls = L"LISTBOX";
                            dwCtlStyle = LBS_NOTIFY |
                                LBS_NOINTEGRALHEIGHT |
                                WS_VSCROLL | WS_BORDER;
                        }
                        else if (strCls == L"combobox")
                        {
                            szCls = L"COMBOBOX";
                            dwCtlStyle = CBS_DROPDOWNLIST |
                                WS_VSCROLL;
                        }
                        if (!szCls)
                        {
                            (*err) = L"运行错误: 未知控件类型: " + strCls;
                            return Value();
                        }
                        int nId = fs->nNextId++;
                        HWND hC = CreateWindowExW(0, szCls,
                            GetS(L"text").c_str(),
                            WS_CHILD | WS_VISIBLE | dwCtlStyle |
                                (strCls == L"button" ? BS_PUSHBUTTON :
                                 strCls == L"edit" ?
                                     ES_LEFT | WS_BORDER : 0),
                            (int)dL, (int)dT, (int)dW, (int)dH,
                            hwnd, (HMENU)(INT_PTR)nId,
                            GetModuleHandle(nullptr), nullptr);
                        if (!hC)
                        {
                            (*err) = L"运行错误: 控件创建失败";
                            return Value();
                        }
                        SendMessageW(hC, WM_SETFONT, (WPARAM)hGui, TRUE);
                        fs->hCtrl[nId] = hC;
                        fs->hCtrlRev[hC] = nId;
                        auto ctrl = std::make_shared<Table>();
                        if (strCls == L"checkbox" || strCls == L"radio")
                        {
                            ctrl->map[L"checked"] = NF([hC](vector<Value>&)
                                -> Value {
                                return Value::Bool_(
                                    SendMessageW(hC, BM_GETCHECK, 0, 0) ==
                                    BST_CHECKED); });
                            ctrl->map[L"setChecked"] =
                                NF([hC](vector<Value>& a3) -> Value {
                                SendMessageW(hC, BM_SETCHECK,
                                    a3.empty() || BNum(a3[0]) == 0
                                        ? BST_UNCHECKED
                                        : BST_CHECKED, 0);
                                return Value(); });
                        }
                        // round 41: listbox/combobox — same method surface,
                        // different message numbers (LB_* vs CB_*)
                        if (strCls == L"listbox" || strCls == L"combobox")
                        {
                            bool bCombo = strCls == L"combobox";
                            UINT mAdd = bCombo ? CB_ADDSTRING : LB_ADDSTRING;
                            UINT mCnt = bCombo ? CB_GETCOUNT : LB_GETCOUNT;
                            UINT mCur = bCombo ? CB_GETCURSEL : LB_GETCURSEL;
                            UINT mSet = bCombo ? CB_SETCURSEL : LB_SETCURSEL;
                            UINT mTxt = bCombo ? CB_GETLBTEXT : LB_GETTEXT;
                            UINT mLen = bCombo ? CB_GETLBTEXTLEN
                                               : LB_GETTEXTLEN;
                            UINT mRst = bCombo ? CB_RESETCONTENT
                                               : LB_RESETCONTENT;
                            UINT mDel = bCombo ? CB_DELETESTRING
                                               : LB_DELETESTRING;
                            auto getitem = [hC, mTxt, mLen](int i1)
                                -> wstring {
                                int n = SendMessageW(hC, mLen, i1 - 1, 0);
                                if (n < 0) return L"";
                                std::vector<wchar_t> buf((size_t)n + 1, 0);
                                SendMessageW(hC, mTxt, i1 - 1,
                                             (LPARAM)buf.data());
                                return buf.data(); };
                            ctrl->map[L"add"] = NF([hC, mAdd, mCnt](
                                vector<Value>& a3) -> Value {
                                wstring s = a3.empty() ? L"" : BStr(a3[0]);
                                SendMessageW(hC, mAdd, 0,
                                             (LPARAM)s.c_str());
                                int c = (int)SendMessageW(hC, mCnt, 0, 0);
                                return Value::Num_((double)c); });
                            ctrl->map[L"count"] = NF([hC, mCnt](
                                vector<Value>&) -> Value {
                                return Value::Num_(
                                    (double)SendMessageW(hC, mCnt, 0, 0)); });
                            ctrl->map[L"getText"] = NF([getitem](
                                vector<Value>& a3) -> Value {
                                return Value::Str_(
                                    (int)BNum(a3[0]) >= 1
                                        ? getitem((int)BNum(a3[0]))
                                        : L""); });
                            ctrl->map[L"sel"] = NF([hC, mCur](
                                vector<Value>&) -> Value {
                                int i = (int)SendMessageW(hC, mCur, 0, 0);
                                return Value::Num_((double)(i < 0 ? 0
                                                                  : i + 1)); });
                            ctrl->map[L"selText"] = NF([hC, mCur, getitem](
                                vector<Value>&) -> Value {
                                int i = (int)SendMessageW(hC, mCur, 0, 0);
                                return Value::Str_(i < 0 ? L""
                                                         : getitem(i + 1)); });
                            ctrl->map[L"setSel"] = NF([hC, mSet](
                                vector<Value>& a3) -> Value {
                                SendMessageW(hC, mSet,
                                    a3.empty() ? -1
                                               : (WPARAM)(int)BNum(a3[0]) - 1,
                                    0);
                                return Value(); });
                            ctrl->map[L"clear"] = NF([hC, mRst](
                                vector<Value>&) -> Value {
                                SendMessageW(hC, mRst, 0, 0);
                                return Value(); });
                            ctrl->map[L"del"] = NF([hC, mDel](
                                vector<Value>& a3) -> Value {
                                SendMessageW(hC, mDel,
                                    (WPARAM)(int)BNum(a3[0]) - 1, 0);
                                return Value(); });
                            // force the selection-change notification the
                            // control would send on real user interaction
                            // (LB/CB_SETCURSEL deliberately do not)
                            ctrl->map[L"notifySel"] =
                                NF([fs, nId, hwnd, hC](
                                    vector<Value>&) -> Value {
                                PostMessageW(hwnd, WM_COMMAND,
                                    MAKEWPARAM(nId, LBN_SELCHANGE),
                                    (LPARAM)hC);
                                return Value(); });
                        }
                        // every control exposes its HWND
                        ctrl->map[L"hwnd"] = Value::Num_(
                            (double)(INT_PTR)hC);
                        // geometry / visibility, same shape as the official
                        // win.ui.ctrl: setPos(left,top,right,bottom) and
                        // show(flag). The design canvas drives the components
                        // with them, so they have to exist here too.
                        ctrl->map[L"setPos"] = NF([hC](vector<Value>& a3)
                            -> Value {
                            int l = a3.size() > 0 ? (int)BNum(a3[0]) : 0;
                            int t = a3.size() > 1 ? (int)BNum(a3[1]) : 0;
                            int r = a3.size() > 2 ? (int)BNum(a3[2]) : l;
                            int b = a3.size() > 3 ? (int)BNum(a3[3]) : t;
                            SetWindowPos(hC, nullptr, l, t, r - l, b - t,
                                SWP_NOZORDER | SWP_NOACTIVATE);
                            return Value(); });
                        ctrl->map[L"show"] = NF([hC](vector<Value>& a3)
                            -> Value {
                            bool bShow = a3.empty() ? true : BNum(a3[0]) != 0;
                            ShowWindow(hC, bShow ? SW_SHOW : SW_HIDE);
                            return Value(); });
                        ctrl->map[L"text"] = NF([hC](vector<Value>&)
                            -> Value {
                            wchar_t buf[512] = L"";
                            GetWindowTextW(hC, buf, 512);
                            return Value::Str_(buf); });
                        ctrl->map[L"setText"] = NF([hC](vector<Value>& a3)
                            -> Value {
                            SetWindowTextW(hC, BStr(a3[0]).c_str());
                            return Value(); });
                        ctrl->map[L"click"] = NF([nId, hwnd, hC](
                            vector<Value>&) -> Value {
                            PostMessageW(hwnd, WM_COMMAND,
                                MAKEWPARAM(nId, BN_CLICKED),
                                (LPARAM)hC);
                            return Value(); });
                        // script-side property dispatch: .items = {...}
                        // (listbox/combobox) and .checked = v (checkbox/radio)
                        // must reach the Win32 control, not sit as plain
                        // data in the ctrl table
                        if (strCls == L"listbox" || strCls == L"combobox")
                        {
                            UINT mAddP = strCls == L"combobox"
                                ? CB_ADDSTRING : LB_ADDSTRING;
                            UINT mRstP = strCls == L"combobox"
                                ? CB_RESETCONTENT : LB_RESETCONTENT;
                            UINT mCntP = strCls == L"combobox"
                                ? CB_GETCOUNT : LB_GETCOUNT;
                            // table-form meta._set: only named attrs
                            // (items / checked) are intercepted; any other
                            // write (onselchange, oncommand, ...) falls
                            // through to a plain data member
                            auto tbMeta = std::make_shared<Table>();
                            auto tbSet = std::make_shared<Table>();
                            tbSet->map[L"items"] = NF([hC, mAddP, mRstP](
                                vector<Value>& a3) -> Value {
                                if (a3.size() > 0 && a3[0].t == Value::TableT &&
                                    a3[0].tab)
                                {
                                    SendMessageW(hC, mRstP, 0, 0);
                                    for (auto& x : a3[0].tab->arr)
                                        SendMessageW(hC, mAddP, 0,
                                                     (LPARAM)BStr(x).c_str());
                                }
                                return Value(); });
                            tbMeta->map[L"_set"] = Value::Table_(tbSet);
                            ctrl->map[L"_meta"] = Value::Table_(tbMeta);
                        }
                        else if (strCls == L"checkbox" || strCls == L"radio")
                        {
                            auto tbMeta = std::make_shared<Table>();
                            auto tbSet = std::make_shared<Table>();
                            tbSet->map[L"checked"] = NF([hC](
                                vector<Value>& a3) -> Value {
                                SendMessageW(hC, BM_SETCHECK,
                                    (!a3.empty() && a3[0].t != Value::Null &&
                                     (a3[0].t != Value::Num ||
                                      (int)BNum(a3[0]) != 0))
                                        ? BST_CHECKED
                                        : BST_UNCHECKED, 0);
                                return Value(); });
                            tbMeta->map[L"_set"] = Value::Table_(tbSet);
                            ctrl->map[L"_meta"] = Value::Table_(tbMeta);
                        }
                        // apply remaining DSG init attributes the way
                        // official win.ui.ctrl classes do in their ctor:
                        // items array for listbox/combobox, checked for
                        // checkbox/radio, password char for edit,
                        // disabled/hide visibility
                        if (ci.vInit.tab)
                        {
                            auto itI = ci.vInit.tab->map.find(L"items");
                            if (itI != ci.vInit.tab->map.end() &&
                                itI->second.t == Value::TableT &&
                                itI->second.tab &&
                                (strCls == L"listbox" ||
                                 strCls == L"combobox"))
                            {
                                UINT mAddD = strCls == L"combobox"
                                    ? CB_ADDSTRING : LB_ADDSTRING;
                                for (auto& x : itI->second.tab->arr)
                                    SendMessageW(hC, mAddD, 0,
                                                 (LPARAM)BStr(x).c_str());
                            }
                            auto itC = ci.vInit.tab->map.find(L"checked");
                            if (itC != ci.vInit.tab->map.end() &&
                                (strCls == L"checkbox" || strCls == L"radio"))
                                SendMessageW(hC, BM_SETCHECK,
                                    (itC->second.t != Value::Null &&
                                     (itC->second.t != Value::Num ||
                                      (int)BNum(itC->second) != 0))
                                        ? BST_CHECKED : BST_UNCHECKED, 0);
                            auto itP = ci.vInit.tab->map.find(L"password");
                            if (itP != ci.vInit.tab->map.end() &&
                                strCls == L"edit" &&
                                (itP->second.t != Value::Null &&
                                 (itP->second.t != Value::Num ||
                                  (int)BNum(itP->second) != 0)))
                                SendMessageW(hC, EM_SETPASSWORDCHAR,
                                             (WPARAM)'*', 0);
                            auto itD = ci.vInit.tab->map.find(L"disabled");
                            if (itD != ci.vInit.tab->map.end() &&
                                itD->second.t != Value::Null &&
                                (itD->second.t != Value::Num ||
                                 (int)BNum(itD->second) != 0))
                                EnableWindow(hC, FALSE);
                            auto itH = ci.vInit.tab->map.find(L"hide");
                            if (itH != ci.vInit.tab->map.end() &&
                                itH->second.t != Value::Null &&
                                (itH->second.t != Value::Num ||
                                 (int)BNum(itH->second) != 0))
                                ShowWindow(hC, SW_HIDE);
                        }
                        // callback slot: assign a function to .oncommand
                        ctrl->map[L"oncommand"] = Value();   // set by script
                        // the dispatch reads ctrl->map["oncommand"] live
                        fs->ctrls[nId] = Value::Table_(ctrl);
                        fs->callbacks[nId] = Value();       // filled lazily
                        // hook: keep the id in the value so dispatch can
                        // resolve; store id on the control table
                        ctrl->map[L"_id"] = Value::Num_((double)nId);
                        ctrl->map[L"id"] = Value::Num_((double)nId);
                        fs->callbacks[nId] = NF([fs, nId, this](
                            vector<Value>&) -> Value {
                            auto ic = fs->ctrls.find(nId);
                            if (ic == fs->ctrls.end() || !ic->second.tab)
                                return Value();
                            auto ib = ic->second.tab->map.find(L"oncommand");
                            if (ib == ic->second.tab->map.end())
                                return Value();
                            vector<Value> noArgs;
                            return this->CallVmFunc(ib->second, noArgs); });
                        fs->changes[nId] = NF([fs, nId, this](
                            vector<Value>&) -> Value {
                            auto ic = fs->ctrls.find(nId);
                            if (ic == fs->ctrls.end() || !ic->second.tab)
                                return Value();
                            auto ib = ic->second.tab->map.find(L"onchange");
                            if (ib == ic->second.tab->map.end())
                                return Value();
                            vector<Value> noArgs;
                            return this->CallVmFunc(ib->second, noArgs); });
                        fs->selchgs[nId] = NF([fs, nId, this](
                            vector<Value>&) -> Value {
                            auto ic = fs->ctrls.find(nId);
                            if (ic == fs->ctrls.end() || !ic->second.tab)
                                return Value();
                            auto ib = ic->second.tab->map.find(L"onselchange");
                            if (ib == ic->second.tab->map.end())
                                return Value();
                            vector<Value> noArgs;
                            return this->CallVmFunc(ib->second, noArgs); });
                        form->map[strName] = Value::Table_(ctrl);
                    }
                    // returns the LAST added control
                    Value vLast;
                    if (!vCtls.empty() && form->map.size())
                    {
                        auto itL = form->map.find(vCtls.back().strName);
                        if (itL != form->map.end()) vLast = itL->second;
                    }
                    return vLast; });
                    return Value::Table_(form); });
            }

            // first-class scalar builtins (also reachable via fast paths)
            globals.VarAt(NameId(L"tostring")) = NF([](vector<Value>& a) {
                if (a.empty()) return Value::Str_(L"null");
                Interp* it = g_interpCurrent;
                if (it && a[0].t == Value::TableT && a[0].tab)
                {
                    auto im = a[0].tab->map.find(L"_meta");
                    if (im != a[0].tab->map.end() && im->second.t == Value::TableT && im->second.tab)
                    {
                        auto itFn = im->second.tab->map.find(L"_tostring");
                        if (itFn != im->second.tab->map.end() && (itFn->second.t == Value::Func || itFn->second.native))
                        {
                            vector<Value> extraArgs(a.begin() + 1, a.end());
                            return it->CallOwner(itFn->second, a[0], extraArgs);
                        }
                    }
                }
                // Official aardio strings are byte arrays, so tostring() of a
                // string returns that same string. Returning a[0] unchanged
                // keeps the byte representation (a raw byte string must not
                // be re-interpreted as wide text).
                if (a[0].t == Value::Str) return a[0];
                return Value::Str_(a[0].ToString()); });
            Value vType = NF([](vector<Value>& a) -> Value {
                if (a.empty()) return Value::Str_(L"null");
                switch (a[0].t)
                {
                case Value::Null: return Value::Str_(L"null");
                case Value::Bool: return Value::Str_(L"boolean");
                case Value::Num: return Value::Str_(L"number");
                case Value::Pointer:
                    if (a[0].tab && a[0].tab->map.count(L"_meta"))
                        return Value::Str_(L"cdata");
                    if (a[0].ptr && IsBuffer(a[0].ptr))
                        return Value::Str_(L"buffer");
                    return Value::Str_(L"pointer");
                case Value::Str: return Value::Str_(L"string");
                case Value::TableT: return Value::Str_(L"table");
                case Value::Class: return Value::Str_(L"class");
                case Value::Func:
                    return Value::Str_(a[0].native ? L"native function"
                                                   : L"function");
                }
                return Value::Str_(L"null"); });
            vType.tab = std::make_shared<Table>();
            vType.tab->map[L"null"] = Value::Str_(L"null");
            vType.tab->map[L"boolean"] = Value::Str_(L"boolean");
            vType.tab->map[L"number"] = Value::Str_(L"number");
            vType.tab->map[L"string"] = Value::Str_(L"string");
            vType.tab->map[L"table"] = Value::Str_(L"table");
            vType.tab->map[L"function"] = Value::Str_(L"function");
            vType.tab->map[L"cdata"] = Value::Str_(L"cdata");
            vType.tab->map[L"pointer"] = Value::Str_(L"pointer");
            vType.tab->map[L"buffer"] = Value::Str_(L"buffer");
            vType.tab->map[L"fiber"] = Value::Str_(L"fiber");
            // official type constants (kernel): used across the lib, e.g.
            // com.interface checkInterface `type(x) != type.class`
            vType.tab->map[L"class"] = Value::Str_(L"class");
            vType.tab->map[L"callable"] = Value::Str_(L"callable");
            vType.tab->map[L"int"] = Value::Str_(L"number");
            vType.tab->map[L"uint"] = Value::Str_(L"number");
            vType.tab->map[L"long"] = Value::Str_(L"number");
            vType.tab->map[L"ulong"] = Value::Str_(L"number");
            vType.tab->map[L"float"] = Value::Str_(L"number");
            vType.tab->map[L"double"] = Value::Str_(L"number");
            vType.tab->map[L"isString"] = NF([](vector<Value>& a) -> Value {
                return Value::Bool_(!a.empty() && a[0].t == Value::Str);
            });
            vType.tab->map[L"isNumber"] = NF([](vector<Value>& a) -> Value {
                return Value::Bool_(!a.empty() && a[0].t == Value::Num);
            });
            vType.tab->map[L"isTable"] = NF([](vector<Value>& a) -> Value {
                return Value::Bool_(!a.empty() && a[0].t == Value::TableT);
            });
            vType.tab->map[L"isFunction"] = NF([](vector<Value>& a) -> Value {
                return Value::Bool_(!a.empty() && (a[0].t == Value::Func || a[0].native != nullptr));
            });
            vType.tab->map[L"isPointer"] = NF([](vector<Value>& a) -> Value {
                return Value::Bool_(!a.empty() && a[0].t == Value::Pointer);
            });
            vType.tab->map[L"isClass"] = NF([](vector<Value>& a) -> Value {
                return Value::Bool_(!a.empty() && a[0].t == Value::Class);
            });
            // type.rget(v) — gets custom type defined by _type property / metatable
            vType.tab->map[L"rget"] = NF([](vector<Value>& a) -> Value {
                if (a.empty()) return Value();
                const Value& v = a[0];
                if ((v.t == Value::TableT || v.t == Value::Class || v.t == Value::Pointer) && v.tab)
                {
                    auto it = v.tab->map.find(L"_type");
                    if (it != v.tab->map.end() && it->second.t == Value::Str)
                        return it->second;
                    auto im = v.tab->map.find(L"_meta");
                    if (im != v.tab->map.end() && im->second.t == Value::TableT && im->second.tab)
                    {
                        auto it2 = im->second.tab->map.find(L"_type");
                        if (it2 != im->second.tab->map.end() && it2->second.t == Value::Str)
                            return it2->second;
                    }
                }
                return Value();
            });
            globals.VarAt(NameId(L"type")) = vType;
            roots[L"type"] = vType;
            globals.VarAt(NameId(L"topointer")) = NF([this](vector<Value>& a) -> Value {
                if (a.empty() || a[0].t == Value::Null) return Value::Pointer_(nullptr);
                INT_PTR offset = a.size() > 1 ? (INT_PTR)(long long)BNum(a[1]) : 0;
                if (a[0].t == Value::Pointer) return Value::Pointer_((char*)a[0].ptr + offset);
                if (a[0].t == Value::TableT || a[0].t == Value::Class)
                {
                    Value vp = GetMember(a[0], L"_topointer");
                    if (vp.t == Value::Func || vp.native)
                    {
                        vector<Value> noArgs;
                        Value r = CallOwner(vp, a[0], noArgs);
                        if (r.t == Value::Pointer) return Value::Pointer_((char*)r.ptr + offset);
                        return Value::Pointer_((char*)(INT_PTR)(long long)BNum(r) + offset);
                    }
                }
                if (a[0].ptr != nullptr) return Value::Pointer_((char*)a[0].ptr + offset);
                if (a[0].t == Value::Num) return Value::Pointer_((char*)(INT_PTR)(long long)a[0].n + offset);
                if (a[0].t == Value::Str) return Value::Pointer_((char*)a[0].s.c_str() + offset);
                return Value::Pointer_((char*)(INT_PTR)(long long)BNum(a[0]) + offset); });
            auto fnToNumber = NF([this](vector<Value>& a) -> Value {
                if (a.empty()) return Value();
                if (a[0].t == Value::Null) return Value();
                if (a[0].t == Value::Num) return a[0];
                if (a[0].t == Value::Pointer) return Value::Num_((double)(INT_PTR)a[0].ptr);
                if (a[0].t == Value::Bool) return Value::Num_(a[0].b ? 1.0 : 0.0);
                if (a[0].t == Value::TableT && a[0].tab)
                {
                    auto im = a[0].tab->map.find(L"_meta");
                    if (im != a[0].tab->map.end() && im->second.t == Value::TableT && im->second.tab)
                    {
                        auto it = im->second.tab->map.find(L"_tonumber");
                        if (it != im->second.tab->map.end() && (it->second.t == Value::Func || it->second.native))
                        {
                            vector<Value> noArgs;
                            Value r = this->CallOwner(it->second, a[0], noArgs);
                            if (r.t == Value::Num) return r;
                        }
                    }
                }
                if (a[0].t == Value::Str)
                {
                    std::string rawBytes = RawBytesOf(BStr(a[0]));
                    if (rawBytes.empty()) return Value();

                    int radix = (a.size() > 1 && a[1].t == Value::Num) ? (int)BNum(a[1]) : 0;
                    int pos = (a.size() > 2 && a[2].t == Value::Num) ? (int)BNum(a[2]) : 1;
                    char decimal = (a.size() > 3 && a[3].t == Value::Num) ? (char)(int)BNum(a[3]) : 0;

                    if (pos < 1 || (size_t)(pos - 1) >= rawBytes.size())
                        return Value();

                    const char* startPtr = rawBytes.data() + (pos - 1);
                    const char* p = startPtr;

                    if (pos == 1)
                    {
                        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
                            p++;
                    }

                    if (!*p) return Value();

                    char* endPtr = nullptr;
                    double numVal = 0.0;
                    bool success = false;

                    if (radix == 0)
                    {
                        if ((p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) ||
                            ((p[0] == '+' || p[0] == '-') && p[1] == '0' && (p[2] == 'x' || p[2] == 'X')))
                        {
                            radix = 16;
                        }
                        else if ((p[0] == '0' && (p[1] == 'b' || p[1] == 'B')) ||
                                 ((p[0] == '+' || p[0] == '-') && p[1] == '0' && (p[2] == 'b' || p[2] == 'B')))
                        {
                            radix = 2;
                        }
                    }

                    if (radix == 16)
                    {
                        bool neg = (*p == '-');
                        if (*p == '+' || *p == '-') p++;
                        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
                        unsigned __int64 u = _strtoui64(p, &endPtr, 16);
                        if (endPtr > p)
                        {
                            numVal = neg ? -(double)(__int64)u : (double)(__int64)u;
                            success = true;
                        }
                    }
                    else if (radix == 2)
                    {
                        bool neg = (*p == '-');
                        if (*p == '+' || *p == '-') p++;
                        if (p[0] == '0' && (p[1] == 'b' || p[1] == 'B')) p += 2;
                        unsigned __int64 u = _strtoui64(p, &endPtr, 2);
                        if (endPtr > p)
                        {
                            numVal = neg ? -(double)(__int64)u : (double)(__int64)u;
                            success = true;
                        }
                    }
                    else if (radix > 0 && radix != 10)
                    {
                        bool neg = (*p == '-');
                        if (*p == '+' || *p == '-') p++;
                        unsigned __int64 u = _strtoui64(p, &endPtr, radix);
                        if (endPtr > p)
                        {
                            numVal = neg ? -(double)(__int64)u : (double)(__int64)u;
                            success = true;
                        }
                    }
                    else
                    {
                        std::string tempStr;
                        const char* parseStr = p;
                        if (decimal != 0 && decimal != '.')
                        {
                            tempStr = p;
                            for (char& ch : tempStr) {
                                if (ch == decimal) ch = '.';
                            }
                            parseStr = tempStr.c_str();
                        }
                        numVal = strtod(parseStr, &endPtr);
                        if (endPtr > parseStr)
                        {
                            success = true;
                            size_t consumed = (size_t)(endPtr - parseStr);
                            endPtr = (char*)p + consumed;
                        }
                    }

                    if (success && endPtr > startPtr)
                    {
                        size_t totalConsumed = (size_t)(endPtr - startPtr);
                        Value ret = Value::Num_(numVal);
                        ret.mrets = std::make_shared<vector<Value>>();
                        ret.mrets->push_back(Value::Num_((double)totalConsumed));
                        return ret;
                    }
                    return Value();
                }
                return Value();
            });
            globals.VarAt(NameId(L"tonumber")) = fnToNumber;
            roots[L"tonumber"] = fnToNumber;
            globals.VarAt(NameId(L"sleep")) = NF([](vector<Value>& a) {
                if (!a.empty()) Sleep((DWORD)BNum(a[0]));
                return Value(); });
            globals.VarAt(NameId(L"_HINSTANCE")) = Value::Pointer_(GetModuleHandleW(nullptr));
            globals.VarAt(NameId(L"_HAPPICON")) = Value::Pointer_(LoadIconW(nullptr, IDI_APPLICATION));
            globals.VarAt(NameId(L"_INVALID_HANDLE_VALUE")) = Value::Pointer_(INVALID_HANDLE_VALUE);
            roots[L"_HINSTANCE"] = globals.VarAt(NameId(L"_HINSTANCE"));
            roots[L"_HAPPICON"] = globals.VarAt(NameId(L"_HAPPICON"));
            roots[L"_INVALID_HANDLE_VALUE"] = globals.VarAt(NameId(L"_INVALID_HANDLE_VALUE"));
            {
                BOOL isWow64 = FALSE;
                IsWow64Process(GetCurrentProcess(), &isWow64);
                bool bWin64 = (sizeof(void*) == 8) || (isWow64 != FALSE);
                globals.VarAt(NameId(L"_WIN_64")) = Value::Bool_(bWin64);
                roots[L"_WIN_64"] = Value::Bool_(bWin64);

                typedef void (WINAPI *RtlGetNtVersionNumbersFn)(DWORD*, DWORD*, DWORD*);
                HMODULE hNt = GetModuleHandleW(L"ntdll.dll");
                if (hNt)
                {
                    auto fnRtl = (RtlGetNtVersionNumbersFn)GetProcAddress(hNt, "RtlGetNtVersionNumbers");
                    if (fnRtl)
                    {
                        DWORD dwMajor = 0, dwMinor = 0, dwBuild = 0;
                        fnRtl(&dwMajor, &dwMinor, &dwBuild);
                        dwBuild &= 0xFFFF;
                        globals.VarAt(NameId(L"_WIN_VER_MAJOR")) = Value::Num_(dwMajor);
                        globals.VarAt(NameId(L"_WIN_VER_MINOR")) = Value::Num_(dwMinor);
                        globals.VarAt(NameId(L"_WIN_VER_BUILD")) = Value::Num_(dwBuild);
                        roots[L"_WIN_VER_MAJOR"] = globals.VarAt(NameId(L"_WIN_VER_MAJOR"));
                        roots[L"_WIN_VER_MINOR"] = globals.VarAt(NameId(L"_WIN_VER_MINOR"));
                        roots[L"_WIN_VER_BUILD"] = globals.VarAt(NameId(L"_WIN_VER_BUILD"));

                        bool bWinXP = (dwMajor < 6);
                        bool bWin7Later = (dwMajor > 6 || (dwMajor == 6 && dwMinor >= 1));
                        bool bWin10Later = (dwMajor >= 10);
                        globals.VarAt(NameId(L"_WINXP")) = Value::Bool_(bWinXP);
                        globals.VarAt(NameId(L"_WIN7_LATER")) = Value::Bool_(bWin7Later);
                        globals.VarAt(NameId(L"_WIN10_LATER")) = Value::Bool_(bWin10Later);
                        roots[L"_WINXP"] = Value::Bool_(bWinXP);
                        roots[L"_WIN7_LATER"] = Value::Bool_(bWin7Later);
                        roots[L"_WIN10_LATER"] = Value::Bool_(bWin10Later);
                    }
                }
            }
            {
                auto fnGcData = NF([this](vector<Value>& a) -> Value {
                    void* p = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 1);
                    RegisterBuffer(p, 1);
                    Value buf = Value::Pointer_(p);
                    buf.tab = std::make_shared<Table>();
                    if (!a.empty())
                        buf.tab->map[L"_meta"] = a[0];
                    return buf;
                });
                globals.VarAt(NameId(L"gcdata")) = fnGcData;
                roots[L"gcdata"] = fnGcData;
            }

            // ---- global namespace + VM introspection (Phase 3) ----
            // debug module opens with global.import("(debug)"), so `global`
            // must resolve to a table carrying a runtime import entry.
            {
                Value fnGlobalImport = NF([this](vector<Value>& a) -> Value {
                    if (a.empty() || a[0].t != Value::Str)
                        return Value();
                    PNode n = NewPNode();
                    n->k = Node::Import;
                    wstring full = BStr(a[0]);
                    if (full.find(L'\\') != wstring::npos ||
                        full.find(L'/') != wstring::npos ||
                        full.find(L':') != wstring::npos)
                    {
                        // path-style import: global.import("D:\\dir\\mod.aardio")
                        // — the whole string is one name, no dot splitting
                        n->names.push_back(full);
                    }
                    else
                    {
                        size_t start = 0;
                        while (start < full.size())
                        {
                            size_t pos = full.find(L'.', start);
                            if (pos == wstring::npos)
                            {
                                n->names.push_back(full.substr(start));
                                break;
                            }
                            n->names.push_back(full.substr(start, pos - start));
                            start = pos + 1;
                        }
                    }
                    if (n->names.empty()) n->names.push_back(full);
                    return this->DoImport(n, &globals);
                });
                auto gtab = std::make_shared<Table>();
                gtab->map[L"import"] = fnGlobalImport;
                // In runtime `global` is not a namespace of its own: global.x
                // reads/writes the ROOT (::) namespace.
                auto gmeta = std::make_shared<Table>();
                gmeta->map[L"_get"] = NF([this](vector<Value>& a) -> Value {
                    if (a.empty() || a[0].t != Value::Str)
                        return Value();
                    const wstring k = a[0].s;
                    auto it = roots.find(k);
                    if (it != roots.end())
                        return it->second;
                    if (Value* pvG = globals.FindInSelf(NameId(k)))
                        return *pvG;
                    {
                        std::lock_guard<std::recursive_mutex> lk(g_threadSharedMutex);
                        auto ish = g_threadSharedMap.find(k);
                        if (ish != g_threadSharedMap.end())
                            return ish->second;
                    }
                    return Value();
                });
                gmeta->map[L"_set"] = NF([this](vector<Value>& a) -> Value {
                    if (a.size() < 2 || a[0].t != Value::Str)
                        return Value();
                    globals.VarAt(NameId(a[0].s)) = a[1];
                    roots[a[0].s] = a[1];
                    {
                        std::lock_guard<std::recursive_mutex> lk(g_threadSharedMutex);
                        g_threadSharedMap[a[0].s] = a[1];
                    }
                    return Value();
                });
                gtab->map[L"_meta"] = Value::Table_(gmeta);
                gtab->map[L"self"] = Value::Table_(gtab);
                roots[L"global"] = Value::Table_(gtab);
                roots[L"self"] = Value::Table_(gtab);
                globals.VarAt(NameId(L"global")) = Value::Table_(gtab);
                globals.VarAt(IdSelf()) = Value::Table_(gtab);

                // ---- the (debug) introspection module ----
                auto dbg = std::make_shared<Table>();
                RegisterComKernel(globals);   // com.cpp: kernel COM host
                // debug.queryinfo(level,[what]) — test runner and
                // debug tools read source.src / currentline from it
                dbg->map[L"queryinfo"] = NF([](vector<Value>& a) -> Value {
                    wstring what = a.size() > 1 ? BStr(a[1]) : L"";
                    auto tb = std::make_shared<Table>();
                    int lvl = a.empty() ? 1 : (int)BNum(a[0]);
                    auto src = std::make_shared<Table>();
                    src->map[L"src"] = Value::Str_(L"");
                    src->map[L"currentline"] = Value::Num_((double)lvl);
                    tb->map[L"source"] = Value::Table_(src);
                    tb->map[L"currentline"] = Value::Num_((double)lvl);
                    tb->map[L"name"] = Value::Str_(L"");
                    tb->map[L"function"] = Value();
                    tb->map[L"upvars"] = Value::Table_(std::make_shared<Table>());
                    tb->map[L"activelines"] =
                        Value::Table_(std::make_shared<Table>());
                    (void)what;
                    return Value::Table_(tb); });
                // debug.traceback([msg],[title],[level]) — console.error uses
                // it to append a stack trace
                dbg->map[L"traceback"] = NF([](vector<Value>& a) -> Value {
                    wstring msg = a.empty() ? L"" : BStr(a[0]);
                    return Value::Str_(msg); });
                dbg->map[L"callcount"] = NF([](vector<Value>&) -> Value {
                    return Value::Num_(0.0); });
                dbg->map[L"hook"] = NF([](vector<Value>&) -> Value {
                    return Value::Table_(std::make_shared<Table>()); });
                dbg->map[L"open"] = NF([](vector<Value>&) -> Value {
                    return Value(); });
                dbg->map[L"close"] = NF([](vector<Value>&) -> Value {
                    return Value(); });
                namedModules[L"(debug)"] = Value::Table_(dbg);
            }

            // ---- fibers (纤程): create / resume / yield / status ----
            globals.VarAt(NameId(L"create")) = NF([](vector<Value>& a) -> Value {
                return a.empty() ? Value() : FiberCreate(a[0]); });
            globals.VarAt(NameId(L"resume")) = NF([](vector<Value>& a) -> Value {
                if (a.empty())
                    return Value();
                vector<Value> args;
                for (size_t i = 1; i < a.size(); i++)
                    args.push_back(a[i]);
                return FiberResume(a[0], args, nullptr); });
            globals.VarAt(NameId(L"yield")) = NF([](vector<Value>& a) -> Value {
                vector<Value> args;
                for (auto& v : a) args.push_back(v);
                return FiberYield(args); });
            {
                auto tbF = GetRoot(L"fiber").tab;
                tbF->map[L"create"] = NF([](vector<Value>& a) -> Value {
                    return a.empty() ? Value() : FiberCreate(a[0]); });
                tbF->map[L"resume"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty())
                        return Value();
                    vector<Value> args;
                    for (size_t i = 1; i < a.size(); i++)
                        args.push_back(a[i]);
                    return FiberResume(a[0], args, nullptr); });
                tbF->map[L"yield"] = NF([](vector<Value>& a) -> Value {
                    vector<Value> args;
                    for (auto& v : a) args.push_back(v);
                    return FiberYield(args); });
                // fiber.status(fib) -> "suspended" | "running" | "dead"
                tbF->map[L"status"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty())
                        return Value();
                    VMFiber* co = FiberOf(a[0]);
                    if (!co)
                        return Value();
                    return Value::Str_(co->status); });
                // fiber.generator(fn,...) — iterator factory over a fiber
                tbF->map[L"generator"] = NF([](vector<Value>& a) -> Value {
                    if (a.empty())
                        return Value();
                    Value v = a[0];
                    VMFiber* co = FiberOf(v);
                    Value fibV = v;
                    if (!co)
                    {
                        fibV = FiberCreate(v);
                        co = FiberOf(fibV);
                    }
                    if (!co)
                        return Value();
                    vector<Value> args;
                    for (size_t i = 1; i < a.size(); i++)
                        args.push_back(a[i]);
                    auto pFib = std::make_shared<Value>(fibV);
                    auto pArgs = std::make_shared<vector<Value>>(args);
                    // iterator: null once the fiber is dead. Strip the ok
                    // flag from resume's (true, yielded...) — for-in binds
                    // the first loop variable to the first YIELDED value
                    // (official fiber.generator semantics)
                    return NF([pFib, pArgs](vector<Value>&) -> Value {
                        VMFiber* c2 = FiberOf(*pFib);
                        if (!c2 || c2->status == L"dead")
                            return Value();
                        Value r = FiberResume(*pFib, *pArgs, nullptr);
                        if (!r.mrets || r.mrets->empty())
                            return Value();
                        Value first = (*r.mrets)[0];
                        if (first.t == Value::Null && r.mrets->size() == 1)
                            return Value();   // fiber finished, nothing yielded
                        auto rest = std::make_shared<vector<Value>>();
                        for (size_t i = 1; i < r.mrets->size(); i++)
                            rest->push_back((*r.mrets)[i]);
                        if (!rest->empty())
                            first.mrets = rest;
                        return first; });
                });
            }

            // ---- documented reserved builtins (docs/language-reference/
            //      builtin-function/index.md). Their absence was the single
            //      largest cause of "尝试调用非函数值" in the shipped examples.
            auto MakeVmFunc = [](PNode fn) -> Value {
                Value f;
                f.t = Value::Func;
                f.fnData = std::make_shared<FuncData>();
                f.fnData->fn = fn->a;
                f.fnData->params = fn->names;
                f.fnData->pdefaults = std::make_shared<vector<PNode>>(fn->defaults);
                return f;
            };
            // print(...) — each argument through tostring, then io.print
            globals.VarAt(NameId(L"print")) = NF([](vector<Value>& a) -> Value {
                wstring out;
                for (size_t i = 0; i < a.size(); i++)
                {
                    if (i) out += L"\t";
                    out += a[i].t == Value::Null ? L"null" : a[i].ToString();
                }
                out += L"\n";
                int n = WideCharToMultiByte(CP_UTF8, 0, out.c_str(),
                                            (int)out.size(), nullptr, 0,
                                            nullptr, nullptr);
                if (n > 0)
                {
                    std::string u8((size_t)n, 0);
                    WideCharToMultiByte(CP_UTF8, 0, out.c_str(),
                                        (int)out.size(), &u8[0], n,
                                        nullptr, nullptr);
                    fwrite(u8.data(), 1, u8.size(), stdout);
                    fflush(stdout);
                }
                return Value(); });
            // assert(value,message,...) — truthy: return all arguments;
            // falsy: throw @message
            globals.VarAt(NameId(L"assert")) = NF([this](vector<Value>& a) -> Value {
                if (!a.empty() && a[0].Truthy())
                {
                    if (a.size() > 1)
                    {
                        auto ex = std::make_shared<vector<Value>>();
                        for (size_t i = 1; i < a.size(); i++)
                            ex->push_back(a[i]);
                        Value r = a[0];
                        r.mrets = ex;
                        return r;
                    }
                    return a.empty() ? Value() : a[0];
                }
                wstring msg = a.size() > 1 ? BStr(a[1])
                                          : L"assertion failed!";
                (*err) = L"运行错误: " + msg;
                return Value(); });
            // assert2(level,value,...) — asserts the value after the error
            // level; on success returns every argument AFTER it, so
            // `assert2(2, resume(fib))` yields the resumed values (the
            // fiber.generator iterator relies on this to
            // hand for-in the yielded value, not the ok flag)
            globals.VarAt(NameId(L"assert2")) = NF([this](vector<Value>& a) -> Value {
                if (a.size() > 1 && a[1].Truthy())
                {
                    if (a.size() > 2)
                    {
                        auto ex = std::make_shared<vector<Value>>();
                        for (size_t i = 3; i < a.size(); i++)
                            ex->push_back(a[i]);
                        Value r = a[2];
                        if (!ex->empty())
                            r.mrets = ex;
                        return r;
                    }
                    return Value();   // only the ok flag: nothing to yield
                }
                wstring msg = a.size() > 2 ? BStr(a[2])
                                          : L"assertion failed!";
                (*err) = L"运行错误: " + msg;
                return Value(); });
            // assertf — inverse assertion: throws when the value is truthy
            globals.VarAt(NameId(L"assertf")) = NF([this](vector<Value>& a) -> Value {
                if (!a.empty() && a[0].Truthy())
                {
                    wstring msg = a.size() > 1 ? BStr(a[1])
                                              : L"assertion failed!";
                    (*err) = L"运行错误: " + msg;
                    return Value();
                }
                return a.empty() ? Value() : a[0]; });
            // eval(codeString) — compiles and runs an expression immediately
            globals.VarAt(NameId(L"eval")) = NF([this, MakeVmFunc](vector<Value>& a) -> Value {
                if (a.empty() || a[0].t != Value::Str)
                    return Value();
                wstring src = BStr(a[0]);
                // If src is a binary-serialized table ({ \0... <len>:<key>=<val>; ... })
                if (src.size() >= 2 && src.find(L'{') != wstring::npos && src.find(L'\0') != wstring::npos)
                {
                    size_t p = src.find(L'{');
                    if (p != wstring::npos)
                    {
                        p++;
                        auto tb = std::make_shared<Table>();
                        bool bBinaryTable = false;
                        while (p < src.size())
                        {
                            while (p < src.size() && ((unsigned short)src[p] <= 32 || src[p] == L';' || src[p] == L','))
                                p++;
                            if (p >= src.size() || src[p] == L'}')
                            {
                                if (p < src.size() && src[p] == L'}') bBinaryTable = true;
                                break;
                            }
                            size_t dStart = p;
                            while (p < src.size() && iswdigit(src[p])) p++;
                            if (dStart == p || p >= src.size() || src[p] != L':')
                                break;
                            int klen = _wtoi(src.substr(dStart, p - dStart).c_str());
                            p++; // skip ':'
                            if (p + (size_t)klen > src.size())
                                break;
                            // key bytes (in Nodio byte-string convention: (unsigned char)src[i])
                            std::string u8key;
                            u8key.resize(klen);
                            for (int i = 0; i < klen; i++)
                                u8key[i] = (char)(unsigned char)src[p + i];
                            p += klen;
                            wstring wkey;
                            int reqW = MultiByteToWideChar(CP_UTF8, 0, u8key.data(), (int)u8key.size(), nullptr, 0);
                            if (reqW > 0)
                            {
                                wkey.resize(reqW);
                                MultiByteToWideChar(CP_UTF8, 0, u8key.data(), (int)u8key.size(), &wkey[0], reqW);
                            }
                            else
                            {
                                for (char ch : u8key) wkey += (wchar_t)(unsigned char)ch;
                            }
                            if (p < src.size() && src[p] == L'=')
                                p++;
                            size_t vStart = p;
                            while (p < src.size() && (iswdigit(src[p]) || src[p] == L'.' || src[p] == L'-'))
                                p++;
                            double numVal = 0;
                            if (vStart < p)
                                numVal = _wtof(src.substr(vStart, p - vStart).c_str());
                            tb->map[wkey] = Value::Num_(numVal);
                            bBinaryTable = true;
                        }
                        if (bBinaryTable)
                            return Value::Table_(tb);
                    }
                }
                wstring e;
                PNode fn = CompileToFunc(L"return " + src, nullptr, &e);
                if (!fn)
                {
                    e.clear();
                    fn = CompileToFunc(src, nullptr, &e);
                    if (!fn)
                    {
                        (*err) = L"运行错误: eval: " + e;
                        return Value();
                    }
                }
                Value f = MakeVmFunc(fn);
                vector<Value> args;
                Value r = this->CallVmFunc(f, args);
                if (!err->empty())
                    return Value();
                return r; });
            // loadcode(code|filepath) — a function object, or null + message
            globals.VarAt(NameId(L"loadcode")) = NF([this, MakeVmFunc](vector<Value>& a) -> Value {
                if (a.empty() || a[0].t != Value::Str)
                    return Value();
                wstring src = BStr(a[0]), text;
                wstring fileDir;
                if (ReadCodeFile(src, &text))
                {
                    src = text;
                    wstring p = BStr(a[0]);
                    if (p[0] == L'/' || p[0] == L'\\')
                    {
                        wstring root;
                        if (g_interpCurrent && !g_interpCurrent->baseDir.empty())
                            root = g_interpCurrent->baseDir;
                        if (root.empty())
                        {
                            wchar_t szExe[MAX_PATH] = L"";
                            GetModuleFileNameW(nullptr, szExe, MAX_PATH);
                            wchar_t* q = wcsrchr(szExe, L'\\');
                            if (q) *q = 0;
                            root = szExe;
                        }
                        p = root + L"\\" + p.substr(1);
                    }
                    else if (p[0] == L'~')
                        p = ResolveTildeDisk(p);
                    size_t slash = p.find_last_of(L"\\/");
                    if (slash != wstring::npos)
                        fileDir = p.substr(0, slash);
                }
                wstring e;
                PNode fn = CompileToFunc(src, fileDir.empty() ? nullptr : &fileDir, &e);
                if (!fn)
                {
                    // null + error message, exactly as the docs specify
                    Value nullV;
                    auto ex = std::make_shared<vector<Value>>();
                    ex->push_back(Value::Str_(L"编译失败: " + e));
                    nullV.mrets = ex;
                    return nullV;
                }
                return MakeVmFunc(fn); });
            // loadcodex(code|filepath|function,...) — run now, return values
            globals.VarAt(NameId(L"loadcodex")) = NF([this, MakeVmFunc](vector<Value>& a) -> Value {
                if (a.empty())
                    return Value();
                Value f;
                if (a[0].t == Value::Func)
                    f = a[0];
                else if (a[0].t == Value::Str)
                {
                    wstring src = BStr(a[0]), text;
                    wstring fileDir;
                    if (ReadCodeFile(src, &text))
                    {
                        src = text;
                        wstring p = BStr(a[0]);
                        if (p[0] == L'/' || p[0] == L'\\')
                        {
                            wstring root;
                            if (g_interpCurrent && !g_interpCurrent->baseDir.empty())
                                root = g_interpCurrent->baseDir;
                            if (root.empty())
                            {
                                wchar_t szExe[MAX_PATH] = L"";
                                GetModuleFileNameW(nullptr, szExe, MAX_PATH);
                                wchar_t* q = wcsrchr(szExe, L'\\');
                                if (q) *q = 0;
                                root = szExe;
                            }
                            p = root + L"\\" + p.substr(1);
                        }
                        else if (p[0] == L'~')
                            p = ResolveTildeDisk(p);
                        size_t slash = p.find_last_of(L"\\/");
                        if (slash != wstring::npos)
                            fileDir = p.substr(0, slash);
                    }
                    wstring e;
                    PNode fn = CompileToFunc(src, fileDir.empty() ? nullptr : &fileDir, &e);
                    if (!fn)
                    {
                        (*err) = L"运行错误: loadcodex: " + e;
                        return Value();
                    }
                    f = MakeVmFunc(fn);
                }
                else
                    return Value();
                vector<Value> args;
                for (size_t i = 1; i < a.size(); i++)
                    args.push_back(a[i]);
                return this->CallVmFunc(f, args); });
            // reduce(array,reducer,initialValue)
            globals.VarAt(NameId(L"reduce")) = NF([this](vector<Value>& a) -> Value {
                if (a.size() < 2 || a[0].t != Value::TableT || !a[0].tab)
                    return Value();
                auto& arr = a[0].tab->arr;
                bool bInit = (a.size() > 2 && a[2].t != Value::Null);
                Value prev;
                size_t start = 1;
                if (bInit)
                    prev = a[2];
                else
                {
                    if (arr.empty())
                    {
                        (*err) = L"运行错误: reduce 空数组且未指定初始值";
                        return Value();
                    }
                    prev = arr[0];
                    start = 2;
                }
                for (size_t i = start; i <= arr.size(); i++)
                {
                    vector<Value> args;
                    args.push_back(prev);
                    args.push_back(arr[i - 1]);
                    args.push_back(Value::Num_((double)i));
                    args.push_back(a[0]);
                    prev = this->CallVmFunc(a[1], args);
                    if (!err->empty())
                        return Value();
                }
                return prev; });
            // switch(switchValue,caseTable,default,...)
            globals.VarAt(NameId(L"switch")) = NF([this](vector<Value>& a) -> Value {
                if (a.size() < 2 || a[1].t != Value::TableT || !a[1].tab)
                {
                    (*err) = L"运行错误: switch 参数@2必须是表对象";
                    return Value();
                }
                Value fn;
                if (a[0].t == Value::Num)
                {
                    double n = a[0].n;
                    if (n >= 1 && (size_t)n <= a[1].tab->arr.size())
                        fn = a[1].tab->arr[(size_t)n - 1];
                }
                if (fn.t == Value::Null)
                {
                    auto it = a[1].tab->map.find(a[0].ToString());
                    if (it != a[1].tab->map.end())
                        fn = it->second;
                }
                if (fn.t != Value::Func)
                    fn = (a.size() > 2) ? a[2] : Value();
                if (fn.t != Value::Func)
                    return Value();
                vector<Value> args;
                for (size_t i = 3; i < a.size(); i++)
                    args.push_back(a[i]);
                return this->CallVmFunc(fn, args); });
            // invoke(f,owner,...) — call with an explicit owner, errors throw
            globals.VarAt(NameId(L"invoke")) = NF([this](vector<Value>& a) -> Value {
                if (a.empty())
                    return Value();
                Value owner = a.size() > 1 ? a[1] : Value();
                vector<Value> args;
                for (size_t i = 2; i < a.size(); i++)
                    args.push_back(a[i]);
                return this->CallOwner(a[0], owner, args); });
            // callex(errHandler, fn, owner, ...) — standard official signature
            globals.VarAt(NameId(L"callex")) = NF([this](vector<Value>& a) -> Value {
                if (a.empty())
                    return Value::Bool_(false);
                Value handler;
                Value fn;
                Value owner;
                size_t nArgStart = 3;

                auto isCallable = [](const Value& v) -> bool {
                    if (v.t == Value::Func || v.t == Value::Class) return true;
                    if (v.t == Value::TableT && v.tab)
                    {
                        if (v.tab->map.count(L"_call") || v.tab->map.count(L"_meta"))
                            return true;
                    }
                    return false;
                };

                if (a.size() >= 2 && isCallable(a[1]))
                {
                    // Official standard signature: callex(errHandler, fn, owner, ...)
                    // or callex(, fn, owner, ...) where a[0] is null/omitted
                    handler = a[0];
                    fn = a[1];
                    owner = a.size() > 2 ? a[2] : Value();
                    nArgStart = 3;
                }
                else if (isCallable(a[0]))
                {
                    // Legacy signature: callex(fn, owner, ...)
                    fn = a[0];
                    owner = a.size() > 1 ? a[1] : Value();
                    handler = a.size() > 2 ? a[2] : Value();
                    nArgStart = 3;
                }
                else
                {
                    Value vRet = Value::Bool_(false);
                    auto ex = std::make_shared<vector<Value>>();
                    ex->push_back(Value::Str_(L"callex: missing callable target"));
                    vRet.mrets = ex;
                    return vRet;
                }

                vector<Value> args;
                for (size_t i = nArgStart; i < a.size(); i++)
                    args.push_back(a[i]);

                wstring keep = err ? *err : wstring();
                if (err) err->clear();
                Value r = this->CallOwner(fn, owner, args);
                if (err && !err->empty())
                {
                    wstring msg = *err;
                    err->clear();
                    if (isCallable(handler))
                    {
                        vector<Value> ha{ Value::Str_(msg) };
                        this->CallOwner(handler, Value(), ha);
                        if (err) err->clear();
                    }
                    if (!keep.empty() && err && err->empty())
                        *err = keep;
                    Value vRet = Value::Bool_(false);
                    auto ex = std::make_shared<vector<Value>>();
                    ex->push_back(Value::Str_(msg));
                    vRet.mrets = ex;
                    return vRet;
                }
                if (!keep.empty() && err && err->empty())
                    *err = keep;

                Value vRet = Value::Bool_(true);
                auto ex = std::make_shared<vector<Value>>();
                ex->push_back(r);
                if (r.mrets)
                {
                    for (auto& item : *r.mrets)
                        ex->push_back(item);
                }
                vRet.mrets = ex;
                return vRet;
            });
            // rget(n,...) — pick from the n-th of trailing values (or unpack table / call func)
            auto fnRget = NF([this](vector<Value>& a) -> Value {
                if (a.empty()) return Value();
                if (a[0].t == Value::TableT && a[0].tab)
                {
                    auto& arr = a[0].tab->arr;
                    if (arr.empty()) return Value();
                    Value vRes = arr[0];
                    if (arr.size() > 1)
                    {
                        auto ex = std::make_shared<vector<Value>>();
                        for (size_t i = 1; i < arr.size(); i++)
                            ex->push_back(arr[i]);
                        vRes.mrets = ex;
                    }
                    return vRes;
                }
                if (a[0].t == Value::Func || a[0].native != nullptr)
                {
                    vector<Value> fargs(a.begin() + 1, a.end());
                    return this->CallOwner(a[0], Value(), fargs);
                }
                if (a.size() < 2 || a[0].t != Value::Num)
                    return a.empty() ? Value() : a[0];
                long long pos = (long long)BNum(a[0]);
                long long total = (long long)a.size() - 1;
                long long start = 1;
                if (pos > 0)
                    start = pos;
                else if (pos < 0)
                    start = total + pos + 1;
                if (start < 1) start = 1;
                if (start > total) return Value();
                Value vRes = a[(size_t)start];
                if ((size_t)start + 1 < a.size())
                {
                    auto ex = std::make_shared<vector<Value>>();
                    for (size_t i = (size_t)start + 1; i < a.size(); i++)
                        ex->push_back(a[i]);
                    vRes.mrets = ex;
                }
                return vRes;
            });
            globals.VarAt(NameId(L"rget")) = fnRget;
            roots[L"rget"] = fnRget;
            // execute(commandLine) — run a system command and wait
            globals.VarAt(NameId(L"execute")) = NF([](vector<Value>& a) -> Value {
                if (a.empty() || a[0].t != Value::Str)
                    return Value();
                int rc = _wsystem(BStr(a[0]).c_str());
                return Value::Num_((double)rc); });
            // errput(message) — report without throwing
            globals.VarAt(NameId(L"errput")) = NF([](vector<Value>& a) -> Value {
                wstring out;
                for (size_t i = 0; i < a.size(); i++)
                {
                    if (i) out += L"\t";
                    out += a[i].t == Value::Null ? L"null" : a[i].ToString();
                }
                out += L"\n";
                int n = WideCharToMultiByte(CP_UTF8, 0, out.c_str(),
                                            (int)out.size(), nullptr, 0,
                                            nullptr, nullptr);
                if (n > 0)
                {
                    std::string u8((size_t)n, 0);
                    WideCharToMultiByte(CP_UTF8, 0, out.c_str(),
                                        (int)out.size(), &u8[0], n,
                                        nullptr, nullptr);
                    fwrite(u8.data(), 1, u8.size(), stderr);
                    fflush(stderr);
                }
                return Value(); });
            globals.VarAt(NameId(L"collectgarbage")) = NF([](vector<Value>&) -> Value {
                // run a full mark & sweep cycle now
                Interp* p = g_interpCurrent;
                if (p) p->CollectGarbage(nullptr);
                return Value::Num_((double)(p ? p->nGCRuns : 0));
            });
            globals.VarAt(NameId(L"__gcdump")) = NF([](vector<Value>&) -> Value {
                GcDumpLive();
                return Value(); });
            globals.VarAt(NameId(L"setlocale")) = NF([](vector<Value>& a) -> Value {
                int cat = LC_ALL;
                if (a.size() > 1 && a[0].t == Value::Num)
                    cat = (int)a[0].n;
                const wchar_t* loc = L"";
                if (!a.empty() && a.back().t == Value::Str)
                    loc = BStr(a.back()).c_str();
                wchar_t buf[256] = L"";
                _wsetlocale(cat, loc);
                if (wcscpy_s(buf, loc ? loc : L"") == 0)
                    return Value::Str_(buf);
                return Value(); });
            // setprivilege(name,enable) — enable/disable a process privilege
            globals.VarAt(NameId(L"setprivilege")) = NF([](vector<Value>& a) -> Value {
                if (a.empty() || a[0].t != Value::Str)
                    return Value::Bool_(false);
                bool bEnable = !(a.size() > 1 && !a[1].Truthy());
                HANDLE hTok = nullptr;
                if (!OpenProcessToken(GetCurrentProcess(),
                                      TOKEN_ADJUST_PRIVILEGES |
                                      TOKEN_QUERY, &hTok))
                    return Value::Bool_(false);
                LUID luid;
                if (!LookupPrivilegeValueW(nullptr, BStr(a[0]).c_str(),
                                           &luid))
                {
                    CloseHandle(hTok);
                    return Value::Bool_(false);
                }
                TOKEN_PRIVILEGES tp;
                tp.PrivilegeCount = 1;
                tp.Privileges[0].Luid = luid;
                tp.Privileges[0].Attributes =
                    bEnable ? SE_PRIVILEGE_ENABLED : 0;
                BOOL ok = AdjustTokenPrivileges(hTok, FALSE, &tp,
                                                sizeof(tp), nullptr, nullptr);
                DWORD e = GetLastError();
                CloseHandle(hTok);
                return Value::Bool_(ok && e == ERROR_SUCCESS); });
            // dumpcode — no bytecode emitter yet
            globals.VarAt(NameId(L"dumpcode")) = NF([](vector<Value>&) -> Value {
                return Value(); });
            // Preload builtin (VM preloads builtin on startup)
            {
                PNode nImp = NewPNode();
                nImp->k = Node::Import;
                nImp->names = { L"builtin" };
                DoImport(nImp, &globals);
                if (err && !err->empty())
                {
                    err->clear();
                }
            }
        }
}
