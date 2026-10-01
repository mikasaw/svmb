@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
call "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvars64.bat" >nul
pushd "%~dp0\.."
"C:\Program Files\Microsoft Visual Studio\18\Insiders\MSBuild\Current\Bin\MSBuild.exe" driver\svmb.vcxproj /t:Rebuild /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo
echo ---CTL---
"C:\Program Files\Microsoft Visual Studio\18\Insiders\MSBuild\Current\Bin\MSBuild.exe" app\svmbctl.vcxproj /t:Rebuild /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo
popd
rem r111: sign both artifacts in-pipeline (r73 law: Release is NOT
rem auto-test-signed by MSBuild - an unsigned .sys fails guest sc start
rem with err 577; this round hit exactly that after the manual sign
rem step was skipped)
call build\sign_ctl_rel.bat
call build\sign_sys_rel.bat
