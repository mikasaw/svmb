@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r109 epilogue: kill the guest melt watchdog (it has no exit condition),
rem detach sysaudit, restore the r110 green build, clear the melt knobs
rem (default thresholds + BehaveResponse=0) and restart the service.
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (wmic process where \"commandline like '%%melt_watchdog%%'\" delete >nul 2>&1 & %GUEST_TEST_DIR%\svmbctl.exe mod detach sysaudit) > %GUEST_DESKTOP%\svmb_steps.log 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c sc stop svmb > %GUEST_DESKTOP%\svmb_steps.log 2>&1 < NUL"
ping -n 5 127.0.0.1 >nul
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromHostToGuest "%VM_VMX%" "%REPO%\x64\Debug\svmb.sys" "%GUEST_TEST_DIR%\svmb.sys"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (reg delete HKLM\SYSTEM\CurrentControlSet\Services\svmb\Parameters /v BehaveRdThs /f & reg delete HKLM\SYSTEM\CurrentControlSet\Services\svmb\Parameters /v BehaveWrThs /f & reg delete HKLM\SYSTEM\CurrentControlSet\Services\svmb\Parameters /v BehaveOpThs /f & reg add HKLM\SYSTEM\CurrentControlSet\Services\svmb\Parameters /v BehaveResponse /t REG_DWORD /d 0 /f & sc start svmb & echo === GREEN RESTORED %DATE% %TIME% ===) >> %GUEST_DESKTOP%\svmb_steps.log 2>&1 < NUL"
exit /b %ERRORLEVEL%
