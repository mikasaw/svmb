@echo off
rem r76 soak: repeated L2-bypass probee2e cycles against the live driver
for /L %%i in (1,1,30) do (
  echo === round %%i %%date%% %%time%% >> %USERPROFILE%\Desktop\probesoak.log
  %USERPROFILE%\Desktop\driverTest\svmb-test\svmbctl.exe cr3 probee2e >> %USERPROFILE%\Desktop\probesoak.log 2>&1
  ping -n 6 127.0.0.1 >nul
)
echo === soak end %%date%% %%time%% >> %USERPROFILE%\Desktop\probesoak.log
