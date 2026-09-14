@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem -noWait AFTER the vmx path (vmrun arg order law)
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" -noWait "C:\Windows\System32\cmd.exe" "/c %GUEST_TEST_DIR%\melt_watchdog.bat"
exit /b %ERRORLEVEL%
