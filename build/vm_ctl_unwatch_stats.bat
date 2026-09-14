@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem svmb test flow - cr3 unwatch + stats in one fresh pass.
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (%GUEST_TEST_DIR%\svmbctl.exe cr3 unwatch & echo ---STATS--- & %GUEST_TEST_DIR%\svmbctl.exe cr3 stats) > %GUEST_DESKTOP%\fresh2.txt 2>&1 < NUL"
exit /b %ERRORLEVEL%
