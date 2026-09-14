@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem svmb test flow - DR-hiding E2E test with FRESH per-read output.
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %GUEST_TEST_DIR%\svmbctl.exe dbgdrtest > %GUEST_DESKTOP%\dbgdr_out.txt 2>&1 < NUL"
exit /b %ERRORLEVEL%
