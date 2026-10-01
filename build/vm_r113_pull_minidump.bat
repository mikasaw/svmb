@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r113: list + pull guest minidumps from the boot-loop crashes for
rem offline cdb analysis (r99 pattern)
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (dir /b /o-d C:\Windows\Minidump\ & echo ---MEMDMP--- & dir /b C:\Windows\MEMORY.DMP 2>nul) > %GUEST_DESKTOP%\r113_md.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\r113_md.txt" "%REPO%\tests\r113_md_list.txt"
exit /b %ERRORLEVEL%
