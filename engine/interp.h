// interp.h — the interpreter object: state + method declarations.
// Method bodies live in runtime.cpp (evaluation core), import.cpp
// (module loading), builtins.cpp (standard namespaces), debug.cpp
// (console mini-debugger), ffi.cpp (P/Invoke) and gdi.cpp (forms).
#pragma once
#include "engine/common.h"

namespace NodioVM
{
    struct Interp
    {
        wstring* err = nullptr;
        Env globals;
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
        // round 31: console mini-debugger
        bool bDebug = false;            // hooks armed
        bool bStep = false;             // stop at every statement
        std::set<int> breakpoints;      // armed lines
        std::unordered_map<wstring, Value> roots;    // string/math/io/...
        std::unordered_map<wstring, Value> modules;  // import cache by path
        // engine-provided modules with no backing file, keyed by the name
        // passed to import ("(debug)" introspection module)
        std::unordered_map<wstring, Value> namedModules;
        wstring baseDir;                             // import file resolution
        std::unordered_map<wstring, wstring> embedded;   // payload v2 files

        explicit Interp(wstring* e) : err(e)
        {
            // globals outlives every frame: give it a self handle with a
            // no-op deleter so child frames can hold it as shared parent
            globals.self =
                std::shared_ptr<Env>(&globals, [](Env*) {});
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

        // ---- builtins.cpp (standard namespaces) ----
        void InitBuiltins();

        // ---- debug.cpp (console mini-debugger) ----
        PNode ExprToNode(const Value& v);
        void DumpVars(Env* env);
        void DebugStop(int nLine, Env* env);
    };
}
