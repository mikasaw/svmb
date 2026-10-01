@echo off
set N=0
:loop
%USERPROFILE%\Desktop\driverTest\svmb-test\svmbctl.exe exitprof > %USERPROFILE%\Desktop\ep%N%.txt 2>&1
set /a N=(N+1)%%6
ping -n 11 127.0.0.1 >nul
goto loop
