// ── Nodio x64 FASM 汇编器桥接实现 ─────────────────────────────────────────────
// 官方 lib/raw/fasm 使用 32 位 FASM.DLL（导出 3 个 C 符号），在 x64 进程下无法加载。
// 本文件实现 64 位原生 fasm64.dll，导出与官方完全一致的 3 个符号：
//   - fasm_GetVersion
//   - fasm_Assemble
//   - fasm_AssembleFile
// ─────────────────────────────────────────────────────────────────────────────

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#pragma pack(push, 4)
typedef struct {
    int condition;      // 0 = FASM_OK, 2 = FASM_ERROR
    int length;         // 机器码长度
    void* outputData;   // 指向机器码
} FASM_STATE;

typedef struct {
    char* filePath;
    int lineNumber;
    int fileOffset;
    int macroLine;
} FASM_LINE_HEADER;

typedef struct {
    int condition;
    int errorCode;
    FASM_LINE_HEADER* errorLine;
} FASM_STATE_ERROR;
#pragma pack(pop)

// 静态错误行信息备用
static FASM_LINE_HEADER g_errLine = { "", 1, 0, 0 };

__declspec(dllexport) int __cdecl fasm_GetVersion(void) {
    // 官方 1.71 版本：低 16 位为 1，高 16 位为 71 -> 0x00470001
    return 0x00470001;
}

// 简单指令解析器：支持常见基础汇编和原始字节定义（如 db, nop, ret, int3 等）
static int assemble_simple(const char* src, unsigned char* outBuf, int maxOut, int* outLen, int* outErrCode) {
    int pos = 0;
    const char* p = src;
    char line[512];
    int lineNum = 0;

    while (*p) {
        // 提取一行
        int idx = 0;
        while (*p && *p != '\r' && *p != '\n' && idx < 510) {
            line[idx++] = *p++;
        }
        line[idx] = '\0';
        while (*p == '\r' || *p == '\n') p++;
        lineNum++;

        // 去除前后空白与注释
        char* start = line;
        while (*start && isspace((unsigned char)*start)) start++;
        char* comment = strchr(start, ';');
        if (comment) *comment = '\0';
        int len = (int)strlen(start);
        while (len > 0 && isspace((unsigned char)start[len - 1])) {
            start[--len] = '\0';
        }
        if (len == 0) continue;

        // 转小写比较
        char lower[512];
        for (int i = 0; i <= len; i++) {
            lower[i] = (char)tolower((unsigned char)start[i]);
        }

        if (strcmp(lower, "use32") == 0 || strcmp(lower, "use64") == 0 || strcmp(lower, "use16") == 0) {
            continue;
        } else if (strcmp(lower, "nop") == 0) {
            if (pos >= maxOut) { *outErrCode = -135; return -2; }
            outBuf[pos++] = 0x90;
        } else if (strcmp(lower, "ret") == 0 || strcmp(lower, "retn") == 0) {
            if (pos >= maxOut) { *outErrCode = -135; return -2; }
            outBuf[pos++] = 0xC3;
        } else if (strcmp(lower, "retf") == 0) {
            if (pos >= maxOut) { *outErrCode = -135; return -2; }
            outBuf[pos++] = 0xCB;
        } else if (strcmp(lower, "int 3") == 0 || strcmp(lower, "int3") == 0) {
            if (pos >= maxOut) { *outErrCode = -135; return -2; }
            outBuf[pos++] = 0xCC;
        } else if (strcmp(lower, "xor eax,eax") == 0 || strcmp(lower, "xor eax, eax") == 0) {
            if (pos + 2 > maxOut) { *outErrCode = -135; return -2; }
            outBuf[pos++] = 0x31;
            outBuf[pos++] = 0xC0;
        } else if (strcmp(lower, "xor rax,rax") == 0 || strcmp(lower, "xor rax, rax") == 0) {
            if (pos + 3 > maxOut) { *outErrCode = -135; return -2; }
            outBuf[pos++] = 0x48;
            outBuf[pos++] = 0x31;
            outBuf[pos++] = 0xC0;
        } else if (strncmp(lower, "db ", 3) == 0) {
            // 解析 db 0x90, 0xC3 ...
            const char* dp = start + 3;
            while (*dp) {
                while (*dp && (isspace((unsigned char)*dp) || *dp == ',')) dp++;
                if (!*dp) break;
                char* endp = NULL;
                long val = strtol(dp, &endp, 0);
                if (endp == dp) {
                    *outErrCode = -109; // Invalid operand
                    g_errLine.lineNumber = lineNum;
                    return 2;
                }
                if (pos >= maxOut) { *outErrCode = -135; return -2; }
                outBuf[pos++] = (unsigned char)(val & 0xFF);
                dp = endp;
            }
        } else {
            // 未实现的复杂指令，返回 Illegal instruction 错误
            *outErrCode = -108; // Illegal instruction
            g_errLine.lineNumber = lineNum;
            return 2; // _FASM_ERROR
        }
    }

    *outLen = pos;
    return 0; // _FASM_OK
}

__declspec(dllexport) int __cdecl fasm_Assemble(
    const char* lpSource,
    void* lpMemory,
    unsigned int cbMemorySize,
    unsigned int nPassesLimit,
    void* hDisplayPipe
) {
    if (!lpSource || !lpMemory || cbMemorySize < 64) {
        return -1; // Invalid Parameter
    }

    // 预留头部：前 32 字节用于放置 FASM_STATE
    unsigned char* base = (unsigned char*)lpMemory;
    unsigned char* codeOut = base + 32;
    int maxCodeSize = (int)cbMemorySize - 32;

    int outLen = 0;
    int errCode = 0;
    int ret = assemble_simple(lpSource, codeOut, maxCodeSize, &outLen, &errCode);

    if (ret == 0) {
        FASM_STATE* state = (FASM_STATE*)base;
        state->condition = 0;
        state->length = outLen;
        state->outputData = (void*)codeOut;
        return 0; // _FASM_OK
    } else if (ret == 2) {
        FASM_STATE_ERROR* stateErr = (FASM_STATE_ERROR*)base;
        stateErr->condition = 2;
        stateErr->errorCode = errCode;
        stateErr->errorLine = &g_errLine;
        return 2; // _FASM_ERROR
    } else {
        return ret;
    }
}

__declspec(dllexport) int __cdecl fasm_AssembleFile(
    const char* lpSourceFile,
    void* lpMemory,
    unsigned int cbMemorySize,
    unsigned int nPassesLimit,
    void* hDisplayPipe
) {
    if (!lpSourceFile) return -4; // Source not found

    FILE* fp = fopen(lpSourceFile, "rb");
    if (!fp) return -4;

    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (sz <= 0 || sz > 10 * 1024 * 1024) {
        fclose(fp);
        return -1;
    }

    char* buf = (char*)malloc(sz + 1);
    if (!buf) {
        fclose(fp);
        return -2;
    }

    fread(buf, 1, sz, fp);
    buf[sz] = '\0';
    fclose(fp);

    g_errLine.filePath = (char*)lpSourceFile;
    int ret = fasm_Assemble(buf, lpMemory, cbMemorySize, nPassesLimit, hDisplayPipe);
    free(buf);
    return ret;
}
