@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem round-14: HideCpuidBits=0 (consistent CPUID - no SVM/NPT bit hiding).
rem MUST run AFTER vm_svc_create and BEFORE vm_svc_start (sc delete wipes
rem the whole service key incl. Parameters).
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c reg add \"HKLM\SYSTEM\CurrentControlSet\Services\svmb\Parameters\" /v HideCpuidBits /t REG_DWORD /d 0 /f > %GUEST_DESKTOP%\reg_hide.txt 2>&1 < NUL"
exit /b %ERRORLEVEL%
