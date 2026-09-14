@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromHostToGuest "%VM_VMX%" "%REPO%\build\guest\abprobe.bat" "%GUEST_TEST_DIR%\abprobe.bat"
exit /b %ERRORLEVEL%
