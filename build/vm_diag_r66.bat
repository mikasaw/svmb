@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r66 diagnostic: is the svmb driver loaded (file locked) despite no service?
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (driverquery /v | findstr /i svmb & sc query svmb & dir %GUEST_TEST_DIR%\svmb.sys) > %GUEST_DESKTOP%\fresh.txt 2>&1 < NUL"
exit /b %ERRORLEVEL%
