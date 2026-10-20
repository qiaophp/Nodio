// NodioVM.h — a self-contained tree-walking interpreter.
// Direct source execution so that published executables run for real.
// Language subset v1:
//   numbers/hex, strings (escapes), booleans, null
//   var locals, globals, member/index access, '..' root prefix
//   arithmetic + - * / % ^, concat .., comparisons, && || ! not, # length
//   ?: ternary, += etc.
//   function literals + named functions, calls
//   if/elseif/else, while, numeric for, break, continue, return
//   table constructors {a=1;["x"]=2;...}
//   builtins: io.print / print, tostring, tonumber
// Statement separation: newline or ';'.
#pragma once

#include "pch.h"
#include <memory>
#include <unordered_map>
#include <vector>
#include <string>
#include <set>

namespace NodioVM
{
    // Run a source string; returns the value of a top-level `return n` as
    // an int (0 when none), or -1 on a runtime error (strError filled).
    // pBaseDir: directory for `import name` file resolution.
    // pScriptFile: the script's own path — S-2, so runtime errors in the
    // main script report a file:line instead of a bare line number.
    int RunSource(const std::wstring& src, std::wstring* strError,
                  const std::wstring* pBaseDir = nullptr,
                  const std::unordered_map<std::wstring, std::wstring>*
                      pEmbedded = nullptr,
                  bool bDebug = false,
                  const std::wstring* pBreakLines = nullptr,
                  const std::wstring* pScriptFile = nullptr);

    // Parse-only syntax check:
    // 0 = clean, -1 = syntax error (strError filled, includes line number).
    int CheckSyntax(const std::wstring& src, std::wstring* strError);

    // Compile script source code to binary bytecode
    bool CompileScript(const std::wstring& src, std::vector<uint8_t>& outBytecode,
                       std::wstring* strError, const std::wstring* pBaseDir = nullptr);

    // Checks whether a string or buffer contains compiled bytecode
    bool IsBytecode(const std::wstring& src);
    bool IsBytecodeData(const void* data, size_t size);

    // A module file that begins with configured bytecode magic header
    // is compiled bytecode, not source text.
    bool IsCompiledModule(const std::wstring& src);
}
