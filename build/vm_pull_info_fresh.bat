@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem svmb test flow - pull the fresh svmbctl info output.
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\fresh2.txt" "%WINDBG_TEST%\logs\fresh2.txt"
exit /b %ERRORLEVEL%
