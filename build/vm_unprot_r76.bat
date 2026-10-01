@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (%GUEST_TEST_DIR%\svmbctl.exe cr3 unprotect 4632 & %GUEST_TEST_DIR%\svmbctl.exe cr3 regions) > %GUEST_DESKTOP%\r76_up.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\r76_up.txt" "%REPO%\tests\r76_up_out.txt"
exit /b %ERRORLEVEL%
