@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (certutil -addstore Root %GUEST_TEST_DIR%\svmb-test.cer & echo ---TP--- & certutil -addstore TrustedPeople %GUEST_TEST_DIR%\svmb-test.cer) > %GUEST_DESKTOP%\r74_cert.txt 2>&1 < NUL"
exit /b %ERRORLEVEL%
