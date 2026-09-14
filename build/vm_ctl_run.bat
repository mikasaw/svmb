@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem svmb test flow - step: run any svmbctl subcommand, redirect its stdout
rem and stderr to a guest file we can later pull back. Hard-quoted args to
rem avoid vmrun quoting layer mangling spaces.
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "%GUEST_TEST_DIR%\svmbctl.exe" "%*"
exit /b %ERRORLEVEL%
