@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r110: generic svmbctl step with output capture (cmd /c + append to
rem steps log + < NUL per the r87/r89 output-capture laws). Usage (no
rem quotes around the verb - %* forwards the whole tail verbatim):
rem   build\vm_ctl_step_r110.bat selftest
rem   build\vm_ctl_step_r110.bat cr3 probee2e
rem: cd to the test dir first (Tools exec defaults to System32 CWD,
rem which breaks tests that read/write files relative to the test dir)
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c cd /d %GUEST_TEST_DIR% && svmbctl.exe %* >> %GUEST_DESKTOP%\svmb_steps.log 2>&1 < NUL"
exit /b %ERRORLEVEL%
