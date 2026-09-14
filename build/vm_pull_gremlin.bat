@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem svmb test flow - pull the gremlin hunt results.
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\gremlin_out.txt" "%WINDBG_TEST%\logs\gremlin_out.txt"
exit /b %ERRORLEVEL%
