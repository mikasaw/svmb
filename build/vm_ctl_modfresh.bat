@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem svmb test flow - attach BOTH test modules (debugger + cr3_monitor) with
rem FRESH per-read output. r41: cr3spoof needs cr3_monitor attached (its
rem Cr3MonInit arms the CR3-read intercept the spoof test depends on).
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (%GUEST_TEST_DIR%\svmbctl.exe mod attach debugger & %GUEST_TEST_DIR%\svmbctl.exe mod attach cr3_monitor & echo ---LIST--- & %GUEST_TEST_DIR%\svmbctl.exe mod list) > %GUEST_DESKTOP%\fresh2.txt 2>&1 < NUL"
exit /b %ERRORLEVEL%
