@echo off
rem r80 A/B: 5x probee2e immediately after a cold svc start
for /L %%i in (1,1,5) do (
  %USERPROFILE%\Desktop\driverTest\svmb-test\svmbctl.exe cr3 probee2e >> %USERPROFILE%\Desktop\abprobe.log 2>&1
  ping -n 2 127.0.0.1 >nul
)
echo === ab done >> %USERPROFILE%\Desktop\abprobe.log
