@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem fixround env probe: test dir, cert stores, testsigning state
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (echo ===DIR=== & dir /b %GUEST_TEST_DIR% 2>&1 & echo ===ROOT-STORE=== & certutil -store Root svmb-test 2>&1 | findstr /i svmb & echo ===TP-STORE=== & certutil -store TrustedPeople svmb-test 2>&1 | findstr /i svmb & echo ===BCD=== & bcdedit /enum {current} | findstr /i testsigning) > %GUEST_DESKTOP%\fixround_env.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\fixround_env.txt" "%REPO%\tests\fixround_env_out.txt"
exit /b %ERRORLEVEL%
