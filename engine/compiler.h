// compiler.h — binary bytecode compiler and deserializer for Nodio.
// Converts AST (Node tree) to/from a compact binary format.
#pragma once

#include "engine/common.h"
#include <vector>
#include <string>
#include <cstdint>

namespace NodioVM
{
    // Magic header for compiled bytecode: 8 bytes
    extern const char kBytecodeMagic[8];

    // Checks whether a memory buffer or engine string is compiled bytecode
    bool IsBytecodeData(const void* data, size_t size);
    bool IsBytecode(const wstring& src);

    // Serializes an AST node tree into compact binary bytecode.
    // When `err` is given, the result is verified by a round-trip read-back
    // and a failure is reported there instead of surfacing later.
    bool SerializeNode(PNode root, std::vector<uint8_t>& outBytecode,
                       wstring* err = nullptr);

    // Deserializes binary bytecode back into an AST node tree
    PNode DeserializeNode(const uint8_t* data, size_t size, wstring* err);
    PNode DeserializeNode(const wstring& src, wstring* err);

    // Compiles source code text directly into binary bytecode
    bool CompileScript(const wstring& src, std::vector<uint8_t>& outBytecode,
                       wstring* err, const wstring* pBaseDir);
}
