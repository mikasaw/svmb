@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem svmb test flow - r57: watch the pre-spawned powershell spinner with the
rem per-core process view (pv opt-in). Single command + redirect: no parens
rem in the guest string (cmd closes the outer group at the first inner ')).
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %GUEST_TEST_DIR%\svmbctl.exe cr3 watch powershell.exe 0 pv > %GUEST_DESKTOP%\fresh2.txt 2>&1 < NUL"
exit /b %ERRORLEVEL%
