@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem svmb test flow - step: remove Parameters\AutoStart so the compiled-in
rem DEFAULT applies (r30: default ON = virtualize at driver load).
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c reg delete HKLM\SYSTEM\CurrentControlSet\Services\svmb\Parameters /v AutoStart /f & reg query HKLM\SYSTEM\CurrentControlSet\Services\svmb\Parameters >> %GUEST_DESKTOP%\svmb_steps.log 2>&1 < NUL"
exit /b %ERRORLEVEL%
