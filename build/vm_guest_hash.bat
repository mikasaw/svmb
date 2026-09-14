@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem svmb test flow - step: guest-side SHA256 of the pushed driver (r27 exp3).
rem Runs in guest via vmrun; output lands in a file we pull back afterwards.
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c dir %GUEST_TEST_DIR%\svmb.sys & certutil -hashfile %GUEST_TEST_DIR%\svmb.sys SHA256 > %GUEST_TEST_DIR%\hash.txt 2>&1 < NUL"
exit /b %ERRORLEVEL%
