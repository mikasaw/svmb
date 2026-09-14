@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem svmb test flow - pull the guest Desktop fresh4.txt (events channel).
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\fresh4.txt" "%WINDBG_TEST%\logs\fresh4.txt"
exit /b %ERRORLEVEL%
