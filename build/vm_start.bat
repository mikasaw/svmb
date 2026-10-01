@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws start "%VM_VMX%"
exit /b %ERRORLEVEL%
