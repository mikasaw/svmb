@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem svmb test flow - step: copy the latest signed driver into the guest test dir.
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromHostToGuest "%VM_VMX%" "%REPO%\x64\Debug\svmb.sys" "%GUEST_TEST_DIR%\svmb.sys"
exit /b %ERRORLEVEL%
