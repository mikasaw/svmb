@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (type %GUEST_DESKTOP%\spin_r72.ps1 & echo ===DIR=== & dir %GUEST_DESKTOP%\spin_r72.ps1) > %GUEST_DESKTOP%\r72_spin.txt 2>&1 < NUL"
exit /b %ERRORLEVEL%
