@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromHostToGuest "%VM_VMX%" "%REPO%\build\guest\spin_r72.ps1" "%GUEST_DESKTOP%\spin_r72.ps1"
exit /b %ERRORLEVEL%
