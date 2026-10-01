@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r84 leg runner: %1 = arm delay in minutes after network-up
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (del %GUEST_DESKTOP%\watchleg.log < NUL) > NUL 2>&1"
exit /b %ERRORLEVEL%
