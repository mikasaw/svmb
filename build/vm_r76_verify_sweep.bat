@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (echo === regions === & %GUEST_TEST_DIR%\svmbctl.exe cr3 regions & echo === stats === & %GUEST_TEST_DIR%\svmbctl.exe cr3 stats & echo === log === & %GUEST_TEST_DIR%\svmbctl.exe log) > %GUEST_DESKTOP%\r76_sweep.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\r76_sweep.txt" "%REPO%\tests\r76_sweep_out.txt"
exit /b %ERRORLEVEL%
