/* lzma64.c —— Nodio x64 补版：LZMA(.lzma) 编解码器
 *
 * 官方 lib/sevenZip/lzma/_.aardio 把 32 位 LzmaUtil.dll 用 $"..." 内嵌进源码
 * 再内存加载，x64 进程装不下该 PE（实测「无法加载 DLL: (内嵌的内存映像)」）。
 * 官方**没有随库发布源码**，因此这里在 Nodio 侧自写一份等价实现。
 *
 * 导出符号与官方 LzmaUtil.dll 同名（3 个），签名与调用约定(cdecl)一致：
 *   LzmaCompress   —— 压缩，输出 raw LZMA 流（不含 13 字节 .lzma 文件头）
 *   LzmaUncompress —— 解压，输入 raw LZMA 流
 *   lzmafile       —— 文件级压缩/解压（.lzma 文件格式，进度回调）
 *
 * 格式：LZMA1（与 7-Zip 的 .lzma 一致）。
 *   属性字节 d = (pb*5 + lp)*9 + lc，字典大小 4 字节小端。
 *   概率模型按 LZMA SDK 的扁平布局排布：
 *     isMatch、isRep 系列、isRep0Long、posSlot、specPos、align、len、repLen 共 1846 项，
 *     其后是字面量区 0x300 << (lc+lp) 项，每项 0x300 =
 *       [0x000..0x0FF] 普通字面量树、[0x100..0x1FF] 匹配位=0 的匹配树、
 *       [0x200..0x2FF] 匹配位=1 的匹配树；
 *     字面量上下文 litState = ((pos & lpMask) << lc) | (prevByte >> (8-lc))。
 *
 * 与官方实现的关系：
 *   · **解压**逐字节兼容官方：官方压出来的流，本实现能原样解出；
 *   · **压缩**不保证与官方逐字节相同（匹配选择策略不同），
 *     但官方能原样解开本实现压出来的流 —— 这一点由回归锁双向交叉验证。
 *
 * 本文件不包含官方 LzmaUtil.dll 的任何代码或二进制。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef LZMA_STATIC
#define LZMA_API
#elif defined(_WIN32)
#define LZMA_API __declspec(dllexport)
#else
#define LZMA_API
#endif

typedef uint8_t  Byte;
typedef uint16_t UInt16;
typedef uint32_t UInt32;
typedef uint64_t UInt64;

/* ---- LZMA 常量（与格式规范一致）---- */
#define kNumBitModelTotalBits 11
#define kBitModelTotal        (1u << kNumBitModelTotalBits)   /* 2048 */
#define kNumMoveBits          5
#define kTopValue             (1u << 24)

#define kNumPosBitsMax        4
#define kNumPosStatesMax      (1 << kNumPosBitsMax)           /* 16 */
#define kNumStates            12
#define kNumLenToPosStates    4
#define kNumPosSlotBits       6
#define kNumAlignBits         4
#define kStartPosModelIndex   4
#define kEndPosModelIndex     14
#define kNumFullDistances     (1u << (kEndPosModelIndex >> 1))/* 128 */
#define kMatchMinLen          2

#define kLenNumLowBits        3
#define kLenNumLowSymbols     (1 << kLenNumLowBits)           /* 8 */
#define kLenNumMidBits        3
#define kLenNumMidSymbols     (1 << kLenNumMidBits)           /* 8 */
#define kLenNumHighBits       8
#define kLenNumHighSymbols    (1 << kLenNumHighBits)          /* 256 */
#define kNumLenProbs          (2 + (kNumPosStatesMax << kLenNumLowBits) \
                                        + (kNumPosStatesMax << kLenNumMidBits) \
                                        + kLenNumHighSymbols)  /* 514 */

/* ---- 概率模型扁平布局（与 LZMA SDK 一致）---- */
#define ProbIsMatch       0
#define ProbIsRep         (ProbIsMatch + (kNumStates << kNumPosBitsMax))
#define ProbIsRepG0       (ProbIsRep + kNumStates)
#define ProbIsRepG1       (ProbIsRepG0 + kNumStates)
#define ProbIsRepG2       (ProbIsRepG1 + kNumStates)
#define ProbIsRep0Long    (ProbIsRepG2 + kNumStates)
#define ProbPosSlot       (ProbIsRep0Long + (kNumStates << kNumPosBitsMax))
#define ProbSpecPos       (ProbPosSlot + (kNumLenToPosStates << kNumPosSlotBits))
#define ProbAlign         (ProbSpecPos + kNumFullDistances - kEndPosModelIndex)
#define ProbLenCoder      (ProbAlign + (1 << kNumAlignBits))
#define ProbRepLenCoder   (ProbLenCoder + kNumLenProbs)
#define ProbLiteral       (ProbRepLenCoder + kNumLenProbs)    /* = 1846 */
#define kLiteralBlockSize 0x300

/* LenCoder 内部偏移 */
#define LenChoice   0
#define LenChoice2  1
#define LenLow      2
#define LenMid      (LenLow + (kNumPosStatesMax << kLenNumLowBits))
#define LenHigh     (LenMid + (kNumPosStatesMax << kLenNumMidBits))

/* 错误码（与 7-Zip 的 SZ_ERROR_* 一致，官方 aardio 侧按这些码取错误信息）*/
#define SZ_OK               0
#define SZ_ERROR_DATA       1
#define SZ_ERROR_MEM        2
#define SZ_ERROR_INPUT_EOF  6
#define SZ_ERROR_OUTPUT_EOF 7
/* 进度回调要求中止（官方回调返回 true 即停止）*/
#define SZ_ERROR_ABORTED    10

/* 解码进度：每 64KB 输出上报一次（与官方实测一致）*/
#define kDecodeProgressChunk (64u * 1024u)
/* 编码进度：每 128KB 输入上报一次 */
#define kEncodeProgressChunk (128u * 1024u)

/* ================================================================== */
/* 概率模型                                                            */
/* ================================================================== */

typedef struct {
    UInt16* p;
    size_t  count;
} ProbModel;

static void probs_init(ProbModel* pm, UInt32 lc, UInt32 lp)
{
    size_t total = (size_t)ProbLiteral + ((size_t)0x300 << (lc + lp));
    size_t i;
    pm->count = total;
    pm->p = (UInt16*)malloc(total * sizeof(UInt16));
    if (!pm->p) { pm->count = 0; return; }
    for (i = 0; i < total; i++) pm->p[i] = (UInt16)(kBitModelTotal >> 1);
}

static void probs_free(ProbModel* pm)
{
    free(pm->p);
    pm->p = NULL;
    pm->count = 0;
}

/* ================================================================== */
/* 范围解码器                                                          */
/* ================================================================== */

typedef struct {
    const Byte* buf;
    size_t      size;
    size_t      pos;
    UInt32      range;
    UInt32      code;
    int         err;
} RangeDec;

static int rd_init(RangeDec* r, const Byte* buf, size_t size)
{
    int i;
    r->buf = buf; r->size = size; r->pos = 0; r->err = 0;
    r->range = 0xFFFFFFFFu; r->code = 0;
    if (size < 5) { r->err = 1; return 0; }
    for (i = 0; i < 5; i++) r->code = (r->code << 8) | r->buf[r->pos++];
    return 1;
}

static inline Byte rd_byte(RangeDec* r)
{
    if (r->pos >= r->size) { r->err = 1; return 0; }
    return r->buf[r->pos++];
}

static inline void rd_normalize(RangeDec* r)
{
    if (r->range < kTopValue) {
        r->range <<= 8;
        r->code = (r->code << 8) | rd_byte(r);
    }
}

static inline int rd_bit(RangeDec* r, UInt16* prob)
{
    UInt32 bound = (r->range >> kNumBitModelTotalBits) * (UInt32)(*prob);
    int sym;
    if (r->code < bound) {
        r->range = bound;
        *prob = (UInt16)(*prob + ((kBitModelTotal - *prob) >> kNumMoveBits));
        sym = 0;
    }
    else {
        r->range -= bound;
        r->code -= bound;
        *prob = (UInt16)(*prob - (*prob >> kNumMoveBits));
        sym = 1;
    }
    rd_normalize(r);
    return sym;
}

static UInt32 rd_bittree(RangeDec* r, UInt16* probs, int numBits)
{
    UInt32 m = 1;
    int i;
    for (i = 0; i < numBits; i++) m = (m << 1) + (UInt32)rd_bit(r, &probs[m]);
    return m - ((UInt32)1 << numBits);
}

static UInt32 rd_bittree_rev(RangeDec* r, UInt16* probs, int numBits)
{
    UInt32 m = 1, sym = 0;
    int i;
    for (i = 0; i < numBits; i++) {
        UInt32 b = (UInt32)rd_bit(r, &probs[m]);
        m = (m << 1) + b;
        sym |= b << i;
    }
    return sym;
}

static UInt32 rd_direct(RangeDec* r, int numBits)
{
    UInt32 res = 0;
    while (numBits--) {
        UInt32 t;
        r->range >>= 1;
        r->code -= r->range;
        t = 0u - (r->code >> 31);
        r->code += r->range & t;
        rd_normalize(r);
        res = (res << 1) + t + 1;
    }
    return res;
}

static UInt32 rd_len(RangeDec* r, const UInt16* lenBase, UInt32 posState)
{
    if (rd_bit(r, (UInt16*)&lenBase[LenChoice]) == 0)
        return rd_bittree(r, (UInt16*)&lenBase[LenLow + (posState << kLenNumLowBits)], kLenNumLowBits);
    if (rd_bit(r, (UInt16*)&lenBase[LenChoice2]) == 0)
        return kLenNumLowSymbols + rd_bittree(r, (UInt16*)&lenBase[LenMid + (posState << kLenNumMidBits)], kLenNumMidBits);
    return kLenNumLowSymbols + kLenNumMidSymbols
         + rd_bittree(r, (UInt16*)&lenBase[LenHigh], kLenNumHighBits);
}

/* ================================================================== */
/* 范围编码器                                                          */
/* ================================================================== */

typedef struct {
    Byte*   buf;
    size_t  size;
    size_t  pos;
    UInt64  low;
    UInt32  range;
    Byte    cache;
    UInt64  cacheSize;
    int     err;
} RangeEnc;

static void re_init(RangeEnc* e, Byte* buf, size_t size)
{
    e->buf = buf; e->size = size; e->pos = 0;
    e->low = 0; e->range = 0xFFFFFFFFu; e->cache = 0; e->cacheSize = 1; e->err = 0;
}

static inline void re_shift_low(RangeEnc* e)
{
    if ((UInt32)e->low < 0xFF000000u || (int)(e->low >> 32) != 0) {
        Byte temp = e->cache;
        do {
            if (e->pos >= e->size) { e->err = 1; e->cacheSize = 0; e->low = 0; return; }
            e->buf[e->pos++] = (Byte)(temp + (Byte)(e->low >> 32));
            temp = 0xFF;
        } while (--e->cacheSize);
        e->cache = (Byte)((UInt32)e->low >> 24);
    }
    e->cacheSize++;
    /* 与 LZMA SDK 一致：左移后必须截断到 32 位（高位进位已在上面输出），
     * 否则进位会一直留在 low 里，把后续输出全部污染。 */
    e->low = (UInt64)((UInt32)e->low << 8);
}

static inline void re_normalize(RangeEnc* e)
{
    while (e->range < kTopValue) { e->range <<= 8; re_shift_low(e); }
}

static inline void re_encode(RangeEnc* e, UInt16* prob, int sym)
{
    UInt32 bound = (e->range >> kNumBitModelTotalBits) * (UInt32)(*prob);
    if (sym == 0) {
        e->range = bound;
        *prob = (UInt16)(*prob + ((kBitModelTotal - *prob) >> kNumMoveBits));
    }
    else {
        e->low += bound;
        e->range -= bound;
        *prob = (UInt16)(*prob - (*prob >> kNumMoveBits));
    }
    re_normalize(e);
}

static void re_bittree(RangeEnc* e, UInt16* probs, int numBits, UInt32 symbol)
{
    UInt32 m = 1;
    int i;
    for (i = numBits; i > 0;) {
        UInt32 bit;
        --i;
        bit = (symbol >> i) & 1;
        re_encode(e, &probs[m], (int)bit);
        m = (m << 1) + bit;
    }
}

static void re_bittree_rev(RangeEnc* e, UInt16* probs, int numBits, UInt32 symbol)
{
    UInt32 m = 1;
    int i;
    for (i = 0; i < numBits; i++) {
        UInt32 bit = symbol & 1;
        symbol >>= 1;
        re_encode(e, &probs[m], (int)bit);
        m = (m << 1) + bit;
    }
}

static void re_direct(RangeEnc* e, int numBits, UInt32 value)
{
    int i;
    for (i = numBits - 1; i >= 0; i--) {
        e->range >>= 1;
        if ((value >> i) & 1) e->low += e->range;
        re_normalize(e);
    }
}

static void re_len(RangeEnc* e, UInt16* lenBase, UInt32 posState, UInt32 len)
{
    if (len < kLenNumLowSymbols) {
        re_encode(e, &lenBase[LenChoice], 0);
        re_bittree(e, &lenBase[LenLow + (posState << kLenNumLowBits)], kLenNumLowBits, len);
    }
    else if (len < kLenNumLowSymbols + kLenNumMidSymbols) {
        re_encode(e, &lenBase[LenChoice], 1);
        re_encode(e, &lenBase[LenChoice2], 0);
        re_bittree(e, &lenBase[LenMid + (posState << kLenNumMidBits)], kLenNumMidBits,
                   len - kLenNumLowSymbols);
    }
    else {
        re_encode(e, &lenBase[LenChoice], 1);
        re_encode(e, &lenBase[LenChoice2], 1);
        re_bittree(e, &lenBase[LenHigh], kLenNumHighBits,
                   len - kLenNumLowSymbols - kLenNumMidSymbols);
    }
}

/* ================================================================== */
/* 距离编码                                                            */
/* ================================================================== */

/* dist -> posSlot（与解码端的表严格互逆）
 * posSlot s 的距离区间：
 *   s=4+2k : [2^(k+1), 2^(k+1)+2^k-1]      基础值 base=(2|0)<<k
 *   s=5+2k : [3*2^k,  4*2^k-1]              基础值 base=(2|1)<<k
 */
static UInt32 get_pos_slot(UInt32 dist)
{
    UInt32 n, s;
    if (dist < kStartPosModelIndex) return dist;
    n = 31;
    while (n > 0 && ((dist >> n) & 1u) == 0) n--;
    s = n << 1;
    if (dist >= (3u << (n - 1))) s += 1;
    return s;
}

static void re_distance(RangeEnc* e, const ProbModel* pm, UInt32 len, UInt32 dist)
{
    UInt32 lenToPosState = (len < kNumLenToPosStates ? len : kNumLenToPosStates - 1);
    UInt32 posSlot = get_pos_slot(dist);
    UInt16* p = pm->p;
    re_bittree(e, &p[ProbPosSlot + (lenToPosState << kNumPosSlotBits)], kNumPosSlotBits, posSlot);
    if (posSlot >= kStartPosModelIndex) {
        UInt32 numDirectBits = (posSlot >> 1) - 1;
        UInt32 base = (2 | (posSlot & 1)) << numDirectBits;
        UInt32 rest = dist - base;
        if (posSlot < kEndPosModelIndex) {
            re_bittree_rev(e, &p[ProbSpecPos + base - posSlot - 1], (int)numDirectBits, rest);
        }
        else {
            re_direct(e, (int)(numDirectBits - kNumAlignBits), rest >> kNumAlignBits);
            re_bittree_rev(e, &p[ProbAlign], kNumAlignBits, rest & ((1u << kNumAlignBits) - 1));
        }
    }
}

/* ================================================================== */
/* 字面量编码                                                          */
/* ================================================================== */

/* lit 指向当前字面量上下文的 0x300 概率块 */
static void re_literal(RangeEnc* e, UInt16* lit, Byte curByte, Byte matchByte, int useMatched)
{
    UInt32 symbol = 1;
    if (useMatched) {
        do {
            UInt32 matchBit = (matchByte >> 7) & 1;
            UInt32 bit = (curByte >> 7) & 1;
            matchByte = (Byte)(matchByte << 1);
            curByte = (Byte)(curByte << 1);
            re_encode(e, &lit[((1 + matchBit) << 8) + symbol], (int)bit);
            symbol = (symbol << 1) | bit;
            if (matchBit != bit) break;
        } while (symbol < 0x100);
    }
    while (symbol < 0x100) {
        UInt32 bit = (curByte >> 7) & 1;
        curByte = (Byte)(curByte << 1);
        re_encode(e, &lit[symbol], (int)bit);
        symbol = (symbol << 1) | bit;
    }
}

/* ================================================================== */
/* 进度回调                                                            */
/* ================================================================== */

/* 解码回调：int(INT inProcessed, INT outProcessed)；返回非 0 表示中止 */
typedef int (__cdecl *Progress2)(unsigned int inProcessed, unsigned int outProcessed);
/* 编码回调：int(pointer, LONG inSize, LONG outSize)；返回非 0 表示中止 */
typedef int (__cdecl *Progress3)(void* opaque, long inSize, long outSize);

/* ================================================================== */
/* 解压                                                                */
/* ================================================================== */

static int lzma_decode_stream(Byte* dest, size_t destLimit, const Byte* src, size_t srcLen,
                              ProbModel* pm, UInt32 lc, UInt32 lp, UInt32 pb,
                              Progress2 prog,
                              size_t* outPosOut, size_t* srcUsed)
{
    RangeDec rd;
    UInt16* p = pm->p;
    UInt32 state = 0;
    UInt32 rep0 = 0, rep1 = 0, rep2 = 0, rep3 = 0;
    size_t outPos = 0;
    UInt32 pbMask = ((UInt32)1 << pb) - 1;
    UInt32 lpMask = ((UInt32)1 << lp) - 1;
    size_t nextReport = 0;
    size_t reportSrc = 0;
    int ret = SZ_OK;

    if (!rd_init(&rd, src, srcLen)) return SZ_ERROR_INPUT_EOF;

    while (outPos < destLimit) {
        UInt32 posState = (UInt32)outPos & pbMask;
        if (rd_bit(&rd, &p[ProbIsMatch + (state << kNumPosBitsMax) + posState]) == 0) {
            /* 字面量 */
            UInt32 prevByte = (outPos > 0) ? dest[outPos - 1] : 0;
            UInt32 litState = (((UInt32)outPos & lpMask) << lc) | (prevByte >> (8 - lc));
            UInt16* lit = &p[ProbLiteral + (size_t)litState * kLiteralBlockSize];
            UInt32 symbol = 1;
            if (state >= 7) {
                Byte matchByte = 0;
                if (rep0 + 1 <= outPos) matchByte = dest[outPos - rep0 - 1];
                do {
                    UInt32 matchBit = (matchByte >> 7) & 1;
                    UInt32 bit;
                    matchByte = (Byte)(matchByte << 1);
                    bit = (UInt32)rd_bit(&rd, &lit[((1 + matchBit) << 8) + symbol]);
                    symbol = (symbol << 1) | bit;
                    if (matchBit != bit) break;
                } while (symbol < 0x100 && !rd.err);
            }
            while (symbol < 0x100 && !rd.err) {
                UInt32 bit = (UInt32)rd_bit(&rd, &lit[symbol]);
                symbol = (symbol << 1) | bit;
            }
            if (rd.err) { ret = SZ_ERROR_INPUT_EOF; goto done; }
            dest[outPos++] = (Byte)symbol;
            state = (state < 4) ? 0 : ((state < 10) ? state - 3 : state - 6);
        }
        else {
            /* 匹配 */
            UInt32 len, dist = 0;
            if (rd_bit(&rd, &p[ProbIsRep + state]) != 0) {
                /* rep 匹配 */
                if (outPos == 0) { ret = SZ_ERROR_DATA; goto done; }
                if (rd_bit(&rd, &p[ProbIsRepG0 + state]) == 0) {
                    if (rd_bit(&rd, &p[ProbIsRep0Long + (state << kNumPosBitsMax) + posState]) == 0) {
                        state = (state < 7) ? 9 : 11;
                        dest[outPos] = dest[outPos - rep0 - 1];
                        outPos++;
                        continue;
                    }
                }
                else {
                    if (rd_bit(&rd, &p[ProbIsRepG1 + state]) == 0) {
                        dist = rep1;
                    }
                    else {
                        if (rd_bit(&rd, &p[ProbIsRepG2 + state]) == 0) dist = rep2;
                        else { dist = rep3; rep3 = rep2; }
                        rep2 = rep1;
                    }
                    rep1 = rep0;
                    rep0 = dist;
                }
                len = rd_len(&rd, &p[ProbRepLenCoder], posState) + kMatchMinLen;
                state = (state < 7) ? 8 : 11;
            }
            else {
                UInt32 posSlot, lenToPosState;
                rep3 = rep2; rep2 = rep1; rep1 = rep0;
                len = rd_len(&rd, &p[ProbLenCoder], posState) + kMatchMinLen;
                state = (state < 7) ? 7 : 10;
                lenToPosState = (len - kMatchMinLen < kNumLenToPosStates)
                              ? (len - kMatchMinLen) : (kNumLenToPosStates - 1);
                posSlot = rd_bittree(&rd, &p[ProbPosSlot + (lenToPosState << kNumPosSlotBits)],
                                     kNumPosSlotBits);
                if (posSlot >= kStartPosModelIndex) {
                    UInt32 numDirectBits = (posSlot >> 1) - 1;
                    dist = (2 | (posSlot & 1)) << numDirectBits;
                    if (posSlot < kEndPosModelIndex) {
                        dist += rd_bittree_rev(&rd, &p[ProbSpecPos + dist - posSlot - 1],
                                               (int)numDirectBits);
                    }
                    else {
                        dist += rd_direct(&rd, (int)(numDirectBits - kNumAlignBits)) << kNumAlignBits;
                        dist += rd_bittree_rev(&rd, &p[ProbAlign], kNumAlignBits);
                    }
                }
                else {
                    dist = posSlot;
                }
                if (dist == 0xFFFFFFFFu) break;   /* 流结束标记 */
                rep0 = dist;
            }
            /* 复制匹配 */
            if (rep0 + 1 > outPos) { ret = SZ_ERROR_DATA; goto done; }
            if (rd.err) { ret = SZ_ERROR_INPUT_EOF; goto done; }
            if (outPos + len > destLimit) len = (UInt32)(destLimit - outPos);
            {
                UInt32 i;
                for (i = 0; i < len; i++) {
                    dest[outPos] = dest[outPos - rep0 - 1];
                    outPos++;
                }
            }
        }
        if (rd.err) { ret = SZ_ERROR_INPUT_EOF; goto done; }
        if (prog) {
            while (outPos >= nextReport + kDecodeProgressChunk) {
                nextReport += kDecodeProgressChunk;
                if (prog((unsigned int)(rd.pos - reportSrc), kDecodeProgressChunk)) {
                    ret = SZ_ERROR_ABORTED;
                    goto done;
                }
                reportSrc = rd.pos;
            }
        }
    }

done:
    if (outPosOut) *outPosOut = outPos;
    if (srcUsed) *srcUsed = rd.pos;
    return ret;
}

LZMA_API int LzmaUncompress(unsigned char* dest, unsigned int* destLen,
                            const unsigned char* src, unsigned int* srcLen,
                            const unsigned char* props, unsigned int propsSize)
{
    ProbModel pm;
    UInt32 lc, lp, pb, dictSize;
    size_t outPos = 0, srcUsed = 0;
    int res;

    if (!dest || !destLen || !src || !srcLen || !props) return SZ_ERROR_DATA;
    if (propsSize < 5) return SZ_ERROR_DATA;

    {
        Byte d = props[0];
        lc = d % 9; d = (Byte)(d / 9);
        lp = d % 5;
        pb = d / 5;
        dictSize = (UInt32)props[1] | ((UInt32)props[2] << 8) |
                   ((UInt32)props[3] << 16) | ((UInt32)props[4] << 24);
    }
    if (lc > 8 || lp > 4 || pb > 4) return SZ_ERROR_DATA;
    if (lc + lp > 4) return SZ_ERROR_DATA;      /* LZMA1 约束 */
    if (dictSize < (1u << 12)) dictSize = 1u << 12;

    probs_init(&pm, lc, lp);
    if (!pm.p) return SZ_ERROR_MEM;

    res = lzma_decode_stream(dest, (size_t)*destLen, src, (size_t)*srcLen, &pm,
                             lc, lp, pb, NULL, &outPos, &srcUsed);
    probs_free(&pm);
    if (res != SZ_OK && res != SZ_ERROR_INPUT_EOF) return res;

    *destLen = (unsigned int)outPos;
    *srcLen = (unsigned int)srcUsed;
    return SZ_OK;
}

/* ================================================================== */
/* 匹配查找                                                            */
/* ================================================================== */

#define kHashBits   16
#define kHashSize   (1u << kHashBits)
#define kMinMatch   3
#define kMaxMatch   273
#define kNiceLen    64

typedef struct {
    UInt32* head;     /* 哈希链头：位置+1 */
    UInt32* chain;    /* 前一个位置+1 */
    size_t   size;
} MatchFinder;

static int mf_init(MatchFinder* mf, size_t size)
{
    mf->size = size;
    mf->head = (UInt32*)calloc(kHashSize, sizeof(UInt32));
    mf->chain = (UInt32*)calloc(size + 1, sizeof(UInt32));
    if (!mf->head || !mf->chain) {
        free(mf->head); free(mf->chain);
        mf->head = NULL; mf->chain = NULL;
        return 0;
    }
    return 1;
}

static void mf_free(MatchFinder* mf)
{
    free(mf->head); free(mf->chain);
    mf->head = NULL; mf->chain = NULL;
}

static inline UInt32 mf_hash(const Byte* p)
{
    UInt32 v = (UInt32)p[0] | ((UInt32)p[1] << 8) | ((UInt32)p[2] << 16) | ((UInt32)p[3] << 24);
    return (v * 2654435761u) >> (32 - kHashBits);
}

static inline void mf_insert(MatchFinder* mf, const Byte* src, size_t pos)
{
    UInt32 h;
    if (pos + 4 > mf->size) return;
    h = mf_hash(src + pos);
    mf->chain[pos] = mf->head[h];
    mf->head[h] = (UInt32)(pos + 1);
}

/* 在 src[pos] 处找最长匹配：返回长度（0 表示无匹配），*distPtr = 距离-1 */
static UInt32 mf_find(const MatchFinder* mf, const Byte* src, size_t pos,
                       size_t limit, UInt32 dictSize, UInt32 maxDepth, UInt32* distPtr)
{
    UInt32 bestLen = 0, bestDist = 0;
    UInt32 cur, depth = 0;
    size_t maxLen = limit - pos;

    if (maxLen > kMaxMatch) maxLen = kMaxMatch;
    if (maxLen < kMinMatch) return 0;

    cur = mf->head[mf_hash(src + pos)];
    while (cur != 0 && depth++ < maxDepth) {
        size_t cand = (size_t)cur - 1;
        size_t i;
        UInt32 len = 0;
        if (cand >= pos) break;
        if ((UInt32)(pos - cand) > dictSize) break;
        for (i = 0; i < maxLen; i++) {
            if (src[cand + i] != src[pos + i]) break;
            len++;
        }
        if (len > bestLen) {
            bestLen = len;
            bestDist = (UInt32)(pos - cand - 1);
            if (len >= kNiceLen || len >= maxLen) break;
        }
        cur = mf->chain[cand];
    }
    if (bestLen < kMinMatch) return 0;
    *distPtr = bestDist;
    return bestLen;
}

/* ================================================================== */
/* 压缩                                                                */
/* ================================================================== */

/* 编码进度：int(pointer, LONG inSize, LONG outSize) */
typedef struct {
    Progress3 cb;
    void*     opaque;
    size_t    nextReport;
} EncodeProgress;

static int lzma_compress_core(unsigned char* dest, unsigned int* destLen,
                              const unsigned char* src, unsigned int srcLen,
                              unsigned char* outProps, unsigned int* outPropsSize,
                              int level, unsigned int dictSize,
                              int lc, int lp, int pb,
                              EncodeProgress* ep)
{
    ProbModel pm;
    RangeEnc re;
    MatchFinder mf;
    UInt32 state = 0, rep0 = 0;
    size_t pos = 0;
    UInt32 pbMask, lpMask;
    UInt32 maxDepth;
    int res = SZ_OK;

    if (!dest || !destLen || !outProps || !outPropsSize) return SZ_ERROR_DATA;
    if (!src && srcLen != 0) return SZ_ERROR_DATA;   /* 空输入允许 src 为 NULL */
    if (*outPropsSize < 5) return SZ_ERROR_DATA;
    if (lc < 0 || lc > 8 || lp < 0 || lp > 4 || pb < 0 || pb > 4) return SZ_ERROR_DATA;
    if (lc + lp > 4) return SZ_ERROR_DATA;          /* LZMA1 约束 */
    if (dictSize < (1u << 12)) dictSize = 1u << 12;

    maxDepth = (level <= 0) ? 8 : ((level >= 9) ? 256 : (UInt32)(level * 24));

    outProps[0] = (Byte)((pb * 5 + lp) * 9 + lc);
    outProps[1] = (Byte)(dictSize & 0xFF);
    outProps[2] = (Byte)((dictSize >> 8) & 0xFF);
    outProps[3] = (Byte)((dictSize >> 16) & 0xFF);
    outProps[4] = (Byte)((dictSize >> 24) & 0xFF);
    *outPropsSize = 5;

    if (!mf_init(&mf, (size_t)srcLen)) return SZ_ERROR_MEM;

    probs_init(&pm, (UInt32)lc, (UInt32)lp);
    if (!pm.p) { mf_free(&mf); return SZ_ERROR_MEM; }

    re_init(&re, dest, (size_t)*destLen);
    pbMask = ((UInt32)1 << pb) - 1;
    lpMask = ((UInt32)1 << lp) - 1;
    if (ep) {
        ep->nextReport = kEncodeProgressChunk;
    }

    while (pos < (size_t)srcLen) {
        UInt32 posState = (UInt32)pos & pbMask;
        UInt32 len = 0, dist = 0;

        if (pos + kMinMatch <= (size_t)srcLen) {
            UInt32 candLen = mf_find(&mf, src, pos, (size_t)srcLen, dictSize, maxDepth, &dist);
            if (candLen >= kMinMatch) len = candLen;
        }

        if (len >= kMinMatch) {
            UInt32 i;
            re_encode(&re, &pm.p[ProbIsMatch + (state << kNumPosBitsMax) + posState], 1);
            re_encode(&re, &pm.p[ProbIsRep + state], 0);
            re_len(&re, &pm.p[ProbLenCoder], posState, len - kMatchMinLen);
            re_distance(&re, &pm, len - kMatchMinLen, dist);
            rep0 = dist;
            state = (state < 7) ? 7 : 10;
            for (i = 0; i < len; i++) mf_insert(&mf, src, pos + i);
            pos += len;
        }
        else {
            Byte cur = src[pos];
            Byte prev = (pos > 0 ? src[pos - 1] : 0);
            Byte matchByte = 0;
            UInt32 litState = (((UInt32)pos & lpMask) << lc) | ((UInt32)prev >> (8 - lc));
            UInt16* lit = &pm.p[ProbLiteral + (size_t)litState * kLiteralBlockSize];
            if (state >= 7 && rep0 + 1 <= pos) matchByte = src[pos - rep0 - 1];
            re_encode(&re, &pm.p[ProbIsMatch + (state << kNumPosBitsMax) + posState], 0);
            re_literal(&re, lit, cur, matchByte, state >= 7);
            state = (state < 4) ? 0 : ((state < 10) ? state - 3 : state - 6);
            mf_insert(&mf, src, pos);
            pos++;
        }
        if (re.err) { res = SZ_ERROR_OUTPUT_EOF; break; }
        if (ep && pos >= ep->nextReport) {
            if (ep->cb(ep->opaque, (long)pos, 0)) { res = SZ_ERROR_ABORTED; break; }
            ep->nextReport = ((pos / kEncodeProgressChunk) + 1) * kEncodeProgressChunk;
        }
    }

    if (res == SZ_OK) {
        /* 不写结束标记：官方 LzmaCompress（即 LZMA SDK 的 LzmaCompress）
         * 同样是 writeEndMark=false —— 解码端靠「输出缓冲区已填满」判断流结束。
         * 官方 unCompress 传入的 destLen 正是原始大小，所以官方流同样没有结束标记
         * （实测：官方流被本实现解码时会多出 1 字节，即没有结束标记）。
         * 冲刷范围编码器 */
        int i;
        for (i = 0; i < 5; i++) re_shift_low(&re);
        if (re.err) res = SZ_ERROR_OUTPUT_EOF;
    }

    mf_free(&mf);
    probs_free(&pm);
    if (res != SZ_OK) return res;
    *destLen = (unsigned int)re.pos;
    return SZ_OK;
}

LZMA_API int LzmaCompress(unsigned char* dest, unsigned int* destLen,
                          const unsigned char* src, unsigned int srcLen,
                          unsigned char* outProps, unsigned int* outPropsSize,
                          int level, unsigned int dictSize,
                          int lc, int lp, int pb, int fb, int numThreads)
{
    (void)fb; (void)numThreads;
    return lzma_compress_core(dest, destLen, src, srcLen, outProps, outPropsSize,
                              level, dictSize, lc, lp, pb, NULL);
}

/* ================================================================== */
/* 文件级接口                                                          */
/* ================================================================== */

static int read_whole_file(const wchar_t* path, Byte** data, size_t* size)
{
    FILE* f;
    long len;
    Byte* buf;
    *data = NULL; *size = 0;
    f = _wfopen(path, L"rb");
    if (!f) return SZ_ERROR_DATA;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return SZ_ERROR_DATA; }
    len = ftell(f);
    if (len < 0) { fclose(f); return SZ_ERROR_DATA; }
    rewind(f);
    buf = (Byte*)malloc((size_t)len + 1);
    if (!buf) { fclose(f); return SZ_ERROR_MEM; }
    if (len > 0 && fread(buf, 1, (size_t)len, f) != (size_t)len) {
        free(buf); fclose(f); return SZ_ERROR_DATA;
    }
    fclose(f);
    *data = buf; *size = (size_t)len;
    return SZ_OK;
}

static int write_whole_file(const wchar_t* path, const Byte* data, size_t size)
{
    FILE* f = _wfopen(path, L"wb");
    if (!f) return SZ_ERROR_DATA;
    if (size > 0 && fwrite(data, 1, size, f) != size) { fclose(f); return SZ_ERROR_DATA; }
    fclose(f);
    return SZ_OK;
}

LZMA_API int lzmafile(const wchar_t* inPath, const wchar_t* outPath,
                      void* progress, int encodeMode)
{
    Byte* in = NULL, *out = NULL;
    size_t inSize = 0, outSize = 0;
    int res;

    if (!inPath || !outPath) return SZ_ERROR_DATA;

    res = read_whole_file(inPath, &in, &inSize);
    if (res != SZ_OK) return res;

    if (encodeMode) {
        /* 压缩成 .lzma 文件：13 字节头 + 码流 */
        Byte header[13];
        unsigned int propsSize = 5;
        unsigned int bodySize = (unsigned int)(inSize * 2 + 64);
        unsigned char* body;
        EncodeProgress ep;

        body = (unsigned char*)malloc(bodySize ? bodySize : 1);
        if (!body) { free(in); return SZ_ERROR_MEM; }

        header[0] = (Byte)((2 * 5 + 0) * 9 + 3);           /* lc=3,lp=0,pb=2 */
        header[1] = 0x00; header[2] = 0x00; header[3] = 0x00; header[4] = 0x01; /* dict 16MB */
        header[5] = (Byte)(inSize & 0xFF);
        header[6] = (Byte)((inSize >> 8) & 0xFF);
        header[7] = (Byte)((inSize >> 16) & 0xFF);
        header[8] = (Byte)((inSize >> 24) & 0xFF);
        header[9] = (Byte)((inSize >> 32) & 0xFF);
        header[10] = (Byte)((inSize >> 40) & 0xFF);
        header[11] = (Byte)((inSize >> 48) & 0xFF);
        header[12] = (Byte)((inSize >> 56) & 0xFF);

        ep.cb = (Progress3)progress;
        ep.opaque = progress;
        ep.nextReport = kEncodeProgressChunk;
        res = lzma_compress_core(body, &bodySize, in, (unsigned int)inSize,
                                 header, &propsSize, 5, 0x1000000, 3, 0, 2,
                                 progress ? &ep : NULL);
        if (res != SZ_OK) { free(body); free(in); return res; }

        outSize = 13 + bodySize;
        out = (Byte*)malloc(outSize);
        if (!out) { free(body); free(in); return SZ_ERROR_MEM; }
        memcpy(out, header, 13);
        memcpy(out + 13, body, bodySize);
        free(body);
    }
    else {
        /* 解压 .lzma 文件 */
        unsigned int propsSize = 5;
        unsigned int bodyLen, destLen;
        const Byte* body;
        unsigned long long fileSize;
        unsigned int i;
        UInt32 lc, lp, pb, dictSize;
        ProbModel pm;
        size_t outPos = 0, srcUsed = 0;

        if (inSize < 13) { free(in); return SZ_ERROR_DATA; }
        fileSize = 0;
        for (i = 0; i < 8; i++) fileSize |= ((unsigned long long)in[5 + i]) << (i * 8);
        body = in + 13;
        bodyLen = (unsigned int)(inSize - 13);
        if (fileSize == 0xFFFFFFFFFFFFFFFFull) destLen = (unsigned int)(bodyLen * 50 + 1024);
        else destLen = (unsigned int)fileSize;
        if (destLen == 0) destLen = (unsigned int)(bodyLen * 50 + 1024);
        out = (Byte*)malloc(destLen ? destLen : 1);
        if (!out) { free(in); return SZ_ERROR_MEM; }

        {
            Byte d = in[0];
            lc = d % 9; d = (Byte)(d / 9);
            lp = d % 5;
            pb = d / 5;
            dictSize = (UInt32)in[1] | ((UInt32)in[2] << 8) |
                       ((UInt32)in[3] << 16) | ((UInt32)in[4] << 24);
        }
        (void)dictSize; (void)propsSize;
        probs_init(&pm, lc, lp);
        if (!pm.p) { free(out); free(in); return SZ_ERROR_MEM; }
        res = lzma_decode_stream(out, (size_t)destLen, body, (size_t)bodyLen, &pm,
                                 lc, lp, pb, (Progress2)progress, &outPos, &srcUsed);
        probs_free(&pm);
        if (res != SZ_OK && res != SZ_ERROR_INPUT_EOF) { free(out); free(in); return res; }
        outSize = outPos;
    }

    free(in);
    res = write_whole_file(outPath, out, outSize);
    free(out);
    return res;
}
