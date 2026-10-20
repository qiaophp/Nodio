param([string]$Probe = 'uiprobe13')
$ErrorActionPreference = 'Continue'
$env:NODIO_LIBDIR = 'D:\aardio\lib'
$env:NODIO_OVERLAY_LIBDIR = 'D:\Nodio\lib'
$out = "C:\Users\QQ\AppData\Local\Temp\nodio_probe\mem_$Probe.txt"
$p = Start-Process -FilePath 'D:\Nodio\build-x64\Release\nodio.exe' -ArgumentList "C:\Users\QQ\AppData\Local\Temp\nodio_probe\$Probe.aardio" -PassThru -NoNewWindow -RedirectStandardOutput $out -RedirectStandardError "$out.err"
$first = 0; $last = 0
while (-not $p.HasExited) {
  Start-Sleep -Milliseconds 250
  try {
    $g = Get-Process -Id $p.Id -ErrorAction Stop
    if ($first -eq 0) { $first = $g.WorkingSet64 }
    $last = $g.WorkingSet64
  } catch { break }
}
$p.WaitForExit()
$t = (Get-Content $out -Raw -Encoding UTF8) | Out-String
$e = (Get-Content "$out.err" -Raw -Encoding UTF8) | Out-String
Write-Output ("{0}: WS {1:N1}MB -> {2:N1}MB  (delta {3:N1}MB)  exit={4}" -f $Probe, ($first/1MB), ($last/1MB), (($last-$first)/1MB), $p.ExitCode)
Write-Output ("   out: " + (($t -split "`r?`n" | Where-Object { $_.Trim() } | Select-Object -Last 3) -join ' | '))
if ($e -match 'FATAL') { Write-Output ("   CRASH: " + (($e -split "`r?`n" | Where-Object { $_ -match 'FATAL|AccessViolation' }) -join ' | ')) }
