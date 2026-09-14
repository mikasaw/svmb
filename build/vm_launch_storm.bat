@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r70: launch the churn storm via the standard spawn pattern (vmrun session-0).
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c start "" /min powershell -NoProfile -ExecutionPolicy Bypass -File %GUEST_DESKTOP%\thread_storm.ps1 < NUL"
exit /b %ERRORLEVEL%
