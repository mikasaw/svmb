@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\r74_cert.txt" "%WINDBG_TEST%\logs\r74_cert.txt"
exit /b %ERRORLEVEL%
