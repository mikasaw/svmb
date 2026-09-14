@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r87: local VM env (credentials/paths) - never commit
rem r92: pull the tail of the guest step log (info/config lines land here).
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\svmb_steps.log" "%WINDBG_TEST%\logs\svmb_steps_r92.log"
exit /b %ERRORLEVEL%
