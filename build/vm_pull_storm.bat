@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\storm_status.txt" "%WINDBG_TEST%\logs\storm_status.txt"
exit /b %ERRORLEVEL%
