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

    // Patch OptionalHeader.Subsystem in an in-memory image: 2 = GUI,
    // 3 = CUI. (The host itself is a console binary; GUI publishes hide
    // the console.) The file-based variant is kept for direct callers.
    static bool PatchSubsystemBuf(std::vector<char>& b, bool bConsole)
    {
        if (b.size() < 0x40) return false;
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
        return true;
    }

    static bool PatchSubsystem(const std::wstring& exePath, bool bConsole)
    {
        std::vector<char> b;
        if (!ReadWholeFile(exePath, b)) return false;
        if (!PatchSubsystemBuf(b, bConsole)) return false;
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

    // ---- resource directory helpers -------------------------------------
    //
    // UpdateResourceW cannot be used here: replacing RT_MANIFEST /
    // RT_VERSION on an exe that already carries them produces a resource
    // section the Windows loader then rejects ("side-by-side
    // configuration is incorrect"). So the publisher rewrites the resource
    // bytes in place: walk the .rsrc directory to the leaf entry, then
    // overwrite the data blob and patch the leaf's size. The new blob must
    // fit inside the existing raw section, which it does because the
    // resources are padded to 4-byte alignment inside a 4 KiB-aligned
    // section.

    struct RsrcLoc
    {
        size_t secRaw = 0;      // file offset of .rsrc raw data
        size_t secRva = 0;      // RVA of .rsrc
        size_t leafOff = 0;     // file offset of the IMAGE_RESOURCE_DATA_ENTRY
    };

    static bool FindRsrcLeaf(std::vector<char>& b, WORD wType, WORD wName,
                             RsrcLoc* out)
    {
        if (b.size() < 0x40) return false;
        if (*(unsigned short*)&b[0] != 0x5A4D) return false;      // 'MZ'
        DWORD peOff = *(DWORD*)&b[0x3C];
        if ((size_t)peOff + 0x5C > b.size()) return false;
        if (memcmp(&b[peOff], "PE\0\0", 4) != 0) return false;
        size_t opt = (size_t)peOff + 24;
        WORD magic = *(WORD*)&b[opt];
        if (magic != 0x10B && magic != 0x20B) return false;
        // DataDirectory[2] = resource. The base offset of the array is
        // opt+96 for PE32 and opt+112 for PE32+; each entry is 8 bytes.
        size_t dd = opt + (magic == 0x20B ? 112 : 96) + (size_t)2 * 8;
        DWORD rva = *(DWORD*)&b[dd];
        DWORD sz = *(DWORD*)&b[dd + 4];
        if (!rva || !sz) return false;
        WORD nSec = *(WORD*)&b[(size_t)peOff + 6];
        size_t sh = opt + 240;
        size_t raw = 0;
        for (WORD i = 0; i < nSec; i++)
        {
            size_t e = sh + (size_t)i * 40;
            DWORD va = *(DWORD*)&b[e + 12];
            DWORD vs = *(DWORD*)&b[e + 8];
            if (rva >= va && rva < va + vs)
            {
                raw = *(DWORD*)&b[e + 20] + (rva - va);
                out->secRva = rva;
                break;
            }
        }
        if (!raw || raw >= b.size()) return false;
        out->secRaw = raw;

        // IMAGE_RESOURCE_DIRECTORY: 16-byte header, then entries of
        // {DWORD nameOrId, DWORD offsetToData} where the high bit marks a
        // subdirectory. Walk type -> name -> language.
        auto DirEntries = [&](size_t dirOff, WORD* pnNamed, WORD* pnId) {
            *pnNamed = *(WORD*)&b[dirOff + 12];
            *pnId = *(WORD*)&b[dirOff + 14];
            return dirOff + 16;
        };
        WORD nN = 0, nI = 0;
        size_t e = DirEntries(raw, &nN, &nI);
        size_t sub = 0;
        for (WORD i = 0; i < nN + nI && !sub; i++)
        {
            DWORD id = *(DWORD*)&b[e + (size_t)i * 8];
            DWORD off = *(DWORD*)&b[e + (size_t)i * 8 + 4];
            if (!(off & 0x80000000)) continue;
            if (nN && i < nN) continue;         // skip named entries
            if ((WORD)id == wType) sub = raw + (off & 0x7FFFFFFF);
        }
        if (!sub) return false;
        e = DirEntries(sub, &nN, &nI);
        size_t sub2 = 0;
        for (WORD i = 0; i < nN + nI && !sub2; i++)
        {
            DWORD id = *(DWORD*)&b[e + (size_t)i * 8];
            DWORD off = *(DWORD*)&b[e + (size_t)i * 8 + 4];
            if (!(off & 0x80000000)) continue;
            if (nN && i < nN) continue;
            if ((WORD)id == wName) sub2 = raw + (off & 0x7FFFFFFF);
        }
        if (!sub2) return false;
        e = DirEntries(sub2, &nN, &nI);
        for (WORD i = 0; i < nN + nI; i++)
        {
            DWORD off = *(DWORD*)&b[e + (size_t)i * 8 + 4];
            if (off & 0x80000000) continue;     // not a leaf
            out->leafOff = raw + off;
            return true;
        }
        return false;
    }

    // Replaces the data blob of an existing resource. The new blob must fit
    // in the space the old one occupied (resources sit back to back inside
    // .rsrc, so overrunning would clobber the next one).
    //
    // The slot is filled to EXACTLY its original size: the loader rejects an
    // exe whose resource data is shorter than the directory entry records,
    // so a shorter blob is padded (with spaces for the XML manifest, zeros
    // elsewhere) up to the slot size. cbPad is the filler byte.
    static bool PatchRsrcData(std::vector<char>& b, const RsrcLoc& loc,
                              const void* pNew, size_t cbNew, size_t cbMax,
                              unsigned char cbPad = 0)
    {
        if (!loc.leafOff || loc.leafOff + 16 > b.size()) return false;
        DWORD dataRva = *(DWORD*)&b[loc.leafOff];
        DWORD dataSize = *(DWORD*)&b[loc.leafOff + 4];
        size_t dataOff = loc.secRaw + (dataRva - loc.secRva);
        if (dataOff >= b.size()) return false;
        if (cbNew > cbMax || cbNew > dataSize) return false;
        if (dataOff + cbNew > b.size()) return false;
        memcpy(&b[dataOff], pNew, cbNew);
        // pad the remainder of the slot to its recorded size
        if (dataSize > cbNew)
            memset(&b[dataOff + cbNew], cbPad, dataSize - cbNew);
        return true;
    }

    static std::string BuildManifestXml(const Options& opt)
    {
        // Same shape as the host's own dev-mode manifest (which is known
        // to load): asm.v1 assembly + asm.v3 application/windowsSettings.
        // The 2016 dpiAwareness element is deliberately omitted — mixing it
        // with the 2005 dpiAware in one manifest is rejected by the loader.
        std::string xml =
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
            "<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" "
            "manifestVersion=\"1.0\">"
            "<assemblyIdentity version=\"1.0.0.0\" "
            "processorArchitecture=\"*\" name=\"Nodio.App\" type=\"win32\"/>";
        if (opt.bDpiAware)
            xml += "<application xmlns=\"urn:schemas-microsoft-com:asm.v3\">"
                   "<windowsSettings>"
                   "<dpiAware xmlns=\"http://schemas.microsoft.com/SMI/2005/WindowsSettings\">"
                   "true</dpiAware>"
                   "</windowsSettings></application>";
        if (!opt.strUacLevel.empty())
            xml += "<trustInfo xmlns=\"urn:schemas-microsoft-com:asm.v2\">"
                   "<security><requestedPrivileges>"
                   "<requestedExecutionLevel level=\"" +
                   std::string(opt.strUacLevel.begin(),
                               opt.strUacLevel.end()) +
                   "\" uiAccess=\"false\"/>"
                   "</requestedPrivileges></security></trustInfo>";
        xml += "</assembly>";
        return xml;
    }

    // ---- PE version resource ---------------------------------------------
    //
    // Builds a VS_VERSION_INFO blob and stamps it as RT_VERSION. The
    // structure is a nested tree of version-info blocks; the string table
    // carries the human-readable fields Windows shows in the file
    // properties dialog.
    static void PutW(std::vector<char>& b, const void* p, size_t n)
    {
        const char* s = (const char*)p;
        b.insert(b.end(), s, s + n);
        while (b.size() & 3) b.push_back(0);   // 4-byte align
    }
    // NB: PutW pads to a 4-byte boundary, which is right for multi-word
    // values but wrong for the fixed 2-byte header fields — those must sit
    // at exact offsets 0/2/4.
    static void PutWord(std::vector<char>& b, WORD w)
    {
        const char* p = (const char*)&w;
        b.insert(b.end(), p, p + sizeof(w));
    }
    // Writes a NUL-terminated wide key. The key starts immediately after
    // the 6-byte block header and is itself padded to a 4-byte boundary —
    // padding BEFORE the key would shift it and desync the whole tree.
    static void PutWStr(std::vector<char>& b, const std::wstring& s)
    {
        b.insert(b.end(), (const char*)s.c_str(),
                 (const char*)s.c_str() + (s.size() + 1) * sizeof(wchar_t));
        while (b.size() & 3) b.push_back(0);
    }

    // A block: wLength, wValueLength, wType, szKey[] (NUL-terminated,
    // padded to 4), then the value (NOT padded — its length is exact),
    // then children (each 4-byte aligned). wType: 1 = text (wchar) data,
    // 0 = binary.
    static std::vector<char> VerBlock(const std::wstring& key,
                                      const std::vector<char>* value,
                                      bool bText,
                                      const std::vector<
                                          std::vector<char>>* children)
    {
        std::vector<char> b;
        size_t lenPos = b.size();
        PutWord(b, 0);            // wLength — patched at the end
        PutWord(b, value && !value->empty() ? (WORD)value->size() : 0);
        PutWord(b, bText ? 1 : 0);
        PutWStr(b, key);             // key + NUL, 4-byte aligned
        if (value && !value->empty())
            b.insert(b.end(), value->begin(), value->end());   // exact, no pad
        if (children)
            for (auto& c : *children) PutW(b, c.data(), c.size());
        *(WORD*)&b[lenPos] = (WORD)b.size();
        return b;
    }

    static std::vector<char> StringEntry(const std::wstring& name,
                                         const std::wstring& val)
    {
        // String entries carry their value as UTF-16 text
        std::vector<char> v;
        const char* s = (const char*)val.c_str();
        v.insert(v.end(), s, s + (val.size() + 1) * sizeof(wchar_t));
        return VerBlock(name, &v, true, nullptr);
    }

    // Builds the VS_VERSION_INFO blob (no file I/O).
    static std::vector<char> BuildVersionBlob(const Options& opt,
                                              const std::wstring& strDefaultName,
                                              size_t cbSlot)
    {
        // "1.2.3.4" -> four 16-bit fields
        unsigned int v[4] = { 1, 0, 0, 0 };
        {
            std::wstring s = opt.strVersion.empty() ? L"1.0" : opt.strVersion;
            int n = 0;
            size_t pos = 0;
            while (n < 4 && pos < s.size())
            {
                size_t d = s.find(L'.', pos);
                if (d == std::wstring::npos) d = s.size();
                v[n++] = (unsigned int)_wtoi(s.substr(pos, d - pos).c_str());
                pos = d + 1;
            }
        }
        std::wstring strVer = opt.strVersion.empty() ? L"1.0" : opt.strVersion;
        std::wstring strProd = opt.strProduct.empty()
            ? strDefaultName : opt.strProduct;
        std::wstring strDesc = opt.strDescription.empty()
            ? strProd : opt.strDescription;
        std::wstring strCopy = opt.strCopyright.empty()
            ? L"" : opt.strCopyright;
        std::wstring strCompany = opt.strCompany;

        // VS_FIXEDFILEINFO: dwSignature then version fields
        std::vector<char> ffi;
        PutWord(ffi, 0xFEEF04BD);           // dwSignature
        PutWord(ffi, 0x00010000);           // dwStrucVersion
        for (int i = 1; i >= 0; i--) PutWord(ffi, (WORD)v[i]);  // fileVerMS
        for (int i = 3; i >= 2; i--) PutWord(ffi, (WORD)v[i]);  // fileVerLS
        for (int i = 1; i >= 0; i--) PutWord(ffi, (WORD)v[i]);  // prodVerMS
        for (int i = 3; i >= 2; i--) PutWord(ffi, (WORD)v[i]);  // prodVerLS
        PutWord(ffi, 0x3F);                 // dwFileFlagsMask
        PutWord(ffi, 0);                    // dwFileFlags
        PutWord(ffi, 0x040904B0);           // dwFileOS (NT 32-bit)
        PutWord(ffi, 1);                    // dwFileType (app)
        PutWord(ffi, 0);                    // dwFileSubtype
        PutWord(ffi, 0);                    // dwFileDateMS
        PutWord(ffi, 0);                    // dwFileDateLS

        // StringFileInfo / 040904B0 (U.S. English, Unicode)
        std::vector<std::vector<char>> strings;
        strings.push_back(StringEntry(L"CompanyName", strCompany));
        strings.push_back(StringEntry(L"FileDescription", strDesc));
        strings.push_back(StringEntry(L"FileVersion", strVer));
        strings.push_back(StringEntry(L"InternalName", strDefaultName));
        strings.push_back(StringEntry(L"LegalCopyright", strCopy));
        strings.push_back(StringEntry(L"OriginalFilename",
                                      strDefaultName + L".exe"));
        strings.push_back(StringEntry(L"ProductName", strProd));
        strings.push_back(StringEntry(L"ProductVersion", strVer));
        std::vector<char> sfi = VerBlock(L"StringFileInfo", nullptr, false, &strings);

        // VarFileInfo / Translation
        std::vector<char> varVal;
        PutWord(varVal, 0x04B0);            // lang 0409
        PutWord(varVal, 0x04B0);            // codepage 1200
        std::vector<std::vector<char>> varKids;
        varKids.push_back(VerBlock(L"Translation", &varVal, false, nullptr));
        std::vector<char> vfi = VerBlock(L"VarFileInfo", nullptr, false, &varKids);

        // root: VS_VERSION_INFO holding the FIXEDFILEINFO then both children
        std::vector<std::vector<char>> kids;
        kids.push_back(sfi);
        kids.push_back(vfi);
        std::vector<char> root = VerBlock(L"VS_VERSION_INFO", &ffi, false,
                                          &kids);

        // Pad the blob so its root wLength equals the resource slot size.
        // The loader cross-checks the two, and a shorter blob is rejected
        // with "side-by-side configuration is incorrect". cbSlot is the
        // slot size measured from the host's own resource directory.
        if (cbSlot > root.size())
        {
            // a trailing child block absorbs the slack; the version reader
            // stops at the root's wLength so the filler is never parsed
            std::vector<char> pad((size_t)cbSlot - root.size(), 0);
            kids.push_back(pad);
            root = VerBlock(L"VS_VERSION_INFO", &ffi, false, &kids);
        }
        return root;
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

        // 4) read the host exe, rewrite its manifest/version resources in
        //    place, patch the subsystem, then append the payload.
        //    Resources are patched in memory (not via UpdateResourceW,
        //    which corrupts the section when replacing entries) and the
        //    payload is appended last so nothing shifts afterwards.
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

        // 4a) application manifest (DPI awareness + UAC level).
        // Opt-in: rewriting the manifest resource in place is rejected by
        // the Windows loader in some configurations (the exe then fails
        // with "side-by-side configuration is incorrect"), so the default
        // keeps the manifest the host was built with — which already
        // declares DPI awareness. NODIO_PUBMANIFEST=1 opts into the
        // rewrite for UAC level changes.
        bool bPatchManifest =
            GetEnvironmentVariableW(L"NODIO_PUBMANIFEST", nullptr, 0) != 0;
        if (bPatchManifest)
        {
            std::string xml = BuildManifestXml(opt);
            RsrcLoc loc;
            if (FindRsrcLeaf(exe, (WORD)RT_MANIFEST, 1, &loc))
                PatchRsrcData(exe, loc, xml.c_str(), xml.size() + 1, 4096,
                              ' ');   // XML: pad with spaces
        }

        // 4b) PE version resource — same opt-in rule as the manifest.
        if (bPatchManifest)
        {
            std::wstring strDefaultName = BaseOf(opt.strEntry);
            size_t d = strDefaultName.find_last_of(L'.');
            if (d != std::wstring::npos) strDefaultName = strDefaultName.substr(0, d);
            RsrcLoc loc;
            bool bFound = FindRsrcLeaf(exe, (WORD)RT_VERSION, 1, &loc);
            size_t cbSlot = 0;
            if (bFound && loc.leafOff + 8 <= exe.size())
                cbSlot = *(DWORD*)&exe[loc.leafOff + 4];
            std::vector<char> blob =
                BuildVersionBlob(opt, strDefaultName, cbSlot);
            if (bFound)
                PatchRsrcData(exe, loc, blob.data(), blob.size(), 4096);
        }

        // 4c) subsystem (2 = GUI, 3 = CUI)
        PatchSubsystemBuf(exe, opt.bConsole);

        // 5) append the payload onto the now-stamped image
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

        return true;
    }
}
