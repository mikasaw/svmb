@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r87: local VM env (credentials/paths) - never commit
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r91 design check: pull the guest kernel image for export-table inspection
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "C:\Windows\System32\ntoskrnl.exe" "%WINDBG_TEST%\logs\guest_ntoskrnl.exe"
exit /b %ERRORLEVEL%
