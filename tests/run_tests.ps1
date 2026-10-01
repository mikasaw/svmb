# svmb 离线功能测试（驱动加载即可，无需 hypervisor start）
# 用法: 在 VM 内管理员 powershell 执行  .\tests\run_tests.ps1 [-Dir C:\svmb]
param(
    [string]$Dir = "C:\svmb"
)
$ErrorActionPreference = "Stop"
$ctl = Join-Path $Dir "svmbctl.exe"
if (-not (Test-Path $ctl)) { Write-Host "[!] svmbctl.exe not found in $Dir"; exit 1 }

function Step([string]$name, [scriptblock]$body)
{
    Write-Host "== $name"
    & $body
    if ($LASTEXITCODE -ne 0) { Write-Host "[FAIL] $name"; exit 1 }
}

Step "driver service" {
    sc.exe query svmb | Out-Null
    if ($LASTEXITCODE -ne 0) {
        sc.exe create svmb type= kernel start= demand binPath= (Join-Path $Dir "svmb.sys")
        if ($LASTEXITCODE -ne 0) { throw "sc create" }
    }
    sc.exe start svmb
    if ($LASTEXITCODE -ne 0) { throw "sc start (577 = signature?)" }
}

Step "selftest (offline asserts)" { & $ctl selftest }
Step "npt list (empty)"           { & $ctl npt list }
Step "cr3 stats"                  { & $ctl cr3 stats }
Step "cr3 watch self (arm)"       { & $ctl cr3 watch svmbctl.exe DEAD0 }
Step "cr3 unwatch"                { & $ctl cr3 unwatch }
Step "dbg events (drain)"         { & $ctl dbg events }
Step "info (hv must be OFF)"      { & $ctl info }
Step "clean unload" {
    sc.exe stop svmb
    if ($LASTEXITCODE -ne 0) { throw "sc stop" }
    sc.exe delete svmb
    if ($LASTEXITCODE -ne 0) { throw "sc delete" }
}

Write-Host "[PASS] offline tier-1 suite green"
exit 0
