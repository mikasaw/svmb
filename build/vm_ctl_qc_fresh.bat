@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem svmb test flow - FRESH helper-source sweep: all services matching svm +
rem svmb-maintenance named object + Tools script config.
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (sc query state= all | findstr /i /c:svm & sc query svmb-maintenance & sc qfailure svmb & dir C:\ProgramData\VMware\VMware Tools & type C:\ProgramData\VMware\VMware Tools\tools.conf) > %GUEST_DESKTOP%\fresh.txt 2>&1 < NUL"
exit /b %ERRORLEVEL%
