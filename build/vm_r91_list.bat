@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r87: local VM env (credentials/paths) - never commit
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r91 plan C- E2E: suspension list (ungated) APPENDED to its own file
rem (acceptance P3-4: > overwrites destroyed the frozen-window evidence
rem once already)
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %GUEST_TEST_DIR%\svmbctl.exe cr3 suspended >> %GUEST_DESKTOP%\susp_list.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\susp_list.txt" "%WINDBG_TEST%\logs\susp_list.txt"
exit /b %ERRORLEVEL%
