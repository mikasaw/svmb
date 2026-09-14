@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r87: local VM env (credentials/paths) - never commit
rem r92: mod attach/detach with its OWN captured output (r89 channel law:
rem state-changing guest ops must capture output; the generic vm_ctl_mod
rem writes to the shared steps log which is ring-dump-noisy).
rem %1 = attach|detach, %2 = module name
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %GUEST_TEST_DIR%\svmbctl.exe mod %1 %2 >> %GUEST_DESKTOP%\mod_r92.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\mod_r92.txt" "%WINDBG_TEST%\logs\mod_r92.txt"
exit /b %ERRORLEVEL%
