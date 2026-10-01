@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem svmb test flow - fresh listing of guest driverTest svmb files.
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c dir %GUEST_TEST_DIR%\svmb* > %GUEST_DESKTOP%\fresh3.txt 2>&1 < NUL"
exit /b %ERRORLEVEL%
