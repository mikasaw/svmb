@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem svmb test flow - run the gremlin hunt script in the guest.
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %GUEST_DESKTOP%\gremlin_hunt.cmd < NUL"
exit /b %ERRORLEVEL%
