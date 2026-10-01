@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (certutil -hashfile %GUEST_TEST_DIR%\svmb.sys SHA256 & certutil -hashfile %GUEST_TEST_DIR%\svmbctl.exe SHA256) > %GUEST_DESKTOP%\r76_hash.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\r76_hash.txt" "%REPO%\tests\r76_hash_out.txt"
exit /b %ERRORLEVEL%
