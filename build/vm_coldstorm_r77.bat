@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (for /L %%i in (1,1,3) do @(echo === cold %%i === & sc stop svmb < NUL & ping -n 3 127.0.0.1 >nul & sc start svmb < NUL & ping -n 3 127.0.0.1 >nul & %GUEST_TEST_DIR%\svmbctl.exe cr3 probee2e & %GUEST_TEST_DIR%\svmbctl.exe cr3 stats)) > %GUEST_DESKTOP%\r77_cold.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\r77_cold.txt" "%REPO%\tests\r77_cold_out.txt"
exit /b %ERRORLEVEL%
