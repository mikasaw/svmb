@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (taskkill /f /im notepad.exe & %GUEST_TEST_DIR%\svmbctl.exe cr3 unwatch & %GUEST_TEST_DIR%\svmbctl.exe mod detach cr3_monitor & %GUEST_TEST_DIR%\svmbctl.exe cr3 probee2e & %GUEST_TEST_DIR%\svmbctl.exe cr3 regions) > %GUEST_DESKTOP%\r83_clean.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\r83_clean.txt" "%REPO%\tests\r83_clean_out.txt"
exit /b %ERRORLEVEL%
