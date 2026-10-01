@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe -ExecutionPolicy Bypass -File %GUEST_TEST_DIR%\deathtest.ps1 > %GUEST_DESKTOP%\r76_death.txt 2>&1 < NUL"
exit /b %ERRORLEVEL%
