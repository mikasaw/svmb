@echo off
rem r75 soak: repeated protect/unprotect cycles against the live driver
for /L %%i in (1,1,30) do (
  echo === round %%i %%date%% %%time%% >> %USERPROFILE%\Desktop\blocksoak.log
  %USERPROFILE%\Desktop\driverTest\svmb-test\svmbctl.exe cr3 blockself >> %USERPROFILE%\Desktop\blocksoak.log 2>&1
  ping -n 61 127.0.0.1 >nul
)
echo === soak end %%date%% %%time%% >> %USERPROFILE%\Desktop\blocksoak.log
