@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem fixround T2: push race harness, run it (cmd-wrapped for redirection),
rem pull harness log AND powershell transcript back
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromHostToGuest "%VM_VMX%" "%REPO%\build\guest\fixround_t2.ps1" "%GUEST_TEST_DIR%\fixround_t2.ps1"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c powershell -NoProfile -ExecutionPolicy Bypass -File %GUEST_TEST_DIR%\fixround_t2.ps1 > %GUEST_DESKTOP%\fixround_t2_ps.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\fixround_t2_ps.txt" "%REPO%\tests\fixround_t2_ps_out.txt"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\fixround_t2_out.txt" "%REPO%\tests\fixround_t2_out.txt"
echo ==== POWERSHELL ====
type "%REPO%\tests\fixround_t2_ps_out.txt" 2>nul
echo ==== HARNESS LOG ====
type "%REPO%\tests\fixround_t2_out.txt" 2>nul
exit /b 0
