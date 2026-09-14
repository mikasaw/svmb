@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem svmb test flow - step: guest-side SHA256 of svmbctl.exe + pull (ctl sync).
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c certutil -hashfile %GUEST_TEST_DIR%\svmbctl.exe SHA256 > %GUEST_TEST_DIR%\hash.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_TEST_DIR%\hash.txt" "%WINDBG_TEST%\logs\guest_hash.txt"
exit /b %ERRORLEVEL%
