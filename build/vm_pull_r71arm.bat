@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\r71_arm.txt" "%WINDBG_TEST%\logs\r71_arm.txt"
exit /b %ERRORLEVEL%
