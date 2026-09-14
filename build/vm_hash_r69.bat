@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (certutil -hashfile %GUEST_TEST_DIR%\svmb.sys SHA256 & certutil -hashfile %GUEST_TEST_DIR%\svmbctl.exe SHA256) > %GUEST_DESKTOP%\r69_hash.txt 2>&1 < NUL"
exit /b %ERRORLEVEL%
