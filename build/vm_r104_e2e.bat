@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r87: local VM env (credentials/paths) - never commit
rem r104 E2E: attach -> probe -> drain + behavior. Validates the NTOP
rem target attribution: after the resolver converges, the behavior
rem snapshot must gain "target: <probe> -> <notepad>" rows with nonzero
rem op= counts (the CLIENT_ID read path), alongside the r103 rd=/wr=.
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
set V="%GUEST_TEST_DIR%\svmbctl.exe"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c del /q %GUEST_DESKTOP%\r104_e2e.txt 2>nul < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromHostToGuest "%VM_VMX%" "%REPO%\build\guest_rpm_r104.ps1" "%GUEST_DESKTOP%\rpm_r104.ps1"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %V% mod attach sysaudit >> %GUEST_DESKTOP%\r104_e2e.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %V% sysaudit >> %GUEST_DESKTOP%\r104_e2e.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c powershell -NoProfile -ExecutionPolicy Bypass -File %GUEST_DESKTOP%\rpm_r104.ps1 >> %GUEST_DESKTOP%\r104_e2e.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c ping -n 4 127.0.0.1 >nul < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %V% sysaudit >> %GUEST_DESKTOP%\r104_e2e.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %V% behavior >> %GUEST_DESKTOP%\r104_e2e.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\r104_e2e.txt" "%WINDBG_TEST%\logs\r104_e2e.txt"
exit /b %ERRORLEVEL%
