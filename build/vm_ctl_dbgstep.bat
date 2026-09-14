@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem svmb test flow - arm one MTF single step, fresh output.
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (%GUEST_TEST_DIR%\svmbctl.exe dbg step & echo ---EVT--- & %GUEST_TEST_DIR%\svmbctl.exe dbg events) > %GUEST_DESKTOP%\fresh2.txt 2>&1 < NUL"
exit /b %ERRORLEVEL%
