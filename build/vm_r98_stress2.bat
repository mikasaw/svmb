@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r87: local VM env (credentials/paths) - never commit
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromHostToGuest "%VM_VMX%" "%REPO%\build\guest_stress2_r98.ps1" "%GUEST_DESKTOP%\stress2_r98.ps1"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c powershell -NoProfile -ExecutionPolicy Bypass -File %GUEST_DESKTOP%\stress2_r98.ps1 > %GUEST_DESKTOP%\stress2_r98.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\stress2_r98.txt" "%WINDBG_TEST%\logs\stress2_r98.txt"
exit /b %ERRORLEVEL%
