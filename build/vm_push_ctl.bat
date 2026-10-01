@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem svmb test flow - step: push the latest svmbctl.exe into the guest test
rem dir (the driver push bat only ships svmb.sys; keep ctl in sync).
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromHostToGuest "%VM_VMX%" "%REPO%\x64\Debug\svmbctl.exe" "%GUEST_TEST_DIR%\svmbctl.exe"
exit /b %ERRORLEVEL%
