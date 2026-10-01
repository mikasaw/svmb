@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem svmb test flow - step: set Parameters\AutoStart=1 (r27 exp1: enter at
rem DriverEntry, reference parity). Appends outcome to the guest step log.
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c reg add HKLM\SYSTEM\CurrentControlSet\Services\svmb\Parameters /v AutoStart /t REG_DWORD /d 1 /f & reg query HKLM\SYSTEM\CurrentControlSet\Services\svmb\Parameters >> %GUEST_DESKTOP%\svmb_steps.log 2>&1 < NUL"
exit /b %ERRORLEVEL%
