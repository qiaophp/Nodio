// allocstat.cpp — 全局分配普查。override operator new/delete 统计每一次堆
// 分配的字节数与次数，按 import 加载的三个阶段归账（1=read/lex/parse、
// 2=exec、3=模块收尾拷贝），由 BootDump 在进程退出时打印。
// 计数开关 g_allocOn 默认关（一次 relaxed 原子读的开销），entry.cpp 在
// 进程启动时探测 NODIO_ALLOC_TRACE / NODIO_BOOT_TRACE 打开。
#include "engine/common.h"
#include "engine/interp.h"
#include <cstdlib>
#include <new>
#include <map>
#include <algorithm>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

namespace NodioVM
{
    std::atomic<bool> g_allocOn{false};
    std::atomic<int> g_allocPhase{0};   // 0=other 1=parse 2=exec 3=finish

    static std::atomic<long> g_nNew{0}, g_nDel{0};
    static std::atomic<unsigned long long> g_bNew{0}, g_bDel{0};
    struct AllocPhaseCnt
    {
        std::atomic<long> n{0};
        std::atomic<unsigned long long> b{0};
    };
    static AllocPhaseCnt g_ph[4];
    struct AllocBucketCnt
    {
        std::atomic<long> n{0};
        std::atomic<unsigned long long> b{0};
    };
    static AllocBucketCnt g_bk[12];
    // 阶段×尺寸交叉：exec 侧 1KB 级分配的来源必须靠这个定位
    static AllocBucketCnt g_px[4][12];

    // ---- 大块分配（>64KB）栈位点采集：exec 侧 433 次 82.6MB 的来源靠它定位。
    // 只在 NODIO_ALLOC_TRACE=1 时启用；map 插入会递归进 operator new，
    // 用 t_inHook 折返保护。
    struct BigSite
    {
        long n = 0;
        unsigned long long b = 0;
        void* fp[8] = {};
    };
    static std::map<unsigned long long, BigSite> g_bigSites;
    static std::mutex g_bigMtx;
    static thread_local int t_inHook = 0;

    static void TrackBig(int ph, size_t n)
    {
        if (n <= 64 * 1024 || t_inHook) return;
        t_inHook++;
        {
            void* fp[10] = {};
            USHORT nFp = CaptureStackBackTrace(0, 10, fp, nullptr);
            unsigned long long key = 0;
            for (int i = 3; i < 8 && i < nFp; i++)
                key ^= (unsigned long long)(uintptr_t)fp[i] + (key << 7);
            std::lock_guard<std::mutex> lk(g_bigMtx);
            BigSite& s = g_bigSites[key];
            if (s.n == 0)
                for (int i = 0; i < 8 && i < nFp; i++) s.fp[i] = fp[i];
            s.n++;
            s.b += n;
            (void)ph;
        }
        t_inHook--;
    }

    static void DumpBigSites()
    {
        std::lock_guard<std::mutex> lk(g_bigMtx);
        if (g_bigSites.empty()) return;
        HANDLE proc = GetCurrentProcess();
        static bool bSym = SymInitialize(proc, nullptr, TRUE);
        (void)bSym;
        // 按字节降序取前 12 个位点
        std::vector<std::pair<unsigned long long, BigSite*>> v;
        v.reserve(g_bigSites.size());
        for (auto& kv : g_bigSites) v.push_back({kv.first, &kv.second});
        std::sort(v.begin(), v.end(),
                  [](auto& a, auto& c) { return a.second->b > c.second->b; });
        fwprintf(stderr, L"[ALLOC] big sites (>64KB), top by bytes:\n");
        for (size_t i = 0; i < v.size() && i < 12; i++)
        {
            BigSite& s = *v[i].second;
            fwprintf(stderr, L"[ALLOC] big #%zu n=%ld %.1fMB at:",
                     i, s.n, (double)s.b / (1024.0 * 1024.0));
            for (int f = 3; f < 8; f++)
            {
                if (!s.fp[f]) break;
                char buf[512] = {};
                SYMBOL_INFO* si = (SYMBOL_INFO*)buf;
                si->SizeOfStruct = sizeof(SYMBOL_INFO);
                si->MaxNameLen = 400;
                DWORD64 disp = 0;
                if (SymFromAddr(proc, (DWORD64)(uintptr_t)s.fp[f], &disp, si))
                {
                    int wl = MultiByteToWideChar(CP_UTF8, 0, si->Name, -1,
                                                 nullptr, 0);
                    std::wstring w((size_t)wl, 0);
                    MultiByteToWideChar(CP_UTF8, 0, si->Name, -1, &w[0], wl);
                    fwprintf(stderr, L" %s", w.c_str());
                }
            }
            fwprintf(stderr, L"\n");
        }
        fflush(stderr);
    }
    static inline int BucketOf(size_t n)
    {
        // 32B 以下一桶，之后按 2 的幂到 >32K
        if (n <= 32) return 0;
        int b = 1;
        while (b < 11 && n > (size_t)(32u << b)) b++;
        return b;
    }

    void AllocStatEnable()
    {
        if (g_allocOn.exchange(true)) return;   // atexit 只挂一次
        atexit(AllocStatDump);
    }

    AllocPhaseScope::AllocPhaseScope(int p)
        : old(g_allocPhase.load(std::memory_order_relaxed))
    {
        g_allocPhase.store(p, std::memory_order_relaxed);
    }
    AllocPhaseScope::~AllocPhaseScope()
    {
        g_allocPhase.store(old, std::memory_order_relaxed);
    }

    static inline void CountNew(int ph, size_t n)
    {
        g_nNew.fetch_add(1, std::memory_order_relaxed);
        g_bNew.fetch_add((unsigned long long)n, std::memory_order_relaxed);
        AllocPhaseCnt& c = g_ph[ph & 3];
        c.n.fetch_add(1, std::memory_order_relaxed);
        c.b.fetch_add((unsigned long long)n, std::memory_order_relaxed);
        AllocBucketCnt& bk = g_bk[BucketOf(n)];
        bk.n.fetch_add(1, std::memory_order_relaxed);
        bk.b.fetch_add((unsigned long long)n, std::memory_order_relaxed);
        AllocBucketCnt& px = g_px[ph & 3][BucketOf(n)];
        px.n.fetch_add(1, std::memory_order_relaxed);
        px.b.fetch_add((unsigned long long)n, std::memory_order_relaxed);
        TrackBig(ph, n);
    }

    void AllocStatDump()
    {
        if (!g_allocOn.load(std::memory_order_relaxed)) return;
        static const wchar_t* phName[4] = {L"other", L"parse", L"exec", L"finish"};
        fwprintf(stderr, L"[ALLOC] total new=%ld del=%ld live=%ld bytes=%.1fMB\n",
                 g_nNew.load(), g_nDel.load(), g_nNew.load() - g_nDel.load(),
                 (double)g_bNew.load() / (1024.0 * 1024.0));
        fwprintf(stderr, L"[ALLOC] sizes Value=%zu Tok=%zu Node=%zu Env=%zu Table=%zu\n",
                 sizeof(Value), sizeof(Tok), sizeof(Node), sizeof(Env), sizeof(Table));
        for (int i = 0; i < 4; i++)
        {
            long n = g_ph[i].n.load();
            if (!n) continue;
            fwprintf(stderr, L"[ALLOC] phase %-6s n=%-10ld %.1fMB avg=%zuB\n",
                     phName[i], n, (double)g_ph[i].b.load() / (1024.0 * 1024.0),
                     (size_t)(g_ph[i].b.load() / (unsigned long long)(n ? n : 1)));
        }
        DumpBigSites();
        // 尺寸直方图（2 的幂分桶）：一眼看出分配体构成
        static const wchar_t* bkName[12] = {
            L"<=32", L"64", L"128", L"256", L"512", L"1K",
            L"2K", L"4K", L"8K", L"16K", L"32K", L">32K"};
        for (int p = 0; p < 4; p++)
        {
            bool bAny = false;
            for (int i = 0; i < 12; i++)
                if (g_px[p][i].n.load()) { bAny = true; break; }
            if (!bAny) continue;
            fwprintf(stderr, L"[ALLOC] %s/size:", phName[p]);
            for (int i = 0; i < 12; i++)
            {
                long n = g_px[p][i].n.load();
                if (n)
                    fwprintf(stderr, L" %s=%ld/%.1fMB",
                             bkName[i], n,
                             (double)g_px[p][i].b.load() / (1024.0 * 1024.0));
            }
            fwprintf(stderr, L"\n");
        }
        fflush(stderr);
    }
}

using namespace NodioVM;

void* operator new(size_t n)
{
    if (g_allocOn.load(std::memory_order_relaxed))
        CountNew(g_allocPhase.load(std::memory_order_relaxed), n);
    void* p = malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    return p;
}
void* operator new[](size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept
{
    // Same gate as operator new: with NODIO_ALLOC_TRACE unset the census
    // must cost nothing beyond one relaxed load. The old unconditional
    // fetch_add taxed every delete in the process (census: 45.1M deletes
    // per call5m run) even though the dump refuses to run when g_allocOn
    // is false.
    if (p && g_allocOn.load(std::memory_order_relaxed))
        g_nDel.fetch_add(1, std::memory_order_relaxed);
    free(p);
}
void operator delete[](void* p) noexcept { ::operator delete(p); }
void operator delete(void* p, size_t) noexcept { ::operator delete(p); }
void operator delete[](void* p, size_t) noexcept { ::operator delete[](p); }
