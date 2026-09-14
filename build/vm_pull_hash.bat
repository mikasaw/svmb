@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem svmb test flow - pull the guest-side driver SHA256 hash file back to host.
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_TEST_DIR%\hash.txt" "%WINDBG_TEST%\logs\hash.txt"
exit /b %ERRORLEVEL%
