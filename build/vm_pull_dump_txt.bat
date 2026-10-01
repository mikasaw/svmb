@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\r67_dump.txt" "%WINDBG_TEST%\logs\r67_dump.txt"
exit /b %ERRORLEVEL%
