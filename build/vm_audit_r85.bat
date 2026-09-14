@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (echo === poison === & %GUEST_TEST_DIR%\svmbctl.exe cr3 poison & echo === probee2e === & %GUEST_TEST_DIR%\svmbctl.exe cr3 probee2e & echo === blockself === & %GUEST_TEST_DIR%\svmbctl.exe cr3 blockself & echo === multitarget-lite === & %GUEST_TEST_DIR%\svmbctl.exe cr3 regions & echo === stats === & %GUEST_TEST_DIR%\svmbctl.exe cr3 stats) > %GUEST_DESKTOP%\r85_audit.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\r85_audit.txt" "%REPO%\tests\r85_audit_out.txt"
exit /b %ERRORLEVEL%
