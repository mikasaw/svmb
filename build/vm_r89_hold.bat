@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r87: local VM env (credentials/paths) - never commit
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r89 kill E2E: holdpage victim with SVMB_PROT_POLICY_KILL, foreground
rem (this bat BLOCKS for the hold duration - run in a background shell);
rem pid/base land in hold_r87.txt right after the protect lands
if "%1"=="" (set SECS=120) else (set SECS=%1)
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %GUEST_TEST_DIR%\svmbctl.exe cr3 holdpage %SECS% %GUEST_DESKTOP%\hold_r87.txt kill >> %GUEST_DESKTOP%\svmb_steps.log 2>&1 < NUL"
exit /b %ERRORLEVEL%
