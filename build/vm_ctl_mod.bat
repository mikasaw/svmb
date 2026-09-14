@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem svmb test flow - svmbctl mod <action> <name> with 5 retries (r40 fix:
rem the old loop used bare `exit` inside a parenthesized for-body, which
rem exits cmd with an unpredictable errorlevel - the phantom "attach exit 1"
rem r39 chased. `exit /b` is the correct form; modfresh bats are preferred
rem for state readings).
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (for /L %i in (1,1,5) do @((%GUEST_TEST_DIR%\svmbctl.exe mod %1 %2 >> %GUEST_DESKTOP%\svmb_steps.log 2>&1 < NUL) & (if not errorlevel 1 exit /b 0))) & exit /b 1"
exit /b %ERRORLEVEL%
