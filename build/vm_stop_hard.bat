@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws stop "%VM_VMX%" hard
exit /b %ERRORLEVEL%
