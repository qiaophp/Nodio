# cleanup_residue.ps1 — 清理官方 aardio 在本机留下的「更新残留 + 僵尸进程」
#
# 现象（2026-10 实测）：
#   官方 aardio 的 std 更新机制会把正在被占用的旧文件改名成
#   `<原名>.<随机数>.old` 再写新文件。std/adb/20240409/adb.exe 被 adb 进程占用时，
#   改名后的 .old 文件既覆盖不了也删不掉，于是每次启动相关功能就多留一份
#   （实测累计 125 个 .old / 638 MB，其中 113 个是 adb.exe 的整份拷贝）。
#
#   根因是官方在 aardio.exe 运行期间会拉起 **不随父进程退出的 adb 常驻进程**；
#   进程不退 → 文件锁不释放 → 残留越攒越多。
#
# 用法：
#   pwsh -NoProfile -ExecutionPolicy Bypass -File tools/cleanup_residue.ps1
#   pwsh -NoProfile -ExecutionPolicy Bypass -File tools/cleanup_residue.ps1 -DryRun
#
# 参数：
#   -DryRun       只报告不删除
#   -AardioHome   官方 aardio 的用户数据目录（默认 %LOCALAPPDATA%\aardio）
#   -KeepProcesses  不结束僵尸 adb 进程（只清文件；文件被占用时会失败）

param(
    [switch]$DryRun,
    [string]$AardioHome = (Join-Path $env:LOCALAPPDATA 'aardio'),
    [switch]$KeepProcesses
)

$ErrorActionPreference = 'Stop'

function Write-Section($title) {
    Write-Host ''
    Write-Host "== $title ==" -ForegroundColor Cyan
}

# ---------------------------------------------------------------- 1. 僵尸进程
$adbDir = Join-Path $AardioHome 'std\adb'
$leaked = @()
if (Test-Path $adbDir) {
    $leaked = Get-Process -Name adb -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -like "$adbDir*" }
}

Write-Section '僵尸 adb 进程'
if ($leaked.Count -eq 0) {
    Write-Host '  无（干净）'
} else {
    Write-Host ("  发现 {0} 个来自 {1} 的 adb 进程" -f $leaked.Count, $adbDir)
    $leaked | Select-Object -First 5 Id, StartTime | Format-Table -AutoSize | Out-String -Width 100 |
        Write-Host
    if ($KeepProcesses -or $DryRun) {
        Write-Host '  跳过（-KeepProcesses / -DryRun）'
    } else {
        $leaked | ForEach-Object { Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue }
        Start-Sleep -Seconds 2
        $left = @(Get-Process -Name adb -ErrorAction SilentlyContinue |
            Where-Object { $_.Path -like "$adbDir*" }).Count
        Write-Host ("  已结束，剩余 {0} 个" -f $left)
    }
}

# ---------------------------------------------------------------- 2. .old 残留
# 只认「同一个目录里存在同名活文件」的 .old —— 这才是更新残留。
# （WebView2/Chromium 的 profile 目录里也有真的叫 *.old 的目录，不能误删，
#   所以这里只处理 std 树下的文件，且要求有活文件对应。）
Write-Section '更新残留（*.old）'
$stdDir = Join-Path $AardioHome 'std'
$residue = @()
if (Test-Path $stdDir) {
    $residue = Get-ChildItem -Path $stdDir -Recurse -File -Force -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '\.old$' } |
        Where-Object {
            $live = $_.FullName -replace '\.\d+\.old$', ''
            $live2 = $_.FullName -replace '\.old$', ''
            (Test-Path $live) -or (Test-Path $live2)
        }
}

if ($residue.Count -eq 0) {
    Write-Host '  无（干净）'
} else {
    $mb = [math]::Round((($residue | Measure-Object Length -Sum).Sum) / 1MB, 1)
    Write-Host ("  发现 {0} 个残留，合计 {1} MB" -f $residue.Count, $mb)
    $residue | Group-Object { ($_.Name -replace '\.\d+\.old$', '.<n>.old') } |
        Sort-Object Count -Descending | ForEach-Object {
            Write-Host ("    {0} x{1}" -f $_.Name, $_.Count)
        }
    if ($DryRun) {
        Write-Host '  跳过（-DryRun）'
    } else {
        # 回收站方式删除：分批，避免命令行过长
        $trash = Join-Path $env:USERPROFILE '.minimax\bin\mavis-trash.cmd'
        if (-not (Test-Path $trash)) {
            Write-Warning "找不到 $trash，请手动删除上述文件。"
        } else {
            $paths = @($residue | Select-Object -ExpandProperty FullName)
            for ($i = 0; $i -lt $paths.Count; $i += 25) {
                $end = [math]::Min($i + 24, $paths.Count - 1)
                & $trash -- $paths[$i..$end] 2>&1 | Out-Null
            }
            $left = @(Get-ChildItem -Path $stdDir -Recurse -File -Force -ErrorAction SilentlyContinue |
                Where-Object { $_.Name -match '\.old$' }).Count
            Write-Host ("  已清理，剩余 {0} 个" -f $left)
        }
    }
}

Write-Host ''
Write-Host '完成。' -ForegroundColor Green
