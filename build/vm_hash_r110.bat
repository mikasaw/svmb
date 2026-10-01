@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r110: guest-side SHA256 of the pushed svmb.sys + svmbctl.exe (mirrors
rem the host certutil check; results land in the steps log side channel)
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (echo --- svmb.sys --- & certutil -hashfile %GUEST_TEST_DIR%\svmb.sys SHA256 & echo --- svmbctl.exe --- & certutil -hashfile %GUEST_TEST_DIR%\svmbctl.exe SHA256) > %GUEST_TEST_DIR%\r110_hash.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_TEST_DIR%\r110_hash.txt" "%REPO%\tests\r110_hash_out.txt"
exit /b %ERRORLEVEL%
