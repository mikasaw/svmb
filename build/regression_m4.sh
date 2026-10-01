#!/bin/bash
# r41 M4-G regression: debugger + cr3 faces through the phase bats.
# Expects: driver deployed, kd closed (r40 law: no kd breaks while
# virtualizing). Runs N cycles of
#   start -> attach -> dbg/cr3 faces -> storm -> stop
# Platform notes (r39/r41): dbgdrtest FAILs on this vhv BY DESIGN
# (DR-access intercept bits do not pass to the nested VMCB) - it is
# run for the record but does not gate the cycle. First mod attach
# has a transient failure class - retried.
# Usage: regression_m4.sh [cycles]   (default 2)
set -u
N=${1:-2}

run() { # run <bat> [arg] - returns 1 on failure (does not exit)
    local out
    out=$(cmd.exe //c "build\\$1" ${2:-} 2>&1)
    local rc=$?
    if [ $rc -ne 0 ]; then
        echo "CYCLE ${i:-?}: $1 ${2:-} FAILED rc=$rc"
        echo "$out" | tail -4
        return 1
    fi
    return 0
}

run_retry() { # run_retry <bat> [arg] [attempts]
    local t
    for ((t = 1; t <= ${3:-3}; ++t)); do
        run "$@" && return 0
        sleep 2
    done
    echo "CYCLE ${i:-?}: $1 exhausted retries"
    exit 1
}

# initial cleanup: tolerate already-stopped
cmd.exe //c "build\vm_svc_stop.bat" >/dev/null 2>&1 || true
sleep 2

for ((i = 1; i <= N; ++i)); do
    echo "=== m4 cycle $i/$N"
    run vm_svc_start.bat || exit 1
    sleep 3
    run_retry vm_ctl_modfresh.bat 3 || exit 1  # zero exc arming
    run vm_ctl_dbgtest2.bat || exit 1          # [=] native (opt-in off)
    run vm_ctl_dbgdr.bat || true               # record-only (vhv law)
    run vm_ctl_viewprobe.bat || exit 1
    run vm_ctl_dbgevents_fresh.bat || exit 1
    run vm_ctl_cr3spoof.bat 0xDEADC0DE || exit 1
    run vm_ctl_storm.bat 200 || exit 1
    run vm_svc_stop.bat || exit 1
done

run vm_pull_steps_log.bat
echo "=== $N m4 cycles done"
exit 0
