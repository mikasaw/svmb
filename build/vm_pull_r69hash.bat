@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\r69_hash.txt" "%WINDBG_TEST%\logs\r69_hash.txt"
exit /b %ERRORLEVEL%
