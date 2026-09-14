@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem svmb test flow - pull the guest step log back to the host for review.
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\svmb_steps.log" "%HOST_HOME%\svmb_steps_out.txt"
exit /b %ERRORLEVEL%
