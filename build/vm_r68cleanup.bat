@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (taskkill /F /IM powershell.exe & ping -n 2 127.0.0.1 >nul & %GUEST_TEST_DIR%\svmbctl.exe cr3 stats) > %GUEST_DESKTOP%\r68_state.txt 2>&1 < NUL"
exit /b %ERRORLEVEL%
