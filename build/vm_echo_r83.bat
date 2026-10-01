@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (echo alive %time% & sc query svmb | findstr STATE & type %GUEST_DESKTOP%\svmb_steps.log 2>nul | findstr /n "^" | findstr /r ".") > %GUEST_DESKTOP%\r83_echo.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\r83_echo.txt" "%REPO%\tests\r83_echo_out.txt"
exit /b %ERRORLEVEL%
