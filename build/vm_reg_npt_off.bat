@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem svmb test flow - step: reset Parameters\NptEnable back to 0 (default
rem safe state; takes effect at next driver load).
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c reg add HKLM\SYSTEM\CurrentControlSet\Services\svmb\Parameters /v NptEnable /t REG_DWORD /d 0 /f >> %GUEST_DESKTOP%\svmb_steps.log 2>&1 < NUL"
exit /b %ERRORLEVEL%
