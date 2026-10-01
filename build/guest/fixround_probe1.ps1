# single probe leg with full output capture
$ErrorActionPreference = "Continue"
$ctl = "$env:USERPROFILE\Desktop\driverTest\svmb-test\svmbctl.exe"
$out = "$env:USERPROFILE\Desktop\fixround_probe1_out.txt"
$log = "$env:USERPROFILE\Desktop\hold_p.txt"
if (Test-Path $log) { Remove-Item $log -Force }
$p = Start-Process -FilePath $ctl -ArgumentList "cr3","holdpage","60",$log -PassThru -WindowStyle Hidden
$ok = $false
foreach ($j in 1..50) { if (Test-Path $log) { $ok = $true; break }; Start-Sleep -Milliseconds 100 }
if (-not $ok) { "NO-OUTFILE" | Out-File $out; exit 1 }
$info = (Get-Content $log | Select-Object -First 1)
$m = [regex]::Match($info, 'pid=(\d+)\s+base=([0-9a-f]+)')
$holderPid = $m.Groups[1].Value
$base = $m.Groups[2].Value
"holder pid=$holderPid base=$base" | Out-File $out -Encoding ascii
"--- probe ---" >> $out
(& $ctl cr3 probe $holderPid $base 2>&1) >> $out
Start-Sleep -Milliseconds 200
"--- second probe (after re-arm window) ---" >> $out
(& $ctl cr3 probe $holderPid $base 2>&1) >> $out
"--- stats ---" >> $out
(& $ctl cr3 stats 2>&1) >> $out
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 500
"--- regions after kill ---" >> $out
(& $ctl cr3 regions 2>&1) >> $out
"--- stats final ---" >> $out
(& $ctl cr3 stats 2>&1) >> $out
