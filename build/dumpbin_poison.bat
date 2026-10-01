@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
call "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvars64.bat" >nul
dumpbin /symbols %REPO%\driver\obj\Debug\cr3_monitor.obj | findstr /c:Poison
exit /b 0
