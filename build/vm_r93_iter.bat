@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r87: local VM env (credentials/paths) - never commit
rem r93 mixed soak iteration (kill + suspend + health), r90-style sequential
rem vmrun calls. Suspend cycle uses Start-Process hatching (r87 law) so the
rem frozen victim never holds the VIX channel: hatch -> wait -> list ->
rem resume all -> wait -> the victim's own output is typed into the steps
rem log as the PASS evidence. %1 = iteration number.
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
set V="%GUEST_TEST_DIR%\svmbctl.exe"
set L="%GUEST_DESKTOP%\svmb_steps.log"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c echo ===SOAK-R93-ITER %1 === >> %L% 2>&1 < NUL"
rem -- kill-owner stress (the driver terminates this process; non-zero
rem    errorlevel here is the EXPECTED outcome, do not fail the iteration)
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %V% cr3 probee2e kill >> %L% 2>&1 < NUL"
rem -- health reads
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %V% cr3 readvm-self >> %L% 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %V% cr3 readvm-hole >> %L% 2>&1 < NUL"
rem -- suspend cycle: hatch the victim (r87 law: cmd `start` is DEAD under
rem    VIX sessions - the only sanctioned detour is powershell Start-Process
rem    onto a pre-placed ps1), give it ~10s to reach the freeze, then list +
rem    resume all from the outside
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c powershell -NoProfile -Command Start-Process powershell -ArgumentList '-NoProfile -ExecutionPolicy Bypass -File %GUEST_DESKTOP%\susp_victim.ps1' -WindowStyle Hidden < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c ping -n 11 127.0.0.1 > nul < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %V% cr3 suspended >> %L% 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %V% cr3 resume all >> %L% 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c ping -n 11 127.0.0.1 > nul < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c type %GUEST_DESKTOP%\susp_r93.txt >> %L% 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %V% cr3 stats >> %L% 2>&1 < NUL"
exit /b 0
