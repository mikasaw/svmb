@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem svmb test flow - step: dump the guest IO port arbiter allocations below
rem 0x1000 (classic COM/UART range) to find where the serial UART really is.
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c powershell -NoProfile -Command \"Get-WmiObject Win32_PortResource | Where-Object {$_.Start -lt 4096} | Sort-Object Start | ForEach-Object { '{0}-{1} {2}' -f $_.Start,$_.End,$_.Name }\" >> %GUEST_DESKTOP%\svmb_steps.log 2>&1 < NUL"
exit /b %ERRORLEVEL%
