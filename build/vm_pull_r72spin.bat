@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\r72_spin.txt" "%WINDBG_TEST%\logs\r72_spin.txt"
exit /b %ERRORLEVEL%
