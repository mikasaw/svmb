@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem undo the WU freeze: let the pending update cycle COMPLETE
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c sc config wuauserv start= demand >> %GUEST_DESKTOP%\svmb_steps.log 2>&1 < NUL & sc config UsoSvc start= auto >> %GUEST_DESKTOP%\svmb_steps.log 2>&1 < NUL & reg delete HKLM\SOFTWARE\Policies\Microsoft\Windows\WindowsUpdate\AU /f >> %GUEST_DESKTOP%\svmb_steps.log 2>&1 < NUL & echo wu-resumed >> %GUEST_DESKTOP%\svmb_steps.log"
exit /b %ERRORLEVEL%
