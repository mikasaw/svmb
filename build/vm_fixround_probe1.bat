@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromHostToGuest "%VM_VMX%" "%REPO%\build\guest\fixround_probe1.ps1" "%GUEST_TEST_DIR%\fixround_probe1.ps1"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c powershell -NoProfile -ExecutionPolicy Bypass -File %GUEST_TEST_DIR%\fixround_probe1.ps1 > %GUEST_DESKTOP%\fixround_probe1_ps.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\fixround_probe1_out.txt" "%REPO%\tests\fixround_probe1_out.txt"
type "%REPO%\tests\fixround_probe1_out.txt" 2>nul
echo ---- PS ----
type "%REPO%\tests\fixround_probe1_ps_out.txt" 2>nul
exit /b 0
