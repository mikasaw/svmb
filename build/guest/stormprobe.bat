@echo off
rem r77 baseline: 20x probee2e, capture per-run region-trip deltas
for /L %%i in (1,1,20) do (
  echo === storm %%i %%time%% >> %USERPROFILE%\Desktop\stormprobe.log
  %USERPROFILE%\Desktop\driverTest\svmb-test\svmbctl.exe cr3 probee2e >> %USERPROFILE%\Desktop\stormprobe.log 2>&1
  ping -n 4 127.0.0.1 >nul
)
echo === storm end %%date%% %%time%% >> %USERPROFILE%\Desktop\stormprobe.log
