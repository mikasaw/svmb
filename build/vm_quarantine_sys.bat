@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem svmb test flow - quarantine the guest svmb.sys (loop-breaker: the gremlin
rem starts the service on boot; without the image sc start fails instantly).
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (sc stop svmb & ren %GUEST_TEST_DIR%\svmb.sys svmb.sys.quarantine & dir %GUEST_TEST_DIR%\svmb*) > %GUEST_DESKTOP%\fresh.txt 2>&1 < NUL"
exit /b %ERRORLEVEL%
