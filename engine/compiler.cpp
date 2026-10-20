// compiler.cpp — binary bytecode compiler and deserializer for Nodio.
#include "engine/compiler.h"
#include "engine/parser.h"
#include <cstring>
#include <unordered_map>

namespace NodioVM
{
    const char kBytecodeMagic[8] = { '=', 'n', 'o', 'd', 'i', 'o', 0x01, 0x00 };

    bool IsBytecodeData(const void* data, size_t size)
    {
        if (!data || size < 8) return false;
        return memcmp(data, kBytecodeMagic, 8) == 0;
    }

    bool IsBytecode(const wstring& src)
    {
        if (src.size() < 8) return false;
        for (size_t i = 0; i < 8; ++i)
        {
            if ((src[i] & 0xFF) != (unsigned char)kBytecodeMagic[i])
                return false;
        }
        return true;
    }

    static inline void WriteU8(std::vector<uint8_t>& out, uint8_t v)
    {
        out.push_back(v);
    }

    static inline void WriteU32(std::vector<uint8_t>& out, uint32_t v)
    {
        out.push_back((uint8_t)(v & 0xFF));
        out.push_back((uint8_t)((v >> 8) & 0xFF));
        out.push_back((uint8_t)((v >> 16) & 0xFF));
        out.push_back((uint8_t)((v >> 24) & 0xFF));
    }

    static inline void WriteDouble(std::vector<uint8_t>& out, double d)
    {
        uint64_t v;
        memcpy(&v, &d, sizeof(double));
        for (int i = 0; i < 8; ++i)
        {
            out.push_back((uint8_t)(v & 0xFF));
            v >>= 8;
        }
    }

    static void CollectStrings(PNode n, std::unordered_map<wstring, uint32_t>& strMap,
                               std::vector<wstring>& strPool)
    {
        if (!n) return;
        auto addStr = [&](const wstring& s) {
            if (!s.empty() && strMap.find(s) == strMap.end())
            {
                uint32_t idx = (uint32_t)strPool.size();
                strMap[s] = idx;
                strPool.push_back(s);
            }
        };

        addStr(n->str);
        for (const auto& name : n->names)
            addStr(name);

        CollectStrings(n->a, strMap, strPool);
        CollectStrings(n->b, strMap, strPool);
        CollectStrings(n->c, strMap, strPool);
        CollectStrings(n->d, strMap, strPool);
        for (const auto& item : n->list)
            CollectStrings(item, strMap, strPool);
        for (const auto& item : n->defaults)
            CollectStrings(item, strMap, strPool);
    }

    static void WriteNode(PNode n, std::vector<uint8_t>& out,
                          const std::unordered_map<wstring, uint32_t>& strMap)
    {
        if (!n)
        {
            WriteU8(out, 0xFF);
            return;
        }
        WriteU8(out, (uint8_t)n->k);

        uint8_t flags = 0;
        if (n->bRoot) flags |= 1;
        if (n->bSafe) flags |= 2;
        if (n->bDirect) flags |= 4;
        if (n->bMeta) flags |= 8;
        if (n->bUtf16) flags |= 16;
        if (n->bRawBytes) flags |= 32;
        WriteU8(out, flags);

        WriteU32(out, (uint32_t)n->line);

        if (n->k == Node::Num)
        {
            WriteDouble(out, n->num);
        }

        uint8_t mask = 0;
        if (!n->str.empty()) mask |= 1;
        if (n->a) mask |= 2;
        if (n->b) mask |= 4;
        if (n->c) mask |= 8;
        if (n->d) mask |= 16;
        if (!n->list.empty()) mask |= 32;
        if (!n->names.empty()) mask |= 64;
        if (!n->defaults.empty()) mask |= 128;
        WriteU8(out, mask);

        if (mask & 1)
        {
            auto it = strMap.find(n->str);
            WriteU32(out, it != strMap.end() ? it->second : 0xFFFFFFFF);
        }
        if (mask & 2) WriteNode(n->a, out, strMap);
        if (mask & 4) WriteNode(n->b, out, strMap);
        if (mask & 8) WriteNode(n->c, out, strMap);
        if (mask & 16) WriteNode(n->d, out, strMap);
        if (mask & 32)
        {
            WriteU32(out, (uint32_t)n->list.size());
            for (const auto& item : n->list)
                WriteNode(item, out, strMap);
        }
        if (mask & 64)
        {
            WriteU32(out, (uint32_t)n->names.size());
            for (const auto& nm : n->names)
            {
                auto it = strMap.find(nm);
                WriteU32(out, it != strMap.end() ? it->second : 0xFFFFFFFF);
            }
        }
        if (mask & 128)
        {
            WriteU32(out, (uint32_t)n->defaults.size());
            for (const auto& def : n->defaults)
                WriteNode(def, out, strMap);
        }
    }

    bool SerializeNode(PNode root, std::vector<uint8_t>& outBytecode)
    {
        outBytecode.clear();
        for (int i = 0; i < 8; ++i)
            outBytecode.push_back((uint8_t)kBytecodeMagic[i]);

        // String pool
        std::unordered_map<wstring, uint32_t> strMap;
        std::vector<wstring> strPool;
        CollectStrings(root, strMap, strPool);

        WriteU32(outBytecode, (uint32_t)strPool.size());
        for (const auto& str : strPool)
        {
            int utf8Len = WideCharToMultiByte(CP_UTF8, 0, str.c_str(), (int)str.size(),
                                             nullptr, 0, nullptr, nullptr);
            if (utf8Len <= 0)
            {
                WriteU32(outBytecode, 0);
            }
            else
            {
                WriteU32(outBytecode, (uint32_t)utf8Len);
                size_t oldSize = outBytecode.size();
                outBytecode.resize(oldSize + utf8Len);
                WideCharToMultiByte(CP_UTF8, 0, str.c_str(), (int)str.size(),
                                    (char*)&outBytecode[oldSize], utf8Len, nullptr, nullptr);
            }
        }

        WriteNode(root, outBytecode, strMap);
        return true;
    }

    struct ByteReader
    {
        const uint8_t* ptr;
        size_t rem;

        bool ReadU8(uint8_t& out)
        {
            if (rem < 1) return false;
            out = *ptr++;
            rem--;
            return true;
        }

        bool ReadU32(uint32_t& out)
        {
            if (rem < 4) return false;
            out = (uint32_t)ptr[0] | ((uint32_t)ptr[1] << 8) |
                  ((uint32_t)ptr[2] << 16) | ((uint32_t)ptr[3] << 24);
            ptr += 4;
            rem -= 4;
            return true;
        }

        bool ReadDouble(double& out)
        {
            if (rem < 8) return false;
            uint64_t v = 0;
            for (int i = 0; i < 8; ++i)
                v |= ((uint64_t)ptr[i]) << (8 * i);
            memcpy(&out, &v, sizeof(double));
            ptr += 8;
            rem -= 8;
            return true;
        }

        bool ReadBytes(uint8_t* dest, size_t count)
        {
            if (rem < count) return false;
            memcpy(dest, ptr, count);
            ptr += count;
            rem -= count;
            return true;
        }
    };

    static PNode ReadNode(ByteReader& r, const std::vector<wstring>& strPool, wstring* err)
    {
        uint8_t k = 0;
        if (!r.ReadU8(k))
        {
            if (err) *err = L"字节码读取错误: 截断的节点操作码";
            return nullptr;
        }
        if (k == 0xFF) return nullptr;
        if (k > (uint8_t)Node::DoWhile)
        {
            if (err) *err = L"字节码格式非法: 无效的节点类型 " + std::to_wstring((int)k);
            return nullptr;
        }

        auto n = std::make_shared<Node>();
        n->k = (Node::K)k;

        uint8_t flags = 0;
        if (!r.ReadU8(flags))
        {
            if (err) *err = L"字节码读取错误: 截断的标志位";
            return nullptr;
        }
        n->bRoot = (flags & 1) != 0;
        n->bSafe = (flags & 2) != 0;
        n->bDirect = (flags & 4) != 0;
        n->bMeta = (flags & 8) != 0;
        n->bUtf16 = (flags & 16) != 0;
        n->bRawBytes = (flags & 32) != 0;

        uint32_t line = 0;
        if (!r.ReadU32(line))
        {
            if (err) *err = L"字节码读取错误: 截断的行号";
            return nullptr;
        }
        n->line = (int)line;

        if (n->k == Node::Num)
        {
            if (!r.ReadDouble(n->num))
            {
                if (err) *err = L"字节码读取错误: 截断的浮点常数";
                return nullptr;
            }
        }

        uint8_t mask = 0;
        if (!r.ReadU8(mask))
        {
            if (err) *err = L"字节码读取错误: 截断的掩码";
            return nullptr;
        }

        if (mask & 1)
        {
            uint32_t strIdx = 0;
            if (!r.ReadU32(strIdx) || strIdx >= strPool.size())
            {
                if (err) *err = L"字节码读取错误: 越界的字符串索引";
                return nullptr;
            }
            n->str = strPool[strIdx];
        }

        if (mask & 2) { n->a = ReadNode(r, strPool, err); if (err && !err->empty()) return nullptr; }
        if (mask & 4) { n->b = ReadNode(r, strPool, err); if (err && !err->empty()) return nullptr; }
        if (mask & 8) { n->c = ReadNode(r, strPool, err); if (err && !err->empty()) return nullptr; }
        if (mask & 16) { n->d = ReadNode(r, strPool, err); if (err && !err->empty()) return nullptr; }

        if (mask & 32)
        {
            uint32_t listCount = 0;
            if (!r.ReadU32(listCount))
            {
                if (err) *err = L"字节码读取错误: 截断的列表大小";
                return nullptr;
            }
            n->list.resize(listCount);
            for (uint32_t i = 0; i < listCount; ++i)
            {
                n->list[i] = ReadNode(r, strPool, err);
                if (err && !err->empty()) return nullptr;
            }
        }

        if (mask & 64)
        {
            uint32_t namesCount = 0;
            if (!r.ReadU32(namesCount))
            {
                if (err) *err = L"字节码读取错误: 截断的符号名列表";
                return nullptr;
            }
            n->names.resize(namesCount);
            for (uint32_t i = 0; i < namesCount; ++i)
            {
                uint32_t idx = 0;
                if (!r.ReadU32(idx) || idx >= strPool.size())
                {
                    if (err) *err = L"字节码读取错误: 越界的符号名索引";
                    return nullptr;
                }
                n->names[i] = strPool[idx];
            }
        }

        if (mask & 128)
        {
            uint32_t defCount = 0;
            if (!r.ReadU32(defCount))
            {
                if (err) *err = L"字节码读取错误: 截断的默认参数列表";
                return nullptr;
            }
            n->defaults.resize(defCount);
            for (uint32_t i = 0; i < defCount; ++i)
            {
                n->defaults[i] = ReadNode(r, strPool, err);
                if (err && !err->empty()) return nullptr;
            }
        }

        return n;
    }

    PNode DeserializeNode(const uint8_t* data, size_t size, wstring* err)
    {
        if (!IsBytecodeData(data, size))
        {
            if (err) *err = L"非法字节码魔数头";
            return nullptr;
        }

        ByteReader r{ data + 8, size - 8 };

        uint32_t strCount = 0;
        if (!r.ReadU32(strCount))
        {
            if (err) *err = L"字节码损坏: 无法读取常量池大小";
            return nullptr;
        }

        std::vector<wstring> strPool(strCount);
        std::vector<char> u8buf;
        for (uint32_t i = 0; i < strCount; ++i)
        {
            uint32_t u8len = 0;
            if (!r.ReadU32(u8len))
            {
                if (err) *err = L"字节码损坏: 无法读取字符串长度";
                return nullptr;
            }
            if (u8len == 0)
            {
                strPool[i] = L"";
                continue;
            }
            u8buf.resize(u8len);
            if (!r.ReadBytes((uint8_t*)u8buf.data(), u8len))
            {
                if (err) *err = L"字节码损坏: 无法读取字符串内容";
                return nullptr;
            }
            int wlen = MultiByteToWideChar(CP_UTF8, 0, u8buf.data(), (int)u8len, nullptr, 0);
            if (wlen > 0)
            {
                strPool[i].resize(wlen);
                MultiByteToWideChar(CP_UTF8, 0, u8buf.data(), (int)u8len, &strPool[i][0], wlen);
            }
        }

        return ReadNode(r, strPool, err);
    }

    PNode DeserializeNode(const wstring& src, wstring* err)
    {
        std::vector<uint8_t> bytes(src.size());
        for (size_t i = 0; i < src.size(); ++i)
            bytes[i] = (uint8_t)(src[i] & 0xFF);
        return DeserializeNode(bytes.data(), bytes.size(), err);
    }

    bool CompileScript(const wstring& src, std::vector<uint8_t>& outBytecode,
                       wstring* err, const wstring* pBaseDir)
    {
        wstring localErr;
        vector<Tok> toks = Lex(src, &localErr);
        if (!localErr.empty())
        {
            if (err) *err = localErr;
            return false;
        }

        Parser ps{ toks, 0, &localErr };
        if (pBaseDir)
            ps.strEmbedBase = *pBaseDir;
        ps.strEmbedRoot = EmbedRoot();
        PNode prog = ps.ParseBlock();
        if (!localErr.empty())
        {
            if (err) *err = localErr;
            return false;
        }

        return SerializeNode(prog, outBytecode);
    }
}
