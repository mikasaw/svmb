@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem svmb test flow - pull the fresh dir listing back to the host.
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\fresh3.txt" "%WINDBG_TEST%\logs\fresh3.txt"
exit /b %ERRORLEVEL%
