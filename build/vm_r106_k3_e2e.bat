@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r87: local VM env (credentials/paths) - never commit
rem r106 K3 E2E: live-EPROCESS image re-verify on the kill path.
rem   leg 1 (regression): probee2e kill - svmbctl.exe is NOT deny-listed,
rem        so the K3 gate passes it and the driver still terminates the
rem        probe (vmrun reporting an abnormal exit IS the pass).
rem   leg 2 (refusal): a COPY of svmbctl.exe named lsass.exe (deny-listed)
rem        runs probee2e kill - the trip-time deny gate must refuse the
rem        response and the process must SURVIVE with a verdict printed.
rem cr3_monitor must be attached (r88 law) for the protect pipeline.
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
set V="%GUEST_TEST_DIR%\svmbctl.exe"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c del /q %GUEST_DESKTOP%\r106_k3.txt %GUEST_DESKTOP%\kill_r106.txt %GUEST_DESKTOP%\k3_refuse.txt %GUEST_TEST_DIR%\lsass.exe 2>nul < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %V% mod attach cr3_monitor >> %GUEST_DESKTOP%\r106_k3.txt 2>&1 < NUL"
rem --- leg 1: regression (kill still works for non-deny images)
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %V% cr3 probee2e kill > %GUEST_DESKTOP%\kill_r106.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %V% log >> %GUEST_DESKTOP%\r106_k3.txt 2>&1 < NUL"
rem --- leg 2: refusal (deny-listed live name)
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromHostToGuest "%VM_VMX%" "%REPO%\x64\Debug\svmbctl.exe" "%GUEST_TEST_DIR%\lsass.exe"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %GUEST_TEST_DIR%\lsass.exe cr3 probee2e kill > %GUEST_DESKTOP%\k3_refuse.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %V% log >> %GUEST_DESKTOP%\r106_k3.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\r106_k3.txt" "%WINDBG_TEST%\logs\r106_k3.txt"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\kill_r106.txt" "%WINDBG_TEST%\logs\kill_r106.txt"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\k3_refuse.txt" "%WINDBG_TEST%\logs\k3_refuse.txt"
exit /b %ERRORLEVEL%
