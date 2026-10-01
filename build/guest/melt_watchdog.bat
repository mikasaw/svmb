@echo off
rem r58 melt flight recorder - guest side. Appends a timestamped sample to
rem melt_watch.log every ~7s: full exitprof histogram + free physical memory
rem + optional cr3 stats. The wedge freezes the loop, so the LAST entry
rem timestamps the melt and the tail shows the pre-death trend. Pure disk
rem writes - survives Tools death, dies only with the kernel.
set LOG=%USERPROFILE%\Desktop\melt_watch.log
set SVMBCTL=%USERPROFILE%\Desktop\driverTest\svmb-test\svmbctl.exe
echo === watchdog start %DATE% %TIME% === >> %LOG%
:loop
>> %LOG% echo [%DATE% %TIME%]
%SVMBCTL% exitprof >> %LOG% 2>&1
wmic OS get FreePhysicalMemory /value >> %LOG% 2>&1
ping -n 6 127.0.0.1 >nul
goto loop
