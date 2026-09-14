@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromHostToGuest "%VM_VMX%" "%REPO%\build\guest\exitprof_sampler.bat" "%GUEST_TEST_DIR%\exitprof_sampler.bat"
exit /b %ERRORLEVEL%
