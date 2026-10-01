# fixround T2: A1/A2 race harness - region teardown vs concurrent death-sweep
# per iter: hatch holdpage holder -> wait protect -> [probe leg: force trip so
# the +100ms re-arm DPC is queued] -> taskkill (death sweep, ungated) while
# main thread issues cr3 unprotect (IOCTL gate) -> verify regions back to 0.
# MDL-leak detector: a leaked pin = 0x76 PROCESS_HAS_LOCKED_PAGES at holder
# exit (r89 law) - guest survival across all iters is the acceptance.
$ErrorActionPreference = "Continue"
$ctl = "$env:USERPROFILE\Desktop\driverTest\svmb-test\svmbctl.exe"
$log = "$env:USERPROFILE\Desktop\fixround_t2_out.txt"
$N = 20
$lines = @()
$lines += "=== T2 start $(Get-Date -Format o) ==="
for ($i = 1; $i -le $N; $i++) {
    $probeLeg = ($i % 2 -eq 0)
    $of = "$env:USERPROFILE\Desktop\hold_$i.txt"
    if (Test-Path $of) { Remove-Item $of -Force }
    $p = Start-Process -FilePath $ctl -ArgumentList "cr3","holdpage","90",$of -PassThru -WindowStyle Hidden
    $ok = $false
    foreach ($j in 1..50) { if (Test-Path $of) { $ok = $true; break }; Start-Sleep -Milliseconds 100 }
    if (-not $ok) {
        $lines += "iter $i : NO-OUTFILE (protect did not land)"
        $p | Stop-Process -Force -ErrorAction SilentlyContinue
        continue
    }
    $info = (Get-Content $of | Select-Object -First 1)
    $m = [regex]::Match($info, 'pid=(\d+)\s+base=([0-9a-f]+)')
    $holderPid = $m.Groups[1].Value
    $base = $m.Groups[2].Value
    if ($probeLeg) {
        # force trip -> resolve -> re-arm DPC queued (~100ms) -> unprotect
        # inside the window = deterministic-ish A2 race
        & $ctl cr3 probe $holderPid $base 2>&1 | Out-Null
    }
    $killer = Start-Process -FilePath cmd.exe -ArgumentList "/c","taskkill /F /PID $holderPid >nul 2>&1" -PassThru -WindowStyle Hidden
    & $ctl cr3 unprotect $holderPid 2>&1 | Out-Null
    Wait-Process -Id $p.Id -ErrorAction SilentlyContinue
    $killer | Wait-Process -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 800
    $regs = @(& $ctl cr3 regions 2>&1 | Select-String "id=")
    $lines += "iter $i : pid=$holderPid probe=$probeLeg regions_after=$($regs.Count)"
}
$lines += "=== final regions ==="
$lines += (& $ctl cr3 regions 2>&1)
$lines += "=== final stats ==="
$lines += (& $ctl stats 2>&1)
$lines += "=== T2 end $(Get-Date -Format o) ==="
$lines | Out-File -FilePath $log -Encoding ascii
Write-Host "T2 done"
