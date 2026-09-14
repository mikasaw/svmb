@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r66 gremlin hunt: find what auto-starts/auto-arms svmb in the guest.
rem No inner quotes allowed (vmrun quoting layer trap).
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (schtasks /query /fo LIST | findstr /i svmb & echo ===RUN=== & reg query HKLM\Software\Microsoft\Windows\CurrentVersion\Run & reg query HKCU\Software\Microsoft\Windows\CurrentVersion\Run & echo ===STARTUP=== & dir /b %GUEST_PROFILE%\AppData\Roaming\Microsoft\Windows\Start Menu\Programs\Startup & echo ===DESKTOP-CMD=== & dir /b %GUEST_DESKTOP%\*.cmd %GUEST_DESKTOP%\*.bat) > %GUEST_DESKTOP%\fresh.txt 2>&1 < NUL"
exit /b %ERRORLEVEL%
