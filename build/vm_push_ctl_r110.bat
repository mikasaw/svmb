@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r110: push svmbctl.exe alongside the driver (Phase 2 companion)
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromHostToGuest "%VM_VMX%" "%REPO%\x64\Debug\svmbctl.exe" "%GUEST_TEST_DIR%\svmbctl.exe"
exit /b %ERRORLEVEL%
