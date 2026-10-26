// interp.h — the interpreter object: state + method declarations.
// Method bodies live in runtime.cpp (evaluation core), import.cpp
// (module loading), builtins.cpp (standard namespaces), debug.cpp
// (console mini-debugger), ffi.cpp (P/Invoke) and gdi.cpp (forms).
#pragma once
#include "engine/common.h"

namespace NodioVM
{
    extern bool g_nsTrace;   // runtime.cpp — NODIO_NS_TRACE=1 diagnostics
    extern bool g_bootTrace; // runtime.cpp — NODIO_BOOT_TRACE=1 总门控（计时器）

    // NODIO_BOOT_TRACE=1 exec-phase accumulators (runtime.cpp), dumped by
    // BootDump() at normal script exit
    extern double g_usClassCtor, g_usFuncLit, g_usNsAssign;
    extern long g_nClassCtor, g_nFuncLit, g_nNsAssign;
    extern double g_usClassDef, g_usNsStmt;
    extern long g_nClassDef, g_nNsStmt;
    extern double g_usTabCtor, g_usAssign;
    extern long g_nTabCtor, g_nAssign;
    extern double g_usBind, g_usImport;
    extern long g_nBind, g_nImport;
    extern double g_usFind;
    extern long g_nFind;
    extern double g_usMember;
    extern long g_nMember;
    extern long g_nSlotConv, g_nKeepResize;   // VarAt 槽越界诊断
    double UsSince(LARGE_INTEGER t0);
    void BootDump();

    // allocstat.cpp — 全局分配普查（NODIO_ALLOC_TRACE=1 门控）。
    // AllocPhaseScope 把作用域内的堆分配归到指定阶段（1=parse 2=exec 3=finish）。
    extern std::atomic<bool> g_allocOn;
    extern std::atomic<int> g_allocPhase;
    void AllocStatEnable();
    void AllocStatDump();
    struct AllocPhaseScope
    {
        int old;
        AllocPhaseScope(int p);
        ~AllocPhaseScope();
    };

    // 官方取模语义（docs/language-reference/operator/arithmetic.md §mod）：
    //   a % b == a - ⌊a/b⌋ * b，计算 ⌊a/b⌋ 时**向负无穷大取整**，
    //   结果的符号与**右操作数 b** 相同（与 Python 一致）。
    // C 的 fmod 是**向零取整**（符号随左操作数），两者不同 ——
    // 官方为此专门提供了 math.fmod。
    inline double AardioMod(double a, double b)
    {
        if (b == 0) return 0;
        double r = std::fmod(a, b);
        // 余数与除数异号时补一个除数，把余数拉到与 b 同号
        if (r != 0 && ((r < 0) != (b < 0))) r += b;
        return r;
    }

    struct Interp
    {
        wstring* err = nullptr;
        std::shared_ptr<Env> globalsSelf;
        Env globals;
        // Round 13: the root env is resident for the interp's whole
        // lifetime — store its vars in the intern-id slot table.
        Interp()
        {
            globals.bSlots = true;
            globals.bSlotsKeep = true;   // 驻留帧：槽空间不受 kSlotMax 限制
        }
        int nBreak = 0, nContinue = 0, nReturn = 0;
        Value ret;
        // M2: static self for meta callbacks fired from static helpers
        static Interp* g_self;
        Interp* self() { return this; }
        static Value GetCurrentOwner();
        static void SetCurrentOwner(const Value& v);
        // root namespaces + module loading
        // (GetRoot/BindModule/DoImport/InitBuiltins defined at the tail)
        int nErrLine = 0;   // last statement line entered (error reporting)
        // S-2: the file that owns the statement currently executing.  A
        // runtime error reports " @ <file>:<line>"; the frames that raised
        // it may already be unwinding when the report is written, so the
        // file is kept here as a copy.
        wstring strErrFile;
        // round 31: console mini-debugger
        bool bDebug = false;            // hooks armed
        bool bStep = false;             // stop at every statement
        std::set<int> breakpoints;      // armed lines
        // external debug session (DAP): when set, DebugStop talks to the
        // adapter over two unidirectional named pipes instead of the console
        void* hDebugPipe = nullptr;     // events pipe (engine -> adapter)
        void* hDebugCmd = nullptr;      // command pipe (adapter -> engine)
        std::unordered_map<wstring, Value> roots;    // string/math/io/...
        // resolved bare `::` system APIs (name -> callable), see sysapi.cpp
        std::unordered_map<wstring, Value> sysApis;
        std::unordered_map<wstring, Value> modules;  // import cache by path
        // engine-provided modules with no backing file, keyed by the name
        // passed to import ("(debug)" introspection module)
        std::unordered_map<wstring, Value> namedModules;
        // ---- GC (gc.cpp) ----
        // active call/block frames — the collector needs them as roots:
        // a callee's parent chain points at the DEFINING frame, not at
        // the caller's, so the caller's locals are otherwise invisible.
        std::vector<std::shared_ptr<Env>> gcFrames;
        // Register a frame that is running but NOT reachable from the VM
        // root graph. A frame's parent chain points UP (child -> parent),
        // and nothing points back DOWN, so block scopes, loop bodies,
        // module bodies and the main script frame are invisible to the
        // collector. Without this registration the collector frees a
        // frame that is still executing - that is how the official
        // win.loopMessage lost its MSG struct and the message loop
        // started spinning on freed memory.
        struct GcFrameScope
        {
            Interp& I;
            std::shared_ptr<Env> sp;
            GcFrameScope(Interp& i, std::shared_ptr<Env> e) : I(i), sp(std::move(e))
            {
                I.gcFrames.push_back(sp);
            }
            ~GcFrameScope() { I.gcFrames.pop_back(); }
            GcFrameScope(const GcFrameScope&) = delete;
            GcFrameScope& operator=(const GcFrameScope&) = delete;
        };
        bool bInGC = false;            // re-entrancy guard
        long nLastGCTick = 0;          // allocation tick of the last sweep
        long nGCRuns = 0;              // completed sweeps
        long nGCFreed = 0;             // objects reclaimed by the last sweep
        long nGCThreshold = 2000;      // allocations between automatic sweeps
        // ---- fiber execution contexts ----
        // The context that is running right now. A fiber swaps it on entry
        // and back on yield, so the frame stack / root stack / control-flow
        // flags always belong to the execution flow that pushed them.
        FiberCtx mainCtx;
        FiberCtx* curCtx = &mainCtx;
        // Suspended fibers: their frames, explicit roots and entry function
        // are NOT reachable from the VM root graph (a fiber value only holds
        // a native pointer), so the collector has to be told about them.
        std::vector<std::shared_ptr<FiberCtx>> fiberCtxs;
        void CollectGarbage(Env* env);
        void MaybeCollectGC(Env* env);
        wstring baseDir;                             // import file resolution
        std::unordered_map<wstring, wstring> embedded;   // payload v2 files

        explicit Interp(wstring* e) : err(e)
        {
            // The collector must not sweep while another interpreter runs
            // script code on another thread: its mark phase only sees its
            // own roots. Count the live interpreters (main + workers).
            g_activeVmThreads.fetch_add(1, std::memory_order_acq_rel);
            // globals outlives every frame: give it a self handle with a
            // no-op deleter so child frames can hold it as shared parent
            globalsSelf =
                std::shared_ptr<Env>(&globals, [](Env*) {});
            globals.self = globalsSelf;
            wchar_t szTrace[MAX_PATH] = L"";
            if (GetEnvironmentVariableW(L"NODIO_NS_TRACE", szTrace, MAX_PATH) > 0)
                g_nsTrace = true;
        }
        ~Interp()
        {
            g_activeVmThreads.fetch_sub(1, std::memory_order_acq_rel);
        }
        // ---- runtime.cpp (evaluation core) ----
        Value Eval(const PNode& n, Env* env);
        double ToNum(const Value& v);
        bool MetaGet(const Value& obj, const Value& keyVal, Value* out,
                     bool* pErr, wstring* pErrMsg, int ownerCallMode = 0);
        bool MetaGet(const Value& obj, const wstring& key, Value* out,
                     bool* pErr, wstring* pErrMsg, bool bOwnerCall = false);
        bool MetaGetLayer(const Value& obj, const Value& mtv,
                          const Value& keyVal, Value* out, bool* pErr,
                          wstring* pErrMsg, int ownerCallMode = 0,
                          bool bInDispatch = false);
        bool MetaGetLayer(const Value& obj, const Value& mtv,
                          const wstring& key, Value* out, bool* pErr,
                          wstring* pErrMsg, bool bOwnerCall,
                          bool bInDispatch = false);
        bool MetaSet(const Value& obj, const Value& keyVal, const Value& val,
                     bool* pErr, wstring* pErrMsg, bool bOwnerAttr = true);
        bool MetaSet(const Value& obj, const wstring& key, const Value& val,
                     bool* pErr, wstring* pErrMsg);
        bool MetaSetLayer(const Value& obj, const Value& mtv,
                          const Value& keyVal, const Value& val,
                          bool* pErr, wstring* pErrMsg, bool bOwnerAttr = true);
        bool MetaSetLayer(const Value& obj, const Value& mtv,
                          const wstring& key, const Value& val,
                          bool* pErr, wstring* pErrMsg);
        Value CallOwner(const Value& f, const Value& obj, vector<Value>& args);
        Value CallThisOwner(const Value& f, const Value& ths,
                            const Value& owner, vector<Value>& args);
        Value CallThisOwner(const Value& ths, const Value& owner,
                            const Value& f, vector<Value>& args, bool* pErr,
                            wstring* pErrMsg);
        Value CallVmFuncStatic(const Value& obj, const Value& f,
                               vector<Value>& args, bool* pErr,
                               wstring* pErrMsg);
        Value GetMeta(const Value& obj);
        wstring ToStringM(const Value& v);
        Value ToStringMValue(const Value& v);
        Value ConcatStrings(const Value& l, const Value& r);
        Value GetMember(const Value& obj, const Value& keyVal,
                        bool bDirect = false, int ownerCallMode = 0);
        Value GetMember(const Value& obj, const wstring& key,
                        bool bDirect = false, bool bOwnerCall = false);
        void RegisterComKernel(Env& globalsEnv);   // com.cpp (COM host)
        Value EvalCall(const PNode& n, Env* env);
        Value EvalBin(const PNode& n, Env* env);
        bool CmpLess(const Value& l, const Value& r);
        bool ValuesEqual(const Value& l, const Value& r, bool strict);
        Value ApplyOp(const wstring& op, const Value& l, const Value& r);
        void DoAssign(const PNode& target, const Value& v, Env* env,
                      const wstring& op);
        Value ExecBlock(const PNode& blk, Env* env);
        Value ExecBlockScoped(const PNode& blk, Env* env);
        Value EvalClass(const PNode& n, Env* env);
        Value InstantiateClass(const Value& f, vector<Value>& args,
                               const Value& vThis, bool bMethod = false);
        Value Exec(const PNode& n, Env* env);
        Value GetRoot(const wstring& name);
        // 阶段 F：执行官方 "=aardio" 预编译块，返回模块命名空间
        // （失败时返回空值并把原因写进 modErr）
        // fileKey = 该块在模块缓存里的键（官方块可能自导入，必须先入缓存）
        Value RunAardioChunkModule(const std::string& raw,
            const vector<wstring>& names, const wstring& fileKey, wstring* modErr);
        Value NsPath(const vector<wstring>& names, Env* env = nullptr,
                     bool bRoot = false);
        Value CallVmFunc(const Value& f, vector<Value>& args);

        // ---- ffi.cpp (P/Invoke) ----
        Value CreateDirectApiWrapper(HMODULE hMod, const wstring& funcName,
                                     bool bDllCdecl = false,
                                     bool bDllUnicode = false);

        // ---- import.cpp (module loading) ----
        Value BindModule(const vector<wstring>& names, const Value& mod);
        Value DoImport(const PNode& n, Env* env);
        // io.libpath 用：按 import 的同一套查找顺序解析模块名 -> 库文件完整路径
        bool ResolveModulePath(const vector<wstring>& names, wstring* outFile) const;

        // ---- builtins.cpp (standard namespaces) ----
        void InitBuiltins();

        // ---- debug.cpp (console mini-debugger + external DAP session) ----
        PNode ExprToNode(const Value& v);
        // 第五十五轮：表达式求值（DAP evaluate）——在 env 里求值一段
        // aardio 表达式，返回显示值 / 类型名 / 错误信息。
        bool EvalExprInEnv(const std::string& expr, Env* env,
                           wstring& valueOut, wstring& typeOut,
                           wstring& errOut);
        void DumpVars(Env* env);
        void DebugStop(int nLine, Env* env);
        // external debug session: serve one stop over the named pipe.
        // Reads "continue" / "next" / "step" / "vars" JSON commands from
        // the pipe and writes back "stopped" / "vars" replies. Returns
        // false when the session ended (pipe broken / disconnect).
        bool DebugPipeStop(int nLine, Env* env);
    };

    // gc.cpp - the reachability collector
    void GcCollect(Interp& I, Env* env);
}
