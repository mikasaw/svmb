@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "%GUEST_TEST_DIR%\svmbctl.exe" %*
exit /b %ERRORLEVEL%
