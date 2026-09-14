@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (for /L %i in (1,1,5) do @((%GUEST_TEST_DIR%\svmbctl.exe mod attach debugger >> %GUEST_DESKTOP%\svmb_steps.log 2>&1 < NUL) & (if not errorlevel 1 exit)))"
exit /b %ERRORLEVEL%
