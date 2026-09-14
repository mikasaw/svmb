@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem svmb test flow - spawn a busy powershell spin loop detached. NOTE: -noWait
rem must come AFTER the vmx path (vmrun arg order law) or vmrun prints usage.
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" -noWait "C:\Windows\System32\cmd.exe" "/c powershell -NoProfile -Command while(1){}"
exit /b %ERRORLEVEL%
