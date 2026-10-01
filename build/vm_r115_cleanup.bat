@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r115 epilogue: kill the storm loop + detach sysaudit (leave the guest
rem in the clean RUNNING baseline)
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (wmic process where \"commandline like '%%storm_loop_r115%%'\" delete >nul 2>&1 & %GUEST_TEST_DIR%\svmbctl.exe mod detach sysaudit) > %GUEST_DESKTOP%\svmb_steps.log 2>&1 < NUL"
exit /b %ERRORLEVEL%
