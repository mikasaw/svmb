@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r87: local VM env (credentials/paths) - never commit
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r87 readvm E2E part 1: self-read + reserved-hole read + stats, all into
rem the steps log (pull afterwards with vm_pull_steps_log.bat)
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (%GUEST_TEST_DIR%\svmbctl.exe cr3 readvm-self & %GUEST_TEST_DIR%\svmbctl.exe cr3 readvm-hole & %GUEST_TEST_DIR%\svmbctl.exe cr3 stats) >> %GUEST_DESKTOP%\svmb_steps.log 2>&1 < NUL"
exit /b %ERRORLEVEL%
