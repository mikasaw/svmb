@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\watch_r66_B.txt" "%WINDBG_TEST%\logs\watch_r66_B.txt"
exit /b %ERRORLEVEL%
