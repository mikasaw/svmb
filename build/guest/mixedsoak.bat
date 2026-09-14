@echo off
rem r79: 2h mixed soak - 24 rounds of probee2e (L2 sensing) + blockself (L1)
for /L %%i in (1,1,24) do (
  echo === mround %%i %%date%% %%time%% >> %USERPROFILE%\Desktop\mixedsoak.log
  %USERPROFILE%\Desktop\driverTest\svmb-test\svmbctl.exe cr3 probee2e >> %USERPROFILE%\Desktop\mixedsoak.log 2>&1
  %USERPROFILE%\Desktop\driverTest\svmb-test\svmbctl.exe cr3 blockself >> %USERPROFILE%\Desktop\mixedsoak.log 2>&1
  %USERPROFILE%\Desktop\driverTest\svmb-test\svmbctl.exe cr3 stats >> %USERPROFILE%\Desktop\mixedsoak.log 2>&1
  ping -n 301 127.0.0.1 >nul
)
echo === mixedsoak end %%date%% %%time%% >> %USERPROFILE%\Desktop\mixedsoak.log
%USERPROFILE%\Desktop\driverTest\svmb-test\svmbctl.exe cr3 regions >> %USERPROFILE%\Desktop\mixedsoak.log 2>&1
