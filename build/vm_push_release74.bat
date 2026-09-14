@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromHostToGuest "%VM_VMX%" "%REPO%\x64\Release\svmb.sys" "%GUEST_TEST_DIR%\svmb.sys"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromHostToGuest "%VM_VMX%" "%REPO%\x64\Release\svmbctl.exe" "%GUEST_TEST_DIR%\svmbctl.exe"
exit /b %ERRORLEVEL%
