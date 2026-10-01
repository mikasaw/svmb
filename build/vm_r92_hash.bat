@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r87: local VM env (credentials/paths) - never commit
rem r92: compute guest-side SHA256 of the freshly pushed .sys/.ctl and pull
rem them back for the dual verification (AGENTS rule 4).
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (certutil -hashfile %GUEST_TEST_DIR%\svmb.sys SHA256 & echo ---CTL--- & certutil -hashfile %GUEST_TEST_DIR%\svmbctl.exe SHA256) > %GUEST_DESKTOP%\hash_r92.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\hash_r92.txt" "%WINDBG_TEST%\logs\hash_r92.txt"
exit /b %ERRORLEVEL%
