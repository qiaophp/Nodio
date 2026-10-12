// NodioVM.h — a self-contained tree-walking interpreter for the aardio
// language (round 17). NOT the original VM: the original's bytecode
// dispatch (FUN_005149a0 family, '=' prefix format) is the residual
// unknown; this interpreter executes SOURCE directly so that published
// exes (ARDPv1 envelope) run for real. Language subset v1:
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
    // pBaseDir: directory for `import name` file resolution (name.aardio).
    int RunSource(const std::wstring& src, std::wstring* strError,
                  const std::wstring* pBaseDir = nullptr,
                  const std::unordered_map<std::wstring, std::wstring>*
                      pEmbedded = nullptr,
                  bool bDebug = false,
                  const std::wstring* pBreakLines = nullptr);

    // Parse-only syntax check (the F8 编译 command, FUN_0048fee0 family):
    // 0 = clean, -1 = syntax error (strError filled, includes line number).
    int CheckSyntax(const std::wstring& src, std::wstring* strError);
}
