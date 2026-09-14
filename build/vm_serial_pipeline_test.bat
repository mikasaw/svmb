@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem svmb test flow - step: COM1 pipeline check. Writes one line to the guest
rem UART from user mode; if the host serial file grows, the VMware sink is
rem healthy and any missing S-tags would be a driver-side issue instead.
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c powershell -NoProfile -Command \"$p=New-Object System.IO.Ports.SerialPort 'COM1';$p.Open();$p.WriteLine('PIPELINE-TEST-'+(Get-Date -Format HHmmss));$p.Close()\" >> %GUEST_DESKTOP%\svmb_steps.log 2>&1 < NUL"
exit /b %ERRORLEVEL%
