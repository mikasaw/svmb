@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem svmb test flow - pull the guest-side ctl SHA256 hash file back to host.
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_TEST_DIR%\hash_ctl.txt" "%WINDBG_TEST%\logs\hash_ctl.txt"
exit /b %ERRORLEVEL%
