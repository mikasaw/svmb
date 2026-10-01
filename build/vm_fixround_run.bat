@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem fixround generic runner: svmbctl <args> with guest-side output capture
if "%~1"=="" exit /b 2
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %GUEST_TEST_DIR%\svmbctl.exe %* > %GUEST_DESKTOP%\fixround_out.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\fixround_out.txt" "%REPO%\tests\fixround_out.txt"
type "%REPO%\tests\fixround_out.txt"
exit /b %ERRORLEVEL%
