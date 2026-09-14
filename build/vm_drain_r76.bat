@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (for /L %%i in (1,1,4) do @%GUEST_TEST_DIR%\svmbctl.exe log) > %GUEST_DESKTOP%\r76_drain.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\r76_drain.txt" "%REPO%\tests\r76_drain_out.txt"
exit /b %ERRORLEVEL%
