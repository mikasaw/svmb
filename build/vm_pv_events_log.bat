@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem svmb test flow - r57 atomic: dbg events + log ring in one guest exec.
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (echo ---EVT--- & %GUEST_TEST_DIR%\svmbctl.exe dbg events & echo ---LOG--- & %GUEST_TEST_DIR%\svmbctl.exe log) > %GUEST_DESKTOP%\fresh5.txt 2>&1 < NUL"
exit /b %ERRORLEVEL%
