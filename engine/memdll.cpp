// memdll.cpp — memory DLL loading (raw.loadDll with in-memory bytes).
//
// aardio can load a DLL from a string: the bytes are embedded at compile
// time with $"path" (or read from a published payload) and handed to
// raw.loadDll, which maps the image into memory and returns a dll object
// whose .api() works exactly like a disk-loaded module.
//
// The loader is a manual PE mapper: it parses the headers, allocates the
// image at its preferred base (falling back to a fresh allocation),
// copies sections, applies base relocations, resolves the import table,
// and calls DllMain. It deliberately does NOT run TLS callbacks or
// handle delay-load/SEH edge cases — the aardio docs warn that only
// simple, dependency-free DLLs support memory loading.
#include "engine/common.h"
#include "engine/interp.h"

#include <imagehlp.h>
#pragma comment(lib, "imagehlp.lib")

namespace NodioVM
{
    // The import thunk's function slot is 4 bytes on x86 and 8 on x64;
    // reading it as 64-bit on x86 would pick up the next entry's bits.
    static ULONGLONG ThunkValue(const IMAGE_THUNK_DATA* th)
    {
#if defined(_M_IX86)
        return (ULONGLONG)(DWORD)(INT_PTR)th->u1.Function;
#else
        return th->u1.Function;
#endif
    }

    // RVA -> file offset inside the raw buffer using the section table.
    static size_t MemDllRvaToOff(const IMAGE_NT_HEADERS* nt, size_t rva)    {
        const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++)
        {
            size_t va = sec[i].VirtualAddress;
            size_t vs = sec[i].Misc.VirtualSize;
            if (rva >= va && rva < va + max(vs, sec[i].SizeOfRawData))
                return sec[i].PointerToRawData + (rva - va);
        }
        return (size_t)-1;
    }

    // Maps a PE image held in memory. Returns the base address or null.
    static void* MemDllMap(const std::vector<char>& buf, MemDll* pOut)
    {
        if (buf.size() < sizeof(IMAGE_DOS_HEADER)) return nullptr;
        const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)buf.data();
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
        if ((size_t)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS) > buf.size())
            return nullptr;
        const IMAGE_NT_HEADERS* nt =
            (const IMAGE_NT_HEADERS*)(buf.data() + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
        if (nt->FileHeader.Machine !=
#if defined(_M_IX86)
            IMAGE_FILE_MACHINE_I386
#else
            IMAGE_FILE_MACHINE_AMD64
#endif
        ) return nullptr;   // wrong architecture — cannot load

        SIZE_T cbImage = nt->OptionalHeader.SizeOfImage;
        if (!cbImage) return nullptr;

        // Try the preferred base first so relocations are usually a no-op.
        void* pBase = VirtualAlloc((void*)nt->OptionalHeader.ImageBase,
                                   cbImage, MEM_RESERVE | MEM_COMMIT,
                                   PAGE_EXECUTE_READWRITE);
        if (!pBase)
            pBase = VirtualAlloc(nullptr, cbImage, MEM_RESERVE | MEM_COMMIT,
                                 PAGE_EXECUTE_READWRITE);
        if (!pBase) return nullptr;

        // headers
        memcpy(pBase, buf.data(), min(buf.size(),
                                      (size_t)nt->OptionalHeader.SizeOfHeaders));

        // sections
        const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++)
        {
            if (!sec[i].SizeOfRawData) continue;
            size_t off = sec[i].PointerToRawData;
            if (off + sec[i].SizeOfRawData > buf.size()) continue;
            memcpy((char*)pBase + sec[i].VirtualAddress,
                   buf.data() + off, sec[i].SizeOfRawData);
        }

        // base relocations when the loader moved us off the preferred base
        if ((char*)pBase != (char*)nt->OptionalHeader.ImageBase)
        {
            const IMAGE_DATA_DIRECTORY& dd =
                nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
            if (dd.VirtualAddress && dd.Size)
            {
                size_t off = MemDllRvaToOff(nt, dd.VirtualAddress);
                if (off != (size_t)-1)
                {
                    char* p = (char*)pBase + dd.VirtualAddress;
                    char* pEnd = p + dd.Size;
                    while (p + sizeof(IMAGE_BASE_RELOCATION) <= pEnd)
                    {
                        IMAGE_BASE_RELOCATION* br = (IMAGE_BASE_RELOCATION*)p;
                        if (!br->SizeOfBlock) break;
                        int n = (br->SizeOfBlock - sizeof(*br)) / sizeof(WORD);
                        WORD* e = (WORD*)(p + sizeof(*br));
                        for (int k = 0; k < n; k++)
                        {
                            int type = e[k] >> 12;
                            int arg = e[k] & 0xFFF;
                            if (!type) continue;
                            char* t = (char*)pBase + br->VirtualAddress + arg;
                            if (type == IMAGE_REL_BASED_DIR64)
                                *(ULONGLONG*)t +=
                                    (ULONGLONG)((char*)pBase -
                                     (char*)nt->OptionalHeader.ImageBase);
                            else if (type == IMAGE_REL_BASED_HIGHLOW)
                                *(DWORD*)t += (DWORD)((char*)pBase -
                                     (char*)nt->OptionalHeader.ImageBase);
                        }
                        p += br->SizeOfBlock;
                    }
                }
            }
        }

        // import table: load each dependency and patch the IAT
        const IMAGE_DATA_DIRECTORY& di =
            nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (di.VirtualAddress && di.Size)
        {
            size_t off = MemDllRvaToOff(nt, di.VirtualAddress);
            if (off != (size_t)-1)
            {
                IMAGE_IMPORT_DESCRIPTOR* im =
                    (IMAGE_IMPORT_DESCRIPTOR*)((char*)pBase + di.VirtualAddress);
                for (; im->Name; im++)
                {
                    const char* szName =
                        (const char*)pBase + im->Name;
                    HMODULE hDep = LoadLibraryA(szName);
                    if (!hDep) { VirtualFree(pBase, 0, MEM_RELEASE); return nullptr; }
                    pOut->vDeps.push_back(hDep);
                    IMAGE_THUNK_DATA* th =
                        (IMAGE_THUNK_DATA*)((char*)pBase + im->OriginalFirstThunk);
                    IMAGE_THUNK_DATA* iat =
                        (IMAGE_THUNK_DATA*)((char*)pBase + im->FirstThunk);
                    if (!im->OriginalFirstThunk) th = iat;
                    for (; ThunkValue(th); th++, iat++)
                    {
                        FARPROC fn = nullptr;
                        if (th->u1.Ordinal & IMAGE_ORDINAL_FLAG)
                        {
                            fn = GetProcAddress(hDep,
                                (LPCSTR)(th->u1.Ordinal & 0xFFFF));
                        }
                        else
                        {
                            IMAGE_IMPORT_BY_NAME* bn =
                                (IMAGE_IMPORT_BY_NAME*)((char*)pBase +
                                                        th->u1.AddressOfData);
                            fn = GetProcAddress(hDep, (LPCSTR)bn->Name);
                        }
                        if (!fn)
                        {
                            for (auto h : pOut->vDeps) FreeLibrary(h);
                            VirtualFree(pBase, 0, MEM_RELEASE);
                            return nullptr;
                        }
                        // u1.Function is a DWORD on x86 and a ULONGLONG on
                        // x64; writing the wider one on x86 would clobber
                        // the following IAT entry.
#if defined(_M_IX86)
                        iat->u1.Function = (DWORD)(INT_PTR)fn;
#else
                        iat->u1.Function = (ULONGLONG)fn;
#endif
                    }
                }
            }
        }

        pOut->pImage = pBase;
        pOut->cbImage = cbImage;
        pOut->hSelf = (HMODULE)pBase;
        pOut->bOk = true;
        return pBase;
    }

    // Runs DllMain(DLL_PROCESS_ATTACH). Returns false when the entry point
    // is missing or reports failure.
    static bool MemDllCallEntry(void* pBase, const IMAGE_NT_HEADERS* nt)
    {
        DWORD rva = nt->OptionalHeader.AddressOfEntryPoint;
        if (!rva) return true;   // resource-only DLL: nothing to run
        typedef BOOL (WINAPI *DllMainFn)(HINSTANCE, DWORD, LPVOID);
        DllMainFn fn = (DllMainFn)((char*)pBase + rva);
        return fn((HINSTANCE)pBase, DLL_PROCESS_ATTACH, nullptr) != FALSE;
    }

    // Public: map a byte string into a loadable module. On success *phMod
    // receives a handle usable with GetProcAddress (i.e. the image base).
    // The MemDll must outlive the module (it owns the allocations).
    bool MemDllLoad(const wstring& bytes, void** phMod, MemDll* pOut)
    {
        if (bytes.empty()) return false;
        std::vector<char> buf(bytes.size());
        for (size_t i = 0; i < bytes.size(); i++)
            buf[i] = (char)(unsigned char)bytes[i];
        void* pBase = MemDllMap(buf, pOut);
        if (!pBase) return false;
        const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)buf.data();
        const IMAGE_NT_HEADERS* nt =
            (const IMAGE_NT_HEADERS*)(buf.data() + dos->e_lfanew);
        if (!MemDllCallEntry(pBase, nt))
        {
            for (auto h : pOut->vDeps) FreeLibrary(h);
            VirtualFree(pBase, 0, MEM_RELEASE);
            pOut->bOk = false;
            return false;
        }
        *phMod = (HMODULE)pBase;
        return true;
    }

    // Resolves an export from a manually mapped image. GetProcAddress
    // cannot be used: it only understands loader-mapped modules, so a
    // memory DLL's exports must be walked through its export directory.
    FARPROC MemDllGetProc(void* pImage, const char* szName)
    {
        if (!pImage || !szName || !*szName) return nullptr;
        const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)pImage;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
        const IMAGE_NT_HEADERS* nt =
            (const IMAGE_NT_HEADERS*)((char*)pImage + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
        DWORD rva = nt->OptionalHeader
            .DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
        DWORD cb = nt->OptionalHeader
            .DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size;
        if (!rva || !cb) return nullptr;
        IMAGE_EXPORT_DIRECTORY* ed =
            (IMAGE_EXPORT_DIRECTORY*)((char*)pImage + rva);

        DWORD* pNames = (DWORD*)((char*)pImage + ed->AddressOfNames);
        WORD* pOrds = (WORD*)((char*)pImage + ed->AddressOfNameOrdinals);
        DWORD* pFuncs = (DWORD*)((char*)pImage + ed->AddressOfFunctions);
        for (DWORD i = 0; i < ed->NumberOfNames; i++)
        {
            const char* sz = (const char*)pImage + pNames[i];
            if (_stricmp(sz, szName) != 0) continue;
            WORD ord = pOrds[i];
            if (ord >= ed->NumberOfFunctions) return nullptr;
            return (FARPROC)((char*)pImage + pFuncs[ord]);
        }
        // ordinal form: "#123"
        if (szName[0] == '#')
        {
            DWORD ord = (DWORD)atoi(szName + 1);
            if (ord >= ed->Base && ord - ed->Base < ed->NumberOfFunctions)
                return (FARPROC)((char*)pImage +
                                 pFuncs[ord - ed->Base]);
        }
        return nullptr;
    }

    void MemDllUnload(MemDll* p)
    {
        if (!p->bOk) return;
        if (p->pImage)
        {
            // best-effort DLL_PROCESS_DETACH
            const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)p->pImage;
            const IMAGE_NT_HEADERS* nt =
                (const IMAGE_NT_HEADERS*)((char*)p->pImage + dos->e_lfanew);
            if (nt->OptionalHeader.AddressOfEntryPoint)
            {
                typedef BOOL (WINAPI *DllMainFn)(HINSTANCE, DWORD, LPVOID);
                DllMainFn fn = (DllMainFn)((char*)p->pImage +
                                           nt->OptionalHeader.AddressOfEntryPoint);
                fn((HINSTANCE)p->pImage, DLL_PROCESS_DETACH, nullptr);
            }
            VirtualFree(p->pImage, 0, MEM_RELEASE);
            p->pImage = nullptr;
        }
        for (auto h : p->vDeps) FreeLibrary(h);
        p->vDeps.clear();
        p->bOk = false;
    }
}
