# r78 multi-process protection set demo:
# 3 holdpage processes -> regions=3 -> kill 1 (sweep) -> 2 ->
# unprotectid -> 1 -> kill last (sweep) -> 0
$ErrorActionPreference = "Stop"
$ctl = "$env:USERPROFILE\Desktop\driverTest\svmb-test\svmbctl.exe"

function RegionIds {
    $lines = @(& $ctl cr3 regions | Select-String "id=(\d+)")
    $ids = @()
    foreach ($l in $lines) { $ids += [int]$l.Matches[0].Groups[1].Value }
    return ,$ids
}

$kids = @()
1..3 | ForEach-Object {
    $p = Start-Process -FilePath $ctl -ArgumentList "cr3","holdpage","180" `
        -PassThru -WindowStyle Hidden
    $kids += $p
}
Start-Sleep -Seconds 4
if ($kids.Count -ne 3) { Write-Host "[-] FAIL spawn ($($kids.Count))"; exit 1 }

$ids = RegionIds
Write-Host "=== step1 expect 3 live, ids: $($ids -join ',')"
if ($ids.Count -ne 3) { Write-Host "[-] multitarget FAIL (step1 got $($ids.Count))"; exit 1 }

Stop-Process -Id $kids[0].Id -Force
Start-Sleep -Seconds 3
$ids = RegionIds
Write-Host "=== step2 after kill1 (sweep): expect 2, got $($ids.Count)"
if ($ids.Count -ne 2) { Write-Host "[-] multitarget FAIL (step2 got $($ids.Count))"; exit 1 }

$id1 = $ids[0]
& $ctl cr3 unprotectid $id1
Start-Sleep -Seconds 1
$ids = RegionIds
Write-Host "=== step3 after unprotectid ${id1}: expect 1 (and it must not contain ${id1}), got $($ids.Count)"
if ($ids.Count -ne 1) { Write-Host "[-] multitarget FAIL (step3 got $($ids.Count))"; exit 1 }
if ($ids -contains [int]$id1) { Write-Host "[-] multitarget FAIL (step3 id $id1 still live)"; exit 1 }

Stop-Process -Id $kids[1].Id -Force
Stop-Process -Id $kids[2].Id -Force
Start-Sleep -Seconds 3
$ids = RegionIds
Write-Host "=== step4 after kill2+kill3 (sweeps): expect 0, got $($ids.Count)"
if ($ids.Count -ne 0) { Write-Host "[-] multitarget FAIL (step4 got $($ids.Count))"; exit 1 }

Write-Host "[+] multitarget PASS"
exit 0
