@echo off
rem r115 E13 leg: periodic CPUID-storm injection - the ambient-exit-density
rem hypothesis (surviving legs r113/r109 all had heavy exit traffic; every
rem frozen leg was idle). Same build as E12 (which froze at ~8min); the
rem ONLY variable is this loop. storm 50000 = 50k hypercall exits per core
rem (r98: ~38k exits/s/core), ~8s of storm per 14s cycle.
set CTL=%USERPROFILE%\Desktop\driverTest\svmb-test\svmbctl.exe
echo === storm loop start %DATE% %TIME% === > %USERPROFILE%\Desktop\storm_loop.log
:loop
%CTL% storm 50000 >> %USERPROFILE%\Desktop\storm_loop.log 2>&1
ping -n 3 127.0.0.1 >nul
goto loop
