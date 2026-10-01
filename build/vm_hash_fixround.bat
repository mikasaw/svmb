@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem fixround: compute SHA256 of both deployed binaries in one guest pass
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (echo ===SYS=== & certutil -hashfile %GUEST_TEST_DIR%\svmb.sys SHA256 & echo ===CTL=== & certutil -hashfile %GUEST_TEST_DIR%\svmbctl.exe SHA256) > %GUEST_DESKTOP%\fixround_hash.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\fixround_hash.txt" "%REPO%\tests\fixround_hash_out.txt"
exit /b %ERRORLEVEL%
