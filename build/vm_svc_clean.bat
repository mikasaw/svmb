@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem svmb test flow - step: clear guest log + stop & delete old svmb service.
rem Wraps vmrun in a bat so no shell quoting layer can mangle the paths.
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c type nul > %GUEST_DESKTOP%\svmb_steps.log & sc stop svmb >> %GUEST_DESKTOP%\svmb_steps.log 2>&1 & ping -n 3 127.0.0.1 >nul & sc delete svmb >> %GUEST_DESKTOP%\svmb_steps.log 2>&1 < NUL"
exit /b %ERRORLEVEL%
