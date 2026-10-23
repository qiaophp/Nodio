// payload.h — shared trailer contract between the publisher (which appends
// it) and the host (which detects and reads it at startup).
//
// Layout inside the exe (all sizes in bytes):
//   [ ...original exe... ][ file0 bytes ][ file1 bytes ]... [ manifest ]
//   [ Trailer ]
//
// manifest lines (UTF-8): "<lib-relative key>\t<size>\n", in the same
// order as the file bytes were appended. Keys use '\\' separators so
// engine imports resolve straight from the map.
#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <cstdio>
#include <cwchar>

namespace NodioPayload
{
    static const unsigned long long kMagic = 0x4255504F49444F4EULL;  // "NODIOPUB"
    static const unsigned int kVersion = 1;

    struct Trailer
    {
        unsigned long long magic;
        unsigned int version;
        unsigned int count;
        unsigned long long manifestSize;
        unsigned long long payloadSize;
    };

    // Reads a whole file; returns false when unreadable.
    inline bool ReadWholeFile(const std::wstring& path,
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

    // Probes the running exe (or any path) for an appended payload.
    // On success keyOut maps lib-relative keys to raw bytes (std::string).
    // Reads ONLY the trailer, the manifest and the individual file slices
    // (seek-based) — the exe image is never loaded into memory whole, so
    // startup cost is proportional to payload content, not exe size.
    inline bool ReadPayload(const wchar_t* szExePath,
                            std::map<std::wstring, std::string>& keyOut)
    {
        FILE* pf = nullptr;
        if (_wfopen_s(&pf, szExePath, L"rb") != 0 || !pf)
            return false;
        if (fseek(pf, 0, SEEK_END) != 0) { fclose(pf); return false; }
        long cbFile = ftell(pf);
        if (cbFile < (long)sizeof(Trailer)) { fclose(pf); return false; }
        Trailer tr{};
        if (fseek(pf, cbFile - (long)sizeof(Trailer), SEEK_SET) != 0 ||
            fread(&tr, 1, sizeof(Trailer), pf) != sizeof(Trailer))
        {
            fclose(pf);
            return false;
        }
        if (tr.magic != kMagic || tr.version != kVersion)
        {
            fclose(pf);
            return false;
        }
        unsigned long long cbManifest = tr.manifestSize;
        unsigned long long cbPayload = tr.payloadSize;
        if ((unsigned long long)cbFile <
            sizeof(Trailer) + cbManifest + cbPayload)
        {
            fclose(pf);
            return false;
        }
        long long offManifest =
            cbFile - (long long)sizeof(Trailer) - (long long)cbManifest;
        std::string manifest((size_t)cbManifest, '\0');
        if (cbManifest &&
            (fseek(pf, offManifest, SEEK_SET) != 0 ||
             fread(&manifest[0], 1, (size_t)cbManifest, pf) != (size_t)cbManifest))
        {
            fclose(pf);
            return false;
        }
        long long offPayload = offManifest - (long long)cbPayload;

        size_t off = 0, pos = 0;
        bool bOk = true;
        while (pos < manifest.size())
        {
            size_t eol = manifest.find('\n', pos);
            if (eol == std::string::npos) eol = manifest.size();
            std::string line = manifest.substr(pos, eol - pos);
            pos = eol + 1;
            size_t tab = line.rfind('\t');
            if (tab == std::string::npos) continue;
            std::string keyU8 = line.substr(0, tab);
            size_t cb = (size_t)_strtoui64(line.substr(tab + 1).c_str(),
                                          nullptr, 10);
            int w = MultiByteToWideChar(CP_UTF8, 0, keyU8.c_str(),
                                        (int)keyU8.size(), nullptr, 0);
            std::wstring key((size_t)max(w, 0), 0);
            if (w > 0)
                MultiByteToWideChar(CP_UTF8, 0, keyU8.c_str(),
                                    (int)keyU8.size(), &key[0], w);
            if (off + cb > cbPayload) { bOk = false; break; }
            std::string data((size_t)cb, '\0');
            if (cb && (fseek(pf, offPayload + (long long)off, SEEK_SET) != 0 ||
                       fread(&data[0], 1, cb, pf) != cb))
            {
                bOk = false;
                break;
            }
            keyOut[key] = std::move(data);
            off += cb;
        }
        fclose(pf);
        return bOk;
    }

    // Payload bytes -> engine string map. Engine strings hold arbitrary
    // bytes one-per-wchar (the engine's byte-string convention), so raw
    // payloads survive the round trip unchanged.
    inline void ToWideMap(const std::map<std::wstring, std::string>& in,
                          std::unordered_map<std::wstring, std::wstring>& out)
    {
        for (auto& kv : in)
        {
            std::wstring w;
            w.resize(kv.second.size());
            for (size_t i = 0; i < kv.second.size(); i++)
                w[i] = (wchar_t)(unsigned char)kv.second[i];
            out[kv.first] = std::move(w);
        }
    }
}
