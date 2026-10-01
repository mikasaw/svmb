@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem svmb test flow - step: set Parameters\AutoStart=0 (opt-out of load-time
rem virtualization; bisect escape hatch - ctl start then hits the fatal path).
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c reg add HKLM\SYSTEM\CurrentControlSet\Services\svmb\Parameters /v AutoStart /t REG_DWORD /d 0 /f & reg query HKLM\SYSTEM\CurrentControlSet\Services\svmb\Parameters >> %GUEST_DESKTOP%\svmb_steps.log 2>&1 < NUL"
exit /b %ERRORLEVEL%
