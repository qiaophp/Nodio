/* keywords64.c —— Nodio x64 补版：string.keywords 的 64 位实现
 *
 * 官方 lib/string/keywords/.res/keywords.dll 只提供 32 位 PE，且未随库发布源码
 * （官方模块用 $"..." 把 DLL 内嵌进 .aardio 源码再内存加载，x64 进程装不下）。
 * 本实现导出与官方 DLL 完全一致的 5 个符号，签名与调用约定（cdecl）也一致，
 * 因此官方模块的调用方式可以原样沿用。
 *
 * 语义（在 x86 上对官方 32 位 keywords.dll 实测确认，逐项一致）：
 *   findOne        返回「在数据里出现位置最靠前」的关键字的 1 基索引，
 *                  出现位置相同时取关键字表里下标最小的那个；没有命中返回 0。
 *                  （官方实测：{"alpha","beta","gamma"} + "gamma and alpha" -> 3，
 *                   {"zebra","apple"} + "apple zebra" -> 2）
 *   findAll        全部关键字都命中返回 0，否则返回非 0
 *                  （官方模块用 0 == findAll(...) 转成 true/false）；
 *                  空数据时官方实测返回 0（true），
 *                  关键字表为空时官方实测同样返回 0（true）。
 *   anyMatchLines  逐行回调「含有任意关键字」的行
 *   allMatchLines  逐行回调「含有全部关键字」的行
 *   noneMatchLines 逐行回调「不含任何关键字」的行
 *
 * 关键字表为空时，三个 *MatchLines 一律不回调任何行（官方实测）。
 *
 * 回调原型：void (__cdecl *)(const char* p, int len)，p 指向行首，
 * len 为该行长度（不含行尾换行符）；切行规则见 matchLines 处的说明。
 *
 * 关键字匹配用 memcmp 逐字节比较（与官方一致，二进制安全，不受 UTF-8 限制）。
 */

#include <string.h>

typedef void(__cdecl *fnMatchLines)(const char *p, int len);

/* 在 data[0..len) 中查找关键字，返回首次出现的字节偏移；未命中返回 -1 */
static int findKeyword(const char *data, int len, const char *kw, int kwlen)
{
    const char *p, *end;
    if (!data || !kw || kwlen <= 0 || len <= 0 || kwlen > len)
        return -1;

    end = data + len - kwlen;
    for (p = data; p <= end; ++p)
    {
        if (*p == *kw && memcmp(p, kw, (size_t)kwlen) == 0)
            return (int)(p - data);
    }
    return -1;
}

/* 统计该行命中的关键字个数（同一个关键字重复出现只算一次） */
static int countMatched(const char *line, int lineLen,
                         char **pKeyWords, int *pSizes, int n)
{
    int i, cnt = 0;
    for (i = 0; i < n; i++)
    {
        const char *kw = pKeyWords ? pKeyWords[i] : 0;
        int kwlen = pSizes ? pSizes[i] : 0;
        if (!kw || kwlen <= 0)
            continue;
        if (findKeyword(line, lineLen, kw, kwlen) >= 0)
            cnt++;
    }
    return cnt;
}

/* mode: 0 = any, 1 = all, 2 = none
 *
 * 官方实测的切行规则：
 *   - 只按 '\n' 切行；孤立 '\r' 不是行分隔符
 *   - 数据以 '\n' 结尾时不再多出一个空行
 *   - 关键字匹配用的是**未剥离的原始行**（含行末 '\r'）
 *   - 但回调报告的行长度会去掉行末**所有** '\r'
 *   官方实测样例：
 *     'ax\ry\rcz'    -> 1 行 "ax\ry\rcz"（长度 7，行内 '\r' 保留）
 *     'abc\r'        -> 1 行 "abc"（长度 3）
 *     '\r'           -> 1 行 ""（长度 0）
 *     'a\r\r'        -> 1 行 "a"（长度 1，两个 '\r' 都被去掉）
 *     'a\r\n\r\nb'   -> 3 行 "a" / "" / "b"
 *     'a\n\rb\n'     -> 2 行 "a" / "\rb"（行内的 '\r' 保留）
 *     关键字 "x\r" 匹配 'x\r\n' 的第一行 -> 命中，报告长度 1（'\r' 被去掉）
 */
static void matchLines(const char *data, int len,
                       char **pKeyWords, int *pSizes, int n,
                       fnMatchLines cb, int mode)
{
    int i = 0;
    if (!data || len <= 0 || !cb)
        return;

    /* 官方实测：关键字表为空时三个 *MatchLines 一律不回调任何行 */
    if (n <= 0)
        return;

    while (i < len)
    {
        int start = i;
        int end = i;
        int rawLen;
        int repLen;
        int matched;

        /* 找行尾：只认 '\n' */
        while (end < len && data[end] != '\n')
            end++;

        rawLen = end - start;

        /* 报告长度：去掉行末所有的 '\r' */
        repLen = rawLen;
        while (repLen > 0 && data[start + repLen - 1] == '\r')
            repLen--;

        /* 匹配用原始行（含行末 '\r'） */
        matched = countMatched(data + start, rawLen, pKeyWords, pSizes, n);

        if ((mode == 0 && matched > 0) ||
            (mode == 1 && matched == n) ||
            (mode == 2 && matched == 0))
        {
            cb(data + start, repLen);
        }

        /* 跳过 '\n'；数据以 '\n' 结尾时不再产生末尾空行 */
        i = end + 1;
    }
}

__declspec(dllexport) int findOne(const char *data, int len,
                                  char **pKeyWords, int *pSizes, int n)
{
    int i, best = 0, bestPos = -1;

    /* 官方实测：关键字表为空时返回 0 */
    if (n <= 0)
        return 0;
    if (!data || len <= 0 || !pKeyWords || !pSizes)
        return 0;

    /* 官方语义（x86 上对官方 32 位 DLL 实测）：返回**在数据里出现位置最靠前**
       的那个关键字的 1 基索引；位置相同时取下标最小的关键字。 */
    for (i = 0; i < n; i++)
    {
        const char *kw = pKeyWords[i];
        int kwlen = pSizes[i];
        int pos;
        if (!kw || kwlen <= 0)
            continue;
        pos = findKeyword(data, len, kw, kwlen);
        if (pos < 0)
            continue;
        if (bestPos < 0 || pos < bestPos)
        {
            bestPos = pos;
            best = i + 1;
        }
    }
    return best;
}

__declspec(dllexport) int findAll(const char *data, int len,
                                  char **pKeyWords, int *pSizes, int n)
{
    int i;

    /* 官方实测：关键字表为空时返回 0（true）—— 没有关键字就等于全部命中 */
    if (n <= 0)
        return 0;
    if (!pKeyWords || !pSizes)
        return 1;
    /* 官方实测：空数据时返回 true（findAll("") == true） */
    if (!data || len <= 0)
        return 0;

    for (i = 0; i < n; i++)
    {
        const char *kw = pKeyWords[i];
        int kwlen = pSizes[i];
        if (!kw || kwlen <= 0)
            continue;
        if (findKeyword(data, len, kw, kwlen) < 0)
            return 1;
    }
    return 0;
}

__declspec(dllexport) void anyMatchLines(const char *data, int len,
                                         char **pKeyWords, int *pSizes, int n,
                                         fnMatchLines cb)
{
    matchLines(data, len, pKeyWords, pSizes, n, cb, 0);
}

__declspec(dllexport) void allMatchLines(const char *data, int len,
                                         char **pKeyWords, int *pSizes, int n,
                                         fnMatchLines cb)
{
    matchLines(data, len, pKeyWords, pSizes, n, cb, 1);
}

__declspec(dllexport) void noneMatchLines(const char *data, int len,
                                          char **pKeyWords, int *pSizes, int n,
                                          fnMatchLines cb)
{
    matchLines(data, len, pKeyWords, pSizes, n, cb, 2);
}
