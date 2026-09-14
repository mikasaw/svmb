@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem svmb - build the driver + ctl (Debug, unified output to repo-root x64\Debug).
rem MSBuild auto test-signs the .sys; svmbctl.exe needs a manual signtool pass.
call "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvars64.bat" >nul
pushd "%~dp0\.."
"C:\Program Files\Microsoft Visual Studio\18\Insiders\MSBuild\Current\Bin\MSBuild.exe" driver\svmb.vcxproj /t:Rebuild /p:Configuration=Debug /p:Platform=x64 /m /v:minimal /nologo
set RC=%ERRORLEVEL%
if not "%RC%"=="0" goto done
"C:\Program Files\Microsoft Visual Studio\18\Insiders\MSBuild\Current\Bin\MSBuild.exe" app\svmbctl.vcxproj /t:Rebuild /p:Configuration=Debug /p:Platform=x64 /m /v:minimal /nologo
set RC=%ERRORLEVEL%
if not "%RC%"=="0" goto done
rem self-sign svmbctl so guest SmartScreen/AV doesn't block execution
signtool sign /fd SHA256 /f build\svmb_test.pfx /p svmb x64\Debug\svmbctl.exe >nul 2>&1
:done
popd
echo build RC=%RC%
exit /b %RC%
