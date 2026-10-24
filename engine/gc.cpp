// gc.cpp — heap-object registry and the reachability collector.
//
// The VM models every script object with shared_ptr: tables (Table) and
// call/block frames (Env). That graph is CYCLIC by construction:
//
//   * a closure captures its defining frame (Value::envUp) and the frame
//     holds the closure (Env::vars)      -> frame cycle
//   * a class instance stores its own ctor closure, and that closure
//     captures `this` (Value::thisVal)   -> instance cycle
//   * util.metaProperty stores _get/_set closures that captured `owner`
//     (Value::ownerVal)                 -> owner cycle
//
// shared_ptr refcounting can never reclaim a cycle, so the official
// library's `table.gc(obj,"delete")` finalizers never run and every
// GDI+ brush/font/picture leaks. This file keeps a registry of every
// live heap object so a reachability sweep (mark & sweep) can find the
// unreachable cycles, run their finalizers and break them.
//
// The collector is only ever run at a statement boundary (runtime.cpp
// ExecBlock), where the VM holds no half-evaluated expression on the C++
// stack. Two extra safety nets:
//
//   * statement epochs — an object born inside the statement currently
//     being evaluated is never collected (its value may still live in a
//     C++ temporary that the collector cannot see);
//   * the explicit root stack (g_gcRoots) — values that native C++ frames
//     keep alive across a possible collection point.
#include "engine/common.h"
#include "engine/interp.h"

namespace NodioVM
{
    std::atomic<long> g_liveTables{ 0 };
    std::atomic<long> g_liveEnvs{ 0 };
    std::atomic<long> g_gcAllocTick{ 0 };
    std::atomic<long> g_gcEpoch{ 0 };
    std::atomic<int> g_activeVmThreads{ 0 };
    std::recursive_mutex g_gcRegistryMutex;
    std::mutex g_gcCollectMutex;
    // Per-thread: the epoch/root stacks are stack-structured (push/pop),
    // so a shared vector would let one thread's restore truncate another
    // thread's entries.
    thread_local std::vector<long> g_gcEpochStack;
    thread_local std::vector<Value> g_gcRoots;

    // The registry is intentionally leaked (never destroyed): heap objects
    // may outlive the static-destruction phase, and a destroyed registry
    // would then be written to from ~Table().
    std::unordered_set<Table*>& LiveTableSet()
    {
        static std::unordered_set<Table*>* s = new std::unordered_set<Table*>();
        return *s;
    }
    std::unordered_set<Env*>& LiveEnvSet()
    {
        static std::unordered_set<Env*>* s = new std::unordered_set<Env*>();
        return *s;
    }
    // Permanent roots: metatables kept alive by static shared_ptr members
    // in the engine (the collector cannot see those references).
    std::vector<std::shared_ptr<Table>>& RootTables()
    {
        static std::vector<std::shared_ptr<Table>>* s =
            new std::vector<std::shared_ptr<Table>>();
        return *s;
    }
    void GcAddRootTable(const std::shared_ptr<Table>& tb)
    {
        if (tb) RootTables().push_back(tb);
    }

    Table::Table()
    {
        g_liveTables++;
        g_gcAllocTick++;
        nEpoch = g_gcEpoch.load();
        // The registry is shared by every thread: lock it, or two threads
        // inserting concurrently corrupt the unordered_set (and the heap).
        std::lock_guard<std::recursive_mutex> lk(g_gcRegistryMutex);
        LiveTableSet().insert(this);
    }

    Table::Table(const Table& rhs)
        : map(rhs.map), arr(rhs.arr), scopeVars(rhs.scopeVars),
          parent(rhs.parent),
          bExplicitMeta(rhs.bExplicitMeta), comCache(rhs.comCache),
          nEpoch(rhs.nEpoch)
    {
        g_liveTables++;
        g_gcAllocTick++;
        std::lock_guard<std::recursive_mutex> lk(g_gcRegistryMutex);
        LiveTableSet().insert(this);
    }

    Table::~Table()
    {
        g_liveTables--;
        std::lock_guard<std::recursive_mutex> lk(g_gcRegistryMutex);
        LiveTableSet().erase(this);
    }

    Env::Env()
    {
        g_liveEnvs++;
        g_gcAllocTick++;
        nEpoch = g_gcEpoch.load();
        std::lock_guard<std::recursive_mutex> lk(g_gcRegistryMutex);
        LiveEnvSet().insert(this);
    }

    Env::~Env()
    {
        g_liveEnvs--;
        std::lock_guard<std::recursive_mutex> lk(g_gcRegistryMutex);
        LiveEnvSet().erase(this);
    }

    // ---- marking ----

    struct GcMark
    {
        std::unordered_set<Table*> tables;
        std::unordered_set<Env*> envs;
        // Reachability path (diagnostics): how did the collector reach
        // this object? Without it a leak is anonymous — the shape
        // histogram says "1500 gdip graphics" but not WHO holds them.
        struct Hop
        {
            void* from = nullptr;   // parent table/env (null = root)
            wstring edge;           // the member/variable that led here
            bool fromIsEnv = false;
        };
        std::unordered_map<Table*, Hop> tPath;
        std::unordered_map<Env*, Hop> ePath;
        // Path tracking costs one map insert per marked object, so it only
        // runs when the diagnostics ask for it.
        bool bTrackPaths = false;
    };

    static void GcMarkTable(Table* t, GcMark& m, void* from = nullptr,
                            const wstring& edge = L"");
    static void GcMarkEnv(Env* e, GcMark& m, void* from = nullptr,
                           const wstring& edge = L"");

    // the last mark phase is kept around so the diagnostics can explain
    // WHY an object is still reachable
    static GcMark s_lastMark;

    static void GcMarkValue(const Value& v, GcMark& m, void* from = nullptr,
                            const wstring& edge = L"")
    {
        if (v.tab) GcMarkTable(v.tab.get(), m, from, edge);
        if (v.nsClosure) GcMarkTable(v.nsClosure.get(), m, from, edge);
        if (v.envUp) GcMarkEnv(v.envUp.get(), m, from, edge);
        if (v.thisVal) GcMarkValue(*v.thisVal, m, from, edge);
        if (v.ownerVal) GcMarkValue(*v.ownerVal, m, from, edge);
        if (v.mrets)
            for (auto& r : *v.mrets) GcMarkValue(r, m, from, edge);
    }

    static void GcMarkTable(Table* t, GcMark& m, void* from,
                            const wstring& edge)
    {
        if (!t || m.tables.count(t)) return;
        m.tables.insert(t);
        if (m.bTrackPaths) m.tPath[t] = GcMark::Hop{ from, edge, false };
        static const wstring s_arr = L"[]";
        for (auto& kv : t->map) GcMarkValue(kv.second, m, t, kv.first);
        for (auto& e : t->arr) GcMarkValue(e, m, t, s_arr);
        for (auto& kv : t->scopeVars) GcMarkValue(kv.second, m, t, L"$" + kv.first);
        if (t->comCache)
            for (auto& kv : *t->comCache)
                GcMarkValue(kv.second.boundMethod, m, t, L"~com");
    }

    static void GcMarkEnv(Env* e, GcMark& m, void* from,
                           const wstring& edge)
    {
        if (!e || m.envs.count(e)) return;
        m.envs.insert(e);
        if (m.bTrackPaths) m.ePath[e] = GcMark::Hop{ from, edge, true };
        // Round 13: slot-aware — mainEnv (resident, slotted) reaches this
        // through gcFrames; a bare vars loop would see an empty map and
        // drop every main-script local from the mark set.
        e->ForEachVar([&](uint32_t gid, Value& v)
            { GcMarkValue(v, m, e, L"$" + NameOf(gid)); });
        if (e->nsTable) GcMarkTable(e->nsTable.get(), m, e, L"@nsTable");
        if (e->modTable) GcMarkTable(e->modTable.get(), m, e, L"@modTable");
        if (e->parent) GcMarkEnv(e->parent.get(), m, e, L"@parent");
    }

    // Print the chain of edges that made an object reachable.
    static void GcPrintPath(const GcMark& m, Table* t)
    {        wstring path;
        void* cur = t;
        bool curIsEnv = false;
        for (int guard = 0; cur && guard < 40; guard++)
        {
            GcMark::Hop hop;
            bool ok = false;
            if (curIsEnv)
            {
                auto it = m.ePath.find((Env*)cur);
                if (it != m.ePath.end()) { hop = it->second; ok = true; }
            }
            else
            {
                auto it = m.tPath.find((Table*)cur);
                if (it != m.tPath.end()) { hop = it->second; ok = true; }
            }
            if (!ok) break;
            path = L" --" + hop.edge + L"--> " + path;
            if (!hop.from) { path = L"ROOT" + path; break; }
            cur = hop.from;
            curIsEnv = hop.fromIsEnv;
        }
        fwprintf(stderr, L"[GC] path: %s\n", path.c_str());
        fflush(stderr);
    }

    // ---- native-held values ----
    // Forms keep their control tables and event callbacks on the native
    // side (FormState); those are roots the VM graph cannot reach.
    static void GcMarkNative(Interp& I, GcMark& m)
    {
        for (auto& kv : g_forms)
        {
            FormState* fs = kv.second;
            if (!fs) continue;
            GcMarkValue(fs->formVal, m);
            for (auto& c : fs->callbacks) GcMarkValue(c.second, m);
            for (auto& c : fs->changes) GcMarkValue(c.second, m);
            for (auto& c : fs->selchgs) GcMarkValue(c.second, m);
            for (auto& c : fs->ctrls) GcMarkValue(c.second, m);
            for (auto& c : fs->menus) GcMarkValue(c.second, m);
            for (auto& c : fs->menuItems) GcMarkValue(c.second, m);
        }
        // Native callback thunks (raw.tostdcall / raw.callback / a
        // script function passed to a dynamic FFI call) keep the script
        // function and its owner in a C++ registry - the VM graph cannot
        // reach them. This is how the official win.form installs its
        // window procedure: the native window only holds the raw
        // function pointer, and the script closure behind it lives in
        // g_cbs. Without marking these, the collector frees the window
        // procedure and the window silently stops receiving messages
        // (timers stop firing, repaints stop).
        for (auto* ctx : g_cbs)
        {
            if (!ctx) continue;
            // A callback that is exposed to script (the official "cdata"
            // object of raw.tostdcall) is reachable through its cdata table:
            // the collector follows that edge on its own. Marking it here as
            // well would pin the closure forever - the cdata would never
            // become unreachable and the callback could never be released.
            if (!ctx->rooted) continue;
            GcMarkValue(ctx->fn, m);
            GcMarkValue(ctx->owner, m);
        }
        // Suspended fibers keep their frames, their explicit roots and their
        // entry function in a native execution context. A fiber value only
        // holds a native pointer, so the VM root graph cannot reach any of
        // it: without this the collector frees a suspended fiber's live
        // locals (and its entry closure) while the fiber is parked.
        for (auto& fc : I.fiberCtxs)
        {
            if (!fc) continue;
            for (auto& e : fc->gcFrames) GcMarkEnv(e.get(), m, nullptr, L"fiberFrame");
            for (auto& v : fc->roots) GcMarkValue(v, m, nullptr, L"fiberRoot");
            GcMarkValue(fc->fn, m, nullptr, L"fiberFn");
            GcMarkValue(fc->ret, m, nullptr, L"fiberRet");
        }
        for (auto& tb : RootTables()) GcMarkTable(tb.get(), m);
        GcMarkValue(I.GetCurrentOwner(), m);
    }

    // ---- collection ----

    // Run the `_gc` finalizer of a collected object. The official
    // library registers one with `table.gc(obj,"delete")`; the callback
    // is either a method name or a function value.
    static void GcRunFinalizer(Interp& I, Table* t)
    {
        auto it = t->map.find(L"_gc");
        if (it == t->map.end()) return;
        Value gcv = it->second;
        // A namespace table carries a "self" member. The engine hands a
        // namespace from one table to another when a module is imported,
        // and the orphaned copy would run the namespace's finalizer
        // (gdip.core shuts GDI+ down that way) while the live namespace
        // still needs the resource — so namespace finalizers never run
        // from the collector.
        if (t->map.find(L"self") != t->map.end())
            return;
        if (GcTraceEnabled())
        {
            wstring keys, where;
            int n = 0;
            for (auto& kv : t->map)
            {
                if (n++ >= 5) { keys += L",..."; break; }
                if (!keys.empty()) keys += L",";
                keys += kv.first;
            }
            // where does this table still live?
            I.globals.ForEachVar([&](uint32_t gid, Value& v)
                { if (v.tab.get() == t) { where += L" global." + NameOf(gid); } });
            for (auto& kv : I.roots)
                if (kv.second.tab.get() == t) { where += L" root." + kv.first; }
            for (auto& kv : I.modules)
                if (kv.second.tab.get() == t) { where += L" module." + kv.first; }
            // what does the root namespace map point at?
            I.globals.ForEachVar([&](uint32_t gid, Value& v)
                {
                    if (gid == NameId(L"gdip"))
                    {
                        wchar_t buf[64];
                        swprintf_s(buf, 64, L" [globals.gdip=%p]", (void*)v.tab.get());
                        where += buf;
                    }
                });
            for (auto& kv : I.roots)
                if (kv.first == L"gdip")
                {
                    wchar_t buf[64];
                    swprintf_s(buf, 64, L" [roots.gdip=%p]", (void*)kv.second.tab.get());
                    where += buf;
                }
            fwprintf(stderr, L"[GC] finalize %p map=[%s] where=[%s]\n",
                     (void*)t, keys.c_str(), where.c_str());
            fflush(stderr);
        }
        t->map.erase(L"_gc");     // never finalize the same object twice
        if (gcv.t == Value::Null) return;

        Value fn;
        if (gcv.t == Value::Func || gcv.native)
        {
            fn = gcv;
        }
        else if (gcv.tab)
        {
            // official pattern (builtin/table.aardio):
            //   o._gc = <buffer> ; buffer@ = { _gc = function(){...} }
            // the finalizer closure lives in the buffer's metatable
            auto im = gcv.tab->map.find(L"_meta");
            if (im != gcv.tab->map.end() && im->second.t == Value::TableT &&
                im->second.tab)
            {
                auto if2 = im->second.tab->map.find(L"_gc");
                if (if2 != im->second.tab->map.end())
                    fn = if2->second;
            }
        }
        else
        {
            // engine-native pattern: the member name of the finalizer
            wstring name = BStr(gcv);
            if (GcTraceEnabled())
                fwprintf(stderr, L"[GC] finalizer %p name=[%s] type=%d\n",
                         (void*)t, name.c_str(), (int)gcv.t);
            if (name.empty()) return;
            std::shared_ptr<Table> sp;
            try { sp = t->shared_from_this(); }
            catch (...) { return; }
            if (!sp) return;
            fn = I.GetMember(Value::Table_(sp), name, false, 1);
        }
        if (GcTraceEnabled())
            fwprintf(stderr, L"[GC] finalizer %p resolved type=%d\n",
                     (void*)t, (int)fn.t);
        if (fn.t != Value::Func && !fn.native) return;

        std::shared_ptr<Table> sp;
        try { sp = t->shared_from_this(); }
        catch (...) { return; }
        if (!sp) return;

        Value owner = Value::Table_(sp);
        vector<Value> noArgs;
        GcRootScope rs;
        rs.add(owner);
        rs.add(fn);
        if (GcTraceEnabled())
        {
            wstring keys;
            for (auto& kv : t->map)
            {
                if (!keys.empty()) keys += L",";
                keys += kv.first;
            }
            fwprintf(stderr, L"[GC] call finalizer %p keys=[%s]\n",
                     (void*)t, keys.c_str());
            fflush(stderr);
        }
        // A finalizer runs script code, and the collector may run it at
        // ANY statement boundary - including inside a window procedure
        // or a timer callback. It must therefore not disturb the VM
        // state of the frame that triggered the collection:
        //   - `ret` / `nReturn` carry the caller's return value
        //   - an error raised by the finalizer must not abort the script
        Value oldRet = I.ret;
        int oldR = I.nReturn, oldB = I.nBreak, oldC = I.nContinue;
        wstring oldErr = I.err ? *I.err : L"";
        if (I.err) I.err->clear();
        try
        {
            I.CallOwner(fn, owner, noArgs);
        }
        catch (...)
        {
            // a finalizer that throws must not take the script down
        }
        I.ret = oldRet;
        I.nReturn = oldR;
        I.nBreak = oldB;
        I.nContinue = oldC;
        if (I.err) *I.err = oldErr;
        if (GcTraceEnabled())
        {
            fwprintf(stderr, L"[GC] finalizer %p done\n", (void*)t);
            fflush(stderr);
        }
    }

    // RAII guard: the collector must ALWAYS leave bInGC clear. A stuck
    // flag silently disables every later collection (MaybeCollectGC
    // returns immediately) and the heap then grows without bound — that
    // is exactly what a finalizer that raises a VM error used to do.
    struct GcInScope
    {
        Interp& I;
        explicit GcInScope(Interp& i) : I(i) { I.bInGC = true; }
        ~GcInScope() { I.bInGC = false; }
    };

    void GcCollect(Interp& I, Env* env)
    {
        if (I.bInGC) return;
        // Another interpreter is running script code on another thread.
        // The mark phase below only sees THIS interpreter's roots, so a
        // sweep right now would free objects the other thread still
        // holds - that is what turned threaded scripts into heap
        // corruption (0xC0000374) with a drifting fault site. Defer.
        if (g_activeVmThreads.load(std::memory_order_acquire) > 1) return;
        std::lock_guard<std::mutex> lkCollect(g_gcCollectMutex);
        if (g_activeVmThreads.load(std::memory_order_acquire) > 1) return;
        // Hold the registry lock for the whole sweep: the mark phase
        // walks every live object's members, and no other thread may
        // insert/erase meanwhile. Recursive, because a finalizer may
        // allocate and the sweep destroys objects whose destructors take
        // the same lock.
        std::lock_guard<std::recursive_mutex> lkReg(g_gcRegistryMutex);
        GcInScope scope(I);
        const bool bTrace = GcTraceEnabled();
        LARGE_INTEGER t0, t1, t2, t3, freq;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&t0);

        GcMark& m = s_lastMark;
        m.tables.clear();
        m.envs.clear();
        m.tPath.clear();
        m.ePath.clear();
        m.bTrackPaths = GcTraceEnabled();
        GcMarkEnv(&I.globals, m);
        for (auto& kv : I.roots) GcMarkValue(kv.second, m, nullptr, L"root:" + kv.first);
        for (auto& kv : I.modules) GcMarkValue(kv.second, m, nullptr, L"module:" + kv.first);
        for (auto& kv : I.namedModules) GcMarkValue(kv.second, m, nullptr, L"named:" + kv.first);
        GcMarkValue(I.ret, m, nullptr, L"ret");
        for (auto& e : I.gcFrames) GcMarkEnv(e.get(), m, nullptr, L"frame");
        for (auto& v : g_gcRoots) GcMarkValue(v, m, nullptr, L"gcRoot");
        GcMarkNative(I, m);
        if (env) GcMarkEnv(env, m, nullptr, L"env");
        if (bTrace)
        {
            fwprintf(stderr, L"[GC] begin frames=%zu live=%ld interp=%p\n",
                     I.gcFrames.size(), g_liveTables + g_liveEnvs, (void*)&I);
            fflush(stderr);
        }

        // objects born inside the statement being evaluated may still be
        // held by a C++ temporary the collector cannot see
        const long protect = GcProtectEpoch();

        // A fiber whose script value is unreachable is garbage. Its
        // execution context still holds the entry function, so an
        // abandoned fiber would pin its closure (and everything the
        // closure captured) alive for the rest of the process. Release it
        // here: the fiber is dead anyway, nobody can resume it.
        for (size_t i = 0; i < I.fiberCtxs.size(); )
        {
            auto& fc = I.fiberCtxs[i];
            bool bDrop = false;
            if (fc)
            {
                // The fiber's script value is a weak reference: if it is
                // gone (or was never reachable) the script dropped the
                // fiber, so nobody can resume it any more.
                std::shared_ptr<Table> fv = fc->fiberVal.lock();
                if (!fv || !m.tables.count(fv.get()))
                {
                    VMFiber* f = (VMFiber*)fc->fiber;
                    if (f && f->status != L"running")
                    {
                        f->status = L"dead";
                        bDrop = true;
                    }
                }
            }
            if (bDrop)
            {
                fc->fn = Value();
                fc->gcFrames.clear();
                fc->roots.clear();
                fc->ret = Value();
                I.fiberCtxs.erase(I.fiberCtxs.begin() + i);
                continue;   // the next context moved into this slot
            }
            i++;
        }

        std::vector<Table*> deadTables;
        for (auto* t : LiveTableSet())
        {
            if (m.tables.count(t)) continue;
            if (t->nEpoch >= protect) continue;
            deadTables.push_back(t);
        }
        std::vector<Env*> deadEnvs;
        for (auto* e : LiveEnvSet())
        {
            if (m.envs.count(e)) continue;
            if (e->nEpoch >= protect) continue;
            if (e->bNsBody) continue;   // namespace/module scope frames
            deadEnvs.push_back(e);
        }

        if (!deadTables.empty() || !deadEnvs.empty())
        {
            QueryPerformanceCounter(&t1);
            if (bTrace)
                fwprintf(stderr, L"[GC] sweep: %zu tables, %zu envs\n",
                         deadTables.size(), deadEnvs.size());
            // Pin the dead tables before running any finalizer: a
            // finalizer may drop the last reference to another dead
            // object, and the sweep must not touch freed memory.
            std::vector<std::shared_ptr<Table>> pins;
            for (auto* t : deadTables)
            {
                if (!LiveTableSet().count(t)) continue;   // already gone
                try { pins.push_back(t->shared_from_this()); }
                catch (...) { }
            }
            // finalizers first: they still need the object's own fields
            for (auto* t : deadTables)
            {
                if (!LiveTableSet().count(t)) continue;
                if (bTrace)
                {
                    wstring keys;
                    int n = 0;
                    for (auto& kv : t->map)
                    {
                        if (n++ >= 8) { keys += L",..."; break; }
                        if (!keys.empty()) keys += L",";
                        keys += kv.first;
                    }
                    fwprintf(stderr, L"[GC] dead table %p [%s]",
                             (void*)t, keys.c_str());
                    // which known root does it look like?
                    I.globals.ForEachVar([&](uint32_t gid, Value& v)
                        {
                            if (v.tab.get() == t)
                                fwprintf(stderr, L" ==globals.%s", NameOf(gid).c_str());
                        });
                    for (auto& kv : I.roots)
                        if (kv.second.tab.get() == t)
                            fwprintf(stderr, L" ==root.%s", kv.first.c_str());
                    for (auto& kv : I.modules)
                        if (kv.second.tab.get() == t)
                            fwprintf(stderr, L" ==module.%s", kv.first.c_str());
                    // How many strong references does this object still
                    // have? A dead object that is never freed is held by
                    // a reference the mark phase cannot see.
                    long nStrong = -1;
                    try { nStrong = (long)t->shared_from_this().use_count(); }
                    catch (...) { nStrong = -2; }
                    fwprintf(stderr, L" strong=%ld", nStrong);

                }
                // a finalizer runs VM code: it may raise a script error or
                // throw. One broken finalizer must not abort the sweep
                // (the objects would stay alive forever).
                if (GcNoFinalEnabled()) continue;
                try
                {
                    GcRunFinalizer(I, t);
                }
                catch (...)
                {
                    if (bTrace)
                    {
                        fwprintf(stderr, L"[GC] finalizer %p threw\n", (void*)t);
                        fflush(stderr);
                    }
                }
            }

            // break the cycles: move the contents aside instead of
            // dropping them, so the referenced objects stay alive until
            // the whole sweep is over (their addresses are still on the
            // dead lists)
            std::vector<Value> trash;
            for (auto* t : deadTables)
            {
                if (!LiveTableSet().count(t)) continue;
                for (auto& kv : t->map) trash.push_back(std::move(kv.second));
                t->map.clear();
                for (auto& e : t->arr) trash.push_back(std::move(e));
                t->arr.clear();
                for (auto& kv : t->scopeVars) trash.push_back(std::move(kv.second));
                t->scopeVars.clear();
                if (t->comCache) t->comCache->clear();
            }
            for (auto* e : deadEnvs)
            {
                if (!LiveEnvSet().count(e)) continue;
                if (bTrace)
                {
                    wstring keys;
                    int n = 0;
                    for (auto& kv : e->vars)
                    {
                        if (n++ >= 8) { keys += L",..."; break; }
                        if (!keys.empty()) keys += L",";
                        keys += NameOf(kv.first);
                    }
                    bool bInStack = false;
                    for (auto& fr : I.gcFrames)
                        if (fr.get() == e) { bInStack = true; break; }
                    fwprintf(stderr, L"[GC] dead env %p vars=[%s] ns=%p mod=%p parent=%p inStack=%d frames=%zu\n",
                             (void*)e, keys.c_str(), (void*)e->nsTable.get(),
                             (void*)e->modTable.get(), (void*)e->parent.get(),
                             (int)bInStack, I.gcFrames.size());
                }
                e->ForEachVar([&](uint32_t, Value& v) { trash.push_back(std::move(v)); });
                e->clearVars();
                e->nsTable.reset();
                e->modTable.reset();
            }
            trash.clear();   // now the cycles are broken and the objects die
            pins.clear();
            QueryPerformanceCounter(&t2);
            if (bTrace)
            {
                fwprintf(stderr, L"[GC] sweep done %.1fms deadT=%zu deadE=%zu\n",
                         (double)(t2.QuadPart - t1.QuadPart) * 1000.0 /
                         (double)freq.QuadPart,
                         deadTables.size(), deadEnvs.size());
                fflush(stderr);
            }
        }

        if (bTrace)
        {
            QueryPerformanceCounter(&t3);
            double ms = (double)(t3.QuadPart - t0.QuadPart) * 1000.0 /
                        (double)freq.QuadPart;
            fwprintf(stderr, L"[GC] cycle %.1fms live=%ld/%ld dead=%zu/%zu\n",
                     ms, g_liveTables.load(), g_liveEnvs.load(),
                     deadTables.size(), deadEnvs.size());
            fflush(stderr);
        }
        // bInGC is cleared by the GcInScope guard
    }

    // Report live-object counts at process exit (diagnostics).
    void GcDumpLive()
    {
        std::lock_guard<std::recursive_mutex> lkReg(g_gcRegistryMutex);
        fwprintf(stderr, L"[GC] live tables=%ld envs=%ld alloc=%ld\n",
                 g_liveTables.load(), g_liveEnvs.load(), g_gcAllocTick.load());
        {
            const long protect = GcProtectEpoch();
            long nProt = 0, nMax = 0;
            for (auto* t : LiveTableSet())
            {
                if (t->nEpoch >= protect) nProt++;
                if (t->nEpoch > nMax) nMax = t->nEpoch;
            }
            fwprintf(stderr, L"[GC] epoch=%ld protect=%ld stack=%zu protected=%ld maxBirth=%ld\n",
                     g_gcEpoch.load(), protect, g_gcEpochStack.size(), nProt, nMax);
            Interp* pI = Interp::g_self;
            if (pI)
                fwprintf(stderr, L"[GC] bInGC=%d thr=%ld lastTick=%ld allocTick=%ld runs=%ld\n",
                         (int)pI->bInGC, pI->nGCThreshold, pI->nLastGCTick,
                         g_gcAllocTick.load(), pI->nGCRuns);
            fwprintf(stderr, L"[GC] activeVmThreads=%d\n",
                     g_activeVmThreads.load(std::memory_order_acquire));
        }
        // shape histogram: which member-name sets are still alive tells us
        // exactly what kind of object leaked
        std::map<wstring, int> shapes;
        std::map<wstring, Table*> rep;
        for (auto* t : LiveTableSet())
        {
            wstring sig;
            int n = 0;
            for (auto& kv : t->map)
            {
                if (n++ >= 6) break;
                if (!sig.empty()) sig += L",";
                sig += kv.first;
            }
            if (t->arr.size()) sig += L"|arr" + std::to_wstring(t->arr.size());
            // class fingerprint: the _class member identifies the class
            // whose instance this is; without it a shape like
            // [_class,_gc,_meta] is ambiguous (a dozen gdip classes
            // share it) and the leak stays anonymous.
            auto itCls = t->map.find(L"_class");
            if (itCls != t->map.end() && itCls->second.tab)
            {
                wstring cs;
                int cn = 0;
                for (auto& kv : itCls->second.tab->map)
                {
                    if (cn++ >= 5) break;
                    if (!cs.empty()) cs += L",";
                    cs += kv.first;
                }
                sig += L" <cls:";
                sig += cs;
                sig += L">";
            }
            shapes[sig]++;
            if (!rep.count(sig)) rep[sig] = t;
        }
        for (auto& kv : shapes)
            fwprintf(stderr, L"[GC] shape x%-6d [%s]\n", kv.second,
                     kv.first.c_str());
        // for the biggest shapes, show HOW the object is still reachable
        std::vector<std::pair<int, wstring>> ranked;
        for (auto& kv : shapes) ranked.push_back({ kv.second, kv.first });
        std::sort(ranked.begin(), ranked.end(),
                  [](const std::pair<int, wstring>& a,
                     const std::pair<int, wstring>& b) { return a.first > b.first; });
        int n = 0;
        for (auto& r : ranked)
        {
            if (n++ >= 3) break;
            if (r.first < 2) continue;
            auto it = rep.find(r.second);
            if (it == rep.end()) continue;
            fwprintf(stderr, L"[GC] why x%d [%s]:\n", r.first, r.second.c_str());
            GcPrintPath(s_lastMark, it->second);
        }
        fflush(stderr);
    }
}
