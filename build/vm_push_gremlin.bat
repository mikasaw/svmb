@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem svmb test flow - push the gremlin hunt script to the guest.
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromHostToGuest "%VM_VMX%" "%REPO%\build\gremlin_hunt.cmd" "%GUEST_DESKTOP%\gremlin_hunt.cmd"
exit /b %ERRORLEVEL%
