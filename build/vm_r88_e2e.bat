@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r87: local VM env (credentials/paths) - never commit
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r88 readvm E2E: strategy A vs B + hardening rejections + noaccess
rem fault-in, all into the steps log (pull afterwards)
rem NOTE: keep this arg-less; adding %1-style substitution to the
rem parenthesized-&-group shape is the vm_r87_read.bat silent-exit-1 trap.
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (%GUEST_TEST_DIR%\svmbctl.exe cr3 readvme2e & %GUEST_TEST_DIR%\svmbctl.exe cr3 readvm-self & %GUEST_TEST_DIR%\svmbctl.exe cr3 readvm-hole) >> %GUEST_DESKTOP%\svmb_steps.log 2>&1 < NUL"
exit /b %ERRORLEVEL%
