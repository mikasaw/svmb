@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r87: local VM env (credentials/paths) - never commit
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r89 crash forensics: dir the minidumps into the steps log, then copy the
rem newest by explicit name (args: %1 = dmp filename from the dir listing)
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c dir /od C:\Windows\Minidump\ >> %GUEST_DESKTOP%\svmb_steps.log 2>&1 < NUL"
if "%1"=="" exit /b 0
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "C:\Windows\Minidump\%1" "%WINDBG_TEST%\logs\%1"
exit /b %ERRORLEVEL%
