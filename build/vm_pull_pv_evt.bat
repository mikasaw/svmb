@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\fresh5.txt" "%WINDBG_TEST%\logs\fresh5.txt"
exit /b %ERRORLEVEL%
