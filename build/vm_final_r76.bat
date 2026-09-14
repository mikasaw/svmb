@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (for /L %%i in (1,1,3) do @%GUEST_TEST_DIR%\svmbctl.exe cr3 probee2e & for /L %%i in (1,1,3) do @%GUEST_TEST_DIR%\svmbctl.exe cr3 blockself & echo === final stats === & %GUEST_TEST_DIR%\svmbctl.exe cr3 stats & %GUEST_TEST_DIR%\svmbctl.exe cr3 regions) > %GUEST_DESKTOP%\r76_final.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\r76_final.txt" "%REPO%\tests\r76_final_out.txt"
exit /b %ERRORLEVEL%
