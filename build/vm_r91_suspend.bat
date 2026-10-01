@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r87: local VM env (credentials/paths) - never commit
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r91 plan C- E2E driver: launch probee2e suspend (it FREEZES mid-IOCTL
rem holding the gate), let the outer script drive list/resume, then pull
rem the victim's captured output
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %GUEST_TEST_DIR%\svmbctl.exe cr3 probee2e suspend > %GUEST_DESKTOP%\susp_r91.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\susp_r91.txt" "%WINDBG_TEST%\logs\susp_r91.txt"
exit /b %ERRORLEVEL%
