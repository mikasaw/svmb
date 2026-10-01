@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem Hard-reset the test VM. Blocks until the guest is back up (30-90s).
rem Usage: build\vm_reset_hard.bat
"%VMRUN%" -T ws reset "%VM_VMX%" hard
exit /b %ERRORLEVEL%
