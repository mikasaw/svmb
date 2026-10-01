@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem svmb test flow - guest process list (loop executor hunt).
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% listProcessesInGuest "%VM_VMX%" > %WINDBG_TEST%\logs\guestproc.txt 2>&1
exit /b %ERRORLEVEL%
