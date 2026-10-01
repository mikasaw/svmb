@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (tasklist | findstr /i powershell & powershell -NoProfile -ExecutionPolicy Bypass -Command echo PSOK & dir /b %GUEST_DESKTOP%\thread_storm.ps1) > %GUEST_DESKTOP%\ps_check.txt 2>&1 < NUL"
exit /b %ERRORLEVEL%
