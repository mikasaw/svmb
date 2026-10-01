@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
call "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvars64.bat" >nul
signtool sign /fd sha256 /n svmb-test /s My "%REPO%\x64\Release\svmbctl.exe"
exit /b %ERRORLEVEL%
