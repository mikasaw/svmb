@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r87: local VM env (credentials/paths) - never commit
rem r102 alert-leg E2E: the X-deny sensor SAMPLES (r100 law - cached TLB
rem translations slide, ~2-3 sensed entries/sec/page on this vhv), so the
rem default 128/window threshold is unreachable by construction. This leg
rem sets BehaveRdThs=2 / BehaveWrThs=2 (knob read at DriverEntry - needs a
rem service restart, NOT just a re-attach), then runs the probe: the
rem spaced 64B reads must cross 2/window and raise exactly ONE alert edge
rem per window (Alerted flag + LOUD kernel-log line), and the behavior
rem snapshot must show PeakRd >= 2 with Alerts >= 1.
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
set V="%GUEST_TEST_DIR%\svmbctl.exe"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c del /q %GUEST_DESKTOP%\r102_alert.txt 2>nul < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c sc stop svmb >> %GUEST_DESKTOP%\r102_alert.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c reg add HKLM\SYSTEM\CurrentControlSet\Services\svmb\Parameters /v BehaveRdThs /t REG_DWORD /d 2 /f >> %GUEST_DESKTOP%\r102_alert.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c reg add HKLM\SYSTEM\CurrentControlSet\Services\svmb\Parameters /v BehaveWrThs /t REG_DWORD /d 2 /f >> %GUEST_DESKTOP%\r102_alert.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c sc start svmb >> %GUEST_DESKTOP%\r102_alert.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromHostToGuest "%VM_VMX%" "%REPO%\build\guest_rpm_r101.ps1" "%GUEST_DESKTOP%\rpm_r101.ps1"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %V% mod attach sysaudit >> %GUEST_DESKTOP%\r102_alert.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %V% sysaudit >> %GUEST_DESKTOP%\r102_alert.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c powershell -NoProfile -ExecutionPolicy Bypass -File %GUEST_DESKTOP%\rpm_r101.ps1 >> %GUEST_DESKTOP%\r102_alert.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %V% sysaudit >> %GUEST_DESKTOP%\r102_alert.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %V% behavior >> %GUEST_DESKTOP%\r102_alert.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %V% log >> %GUEST_DESKTOP%\r102_alert.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\r102_alert.txt" "%WINDBG_TEST%\logs\r102_alert.txt"
exit /b %ERRORLEVEL%
