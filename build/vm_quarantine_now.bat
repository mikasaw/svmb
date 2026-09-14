@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem svmb test flow - race the gremlin: stop+delete the service and quarantine
rem the image in ONE guest exec. Boot #3 proved the gremlin auto-starts the
rem leftover svmb service right after logon; the AutoStart+NPT driver then
rem melts VMware Tools within ~30-60s, so every second after logon counts.
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (sc stop svmb & sc delete svmb & ren %GUEST_TEST_DIR%\svmb.sys svmb.sys.quarantine & echo GREMLIN_RACE_DONE) > %GUEST_DESKTOP%\fresh.txt 2>&1 < NUL"
exit /b %ERRORLEVEL%
