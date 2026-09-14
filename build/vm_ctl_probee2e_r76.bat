@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r76 - probee2e with fresh per-run output.
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %GUEST_TEST_DIR%\svmbctl.exe cr3 probee2e > %GUEST_DESKTOP%\r76_probe.txt 2>&1 < NUL"
exit /b %ERRORLEVEL%
