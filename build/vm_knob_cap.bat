@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r87: local VM env (credentials/paths) - never commit
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r89 knob verification: registry value + readvm-self output, captured
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (reg query HKLM\SYSTEM\CurrentControlSet\Services\svmb\Parameters /v ReadVmEnable & %GUEST_TEST_DIR%\svmbctl.exe cr3 readvm-self) > %GUEST_DESKTOP%\knob.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\knob.txt" "%WINDBG_TEST%\logs\knob.txt"
exit /b %ERRORLEVEL%
