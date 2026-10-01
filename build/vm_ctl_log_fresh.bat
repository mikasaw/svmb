@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem svmb test flow - dump the driver log ring to the fresh channel.
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %GUEST_TEST_DIR%\svmbctl.exe log > %GUEST_DESKTOP%\fresh4.txt 2>&1 < NUL"
exit /b %ERRORLEVEL%
