@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem svmb test flow - r57 atomic: spawn a detached spinner, watch it with
rem the per-core process view (pv), wait for trips, dump stats - ALL inside
rem one guest exec so the ~30-90s melt window cannot close mid-flow.
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (start \"\" /min powershell -NoProfile -Command while(1){} & ping -n 4 127.0.0.1 >nul & %GUEST_TEST_DIR%\svmbctl.exe cr3 watch powershell.exe 0 pv & ping -n 21 127.0.0.1 >nul & echo ---STATS--- & %GUEST_TEST_DIR%\svmbctl.exe cr3 stats) > %GUEST_DESKTOP%\fresh2.txt 2>&1 < NUL"
exit /b %ERRORLEVEL%
