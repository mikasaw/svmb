@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem svmb test flow - pull the full guest event dump.
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\evt_full.txt" "%WINDBG_TEST%\logs\evt_full.txt"
exit /b %ERRORLEVEL%
