@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem svmb test flow - step: svmbctl info (baseline state via IOCTL), append to step log.
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %GUEST_TEST_DIR%\svmbctl.exe info >> %GUEST_DESKTOP%\svmb_steps.log 2>&1 < NUL"
exit /b %ERRORLEVEL%
