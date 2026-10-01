@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r87: local VM env (credentials/paths) - never commit
rem r101: dump qwords at a runtime kernel VA (the SSDT table / the old
rem LSTAR candidate region). Args: %1 = hex VA, %2 = qword count (default 16).
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
if "%~2"=="" (set N=16) else (set N=%~2)
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %GUEST_TEST_DIR%\svmbctl.exe kdump %~1 %N% > %GUEST_DESKTOP%\r101_kdump.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\r101_kdump.txt" "%WINDBG_TEST%\logs\r101_kdump.txt"
exit /b %ERRORLEVEL%
