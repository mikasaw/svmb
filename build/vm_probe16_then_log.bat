@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem svmb test flow - r55 one-shot: run stage-16 probe and dump the driver
rem log ring in a SINGLE guest exec. The AutoStart+NptEnable boot melts
rem VMware Tools ~15-90s after sc start (two melts, kd-forensic signature:
rem Mm working-set IPI storm + svmb vanishing from the module list), so
rem every extra vmrun round-trip risks losing the window. One exec = the
rem probe result AND the s16 ring lines land in the same file.
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (%GUEST_TEST_DIR%\svmbctl.exe probestage 16 & echo ---LOGSTART--- & %GUEST_TEST_DIR%\svmbctl.exe log & echo ---LOGEND---) > %GUEST_DESKTOP%\fresh2.txt 2>&1 < NUL"
exit /b %ERRORLEVEL%
