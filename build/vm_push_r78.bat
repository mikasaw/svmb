@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromHostToGuest "%VM_VMX%" "%REPO%\build\guest\multitarget.ps1" "%GUEST_TEST_DIR%\multitarget.ps1"
exit /b %ERRORLEVEL%
