@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\reg_q.txt" "%WINDBG_TEST%\logs\reg_q.txt"
exit /b %ERRORLEVEL%
