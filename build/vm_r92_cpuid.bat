@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r87: local VM env (credentials/paths) - never commit
rem r92 CPUID verification leg. Shape mirrors the proven vm_r91_list.bat
rem single-command form exactly (cmd >> file 2>&1 < NUL, no parens, no del):
rem each leg writes its own file so freshness comes from the filename.
rem %1 = leg suffix (base|on|off|on2), passed through by the caller.
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
set LEG=%1
if "%LEG%"=="" set LEG=base
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %GUEST_TEST_DIR%\svmbctl.exe cpuid 1 >> %GUEST_DESKTOP%\cpuid_r92_%LEG%.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %GUEST_TEST_DIR%\svmbctl.exe cpuid 40000000 >> %GUEST_DESKTOP%\cpuid_r92_%LEG%.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %GUEST_TEST_DIR%\svmbctl.exe cpuid 40000010 >> %GUEST_DESKTOP%\cpuid_r92_%LEG%.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %GUEST_TEST_DIR%\svmbctl.exe cpuid 0 >> %GUEST_DESKTOP%\cpuid_r92_%LEG%.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %GUEST_TEST_DIR%\svmbctl.exe cpuid 80000001 >> %GUEST_DESKTOP%\cpuid_r92_%LEG%.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %GUEST_TEST_DIR%\svmbctl.exe cpuid 400000ff >> %GUEST_DESKTOP%\cpuid_r92_%LEG%.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\cpuid_r92_%LEG%.txt" "%WINDBG_TEST%\logs\cpuid_r92_%LEG%.txt"
exit /b %ERRORLEVEL%
