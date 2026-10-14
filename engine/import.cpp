#include "engine/interp.h"
#include "engine/parser.h"

namespace NodioVM
{
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
                            // the module body already built it (win.form
                            // is "class win.form" inside win/ui/_.aardio,
                            // and the delegator file win/form.aardio — one
                            // `import win.ui;` line — caches the
                            // delegatee's module table as its own value).
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
            // M7: global lib root — the ORIGINAL aardio install's lib\
            // (plaintext source), enabled via NODIO_LIBDIR; resolution
            // order: project baseDir first, then the lib root
            static const wstring strLibDir = []
            {
                wchar_t szLib[MAX_PATH] = L"";
                GetEnvironmentVariableW(L"NODIO_LIBDIR", szLib, MAX_PATH);
                return wstring(szLib);
            }();
            // import resolution accepts both .aardio (official ecosystem)
            // and .nodio (native) extensions; each root prefers the
            // folder-module layout (rel\_.ext) over the flat file
            // (rel.ext), and the aardio extension over nodio
            auto LibCand = [&](const wstring& relX, bool bAlt,
                               const wchar_t* ext) -> wstring
            {
                if (strLibDir.empty()) return L"";
                return strLibDir + L"\\" + relX +
                    (bAlt ? (wstring(L"\\_.") + ext)
                          : (wstring(L".") + ext));
            };
            auto Exists = [](const wstring& p) -> bool
            {
                return !p.empty() && _waccess(p.c_str(), 0) == 0;
            };
            // 1) pre-bound builtin namespaces resolve immediately —
            // but a matching lib FILE merges into the namespace first
            // (aardio: import X loads lib source into namespace X)
            {
                wstring relT;
                for (size_t i = 0; i < n->names.size(); i++)
                {
                    if (i) relT += L"\\";
                    relT += n->names[i];
                }
                bool bAnyFile =
                    Exists(baseDir.empty() ? relT + L".aardio"
                                           : baseDir + L"\\" + relT + L".aardio") ||
                    Exists(baseDir.empty() ? relT + L".nodio"
                                           : baseDir + L"\\" + relT + L".nodio") ||
                    Exists(baseDir.empty() ? relT + L"\\_.aardio"
                                           : baseDir + L"\\" + relT + L"\\_.aardio") ||
                    Exists(baseDir.empty() ? relT + L"\\_.nodio"
                                           : baseDir + L"\\" + relT + L"\\_.nodio") ||
                    Exists(LibCand(relT, false, L"aardio")) ||
                    Exists(LibCand(relT, false, L"nodio")) ||
                    Exists(LibCand(relT, true, L"aardio")) ||
                    Exists(LibCand(relT, true, L"nodio"));
                // 0) engine-provided modules with no backing file
                if (n->names.size() == 1)
                {
                    auto itN = namedModules.find(n->names[0]);
                    if (itN != namedModules.end())
                        return itN->second;
                }
                static bool bDbgI = GetEnvironmentVariableW(L"CMT", nullptr, 0) != 0;
                if (bDbgI)
                {
                    wstring full2;
                    for (size_t i = 0; i < n->names.size(); ++i) { if (i) full2 += L"."; full2 += n->names[i]; }
                    fwprintf(stderr, L"[IMPORT] %s inRoots=%d anyFile=%d\n", full2.c_str(),
                             (int)roots.count(n->names[0]), (int)bAnyFile);
                    fflush(stderr);
                }
                if (roots.count(n->names[0]) &&
                    !embedded.count(relT + L".aardio") &&
                    !embedded.count(relT + L"\\_.aardio") &&
                    !embedded.count(relT + L".nodio") &&
                    !embedded.count(relT + L"\\_.nodio") &&
                    !bAnyFile)
                    return roots[n->names[0]];
            }
            // 2) file module: baseDir\a\b\c.aardio for `import a.b.c`
            wstring rel;
            for (size_t i = 0; i < n->names.size(); i++)
            {
                if (i) rel += L"\\";
                rel += n->names[i];
            }
            // candidate priority: baseDir first, then the lib root;
            // within a root: rel\_.aardio > rel.aardio >
            //                rel\_.nodio > rel.nodio
            wstring file;
            {
                wstring c0 = baseDir.empty() ? wstring()
                    : baseDir + L"\\" + rel + L"\\_.aardio";
                wstring c1 = baseDir.empty() ? wstring()
                    : baseDir + L"\\" + rel + L".aardio";
                wstring c2 = baseDir.empty() ? wstring()
                    : baseDir + L"\\" + rel + L"\\_.nodio";
                wstring c3 = baseDir.empty() ? wstring()
                    : baseDir + L"\\" + rel + L".nodio";
                if (Exists(c0)) file = c0;
                else if (Exists(c1)) file = c1;
                else if (Exists(c2)) file = c2;
                else if (Exists(c3)) file = c3;
                else if (Exists(LibCand(rel, true, L"aardio")))
                    file = LibCand(rel, true, L"aardio");
                else if (Exists(LibCand(rel, false, L"aardio")))
                    file = LibCand(rel, false, L"aardio");
                else if (Exists(LibCand(rel, true, L"nodio")))
                    file = LibCand(rel, true, L"nodio");
                else if (Exists(LibCand(rel, false, L"nodio")))
                    file = LibCand(rel, false, L"nodio");
                else
                    file = baseDir.empty() ? rel + L".aardio"
                                           : baseDir + L"\\" + rel + L".aardio";
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
                BindModule(n->names, it->second);
                return it->second;
            }
            // payload v2: embedded project files resolve before disk
            // (published exe: libEmbed closure ships under lib-relative
            // keys — "win\_.aardio" — so try both lib layouts, both
            // extensions)
            auto itE = embedded.find(rel + L".aardio");
            if (itE == embedded.end())
                itE = embedded.find(rel + L"\\_.aardio");
            if (itE == embedded.end())
                itE = embedded.find(rel + L".nodio");
            if (itE == embedded.end())
                itE = embedded.find(rel + L"\\_.nodio");
            if (itE != embedded.end())
            {
                wstring src = itE->second;
                wstring modErr;
                vector<Tok> toks = Lex(src, &modErr);
                Parser ps{ toks, 0, &modErr };
                ps.strEmbedBase = baseDir;
                ps.strEmbedRoot = EmbedRoot();
                PNode prog = ps.ParseBlock();
                if (!modErr.empty())
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
                BindModule(n->names, modPlaceholder2);
                Env modEnv2;
                modEnv2.parent = &globals;
                modEnv2.modTable = placeholder2;
                modEnv2.bNsBody = true;   // module-level vars are members
                wstring oldModFile2 = g_curModuleFile;
                g_curModuleFile = file;
                Value oldRet = ret;
                int oldR = nReturn, oldB = nBreak, oldC = nContinue;
                nReturn = nBreak = nContinue = 0;
                ret = Value();
                ExecBlock(prog, &modEnv2);
                bool bTopReturn = (nReturn != 0);
                for (auto& kv : modEnv2.vars)
                    placeholder2->scopeVars[kv.first] = kv.second;
                g_curModuleFile = oldModFile2;
                Value mod2 = ret;
                nReturn = oldR; nBreak = oldB; nContinue = oldC;
                ret = oldRet;
                if (!err->empty())
                    return Value();
                // A module whose top level never executed `return` has no
                // module value of its own: `ret` here is just the value of
                // the LAST statement — for process/_.aardio that is
                // `import process.job.limitKill;`, whose module table then
                // got bound AS `process`, replacing the class the file
                // declared. Without a top-level return, resolve the
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
                        for (auto& kv : modEnv2.vars)
                            placeholder2->map[kv.first] = kv.second;
                        mod2 = modPlaceholder2;
                    }
                }
                // lib convention: file a/b/C.aardio with 'namespace a.b;'
                // + 'class C { }' binds the CLASS, not the namespace
                {
                    size_t sl = file.find_last_of(L"\\/");
                    wstring base = (sl == wstring::npos) ? file
                        : file.substr(sl + 1);
                    if (base.size() > 7 &&
                        base.substr(base.size() - 7) == L".aardio")
                        base = base.substr(0, base.size() - 7);
                    if (base.size() > 6 &&
                        base.substr(base.size() - 6) == L".nodio")
                        base = base.substr(0, base.size() - 6);
                    if (mod2.t == Value::TableT && mod2.tab)
                    {
                        auto im = mod2.tab->map.find(base);
                        if (im != mod2.tab->map.end() &&
                            (im->second.t == Value::Class ||
                             im->second.t == Value::Func))
                            mod2 = im->second;
                    }
                }
                if (mod2.t == Value::TableT && mod2.tab && mod2.tab != placeholder2)
                {
                    for (auto& kv : mod2.tab->map)
                        placeholder2->map[kv.first] = kv.second;
                }
                modules[file] = mod2;
                return BindModule(n->names, mod2);
            }
            FILE* pf = nullptr;
            if (_wfopen_s(&pf, file.c_str(), L"rb") != 0 || !pf)
            {
                // pure-namespace import: no <name>.aardio / _\_.aardio
                // but a submodule folder exists (com/, util/...) in baseDir or strLibDir
                wstring dirCand = file.substr(0, file.size() - 7);
                DWORD attr = GetFileAttributesW(dirCand.c_str());
                if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY))
                {
                    if (!strLibDir.empty())
                    {
                        wstring dirLib = strLibDir + L"\\" + rel;
                        attr = GetFileAttributesW(dirLib.c_str());
                    }
                }
                if (attr != INVALID_FILE_ATTRIBUTES &&
                    (attr & FILE_ATTRIBUTE_DIRECTORY))
                {
                    auto tbN = std::make_shared<Table>();
                    Value modN = Value::Table_(tbN);
                    modules[file] = modN;
                    return BindModule(n->names, modN);
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
            vector<Tok> toks;
            if (IsCompiledModule(src))
            {
                (*err) = L"库 " + file +
                    L" 是已编译模块 (字节码)，当前版本不支持加载";
                return Value();
            }
            toks = Lex(src, &modErr);
            Parser ps{ toks, 0, &modErr };
            {
                size_t sl = file.find_last_of(L"\\/");
                ps.strEmbedBase = (sl == wstring::npos) ? wstring()
                    : file.substr(0, sl);
                ps.strEmbedRoot = EmbedRoot();
            }
            PNode prog = ps.ParseBlock();
            if (!modErr.empty())
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
            BindModule(n->names, modPlaceholder);

            // module runs in a child scope; its `return v` (or its collected
            // top-level globals) becomes the module value
            Env modEnv;
            modEnv.parent = &globals;
            modEnv.modTable = placeholder;
            modEnv.bNsBody = true;   // module-level vars are members
            wstring oldModFile = g_curModuleFile;
            g_curModuleFile = file;
            Value oldRet = ret;
            int oldR = nReturn, oldB = nBreak, oldC = nContinue;
            nReturn = nBreak = nContinue = 0;
            ret = Value();
            ExecBlock(prog, &modEnv);
            bool bTopReturn = (nReturn != 0);
            for (auto& kv : modEnv.vars)
                placeholder->scopeVars[kv.first] = kv.second;
            g_curModuleFile = oldModFile;
            Value mod = ret;
            nReturn = oldR; nBreak = oldB; nContinue = oldC;
            ret = oldRet;
            if (!err->empty())
                return Value();
            // Without a top-level return the module has no value of its
            // own — `ret` is just the last statement's value (for
            // process/_.aardio, the process.job.limitKill module table,
            // which then got bound AS `process`). Resolve the namespace
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
                    for (auto& kv : modEnv.vars)
                        placeholder->map[kv.first] = kv.second;
                    mod = modPlaceholder;
                }
            }
            // lib convention: bind a same-named Class/Func member
            {
                size_t sl = file.find_last_of(L"\\/");
                wstring base = (sl == wstring::npos) ? file
                    : file.substr(sl + 1);
                if (base.size() > 7 &&
                    base.substr(base.size() - 7) == L".aardio")
                    base = base.substr(0, base.size() - 7);
                if (base.size() > 6 &&
                    base.substr(base.size() - 6) == L".nodio")
                    base = base.substr(0, base.size() - 6);
                // folder-module convention: a/b/C/_.aardio binds C
                // (the class declared inside), not '_'
                if (base == L"_")
                {
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
                         im->second.t == Value::Func))
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
                                 im2->second.t == Value::Func))
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
            return BindModule(n->names, mod);
        }

        // invoke a VM function value from native code (callback dispatch);
        // saves/restores interp state so re-entrant calls are safe
}
