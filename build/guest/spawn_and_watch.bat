@echo off
rem r57 guest-side helper: spawn a detached powershell spinner, wait for the
rem seed table, watch it with the per-core process view (pv), wait for
rem trips, then dump stats. Runs INSIDE the guest so it costs ONE vmrun op
rem (the AutoStart melt window is ~30-90s; every host-side round trip risks
rem losing it). No parens in commands - cmd would close the caller's group.
start "" /min powershell -NoProfile -Command "while(1){}"
ping -n 4 127.0.0.1 >nul
%USERPROFILE%\Desktop\driverTest\svmb-test\svmbctl.exe cr3 watch powershell.exe 0 pv
ping -n 21 127.0.0.1 >nul
echo ---STATS---
%USERPROFILE%\Desktop\driverTest\svmb-test\svmbctl.exe cr3 stats
