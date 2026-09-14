@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "C:\Windows\System32\cmd.exe" "/c echo x" 2>nul
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" -noWait "C:\Windows\System32\cmd.exe" "/c %GUEST_TEST_DIR%\stormprobe.bat"
exit /b %ERRORLEVEL%
