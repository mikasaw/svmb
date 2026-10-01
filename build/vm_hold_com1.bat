@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem svmb test flow - step: hold COM1 open (D0) in the guest with a resident
rem PowerShell. This vmrun (1.17.0) wants -noWait AFTER the vmx path.
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" -noWait "C:\Windows\System32\cmd.exe" "/c powershell -NoProfile -Command \"$p=New-Object IO.Ports.SerialPort 'COM1';$p.Open();Start-Sleep 14400\" < NUL"
exit /b %ERRORLEVEL%
