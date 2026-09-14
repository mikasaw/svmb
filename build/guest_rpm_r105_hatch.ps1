# r105 E2E hatcher: spawn the probe DETACHED (r93 law - the only working
# hatcher under VIX is powershell Start-Process to a pre-placed ps1).
# The probe WILL be suspended mid-run by the behavior response (that is
# the test); a synchronous runProgramInGuest would hang the host.
Start-Process powershell -WindowStyle Hidden -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-File',"$env:USERPROFILE\Desktop\rpm_r104.ps1"
Write-Output "hatched"
