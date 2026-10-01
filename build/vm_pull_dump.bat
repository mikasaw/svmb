@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "C:\Windows\Minidump\091126-16593-01.dmp" "%WINDBG_TEST%\logs\091126-16593-01.dmp"
exit /b %ERRORLEVEL%
