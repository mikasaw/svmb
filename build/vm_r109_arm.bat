@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r109 freeze-repro arm: the r105/r107 melt recipe (low thresholds +
rem BehaveResponse=1, armed idle ~8min) on the r110 build. Knobs are read
rem once at DriverEntry - the service must be restarted AFTER the reg adds.
rem NptEnable/SerialLog persist in Parameters from this boot's Phase 3a/3b.
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (reg add HKLM\SYSTEM\CurrentControlSet\Services\svmb\Parameters /v BehaveRdThs /t REG_DWORD /d 2 /f & reg add HKLM\SYSTEM\CurrentControlSet\Services\svmb\Parameters /v BehaveWrThs /t REG_DWORD /d 2 /f & reg add HKLM\SYSTEM\CurrentControlSet\Services\svmb\Parameters /v BehaveOpThs /t REG_DWORD /d 2 /f & reg add HKLM\SYSTEM\CurrentControlSet\Services\svmb\Parameters /v BehaveResponse /t REG_DWORD /d 1 /f & sc stop svmb & ping -n 4 127.0.0.1 >nul & sc start svmb) > %GUEST_DESKTOP%\svmb_steps.log 2>&1 < NUL"
exit /b %ERRORLEVEL%
