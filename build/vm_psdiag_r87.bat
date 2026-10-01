@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r87: local VM env (credentials/paths) - never commit
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r87 diagnostic v2: foreground -File run of the marker-holder (blocks for
rem the whole 60s sleep - do not call while the read leg must run in window;
rem this leg only answers "does the ps1 execute at all")
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c powershell -NoProfile -ExecutionPolicy Bypass -File %GUEST_TEST_DIR%\hold_r87.ps1 > %GUEST_DESKTOP%\ps_diag.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\ps_diag.txt" "%WINDBG_TEST%\logs\ps_diag.txt"
exit /b %ERRORLEVEL%
