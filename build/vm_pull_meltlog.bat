@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\melt_watch.log" "%WINDBG_TEST%\logs\melt_watch.log"
exit /b %ERRORLEVEL%
