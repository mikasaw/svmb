@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r87: local VM env (credentials/paths) - never commit
rem r104 idle-armed soak: ONE timestamped snapshot (sysaudit drain +
rem behavior) appended to a guest-side file that SURVIVES a guest freeze
rem and hard reset - the persistent blackbox the serial channel never
rem was. The host-side loop (bash, timeout-wrapped) calls this bat every
rem ~45s for ~1h; a vmrun hang past the host timeout = freeze detected,
rem and the last lines of the guest file localize the wedge.
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
set V="%GUEST_TEST_DIR%\svmbctl.exe"
set OUT=%GUEST_DESKTOP%\r104_idle.txt
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c echo === %date% %time% === >> %OUT% 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %V% sysaudit >> %OUT% 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %V% behavior >> %OUT% 2>&1 < NUL"
exit /b %ERRORLEVEL%
