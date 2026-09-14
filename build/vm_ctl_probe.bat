@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem svmb test flow - step: svmbctl npfprobe <n> with captured output (r31 M3
rem integration). Output lands in the guest steps log we pull back afterwards.
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %GUEST_TEST_DIR%\svmbctl.exe npfprobe %1 >> %GUEST_DESKTOP%\svmb_steps.log 2>&1 < NUL"
exit /b %ERRORLEVEL%
