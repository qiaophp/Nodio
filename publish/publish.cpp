// publish.cpp — Nodio standalone publisher (Phase 2).
//
// Pipeline:
//   1. read the .aproj project file next to the entry script (optional):
//      <file path= embed= ignored= local=true> entries
//   2. syntax-gate the entry script AND the import closure (CheckSyntax)
//   3. collect payload files BYTE-EXACT (never re-encoded): the entry
//      script + its `import` closure from the lib root + embedded res
//   4. copy the running host exe to <out>.exe and append the payload:
//
//        [files...] [manifest] [trailer]
//        trailer = { u64 magic 'NODIOPUB', u32 version, u32 count,
//                    u64 manifestSize, u64 payloadSize }
//
// At startup the host probes its own file end for the trailer: present →
// run the embedded entry script with the embedded file map (no external
// lib tree needed); absent → normal dev-mode host behaviour.
//
// Binary safety: every file is stored as raw bytes; the entry script is
// UTF-8 decoded only for execution. Embedded keys use lib-relative paths
// ("win\\_.aardio", "res\\a.png") so the engine's embedded lookup works.

#include <windows.h>
#include "engine/NodioVM.h"
#include "publish/publish.h"
#include <string>
#include <vector>
#include <map>
#include <set>

namespace NodioPublish
{
    // ---- payload trailer -------------------------------------------------

    static const unsigned long long kMagic = 0x4255504F49444F4EULL; // "NODIOPUB"
    static const unsigned int kVersion = 1;

    struct Trailer
    {
        unsigned long long magic;
        unsigned int version;
        unsigned int count;
        unsigned long long manifestSize;
        unsigned long long payloadSize;
    };

    static bool ReadWholeFile(const std::wstring& path,
                              std::vector<char>& out)
    {
        FILE* pf = nullptr;
        if (_wfopen_s(&pf, path.c_str(), L"rb") != 0 || !pf)
            return false;
        fseek(pf, 0, SEEK_END);
        long cb = ftell(pf);
        fseek(pf, 0, SEEK_SET);
        out.resize(cb > 0 ? (size_t)cb : 0);
        size_t rd = out.empty() ? 0 : fread(out.data(), 1, out.size(), pf);
        fclose(pf);
        out.resize(rd);
        return true;
    }

    static bool WriteWholeFile(const std::wstring& path,
                               const std::vector<char>& data)
    {
        FILE* pf = nullptr;
        if (_wfopen_s(&pf, path.c_str(), L"wb") != 0 || !pf)
            return false;
        if (!data.empty())
            fwrite(data.data(), 1, data.size(), pf);
        fclose(pf);
        return true;
    }

    static std::wstring DirOf(const std::wstring& path)
    {
        size_t sl = path.find_last_of(L"\\/");
        return sl == std::wstring::npos ? L"" : path.substr(0, sl);
    }

    static std::wstring BaseOf(const std::wstring& path)
    {
        size_t sl = path.find_last_of(L"\\/");
        return sl == std::wstring::npos ? path : path.substr(sl + 1);
    }

    static std::wstring LowerExt(const std::wstring& path)
    {
        size_t d = path.find_last_of(L'.');
        if (d == std::wstring::npos) return L"";
        std::wstring e = path.substr(d);
        for (auto& c : e) c = towlower(c);
        return e;
    }

    static std::wstring ReadTextUtf8(const std::wstring& path)
    {
        std::vector<char> raw;
        if (!ReadWholeFile(path, raw)) return L"";
        size_t off = (raw.size() >= 3 && (unsigned char)raw[0] == 0xEF &&
                      (unsigned char)raw[1] == 0xBB &&
                      (unsigned char)raw[2] == 0xBF) ? 3 : 0;
        int w = MultiByteToWideChar(CP_UTF8, 0, raw.data() + off,
                                    (int)(raw.size() - off), nullptr, 0);
        std::wstring s((size_t)max(w, 0), 0);
        if (w > 0)
            MultiByteToWideChar(CP_UTF8, 0, raw.data() + off,
                                (int)(raw.size() - off), &s[0], w);
        return s;
    }

    static std::wstring JoinPath(const std::wstring& a,
                                 const std::wstring& b)
    {
        if (a.empty()) return b;
        if (b.empty()) return a;
        wchar_t back = a.back();
        if (back == L'\\' || back == L'/') return a + b;
        return a + L"\\" + b;
    }

    // ---- .aproj ----------------------------------------------------------

    struct ProjFile
    {
        std::wstring strPath;   // relative to the project dir
        bool bEmbed = false;
        bool bIgnored = false;
        bool bLocal = false;
    };

    // Minimal XML scan: <file ... /> attributes path= embed= ignored= local=
    static void ParseAproj(const std::wstring& xml,
                           std::vector<ProjFile>& out)
    {
        size_t pos = 0;
        while (true)
        {
            size_t lt = xml.find(L"<file", pos);
            if (lt == std::wstring::npos) break;
            size_t gt = xml.find(L'>', lt);
            if (gt == std::wstring::npos) break;
            std::wstring tag = xml.substr(lt, gt - lt);
            pos = gt + 1;

            ProjFile f;
            auto attr = [&tag](const wchar_t* name) -> std::wstring
            {
                std::wstring key = name;
                size_t k = tag.find(key);
                if (k == std::wstring::npos) return L"";
                size_t q = tag.find(L'"', k);
                if (q == std::wstring::npos) return L"";
                size_t q2 = tag.find(L'"', q + 1);
                if (q2 == std::wstring::npos) return L"";
                return tag.substr(q + 1, q2 - q - 1);
            };
            f.strPath = attr(L"path=");
            auto truthy = [](const std::wstring& v) { return v == L"true" || v == L"1"; };
            f.bEmbed = truthy(attr(L"embed="));
            f.bIgnored = truthy(attr(L"ignored="));
            f.bLocal = truthy(attr(L"local="));
            if (!f.strPath.empty())
                out.push_back(f);
        }
    }

    // ---- import closure --------------------------------------------------

    // Scans source for `import a.b.c` / `import ..a.b` / `import a` and
    // resolves each name to a lib file (aardio / nodio, folder layout
    // preferred). Recursive with a visited set.
    static void CollectClosure(const std::wstring& src,
                               const std::wstring& strLibDir,
                               std::map<std::wstring, std::wstring>& files,
                               std::set<std::wstring>& visited,
                               int depth)
    {
        if (depth > 24) return;
        size_t pos = 0;
        while (true)
        {
            size_t k = src.find(L"import", pos);
            if (k == std::wstring::npos) break;
            // word boundary
            if (k > 0)
            {
                wchar_t c = src[k - 1];
                if (iswalnum(c) || c == L'_' || c == L'$') { pos = k + 6; continue; }
            }
            pos = k + 6;
            // skip spaces / dots (root prefix)
            while (pos < src.size() &&
                   (src[pos] == L' ' || src[pos] == L'\t' || src[pos] == L'.'))
                pos++;
            // read dotted name
            std::wstring name;
            while (pos < src.size() &&
                   (iswalnum(src[pos]) || src[pos] == L'_' ||
                    src[pos] == L'.' || src[pos] == L'$'))
            {
                name += src[pos];
                pos++;
            }
            if (name.empty() || name[0] == L'.') continue;

            std::wstring rel = name;
            for (auto& c : rel) if (c == L'.') c = L'\\';
            // visit key includes the search root: the same import
            // is looked up under BOTH the global lib root and the
            // project lib, so it must not be marked visited before
            // the lookup succeeds in either
            std::wstring visitKey = strLibDir + L"|" + rel;
            if (visited.count(visitKey)) continue;

            const wchar_t* exts[] = { L"aardio", L"nodio" };
            const bool alts[] = { true, false };
            std::wstring found;
            for (bool bAlt : alts)
            {
                for (const wchar_t* ext : exts)
                {
                    std::wstring cand = JoinPath(strLibDir, rel) +
                        (bAlt ? (std::wstring(L"\\_.") + ext)
                              : (std::wstring(L".") + ext));
                    if (GetFileAttributesW(cand.c_str()) != INVALID_FILE_ATTRIBUTES)
                    {
                        found = cand;
                        break;
                    }
                }
                if (!found.empty()) break;
            }
            if (found.empty()) continue;

            std::wstring key = rel +
                (found.find(L"\\_.") != std::wstring::npos
                     ? (std::wstring(L"\\_") + LowerExt(found))
                     : LowerExt(found));
            visited.insert(visitKey);
            std::vector<char> bytes;
            if (!ReadWholeFile(found, bytes)) continue;
            {
                fwprintf(stderr, L"[PUBDBG] import %s -> key=%s file=%s\n",
                         name.c_str(), key.c_str(), found.c_str());
                fflush(stderr);
            }
            files[key] = found;
            // recurse into nested imports
            CollectClosure(ReadTextUtf8(found), strLibDir, files, visited,
                           depth + 1);
        }
    }

    // ---- PE stamping -----------------------------------------------------

    // Patch OptionalHeader.Subsystem in the OUTPUT exe: 2 = GUI, 3 = CUI.
    // (The host itself is a console binary; GUI publishes hide the console.)
    static bool PatchSubsystem(const std::wstring& exePath, bool bConsole)
    {
        std::vector<char> b;
        if (!ReadWholeFile(exePath, b) || b.size() < 0x40) return false;
        if (*(unsigned short*)&b[0] != 0x5A4D) return false;   // 'MZ'
        DWORD peOff = *(DWORD*)&b[0x3C];
        if ((size_t)peOff + 0x5C > b.size()) return false;
        if (*(unsigned char*)&b[peOff] != 'P' ||
            *(unsigned char*)&b[peOff + 1] != 'E') return false;
        size_t opt = (size_t)peOff + 24;
        WORD magic = *(WORD*)&b[opt];
        size_t sub = opt + 68;   // Subsystem offset, PE32 and PE32+ alike
        if (sub + 2 > b.size()) return false;
        if (magic != 0x10B && magic != 0x20B) return false;
        *(WORD*)&b[sub] = bConsole ? 3 : 2;
        return WriteWholeFile(exePath, b);
    }

    static bool StampIcon(const std::wstring& exePath,
                          const std::wstring& icoPath)
    {
        std::vector<char> ico;
        if (!ReadWholeFile(icoPath, ico) || ico.size() < 6)
            return false;
        // .ico container: ICONDIR(6) + ICONDIRENTRY(16 each)
        WORD count = *(WORD*)(ico.data() + 4);
        if (count == 0 || ico.size() < (size_t)6 + (size_t)16 * count)
            return false;

        HANDLE hUpd = BeginUpdateResourceW(exePath.c_str(), FALSE);
        if (!hUpd) return false;
        bool bOk = true;
        for (WORD i = 0; i < count; i++)
        {
            const unsigned char* e =
                (const unsigned char*)ico.data() + 6 + 16 * i;
            DWORD cb = *(DWORD*)(e + 8);
            DWORD off = *(DWORD*)(e + 12);
            if ((size_t)off + cb > ico.size()) { bOk = false; break; }
            if (!UpdateResourceW(hUpd, RT_ICON, MAKEINTRESOURCEW(i + 1),
                                 MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL),
                                 (LPVOID)(ico.data() + off), cb))
                bOk = false;
        }
        if (bOk)
        {
            // group icon referencing the RT_ICON entries
            std::vector<unsigned char> grp(6 + 14 * count, 0);
            *(WORD*)&grp[0] = 0;          // reserved
            *(WORD*)&grp[2] = 1;          // type = icon
            *(WORD*)&grp[4] = count;
            for (WORD i = 0; i < count; i++)
            {
                const unsigned char* e =
                    (const unsigned char*)ico.data() + 6 + 16 * i;
                unsigned char* g = &grp[6 + 14 * i];
                memcpy(g, e, 12);         // w,h,colors,reserved,planes,bpp
                *(WORD*)(g + 12) = i + 1; // RT_ICON id
            }
            if (!UpdateResourceW(hUpd, RT_GROUP_ICON, MAKEINTRESOURCEW(1),
                                 MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL),
                                 grp.data(), (DWORD)grp.size()))
                bOk = false;
        }
        EndUpdateResourceW(hUpd, bOk ? FALSE : TRUE);
        return bOk;
    }

    // ---- main pipeline ---------------------------------------------------

    bool Publish(const Options& opt, std::wstring& strErr)
    {
        if (GetFileAttributesW(opt.strEntry.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            strErr = L"找不到启动脚本: " + opt.strEntry;
            return false;
        }
        std::wstring strProjDir = DirOf(opt.strEntry);

        // 1) .aproj
        std::vector<ProjFile> projFiles;
        {
            std::wstring aproj = JoinPath(strProjDir, L"default.aproj");
            if (GetFileAttributesW(aproj.c_str()) != INVALID_FILE_ATTRIBUTES)
                ParseAproj(ReadTextUtf8(aproj), projFiles);
        }

        // 2) syntax gate: entry + every embedded script
        std::wstring srcMain = ReadTextUtf8(opt.strEntry);
        {
            std::wstring se;
            if (NodioVM::CheckSyntax(srcMain, &se) != 0)
            {
                strErr = L"启动脚本语法错误: " + se;
                return false;
            }
        }

        std::map<std::wstring, std::wstring> colFiles;   // key -> abs path
        std::set<std::wstring> visited;
        if (GetEnvironmentVariableW(L"NODIO_PUBDBG", nullptr, 0))
        {
            fwprintf(stderr, L"[PUBDBG] srcMain=%d libdir=%s\n",
                     (int)srcMain.size(), opt.strLibDir.c_str());
            fflush(stderr);
        }
        if (!opt.strLibDir.empty())
            CollectClosure(srcMain, opt.strLibDir, colFiles, visited, 0);
        // project-local lib: proj\lib\X.aardio == import X (official
        // project layout), resolved after the global lib root
        {
            std::wstring strProjLib = JoinPath(strProjDir, L"lib");
            if (GetFileAttributesW(strProjLib.c_str()) != INVALID_FILE_ATTRIBUTES)
                CollectClosure(srcMain, strProjLib, colFiles, visited, 0);
        }
        fwprintf(stderr, L"[PUBDBG] closure=%d libdir=%s srclen=%d\n",
                 (int)colFiles.size(), opt.strLibDir.c_str(),
                 (int)srcMain.size());
        fflush(stderr);

        // embedded resources per .aproj (embed=true, ignored=false)
        for (auto& f : projFiles)
        {
            if (f.bIgnored || !f.bEmbed) continue;
            std::wstring abs = JoinPath(strProjDir, f.strPath);
            std::wstring key = f.strPath;
            for (auto& c : key) if (c == L'/') c = L'\\';
            colFiles[key] = abs;
        }

        // 3) payload: entry as "\0main"? keep entry under its file name
        std::map<std::wstring, std::string> payload;   // key -> bytes
        {
            std::vector<char> bytes;
            if (!ReadWholeFile(opt.strEntry, bytes)) { strErr = L"读取启动脚本失败"; return false; }
            payload[BaseOf(opt.strEntry)] =
                std::string(bytes.data(), bytes.size());
        }
        for (auto& kv : colFiles)
        {
            std::vector<char> bytes;
            if (!ReadWholeFile(kv.second, bytes)) continue;
            std::wstring key = kv.first;
            payload[key] = std::string(bytes.data(), bytes.size());
        }

        // manifest: "key\t<size>\n" lines in UTF-8
        std::string manifest;
        unsigned long long payloadSize = 0;
        for (auto& kv : payload)
        {
            int n = WideCharToMultiByte(CP_UTF8, 0, kv.first.c_str(),
                                        (int)kv.first.size(), nullptr, 0,
                                        nullptr, nullptr);
            std::string u8((size_t)max(n, 0), 0);
            if (n > 0)
                WideCharToMultiByte(CP_UTF8, 0, kv.first.c_str(),
                                    (int)kv.first.size(), &u8[0], n,
                                    nullptr, nullptr);
            manifest += u8 + "\t" + std::to_string(kv.second.size()) + "\n";
            payloadSize += kv.second.size();
        }

        // 4) copy the host exe and append the payload
        std::vector<char> exe;
        {
            wchar_t szSelf[MAX_PATH] = L"";
            GetModuleFileNameW(nullptr, szSelf, MAX_PATH);
            if (!ReadWholeFile(szSelf, exe))
            {
                strErr = L"读取宿主程序失败";
                return false;
            }
        }
        for (auto& kv : payload)
            exe.insert(exe.end(), kv.second.begin(), kv.second.end());
        exe.insert(exe.end(), manifest.begin(), manifest.end());
        Trailer tr{};
        tr.magic = kMagic;
        tr.version = kVersion;
        tr.count = (unsigned int)payload.size();
        tr.manifestSize = manifest.size();
        tr.payloadSize = payloadSize;
        const char* pTr = (const char*)&tr;
        exe.insert(exe.end(), pTr, pTr + sizeof(Trailer));

        if (!WriteWholeFile(opt.strOut, exe))
        {
            strErr = L"写入输出失败: " + opt.strOut;
            return false;
        }

        PatchSubsystem(opt.strOut, opt.bConsole);

        if (!opt.strIcon.empty())
            StampIcon(opt.strOut, opt.strIcon);   // best-effort

        return true;
    }
}
