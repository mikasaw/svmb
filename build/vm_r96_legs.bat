@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r87: local VM env (credentials/paths) - never commit
rem r96 verification legs: deadbuf refusal + readvm-self (stale flag must
rem NOT fire on a live target) + ctl sanity. Fresh file per run.
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
set V="%GUEST_TEST_DIR%\svmbctl.exe"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c del /q %GUEST_DESKTOP%\r96_legs.txt 2>nul < NUL & %V% cr3 readvm-deadbuf >> %GUEST_DESKTOP%\r96_legs.txt 2>&1 < NUL & %V% cr3 readvm-self >> %GUEST_DESKTOP%\r96_legs.txt 2>&1 < NUL & %V% cpuid 1 >> %GUEST_DESKTOP%\r96_legs.txt 2>&1 < NUL & %V% rdtsc >> %GUEST_DESKTOP%\r96_legs.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\r96_legs.txt" "%WINDBG_TEST%\logs\r96_legs.txt"
exit /b %ERRORLEVEL%
