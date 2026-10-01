@echo off
rem r84 leg: attach + watch + spawn notepad, then observe 10min (20x30s)
echo === arm begin %date% %time% >> %USERPROFILE%\Desktop\watchleg.log
%USERPROFILE%\Desktop\driverTest\svmb-test\svmbctl.exe mod attach cr3_monitor >> %USERPROFILE%\Desktop\watchleg.log 2>&1
%USERPROFILE%\Desktop\driverTest\svmb-test\svmbctl.exe cr3 watch notepad.exe >> %USERPROFILE%\Desktop\watchleg.log 2>&1
start notepad.exe
for /L %%i in (1,1,20) do (
  echo === obs %%i %time% >> %USERPROFILE%\Desktop\watchleg.log
  %USERPROFILE%\Desktop\driverTest\svmb-test\svmbctl.exe cr3 stats >> %USERPROFILE%\Desktop\watchleg.log 2>&1
  ping -n 31 127.0.0.1 >nul
)
echo === leg end %date% %time% >> %USERPROFILE%\Desktop\watchleg.log
