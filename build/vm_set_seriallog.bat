@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem svmb test flow - step: enable the driver's serial mirror (read at
rem DriverEntry, so must be set BEFORE sc start). Idempotent.
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c reg add \"HKLM\SYSTEM\CurrentControlSet\Services\svmb\Parameters\" /v SerialLog /t REG_DWORD /d 1 /f >> %GUEST_DESKTOP%\svmb_steps.log 2>&1 < NUL"
exit /b %ERRORLEVEL%
