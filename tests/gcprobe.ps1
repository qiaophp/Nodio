param([string]$Name = 'A')
$ErrorActionPreference = 'Continue'
$env:NODIO_LIBDIR = 'D:\aardio\lib'
$env:NODIO_OVERLAY_LIBDIR = 'D:\Nodio\lib'
$f = "C:\Users\QQ\AppData\Local\Temp\nodio_probe\m$Name.aardio"
$out = "C:\Users\QQ\AppData\Local\Temp\nodio_probe\gc_$Name.txt"
$p = Start-Process -FilePath 'D:\Nodio\build-x64\Release\nodio.exe' -ArgumentList $f -PassThru -NoNewWindow -RedirectStandardOutput $out -RedirectStandardError "$out.err"
$peak = 0
while (-not $p.HasExited) {
  Start-Sleep -Milliseconds 30
  try { $g = Get-Process -Id $p.Id -ErrorAction Stop; if ($g.PrivateMemorySize64 -gt $peak) { $peak = $g.PrivateMemorySize64 } } catch { break }
}
$p.WaitForExit()
$t = (Get-Content $out -Raw -Encoding UTF8) | Out-String
$e = (Get-Content "$out.err" -Raw -Encoding UTF8) | Out-String
$gcLine = ''
foreach ($ln in ($e -split "`r?`n")) { if ($ln -match '\[GC\] live') { $gcLine = $ln.Trim(); break } }
Write-Output ("{0}: exit={1} peakPM={2:N1}MB out=[{3}]" -f $Name, $p.ExitCode, ($peak / 1MB), $t.Trim())
Write-Output ("   {0}" -f $gcLine)
if ($e -match 'FATAL') { Write-Output ("   CRASH: " + (($e -split "`r?`n" | Where-Object { $_ -match 'FATAL|AccessViolation|RIP=' }) -join ' | ')) }
