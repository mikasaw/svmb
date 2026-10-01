@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem svmb test flow - log dump with markers to distinguish empty-vs-failed.
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (echo ---LOGSTART--- & %GUEST_TEST_DIR%\svmbctl.exe log & echo ---LOGEND--- rc=%errorlevel%) > %GUEST_DESKTOP%\fresh4.txt 2>&1 < NUL"
exit /b %ERRORLEVEL%
