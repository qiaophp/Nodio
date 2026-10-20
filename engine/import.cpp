#include "engine/interp.h"
#include "engine/parser.h"
#include "engine/compiler.h"
#include "engine/aardio_chunk.h"
#include <windows.h>

namespace NodioVM
{
    extern bool g_nsTrace;
    static std::wstring GetExecutableDir()
    {
        wchar_t szPath[MAX_PATH] = L"";
        if (GetModuleFileNameW(nullptr, szPath, MAX_PATH) > 0)
        {
            size_t sl = wstring(szPath).find_last_of(L"\\/");
            if (sl != wstring::npos) return wstring(szPath).substr(0, sl);
        }
        return L"";
    }

    static std::wstring GetOverlayLibDir()
    {
#if !defined(_WIN64) && !defined(__x86_64__)
        // In 32-bit nodio: use aardio 32-bit standard library directly; do not use 64-bit overlay
        wchar_t szEnv[MAX_PATH] = L"";
        if (GetEnvironmentVariableW(L"NODIO_OVERLAY_LIBDIR", szEnv, MAX_PATH) > 0)
            return wstring(szEnv);
        return L"";
#else
        wchar_t szEnv[MAX_PATH] = L"";
        if (GetEnvironmentVariableW(L"NODIO_OVERLAY_LIBDIR", szEnv, MAX_PATH) > 0)
            return wstring(szEnv);
        std::wstring exeDir = GetExecutableDir();
        if (!exeDir.empty())
        {
            std::wstring cand1 = exeDir + L"\\lib";
            if (_waccess(cand1.c_str(), 0) == 0) return cand1;
            size_t sl = exeDir.find_last_of(L"\\/");
            if (sl != wstring::npos)
            {
                std::wstring cand2 = exeDir.substr(0, sl) + L"\\lib";
                if (_waccess(cand2.c_str(), 0) == 0) return cand2;
            }
        }
        if (_waccess(L"D:\\Nodio\\lib", 0) == 0)
            return L"D:\\Nodio\\lib";
        return L"";
#endif
    }
// ---- BindModule ----
Value Interp::BindModule(const vector<wstring>& names, const Value& mod)
        {
            static bool bDbg = GetEnvironmentVariableW(L"CMT", nullptr, 0) != 0;
            if (bDbg)
            {
                wstring full;
                for (size_t i = 0; i < names.size(); ++i) { if (i) full += L"."; full += names[i]; }
                int nOldT = -1;
                bool bSameTab = false;
                auto itDbg = roots.find(names[0]);
                if (itDbg != roots.end() && names.size() == 1)
                {
                    nOldT = (int)itDbg->second.t;
                    bSameTab = (itDbg->second.tab && mod.tab &&
                                itDbg->second.tab == mod.tab);
                }
                fwprintf(stderr, L"[DEBUG BIND] names=%s mod.t=%d old.t=%d same=%d\n",
                         full.c_str(), (int)mod.t, nOldT, (int)bSameTab);
                fflush(stderr);
            }
            // single-segment import: the module IS the root namespace
            if (names.size() == 1)
            {
                auto itOld = roots.find(names[0]);
                if (itOld != roots.end() && itOld->second.t == Value::TableT && itOld->second.tab &&
                    mod.t == Value::TableT && mod.tab && itOld->second.tab != mod.tab)
                {
                    for (auto& kv : itOld->second.tab->map)
                    {
                        if (!mod.tab->map.count(kv.first))
                            mod.tab->map[kv.first] = kv.second;
                    }
                    // The old table is superseded by the module table (the
                    // namespace now lives there). A `_gc` finalizer copied
                    // across would fire TWICE — once for the live table and
                    // once for the orphaned copy — and the orphan's finalizer
                    // would release a native resource the live namespace is
                    // still using (gdip.core shuts GDI+ down this way).
                    // Hand the finalizer over instead of duplicating it.
                    auto itGc = itOld->second.tab->map.find(L"_gc");
                    if (itGc != itOld->second.tab->map.end())
                    {
                        if (!mod.tab->map.count(L"_gc"))
                            mod.tab->map[L"_gc"] = itGc->second;
                        itOld->second.tab->map.erase(L"_gc");
                    }
                }
                // A module body that declares `class X {}` (+ the trailing
                // `namespace X{}` extension) builds the real namespace in
                // place: keep that class when the incoming module value is
                // a placeholder table, merging the module's own members
                // into it. The re-entrant `import process;` inside
                // process/job/limitKill used to bind the empty cache
                // placeholder OVER the class, dropping every member the
                // namespace declared (PROCESS_INFORMATION, STARTUPINFO...).
                auto itCl = roots.find(names[0]);
                if (itCl != roots.end() &&
                    (itCl->second.t == Value::Class ||
                     itCl->second.t == Value::Func) &&
                    itCl->second.tab &&
                    mod.t == Value::TableT && mod.tab &&
                    itCl->second.tab != mod.tab)
                {
                    Value& vOld = itCl->second;
                    for (auto& kv : mod.tab->map)
                        if (!vOld.tab->map.count(kv.first))
                            vOld.tab->map[kv.first] = kv.second;
                    for (auto& kv : mod.tab->scopeVars)
                        if (!vOld.tab->scopeVars.count(kv.first))
                            vOld.tab->scopeVars[kv.first] = kv.second;
                    globals.vars[names[0]] = vOld;
                    return vOld;
                }
                roots[names[0]] = mod;
                globals.vars[names[0]] = mod;
                return mod;
            }
            // bind a dotted import path into the root namespaces
            Value ns = GetRoot(names[0]);
            for (size_t i = 1; i < names.size(); i++)
            {
                if (ns.t != Value::TableT || !ns.tab)
                    break;
                if (i + 1 == names.size())
                {
                    auto itTarget = ns.tab->map.find(names[i]);
                    if (itTarget != ns.tab->map.end() && itTarget->second.tab &&
                        mod.t == Value::TableT && mod.tab &&
                        itTarget->second.tab != mod.tab)
                    {
                        Value& vT = itTarget->second;
                        if (vT.t == Value::Class || vT.t == Value::Func)
                        {
                            // The existing class/function binding wins:
                            // the module body already built it.
                            // Binding that table over the class dropped
                            // it. Merge the module's members in and keep.
                            for (auto& kv : mod.tab->map)
                                if (!vT.tab->map.count(kv.first))
                                    vT.tab->map[kv.first] = kv.second;
                            for (auto& kv : mod.tab->scopeVars)
                                if (!vT.tab->scopeVars.count(kv.first))
                                    vT.tab->scopeVars[kv.first] = kv.second;
                            return vT;
                        }
                        for (auto& kv : vT.tab->map)
                        {
                            if (!mod.tab->map.count(kv.first))
                                mod.tab->map[kv.first] = kv.second;
                        }
                    }
                    ns.tab->map[names[i]] = mod;
                    break;
                }
                auto it = ns.tab->map.find(names[i]);
                if (it != ns.tab->map.end() && (it->second.t == Value::TableT || it->second.t == Value::Class))
                    ns = it->second;
                else
                {
                    auto nt = std::make_shared<Table>();
                    Value v = Value::Table_(nt);
                    ns.tab->map[names[i]] = v;
                    ns = v;
                }
            }
            return mod;
        }

// ---- DoImport ----
Value Interp::DoImport(const PNode& n, Env* env)
        {
            (void)env;
            // global lib root — enabled via NODIO_LIBDIR or configuration;
            // resolution order: overlay lib dir (D:\Nodio\lib) first, then strLibDir
            static const wstring strOverlayLibDir = GetOverlayLibDir();
            static const wstring strLibDir = []
            {
                wchar_t szLib[MAX_PATH] = L"";
                if (GetEnvironmentVariableW(L"NODIO_LIBDIR", szLib, MAX_PATH) > 0)
                    return wstring(szLib);
                std::wstring cfg = GetNodioConfig().libDir;
                if (!cfg.empty()) return cfg;
                if (_waccess(L"D:\\aardio\\lib", 0) == 0) return std::wstring(L"D:\\aardio\\lib");
                return std::wstring(L"");
            }();
            auto Exists = [](const wstring& p) -> bool
            {
                return !p.empty() && _waccess(p.c_str(), 0) == 0;
            };

            // path-style import: a single name that looks like a file path
            // ("D:\dir\mod.aardio") is loaded directly and bound under the
            // file's base name — this is how the designer discovers user
            // components dropped into a project folder.
            vector<wstring> vBind = n->names;
            bool bPathImport = false;
            if (n->names.size() == 1)
            {
                const wstring& nm = n->names[0];
                if (nm.find(L'\\') != wstring::npos ||
                    nm.find(L'/') != wstring::npos ||
                    nm.find(L':') != wstring::npos)
                {
                    bPathImport = true;
                    wstring bn = nm;
                    size_t sl = bn.find_last_of(L"\\/");
                    if (sl != wstring::npos) bn = bn.substr(sl + 1);
                    size_t dot = bn.find_last_of(L'.');
                    if (dot != wstring::npos && dot > 0) bn = bn.substr(0, dot);
                    vBind.clear();
                    vBind.push_back(bn);
                }
            }

            // NOTE: no hierarchical (parent namespace) pre-loading here.
            //
            // Official aardio never loads a parent namespace implicitly:
            //   - docs/library-guide/import.md lists the ONLY lookup paths for
            //     `import a.b.c`:
            //         ~\lib\a.b.c.aardio
            //         ~\lib\a\b.c.aardio
            //         ~\lib\a\b\c.aardio
            //         ~\lib\a\b\c\_.aardio
            //     (plus the same four under the app base dir for user libs).
            //     No parent `a` / `a.b` library is loaded as a side effect.
            //   - "在 aardio 中库一定要自行创建与路径相同的命名空间。如果未创建一致
            //      的命名空间，则导入库失败。" -> the namespace chain is created by
            //      the library's own `namespace a.b;` declaration (see NsPath),
            //      not by the importer.
            //   - official libraries declare their own dependencies explicitly,
            //     e.g. lib\win\ui\_.aardio starts with
            //        import win; import win.ui.background; import win.ui.ctrl; import gdi;
            //   - official guards rely on the parent NOT being pre-loaded, e.g.
            //        lib\web\form\query.aardio : `if(type(web[["form"]]) != type.class)
            //        error("请直接导入web.form")`
            //        lib\web\view\dev.aardio   : `type(web[["view"]])!=type.class`
            //        lib\web\acme\httpClient.aardio : `type(web.acme)!=type.class`
            //        lib\winex\accObject\element.aardio : `type(winex[["accObject"]])!="class"`
            //     Pre-loading the parent made those guards unreachable.
            // NsPath() creates any missing intermediate namespace tables, so the
            // submodule still binds into `a.b.c` exactly like official does.

            vector<wstring> relCands;
            for (size_t p = 0; p < n->names.size(); ++p)
            {
                wstring cand;
                for (size_t i = 0; i < p; ++i)
                {
                    if (i > 0) cand += L"\\";
                    cand += n->names[i];
                }
                wstring filePart;
                for (size_t i = p; i < n->names.size(); ++i)
                {
                    if (i > p) filePart += L".";
                    filePart += n->names[i];
                }
                if (!cand.empty()) cand += L"\\";
                cand += filePart;
                relCands.push_back(cand);
            }
            {
                wstring folderCand;
                for (size_t i = 0; i < n->names.size(); ++i)
                {
                    if (i > 0) folderCand += L"\\";
                    folderCand += n->names[i];
                }
                folderCand += L"\\_";
                relCands.push_back(folderCand);
            }

            // 1) pre-bound builtin namespaces resolve immediately —
            // but a matching lib FILE merges into the namespace first
            // (import X loads lib source into namespace X)
            {
                bool bLibFile = false;
                if (!strOverlayLibDir.empty())
                {
                    for (const auto& rc : relCands)
                    {
                        for (const auto& ext : GetConfiguredExtensions())
                        {
                            if (Exists(strOverlayLibDir + L"\\" + rc + ext))
                            {
                                bLibFile = true;
                                break;
                            }
                        }
                        if (bLibFile) break;
                    }
                }
                if (!bLibFile && !strLibDir.empty())
                {
                    for (const auto& rc : relCands)
                    {
                        for (const auto& ext : GetConfiguredExtensions())
                        {
                            if (Exists(strLibDir + L"\\" + rc + ext))
                            {
                                bLibFile = true;
                                break;
                            }
                        }
                        if (bLibFile) break;
                    }
                }
                bool bAnyFile = bLibFile;
                if (!bAnyFile && !baseDir.empty())
                {
                    for (const auto& rc : relCands)
                    {
                        for (const auto& ext : GetConfiguredExtensions())
                        {
                            if (Exists(baseDir + L"\\" + rc + ext) ||
                                Exists(baseDir + L"\\lib\\" + rc + ext))
                            {
                                bAnyFile = true;
                                break;
                            }
                        }
                        if (bAnyFile) break;
                    }
                }
                // 0) engine-provided modules with no backing file
                if (n->names.size() == 1)
                {
                    auto itN = namedModules.find(n->names[0]);
                    if (itN != namedModules.end())
                    {
                        BindModule(vBind, itN->second);
                        return itN->second;
                    }
                }
                static bool bDbgI = GetEnvironmentVariableW(L"CMT", nullptr, 0) != 0;
                if (bDbgI)
                {
                    wstring full2;
                    for (size_t i = 0; i < n->names.size(); ++i) { if (i) full2 += L"."; full2 += n->names[i]; }
                    fwprintf(stderr, L"[IMPORT] %s inRoots=%d anyFile=%d libFile=%d\n", full2.c_str(),
                             (int)roots.count(n->names[0]), (int)bAnyFile, (int)bLibFile);
                    fflush(stderr);
                }
                bool bSkipShadow = false;
                static const std::unordered_set<wstring> s_coreBuiltins = {
                    L"time", L"string", L"math", L"table", L"raw", L"thread", L"io", L"fiber"
                };
                if (n->names.size() == 1 && s_coreBuiltins.count(n->names[0]) && !strLibDir.empty())
                {
                    // Core language builtin should only be shadowed by official lib, not local baseDir file
                    bSkipShadow = !bLibFile;
                }
                else
                {
                    bSkipShadow = !bAnyFile;
                }

                bool bEmbedded = false;
                for (const auto& rc : relCands)
                {
                    for (const auto& ext : GetConfiguredExtensions())
                    {
                        if (embedded.count(rc + ext))
                        {
                            bEmbedded = true;
                            break;
                        }
                    }
                    if (bEmbedded) break;
                }

                if (roots.count(n->names[0]) &&
                    !bEmbedded &&
                    bSkipShadow)
                    return roots[n->names[0]];
            }

            // file module: check overlay lib (D:\Nodio\lib) first, then official lib root, then baseDir
            wstring file;
            if (bPathImport)
            {
                wstring cand = vBind.empty() ? n->names[0] : n->names[0];
                if (Exists(cand)) file = cand;
                else
                {
                    for (const auto& ext : GetConfiguredExtensions())
                    {
                        wstring c2 = cand + ext;
                        if (Exists(c2)) { file = c2; break; }
                    }
                }
            }
            if (file.empty() && !strOverlayLibDir.empty())
            {
                for (const auto& rc : relCands)
                {
                    for (const auto& ext : GetConfiguredExtensions())
                    {
                        wstring cand = strOverlayLibDir + L"\\" + rc + ext;
                        if (Exists(cand)) { file = cand; break; }
                    }
                    if (!file.empty()) break;
                }
            }
            if (file.empty() && !strLibDir.empty())
            {
                for (const auto& rc : relCands)
                {
                    for (const auto& ext : GetConfiguredExtensions())
                    {
                        wstring cand = strLibDir + L"\\" + rc + ext;
                        if (Exists(cand)) { file = cand; break; }
                    }
                    if (!file.empty()) break;
                }
            }
            if (file.empty() && !baseDir.empty())
            {
                for (const auto& rc : relCands)
                {
                    for (const auto& ext : GetConfiguredExtensions())
                    {
                        wstring cand = baseDir + L"\\" + rc + ext;
                        if (Exists(cand)) { file = cand; break; }
                        wstring candLib = baseDir + L"\\lib\\" + rc + ext;
                        if (Exists(candLib)) { file = candLib; break; }
                    }
                    if (!file.empty()) break;
                }
            }
            if (file.empty())
            {
                wstring defRel = relCands.empty() ? L"" : relCands.back();
                wstring defExt = GetConfiguredExtensions().empty() ? L".nodio" : GetConfiguredExtensions().front();
                file = baseDir.empty() ? (defRel + defExt)
                                       : (baseDir + L"\\" + defRel + defExt);
            }
            static bool bDbg = GetEnvironmentVariableW(L"CMT", nullptr, 0) != 0;
            if (bDbg)
            {
                fwprintf(stderr, L"[DEBUG DOIMPORT] file=%s in_modules=%d\n", file.c_str(), (int)(modules.find(file) != modules.end()));
                fflush(stderr);
            }
            auto it = modules.find(file);
            if (it != modules.end())
            {
                BindModule(vBind, it->second);
                return it->second;
            }

            // payload v2: embedded project files resolve before disk
            auto itE = embedded.end();
            for (const auto& rc : relCands)
            {
                for (const auto& ext : GetConfiguredExtensions())
                {
                    auto itCand = embedded.find(rc + ext);
                    if (itCand != embedded.end()) { itE = itCand; break; }
                }
                if (itE != embedded.end()) break;
            }
            if (itE != embedded.end())
            {
                wstring src = itE->second;
                wstring modErr;
                PNode prog;
                if (IsBytecode(src))
                {
                    prog = DeserializeNode(src, &modErr);
                }
                else if (IsCompiledModule(src))
                {
                    // bytecode magic header but not Nodio's own encoding:
                    // a format error, not a syntax error.
                    if (src.compare(0, 7, L"=aardio") == 0)
                    {
                        // 官方预编译块（aardio.exe 内部称 precompiled chunk）。
                        // 容器层已经能读，但字节码解释器尚未实现。
                        modErr = L"格式错误: 官方预编译模块 (=aardio) 暂不可执行 — "
                                 L"Nodio 尚未实现该字节码的解释器";
                    }
                    else
                    {
                        modErr = L"格式错误: 不支持的字节码格式 (" + src.substr(0, 16) + L")";
                    }
                }
                else
                {
                    vector<Tok> toks = Lex(src, &modErr);
                    Parser ps{ toks, 0, &modErr };
                    ps.strEmbedBase = baseDir;
                    ps.strEmbedRoot = EmbedRoot();
                    prog = ps.ParseBlock();
                }
                if (!modErr.empty() || !prog)
                {
                    (*err) = L"库 " + file + L" 编译失败: " + modErr;
                    return Value();
                }
                shared_ptr<Table> placeholder2;
                if (n->names.size() == 1)
                {
                    auto itR = roots.find(n->names[0]);
                    if (itR != roots.end() && itR->second.t == Value::TableT && itR->second.tab)
                        placeholder2 = itR->second.tab;
                }
                else if (n->names.size() > 1)
                {
                    auto itR = roots.find(n->names[0]);
                    if (itR != roots.end() && (itR->second.t == Value::TableT || itR->second.t == Value::Class) && itR->second.tab)
                    {
                        Value cur = itR->second;
                        for (size_t i = 1; i < n->names.size(); ++i)
                        {
                            if (cur.t != Value::TableT && cur.t != Value::Class) break;
                            if (!cur.tab) break;
                            auto itSub = cur.tab->map.find(n->names[i]);
                            if (itSub != cur.tab->map.end() && (itSub->second.t == Value::TableT || itSub->second.t == Value::Class) && itSub->second.tab)
                            {
                                if (i + 1 == n->names.size() && itSub->second.t == Value::TableT)
                                    placeholder2 = itSub->second.tab;
                                else
                                    cur = itSub->second;
                            }
                            else break;
                        }
                    }
                }
                if (!placeholder2)
                    placeholder2 = std::make_shared<Table>();
                Value modPlaceholder2 = Value::Table_(placeholder2);
                placeholder2->map[L"self"] = modPlaceholder2;
                modules[file] = modPlaceholder2;
                BindModule(vBind, modPlaceholder2);
                auto modEnv2 = std::make_shared<Env>();
                modEnv2->self = modEnv2;
                modEnv2->parent = globals.self;
                modEnv2->vars[L"owner"] = modPlaceholder2;
                // S-2: the module frame owns its source file, so a runtime
                // error inside it can be attributed to the file the line
                // number really comes from.  An embedded module has no
                // disk path — its identity inside the package is the key.
                modEnv2->srcFile = itE->first;
                bool bIsBuiltin2 = (n->names.size() == 1 && n->names[0] == L"builtin");
                if (!bIsBuiltin2)
                {
                    modEnv2->modTable = placeholder2;
                    modEnv2->bNsBody = true;   // module-level vars are members
                }
                wstring oldModFile2 = g_curModuleFile;
                g_curModuleFile = file;
                Value oldRet = ret;
                int oldR = nReturn, oldB = nBreak, oldC = nContinue;
                nReturn = nBreak = nContinue = 0;
                ret = Value();
                GcFrameScope fsMod2(*this, modEnv2);
                ExecBlock(prog, modEnv2.get());
                bool bTopReturn = (nReturn != 0);
                for (auto& kv : modEnv2->vars)
                    placeholder2->scopeVars[kv.first] = kv.second;
                g_curModuleFile = oldModFile2;
                Value mod2 = ret;
                nReturn = oldR; nBreak = oldB; nContinue = oldC;
                ret = oldRet;
                if (!err->empty())
                    return Value();
                // A module whose top level never executed `return` has no
                // module value of its own: `ret` here is just the value of
                // the LAST statement. Without a top-level return, resolve the
                // namespace path instead.
                if (!bTopReturn)
                    mod2 = Value();
                if (mod2.t != Value::TableT)
                {
                    Value curNs;
                    auto itFirst = globals.vars.find(n->names[0]);
                    if (itFirst != globals.vars.end())
                        curNs = itFirst->second;
                    for (size_t ni = 1; ni < n->names.size(); ++ni)
                    {
                        if (curNs.t == Value::TableT && curNs.tab)
                        {
                            auto itSub = curNs.tab->map.find(n->names[ni]);
                            if (itSub != curNs.tab->map.end())
                                curNs = itSub->second;
                            else
                            {
                                curNs = Value();
                                break;
                            }
                        }
                        else
                        {
                            curNs = Value();
                            break;
                        }
                    }
                    if ((curNs.t == Value::TableT && curNs.tab) || curNs.t == Value::Class || curNs.t == Value::Func)
                        mod2 = curNs;
                    else
                    {
                        for (auto& kv : modEnv2->vars)
                            placeholder2->map[kv.first] = kv.second;
                        mod2 = modPlaceholder2;
                    }
                }
                // lib convention: file a/b/C.<ext> with 'namespace a.b;'
                // + 'class C { }' binds the CLASS, not the namespace
                {
                    size_t sl = file.find_last_of(L"\\/");
                    wstring base = (sl == wstring::npos) ? file
                        : file.substr(sl + 1);
                    base = StripScriptExtension(base);
                    bool bIsFolderModule = false;
                    if (base == L"_")
                    {
                        bIsFolderModule = true;
                        size_t sl2 = file.find_last_of(L"\\/");
                        if (sl2 != wstring::npos && sl2 > 0)
                        {
                            size_t sl3 = file.find_last_of(L"\\/", sl2 - 1);
                            if (sl3 != wstring::npos)
                                base = file.substr(sl3 + 1, sl2 - sl3 - 1);
                        }
                    }
                    if (mod2.t == Value::TableT && mod2.tab)
                    {
                        auto im = mod2.tab->map.find(base);
                        if (im != mod2.tab->map.end() &&
                            (im->second.t == Value::Class ||
                             (im->second.t == Value::Func && !bIsFolderModule)))
                            mod2 = im->second;
                    }
                }
                if (mod2.t == Value::TableT && mod2.tab && mod2.tab != placeholder2)
                {
                    for (auto& kv : mod2.tab->map)
                        placeholder2->map[kv.first] = kv.second;
                }
                modules[file] = mod2;
                return BindModule(vBind, mod2);
            }
            FILE* pf = nullptr;
            if (_wfopen_s(&pf, file.c_str(), L"rb") != 0 || !pf)
            {
                // pure-namespace import: no backing file
                // but a submodule folder exists in baseDir or strLibDir
                wstring relDir;
                for (size_t i = 0; i < n->names.size(); i++)
                {
                    if (i) relDir += L"\\";
                    relDir += n->names[i];
                }
                wstring dirCand = baseDir.empty() ? relDir : baseDir + L"\\" + relDir;
                DWORD attr = GetFileAttributesW(dirCand.c_str());
                if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY))
                {
                    if (!strLibDir.empty())
                    {
                        wstring dirLib = strLibDir + L"\\" + relDir;
                        attr = GetFileAttributesW(dirLib.c_str());
                    }
                }
                if (attr != INVALID_FILE_ATTRIBUTES &&
                    (attr & FILE_ATTRIBUTE_DIRECTORY))
                {
                    auto tbN = std::make_shared<Table>();
                    Value modN = Value::Table_(tbN);
                    modules[file] = modN;
                    return BindModule(vBind, modN);
                }
                (*err) = L"找不到库: " + file;
                return Value();
            }
            std::string raw;
            {
                char buf[4096];
                size_t rd;
                while ((rd = fread(buf, 1, sizeof(buf), pf)) > 0)
                    raw.append(buf, rd);
                fclose(pf);
            }
            { static bool bDbg = GetEnvironmentVariableW(L"CMT", nullptr, 0) != 0;
              if (bDbg) fwprintf(stderr, L"[LOAD] %s\n", file.c_str()); }
            int off = (raw.size() >= 3 && (BYTE)raw[0] == 0xEF &&
                       (BYTE)raw[1] == 0xBB && (BYTE)raw[2] == 0xBF) ? 3 : 0;
            int w = MultiByteToWideChar(CP_UTF8, 0, raw.data() + off,
                                        (int)(raw.size() - off), nullptr, 0);
            wstring src((size_t)w, 0);
            if (w > 0)
                MultiByteToWideChar(CP_UTF8, 0, raw.data() + off,
                                    (int)(raw.size() - off), &src[0], w);
            wstring modErr;
            PNode prog;
            if (IsBytecodeData(raw.data(), raw.size()))
            {
                prog = DeserializeNode((const uint8_t*)raw.data(), raw.size(), &modErr);
            }
            else if (IsCompiledModule(src))
            {
                // A module with a bytecode magic header (for example the
                // official "=aardio" format) is not source text. Its
                // bytecode encoding is not implemented, so report a FORMAT
                // error instead of a misleading syntax error.
                if (IsAardioChunk(raw))
                {
                    // 官方预编译块：容器层可以完整解析，给出结构化的诊断，
                    // 而不是笼统的「不支持的字节码格式」。
                    AardioChunkNode root;
                    AardioChunkStats st;
                    wstring perr;
                    if (ParseAardioChunkTree(raw, &root, &st, &perr))
                    {
                        modErr = AardioChunkDiagnostic(st);
                        // S-1b：再跑一遍官方校验器 FUN_004D4450 的等价实现，
                        // 把「结构是否合规」也写进诊断（官方给不出这份结论）。
                        AardioValidateResult vres;
                        ValidateAardioChunkTree(root, &vres, nullptr);
                        modErr += L"；结构校验：" + std::to_wstring(vres.objects) +
                            L" 个对象 / " + std::to_wstring(vres.records) +
                            L" 条记录 / " + std::to_wstring(vres.errors) + L" 处违规";
                    }
                    else
                    {
                        modErr = L"格式错误: 官方预编译模块 (=aardio) 结构不完整 (" + perr + L")";
                    }
                }
                else
                {
                    modErr = L"格式错误: 不支持的字节码格式 (" + src.substr(0, 16) + L")";
                }
            }
            else
            {
                vector<Tok> toks = Lex(src, &modErr);
                Parser ps{ toks, 0, &modErr };
                {
                    size_t sl = file.find_last_of(L"\\/");
                    ps.strEmbedBase = (sl == wstring::npos) ? wstring()
                        : file.substr(0, sl);
                    ps.strEmbedRoot = EmbedRoot();
                }
                prog = ps.ParseBlock();
            }
            if (!modErr.empty() || !prog)
            {
                (*err) = L"库 " + file + L" 编译失败: " + modErr;
                return Value();
            }
            // Circular import guard: seed a placeholder table before execution
            shared_ptr<Table> placeholder;
            if (n->names.size() == 1)
            {
                auto itR = roots.find(n->names[0]);
                if (itR != roots.end() && itR->second.t == Value::TableT && itR->second.tab)
                    placeholder = itR->second.tab;
            }
            else if (n->names.size() > 1)
            {
                auto itR = roots.find(n->names[0]);
                if (itR != roots.end() && (itR->second.t == Value::TableT || itR->second.t == Value::Class) && itR->second.tab)
                {
                    Value cur = itR->second;
                    for (size_t i = 1; i < n->names.size(); ++i)
                    {
                        if (cur.t != Value::TableT && cur.t != Value::Class) break;
                        if (!cur.tab) break;
                        auto itSub = cur.tab->map.find(n->names[i]);
                        if (itSub != cur.tab->map.end() && (itSub->second.t == Value::TableT || itSub->second.t == Value::Class) && itSub->second.tab)
                        {
                            if (i + 1 == n->names.size() && itSub->second.t == Value::TableT)
                                placeholder = itSub->second.tab;
                            else
                                cur = itSub->second;
                        }
                        else break;
                    }
                }
            }
            if (!placeholder)
                placeholder = std::make_shared<Table>();
            Value modPlaceholder = Value::Table_(placeholder);
            placeholder->map[L"self"] = modPlaceholder;
            modules[file] = modPlaceholder;
            // Single-segment imports must NOT pre-bind the root global:
            // the module's own name is left unbound while
            // the body runs, so a module's `assert(!name)` self-guard
            // sees null. Re-entrant imports still resolve through the
            // modules[] cache above.
            // For dotted paths (e.g. `import a.b.c`), ensure parent namespace
            // tables (`a`, `a.b`) exist mid-body, but do NOT pre-bind the leaf
            // name (`c`), so self-guards like `if(layout)` in `web.layout` see null.
            if (n->names.size() > 1)
            {
                Value ns = GetRoot(n->names[0]);
                for (size_t i = 1; i + 1 < n->names.size(); i++)
                {
                    if (ns.t != Value::TableT || !ns.tab) break;
                    auto it = ns.tab->map.find(n->names[i]);
                    if (it != ns.tab->map.end() && (it->second.t == Value::TableT || it->second.t == Value::Class))
                    {
                        ns = it->second;
                    }
                    else
                    {
                        auto nt = std::make_shared<Table>();
                        Value v = Value::Table_(nt);
                        ns.tab->map[n->names[i]] = v;
                        ns = v;
                    }
                }
            }

            // module runs in a child scope; its `return v` (or its collected
            // top-level globals) becomes the module value
            auto modEnv = std::make_shared<Env>();
            modEnv->self = modEnv;
            modEnv->parent = globals.self;
            modEnv->vars[L"owner"] = modPlaceholder;
            // S-2: the module frame owns its source file — a runtime error
            // inside it reports "<this file>:<line>" instead of a bare line
            // number that could belong to any imported package.
            modEnv->srcFile = file;
            bool bIsBuiltin = (n->names.size() == 1 && n->names[0] == L"builtin");
            if (!bIsBuiltin)
            {
                modEnv->modTable = placeholder;
                modEnv->bNsBody = true;   // module-level vars are members
            }
            wstring oldModFile = g_curModuleFile;
            g_curModuleFile = file;
            if (g_nsTrace)
            {
                wstring pn;
                for (size_t i = 0; i < n->names.size(); i++)
                    pn += (i ? L"." : L"") + n->names[i];
                fwprintf(stderr, L"[IMP] load file='%s' as '%s' placeholder=%p\n",
                         file.c_str(), pn.c_str(), (void*)placeholder.get());
                fflush(stderr);
            }
            Value oldRet = ret;
            int oldR = nReturn, oldB = nBreak, oldC = nContinue;
            nReturn = nBreak = nContinue = 0;
            ret = Value();
            GcFrameScope fsMod(*this, modEnv);
            ExecBlock(prog, modEnv.get());
            bool bTopReturn = (nReturn != 0);
            for (auto& kv : modEnv->vars)
                placeholder->scopeVars[kv.first] = kv.second;
            g_curModuleFile = oldModFile;
            Value mod = ret;
            nReturn = oldR; nBreak = oldB; nContinue = oldC;
            ret = oldRet;
            if (!err->empty())
                return Value();
            // Without a top-level return the module has no value of its
            // own — `ret` is just the last statement's value. Resolve the namespace
            // path instead.
            if (!bTopReturn)
                mod = Value();
            if (mod.t != Value::TableT)
            {
                Value curNs;
                auto itFirst = globals.vars.find(n->names[0]);
                if (itFirst != globals.vars.end())
                    curNs = itFirst->second;
                for (size_t ni = 1; ni < n->names.size(); ++ni)
                {
                    if (curNs.t == Value::TableT && curNs.tab)
                    {
                        auto itSub = curNs.tab->map.find(n->names[ni]);
                        if (itSub != curNs.tab->map.end())
                            curNs = itSub->second;
                        else
                        {
                            curNs = Value();
                            break;
                        }
                    }
                    else
                    {
                        curNs = Value();
                        break;
                    }
                }
                if ((curNs.t == Value::TableT && curNs.tab) || curNs.t == Value::Class || curNs.t == Value::Func)
                    mod = curNs;
                else
                {
                    for (auto& kv : modEnv->vars)
                        placeholder->map[kv.first] = kv.second;
                    mod = modPlaceholder;
                }
            }
            // lib convention: bind a same-named Class/Func member
            {
                size_t sl = file.find_last_of(L"\\/");
                wstring base = (sl == wstring::npos) ? file
                    : file.substr(sl + 1);
                base = StripScriptExtension(base);
                // folder-module convention: a/b/C/_.<ext> binds C
                // (the class declared inside), not '_'
                bool bIsFolderModule = false;
                if (base == L"_")
                {
                    bIsFolderModule = true;
                    size_t sl2 = file.find_last_of(L"\\/");
                    if (sl2 != wstring::npos && sl2 > 0)
                    {
                        size_t sl3 = file.find_last_of(L"\\/", sl2 - 1);
                        if (sl3 != wstring::npos)
                            base = file.substr(sl3 + 1, sl2 - sl3 - 1);
                    }
                }
                Value cand = mod;
                if (cand.t == Value::TableT && cand.tab)
                {
                    auto im = cand.tab->map.find(base);
                    if (im != cand.tab->map.end() &&
                        (im->second.t == Value::Class ||
                         (im->second.t == Value::Func && !bIsFolderModule &&
                          !cand.tab->map.count(L"_meta") && cand.tab->map.size() <= 2)))
                        mod = im->second;
                    else
                    {
                        // 'namespace X;' files declare into the global
                        // namespace chain: probe globals.X1.X2...base
                        Value ns = Value();
                        for (size_t ni = 0; ni + 1 < n->names.size(); ni++)
                        {
                            const wstring& nm = n->names[ni];
                            if (ns.t != Value::TableT || !ns.tab)
                            {
                                auto ig = globals.vars.find(nm);
                                if (ig != globals.vars.end())
                                    ns = ig->second;
                                else break;
                            }
                            else
                            {
                                auto it2 = ns.tab->map.find(nm);
                                if (it2 != ns.tab->map.end())
                                    ns = it2->second;
                                else break;
                            }
                        }
                        if (ns.t == Value::TableT && ns.tab)
                        {
                            auto im2 = ns.tab->map.find(base);
                            if (im2 != ns.tab->map.end() &&
                                (im2->second.t == Value::Class ||
                                 (im2->second.t == Value::Func && !ns.tab->map.count(L"_meta") && ns.tab->map.size() <= 2)))
                                mod = im2->second;
                        }
                    }
                }
            }
            {
                static bool bDbg = GetEnvironmentVariableW(L"CMT", nullptr, 0) != 0;
                if (bDbg)
                {
                    wstring b2 = file;
                    size_t s2 = b2.find_last_of(L"\\/");
                    if (s2 != wstring::npos) b2 = b2.substr(s2 + 1);
                    fwprintf(stderr, L"[BIND] file=%s type=%d hasbase=%d\n",
                             file.c_str(), (int)mod.t,
                             (int)(mod.t == Value::TableT && mod.tab &&
                                   mod.tab->map.count(
                                       b2.substr(0, b2.size() - 7))));
                }
            }
            if (mod.t == Value::TableT && mod.tab && mod.tab != placeholder)
            {
                for (auto& kv : mod.tab->map)
                    placeholder->map[kv.first] = kv.second;
            }
            modules[file] = mod;
            return BindModule(vBind, mod);
        }

        // invoke a VM function value from native code (callback dispatch);
        // saves/restores interp state so re-entrant calls are safe
}
