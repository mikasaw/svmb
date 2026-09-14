@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem svmb test flow - pull the guest Desktop probe16.txt (atomic start+probe+log).
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\probe16.txt" "%WINDBG_TEST%\logs\probe16.txt"
exit /b %ERRORLEVEL%
