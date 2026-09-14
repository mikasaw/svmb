@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (%GUEST_TEST_DIR%\svmbctl.exe cr3 probee2e & %GUEST_TEST_DIR%\svmbctl.exe cr3 blockself & %GUEST_TEST_DIR%\svmbctl.exe cr3 regions & %GUEST_TEST_DIR%\svmbctl.exe cr3 stats) > %GUEST_DESKTOP%\r78_reg.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\r78_reg.txt" "%REPO%\tests\r78_reg_out.txt"
exit /b %ERRORLEVEL%
