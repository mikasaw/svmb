@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r66 ground truth: current watch state + powershell CPU time, unique filename.
rem No inner quotes allowed (vmrun quoting layer trap).
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (%GUEST_TEST_DIR%\svmbctl.exe cr3 stats & echo ===PS=== & tasklist /v | findstr /i powershell & echo ===EXITS=== & %GUEST_TEST_DIR%\svmbctl.exe exitprof) > %GUEST_DESKTOP%\r66_state.txt 2>&1 < NUL"
exit /b %ERRORLEVEL%
