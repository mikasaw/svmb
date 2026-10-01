@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem one-time guest hygiene: stop Windows Update from rebooting the test VM
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c sc stop wuauserv >> %GUEST_DESKTOP%\svmb_steps.log 2>&1 < NUL & sc config wuauserv start=disabled >> %GUEST_DESKTOP%\svmb_steps.log 2>&1 < NUL & sc stop UsoSvc >> %GUEST_DESKTOP%\svmb_steps.log 2>&1 < NUL & reg add HKLM\SOFTWARE\Policies\Microsoft\Windows\WindowsUpdate\AU /v NoAutoRebootWithLoggedOnUsers /t REG_DWORD /d 1 /f >> %GUEST_DESKTOP%\svmb_steps.log 2>&1 < NUL & echo stable-done >> %GUEST_DESKTOP%\svmb_steps.log"
exit /b %ERRORLEVEL%
