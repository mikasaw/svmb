@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (tasklist /v | findstr /i powershell & echo ---STATS--- & %GUEST_TEST_DIR%\svmbctl.exe cr3 stats) > %GUEST_DESKTOP%\r72_ps.txt 2>&1 < NUL"
exit /b %ERRORLEVEL%
