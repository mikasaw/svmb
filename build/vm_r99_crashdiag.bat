@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r87: local VM env (credentials/paths) - never commit
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (sc query svmb & echo ===MINIDUMP=== & dir /b /od C:\Windows\Minidump & echo ===MEMDUMP=== & dir /b C:\Windows\MEMORY.DMP) > %GUEST_DESKTOP%\r99_crash.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\r99_crash.txt" "%WINDBG_TEST%\logs\r99_crash.txt"
exit /b %ERRORLEVEL%
