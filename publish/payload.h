// payload.h — shared trailer contract between the publisher (which appends
// it) and the host (which detects and reads it at startup).
//
// Layout inside the exe (all sizes in bytes):
//   [ ...original exe... ][ file0 bytes ][ file1 bytes ]... [ manifest ]
//   [ Trailer ]
//
// manifest lines (UTF-8): "<lib-relative key>\t<size>\n", in the same
// order as the file bytes were appended. Keys use '\\' separators so
// engine imports ("win\\ui\\_.aardio") resolve straight from the map.
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
    inline bool ReadPayload(const wchar_t* szExePath,
                            std::map<std::wstring, std::string>& keyOut)
    {
        std::vector<char> exe;
        if (!ReadWholeFile(szExePath, exe)) return false;
        if (exe.size() < sizeof(Trailer)) return false;
        Trailer tr{};
        memcpy(&tr, exe.data() + exe.size() - sizeof(Trailer), sizeof(Trailer));
        if (tr.magic != kMagic || tr.version != kVersion) return false;
        size_t cbManifest = (size_t)tr.manifestSize;
        size_t cbPayload = (size_t)tr.payloadSize;
        if (exe.size() < sizeof(Trailer) + cbManifest + cbPayload) return false;
        const char* pPayload =
            exe.data() + exe.size() - sizeof(Trailer) - cbManifest - cbPayload;
        const char* pManifest = pPayload + cbPayload;

        std::string manifest(pManifest, cbManifest);
        size_t off = 0, pos = 0;
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
            if (off + cb > cbPayload) break;
            keyOut[key] = std::string(pPayload + off, cb);
            off += cb;
        }
        return true;
    }

    // Payload bytes -> engine string map. Aardio strings hold arbitrary
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
