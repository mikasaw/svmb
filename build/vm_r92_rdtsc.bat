@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r87: local VM env (credentials/paths) - never commit
rem r92 TSC probe leg: guest-view raw TSC (compare against host raw TSC via
rem kd rdmsr 0x10). APPEND mode; delete the target file for a fresh reading.
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %GUEST_TEST_DIR%\svmbctl.exe rdtsc >> %GUEST_DESKTOP%\rdtsc_r92.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\rdtsc_r92.txt" "%WINDBG_TEST%\logs\rdtsc_r92.txt"
exit /b %ERRORLEVEL%
