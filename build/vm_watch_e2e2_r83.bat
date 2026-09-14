@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (%GUEST_TEST_DIR%\svmbctl.exe cr3 watch notepad.exe & start notepad.exe & ping -n 16 127.0.0.1 >nul & %GUEST_TEST_DIR%\svmbctl.exe cr3 stats & taskkill /f /im notepad.exe) > %GUEST_DESKTOP%\r83_watch.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\r83_watch.txt" "%REPO%\tests\r83_watch_out.txt"
exit /b %ERRORLEVEL%
