$t = Measure-Command { & "$env:USERPROFILE\Desktop\driverTest\svmb-test\svmbctl.exe" storm 1000000 }
Write-Output ("storm_ms=" + [math]::Round($t.TotalMilliseconds,1))
& "$env:USERPROFILE\Desktop\driverTest\svmb-test\svmbctl.exe" exitprof
