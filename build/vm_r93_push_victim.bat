@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r87: local VM env (credentials/paths) - never commit
rem r93: push the soak suspend-victim ps1 to the guest desktop.
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromHostToGuest "%VM_VMX%" "%REPO%\build\guest_susp_victim.ps1" "%GUEST_DESKTOP%\susp_victim.ps1"
exit /b %ERRORLEVEL%
