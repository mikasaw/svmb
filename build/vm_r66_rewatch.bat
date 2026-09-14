@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r66: rewatch existing spinner AFTER module attach (sentinel now armed).
rem pv opt-in + independent output filename.
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (%GUEST_TEST_DIR%\svmbctl.exe cr3 watch powershell.exe 0 pv & echo ---STATS+15s--- & ping -n 16 127.0.0.1 >nul & %GUEST_TEST_DIR%\svmbctl.exe cr3 stats) > %GUEST_DESKTOP%\watch_r66_B2.txt 2>&1 < NUL"
exit /b %ERRORLEVEL%
