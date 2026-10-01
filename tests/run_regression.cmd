@echo off
rem ============================================================================
rem  svmb regression flow - run INSIDE the test VM, as Administrator.
rem
rem  Performs: driver install -> info -> start -> info (assert SVM bit hidden,
rem  P0-1) -> storm -> demo -> mod attach -> cycle -> mod list (assert module
rem  survived, P2-5) -> stop -> info (assert OFF) -> log dump.
rem
rem  Usage:   run_regression.cmd [stormIterations] [cycleCount]
rem  Default: 100000 / 100
rem
rem  Prereq:  svmb.sys + svmbctl.exe (built and signed via build\sign.cmd on
rem  the dev box) sitting NEXT TO this script. Snapshot the VM first - see
rem  tests/VM_SETUP.md.
rem ============================================================================
setlocal
set "SCRIPT_DIR=%~dp0"
set "CTL=%SCRIPT_DIR%svmbctl.exe"
set "SYS=%SCRIPT_DIR%svmb.sys"
set "TMP_OUT=%TEMP%\svmb_reg.out"

set "STORM_N=100000"
set "CYCLE_N=100"
if not "%~1"=="" set "STORM_N=%~1"
if not "%~2"=="" set "CYCLE_N=%~2"

set /a FAILED=0
set "FAILED_STEPS="
goto :main

:step_banner
echo.
echo ============================================================
echo  %~1
echo ============================================================
exit /b 0

:fail
echo [FAIL] %~1
set /a FAILED+=1
set "FAILED_STEPS=%FAILED_STEPS% [%~1]"
exit /b 0

:ctl
rem run svmbctl with all args, show output, propagate its exit code
"%CTL%" %* > "%TMP_OUT%" 2>&1
set "RC=%ERRORLEVEL%"
type "%TMP_OUT%"
if not "%RC%"=="0" exit /b 1
exit /b 0

:main
echo svmb regression flow: storm=%STORM_N% cycle=%CYCLE_N%
echo binaries from: %SCRIPT_DIR%

rem ---- 0. environment ------------------------------------------------------
fltmc >nul 2>&1
if errorlevel 1 (
    echo [FAIL] this script must run as Administrator
    exit /b 1
)
if not exist "%CTL%" (
    echo [FAIL] svmbctl.exe not found next to this script
    exit /b 1
)
if not exist "%SYS%" (
    echo [FAIL] svmb.sys not found next to this script
    exit /b 1
)

bcdedit /enum {current} > "%TMP_OUT%" 2>nul
findstr /I "testsigning" "%TMP_OUT%" | findstr /I "Yes" >nul
if errorlevel 1 (
    echo [WARN] testsigning appears OFF - sc start will likely fail with 577.
    echo        fix: bcdedit /set testsigning on  then reboot and re-run.
)

rem ---- 1. driver install ---------------------------------------------------
call :step_banner "1/10 driver install"
sc query svmb >nul 2>&1
if errorlevel 1 (
    sc create svmb type= kernel start= demand binPath= "%SYS%" >nul
    if errorlevel 1 (
        call :fail "sc create"
        goto :finalize
    )
    echo [ ok ] service created
) else (
    echo [ ok ] service already exists
)
sc query svmb | findstr /C:"RUNNING" >nul
if errorlevel 1 (
    sc start svmb >nul
    if errorlevel 1 (
        echo        hint: 577 = signature missing - run build\sign.cmd
        echo        hint: c000036 = same, driver not signed
        call :fail "sc start"
        goto :finalize
    )
)
echo [ ok ] driver service running

rem ---- 2. baseline info ----------------------------------------------------
call :step_banner "2/10 info (baseline, expect OFF)"
call :ctl info || (call :fail "info baseline" & goto :finalize)
findstr /B /C:"hv state" "%TMP_OUT%" | findstr /C:": OFF" >nul
if errorlevel 1 (
    echo        hv not OFF - normalizing with stop first
    "%CTL%" stop >nul 2>&1
    call :ctl info
    findstr /B /C:"hv state" "%TMP_OUT%" | findstr /C:": OFF" >nul
    if errorlevel 1 (
        call :fail "baseline not OFF"
        goto :finalize
    )
)
findstr /B /C:"magic" "%TMP_OUT%" | findstr /C:"(ok)" >nul
if errorlevel 1 call :fail "protocol magic mismatch"

rem ---- 3. start virtualization ---------------------------------------------
call :step_banner "3/10 start"
call :ctl start || (
    call :fail "start"
    echo        kernel-side reason is in the log - see tail below
    goto :finalize
)

rem ---- 4. info: running + SVM bit hidden (P0-1) ---------------------------
call :step_banner "4/10 info (expect RUNNING + svm bit hidden)"
call :ctl info || call :fail "info running"
findstr /B /C:"hv state" "%TMP_OUT%" | findstr /C:": RUNNING" >nul
if errorlevel 1 call :fail "hv not RUNNING after start"
findstr /C:"ok, hidden while running" "%TMP_OUT%" >nul
if errorlevel 1 call :fail "P0-1 svm cpuid bit NOT hidden (LEAK)"

rem ---- 5. exit storm -------------------------------------------------------
call :step_banner "5/10 storm %STORM_N%"
call :ctl storm %STORM_N% || call :fail "storm"

rem ---- 6. demo module end-to-end ------------------------------------------
call :step_banner "6/10 demo"
call :ctl demo || call :fail "demo exit code"
findstr /C:"is live" "%TMP_OUT%" >nul
if errorlevel 1 call :fail "demo signature not live"

rem ---- 7. attach module and keep it (P2-5 setup) --------------------------
call :step_banner "7/10 mod attach demo_cpuid (kept attached)"
call :ctl mod attach demo_cpuid || call :fail "mod attach"

rem ---- 8. enter/exit stress ------------------------------------------------
call :step_banner "8/10 cycle %CYCLE_N%"
call :ctl cycle %CYCLE_N% || call :fail "cycle"

rem ---- 9. module must have survived the cycle (P2-5) ----------------------
call :step_banner "9/10 mod list (demo_cpuid must survive cycle)"
call :ctl mod list || call :fail "mod list"
findstr /C:"demo_cpuid" "%TMP_OUT%" >nul
if errorlevel 1 call :fail "P2-5 demo_cpuid lost after cycle"

rem ---- 10. stop + final state ----------------------------------------------
call :step_banner "10/10 stop + final info (expect OFF)"
call :ctl stop || call :fail "stop"
call :ctl info || call :fail "info final"
findstr /B /C:"hv state" "%TMP_OUT%" | findstr /C:": OFF" >nul
if errorlevel 1 call :fail "hv not OFF after stop"

:finalize
echo.
call :step_banner "kernel log tail"
"%CTL%" log >nul 2>&1

echo.
echo ============================================================
if %FAILED%==0 (
    echo  RESULT: PASS - all steps green
    echo  now exercising clean driver unload ^(DevUnload^)...
    sc stop svmb >nul 2>&1
    if errorlevel 1 (
        echo  [WARN] sc stop failed - leave it installed, investigate log
    ) else (
        sc delete svmb >nul 2>&1
        echo  [ ok ] driver unloaded, service removed
    )
    endlocal & exit /b 0
)
echo  RESULT: FAIL -%FAILED_STEPS%
echo  hypervisor best-effort stop attempted, driver left installed
echo  for debugging: check 'svmbctl log', then 'sc stop svmb' + 'sc delete svmb'
endlocal & exit /b 1
