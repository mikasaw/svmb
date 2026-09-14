@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem svmb test flow - step: pull guest hash.txt back to host logs (r27 exp3).
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_TEST_DIR%\hash.txt" "%WINDBG_TEST%\logs\guest_hash.txt"
exit /b %ERRORLEVEL%
