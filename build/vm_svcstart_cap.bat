@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r87: local VM env (credentials/paths) - never commit
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r89 diagnostic: service start with captured output + driver file presence
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (sc query svmb & dir %GUEST_TEST_DIR%\svmb.sys & sc start svmb) >> %GUEST_DESKTOP%\svmb_steps.log 2>&1 < NUL"
exit /b %ERRORLEVEL%
