@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c dir %GUEST_DESKTOP%\ep*.txt > %GUEST_DESKTOP%\fresh4.txt 2>&1 < NUL"
exit /b %ERRORLEVEL%
