@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r66 armed-watch hunt: who armed the watch on the fresh driver instance?
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (tasklist | findstr /i powershell & echo ===EVENTS=== & %GUEST_TEST_DIR%\svmbctl.exe dbg events & echo ===STEPS=== & type %GUEST_DESKTOP%\svmb_steps.log) > %GUEST_DESKTOP%\fresh.txt 2>&1 < NUL"
exit /b %ERRORLEVEL%
