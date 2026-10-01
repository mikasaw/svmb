@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem round-18: revert to the post-fix known-good snapshot (clears wedged state)
"%VMRUN%" -T ws revertToSnapshot "%VM_VMX%" "post-fix-2026-09-07-serial-file"
exit /b %ERRORLEVEL%
