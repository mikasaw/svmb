@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem svmb test flow - copy guest hash files to the known-good Desktop fresh.txt
rem channel (vmrun CopyFileFromGuestToHost was failing on the svmb-test subdir
rem path with "file not found" while dir listed the file - workaround).
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (type %GUEST_TEST_DIR%\hash.txt & echo ---CTL--- & type %GUEST_TEST_DIR%\hash_ctl.txt) > %GUEST_DESKTOP%\fresh.txt 2>&1 < NUL"
exit /b %ERRORLEVEL%
