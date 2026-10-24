# Nodio String UTF-8 / Codepoint character slicing regression lock
$ErrorActionPreference = 'Stop'

$exe = "D:\Nodio\build-x64\Release\nodio.exe"
if (-not (Test-Path $exe)) {
    Write-Error "nodio.exe not found at $exe"
}

$sampleCode = @'
import console;

var s = "abc中文字符123";
assert(string.len(s) == 10, "string.len should return codepoint count 10, got " ++ tostring(string.len(s)));
assert(string.sub(s, 1, 3) == "abc", "byte sub failed");
assert(string.sub(s, 4, 7, true) == "中文字符", "char sub failed: " ++ tostring(string.sub(s, 4, 7, true)));
assert(string.sub(s, 4, true) == "中文字符123", "char sub bool-3 failed: " ++ tostring(string.sub(s, 4, true)));
assert(string.left(s, 5, true) == "abc中文", "char left failed: " ++ tostring(string.left(s, 5, true)));
assert(string.right(s, 5, true) == "字符123", "char right failed: " ++ tostring(string.right(s, 5, true)));
assert(string.slice(s, 4, 7, true) == "中文字符", "char slice failed: " ++ tostring(string.slice(s, 4, 7, true)));

var s2 = "😀😁😂abc";
assert(string.len(s2) == 6, "emoji len should be 6, got " ++ tostring(string.len(s2)));
assert(string.left(s2, 2, true) == "😀😁", "emoji left failed: " ++ tostring(string.left(s2, 2, true)));
assert(string.sub(s2, 3, 5, true) == "😂ab", "emoji sub failed: " ++ tostring(string.sub(s2, 3, 5, true)));
assert(string.right(s2, 4, true) == "😂abc", "emoji right failed: " ++ tostring(string.right(s2, 4, true)));

console.log("STRING_CHARS_CHECK_PASS");
'@

$sampleFile = Join-Path $PSScriptRoot "fixtures_string_chars.aardio"
[System.IO.File]::WriteAllText($sampleFile, $sampleCode, [System.Text.Encoding]::UTF8)

try {
    $out = & $exe $sampleFile
    # Out-String first: -notmatch on a raw $out ARRAY filters instead of
    # failing — an empty stdout would silently pass the check below
    $outText = ($out | Out-String)
    Write-Host "Output: $($outText.Trim())"
    if ($LASTEXITCODE -ne 0 -or $outText -notmatch "STRING_CHARS_CHECK_PASS") {
        throw "Verification failed"
    }
    Write-Host "[OK] string utf-8 / unicode codepoint character semantics verified" -ForegroundColor Green
}
finally {
    # keep fixture or clean up
}
