@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r87: local VM env (credentials/paths) - never commit
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r89 kill E2E: probee2e with kill policy (self-victim), captured output;
rem expect the process to be TERMINATED by the driver after the bypass
rem write (vmrun may report a nonzero/abnormal exit - that IS the pass)
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %GUEST_TEST_DIR%\svmbctl.exe cr3 probee2e kill > %GUEST_DESKTOP%\kill_r89.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\kill_r89.txt" "%WINDBG_TEST%\logs\kill_r89.txt"
exit /b %ERRORLEVEL%
