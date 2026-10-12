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
        // round 19: aardio root namespaces + module loading
        // (GetRoot/BindModule/DoImport/InitBuiltins defined at the tail)
        int nErrLine = 0;   // last statement line entered (error reporting)
        // round 31: console mini-debugger
        bool bDebug = false;            // hooks armed
        bool bStep = false;             // stop at every statement
        std::set<int> breakpoints;      // armed lines
        std::unordered_map<wstring, Value> roots;    // string/math/io/...
        std::unordered_map<wstring, Value> modules;  // import cache by path
        wstring baseDir;                             // import file resolution
        std::unordered_map<wstring, wstring> embedded;   // payload v2 files

        explicit Interp(wstring* e) : err(e) {}

        // ---- runtime.cpp (evaluation core) ----
        Value Eval(const PNode& n, Env* env);
        double ToNum(const Value& v);
        bool MetaGet(const Value& obj, const wstring& key, Value* out,
                     bool* pErr, wstring* pErrMsg, bool bOwnerCall = false);
        bool MetaSet(const Value& obj, const wstring& key, const Value& val,
                     bool* pErr, wstring* pErrMsg);
        Value CallOwner(const Value& f, const Value& obj, vector<Value>& args);
        Value CallVmFuncStatic(const Value& obj, const Value& f,
                               vector<Value>& args, bool* pErr,
                               wstring* pErrMsg);
        Value GetMeta(const Value& obj);
        wstring ToStringM(const Value& v);
        Value GetMember(const Value& obj, const wstring& key,
                        bool bDirect = false, bool bOwnerCall = false);
        Value EvalCall(const PNode& n, Env* env);
        Value EvalBin(const PNode& n, Env* env);
        bool CmpLess(const Value& l, const Value& r);
        Value ApplyOp(const wstring& op, const Value& l, const Value& r);
        void DoAssignSimple(const wstring& name, const Value& v);
        void DoAssign(const PNode& target, const Value& v, Env* env,
                      const wstring& op);
        Value ExecBlock(const PNode& blk, Env* env);
        Value EvalClass(const PNode& n, Env* env);
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
