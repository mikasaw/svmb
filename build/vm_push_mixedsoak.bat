@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromHostToGuest "%VM_VMX%" "%REPO%\build\guest\mixedsoak.bat" "%GUEST_TEST_DIR%\mixedsoak.bat"
exit /b %ERRORLEVEL%
