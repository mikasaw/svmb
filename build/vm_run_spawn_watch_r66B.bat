@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r66 Leg B (mature boot): run the spawn+watch recipe, output to an
rem INDEPENDENT filename (r64 law: shared fresh* names are unreliable).
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %GUEST_TEST_DIR%\spawn_and_watch.bat > %GUEST_DESKTOP%\watch_r66_B.txt 2>&1 < NUL"
exit /b %ERRORLEVEL%
