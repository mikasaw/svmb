@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r87: local VM env (credentials/paths) - never commit
rem r101: guest-side copy of the running kernel image to Temp (direct pull of
rem System32\ntoskrnl.exe is access-denied through the Tools channel), then
rem pull the copy. Offline use: .data holds KiServiceTable's initialized u32
rem entries -> SSN derivation (NtRead/WriteVirtualMemory have no Zw stubs).
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c copy /y C:\Windows\System32\ntoskrnl.exe C:\Windows\Temp\ntos_r101.exe < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "C:\Windows\Temp\ntos_r101.exe" "%WINDBG_TEST%\logs\guest_ntoskrnl.exe"
exit /b %ERRORLEVEL%
