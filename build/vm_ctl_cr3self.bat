@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem svmb test flow - cr3self flow via append channel with markers.
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (echo MARK_X_WATCH & %GUEST_TEST_DIR%\svmbctl.exe cr3 watch svmbctl.exe 0xDEADC0DE & echo MARK_Y_SELF & %GUEST_TEST_DIR%\svmbctl.exe cr3self 0xDEADC0DE & echo MARK_Z_UNWATCH & %GUEST_TEST_DIR%\svmbctl.exe cr3 unwatch) >> %GUEST_DESKTOP%\svmb_steps.log 2>&1 < NUL"
exit /b %ERRORLEVEL%
