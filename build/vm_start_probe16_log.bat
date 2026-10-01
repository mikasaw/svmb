@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem svmb test flow - r55 atomic shot: start the service AND run the stage-16
rem probe AND dump the log ring inside ONE guest exec, output to disk.
rem Host-side ops after `sc start` were losing the ~30s melt window (the
rem AutoStart enter sequence makes sc start slow, and the next vmrun exec
rem arrives after VMware Tools is already starved). With everything in one
rem exec there is zero channel latency between load and probe, and the
rem result file persists on the guest disk even through a freeze.
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (sc start svmb & %GUEST_TEST_DIR%\svmbctl.exe probestage 16 & echo ---LOGSTART--- & %GUEST_TEST_DIR%\svmbctl.exe log & echo ---LOGEND---) > %GUEST_DESKTOP%\probe16.txt 2>&1 < NUL"
exit /b %ERRORLEVEL%
