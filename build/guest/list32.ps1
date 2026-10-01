# r82: 9 concurrent protected regions - LIST must report all 32 slots now
$ErrorActionPreference = "Stop"
$ctl = "$env:USERPROFILE\Desktop\driverTest\svmb-test\svmbctl.exe"
$kids = @()
1..9 | ForEach-Object {
    $kids += Start-Process -FilePath $ctl -ArgumentList "cr3","holdpage","120" -PassThru -WindowStyle Hidden
}
Start-Sleep -Seconds 5
$lines = @(& $ctl cr3 regions | Select-String "id=")
Write-Host "=== listed regions: $($lines.Count) (expect 9)"
if ($lines.Count -ne 9) { Write-Host "[-] list32 FAIL"; exit 1 }
$kids | Stop-Process -Force
Start-Sleep -Seconds 4
$after = @(& $ctl cr3 regions | Select-String "id=")
Write-Host "=== after kill-all: $($after.Count) (expect 0)"
if ($after.Count -ne 0) { Write-Host "[-] list32 FAIL (sweep)"; exit 1 }
Write-Host "[+] list32 PASS"
