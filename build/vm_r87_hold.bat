@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r87: local VM env (credentials/paths) - never commit
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r87/r88 readvm E2E part 2 (v6): PUSH the current ps1 first (v5 lost the
rem push line - the guest kept running the stale script), then background
rem the marker-holder via PowerShell Start-Process (cmd `start` under the
rem VIX session silently never executes the child - v3/v4 lesson).
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromHostToGuest "%VM_VMX%" "%REPO%\build\hold_r87.ps1" "%GUEST_TEST_DIR%\hold_r87.ps1"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c powershell -NoProfile -Command Start-Process powershell -ArgumentList '-NoProfile -ExecutionPolicy Bypass -File %GUEST_TEST_DIR%\hold_r87.ps1' -WindowStyle Hidden < NUL"
exit /b %ERRORLEVEL%
