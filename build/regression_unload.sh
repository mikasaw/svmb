#!/bin/bash
# r36 unload-stress regression: N cycles of start -> hooks -> storm -> stop.
# Drives the existing phase bats only (AGENTS rule 1). Any bat failure or
# hang aborts with the cycle number - the steps log holds the details.
# Usage: regression_unload.sh [cycles]   (default 3)
set -u
N=${1:-3}

run() { # run <bat> [arg]
    local out
    out=$(cmd.exe //c "build\\$1" ${2:-} 2>&1)
    local rc=$?
    if [ $rc -ne 0 ]; then
        echo "CYCLE ${i:-?}: $1 $2 FAILED rc=$rc"
        echo "$out" | tail -5
        exit 1
    fi
}

for ((i = 1; i <= N; ++i)); do
    echo "=== unload cycle $i/$N"
    run vm_svc_start.bat
    run vm_ctl_storm.bat 200
    run vm_ctl_nthook.bat 2
    run vm_ctl_nthookov.bat 2
    run vm_ctl_probe.bat 4
    run vm_svc_stop.bat
done

run vm_pull_steps_log.bat
echo "=== $N unload cycles done; last steps-log lines:"
tail -6 "${HOST_HOME:-C:/Users/<user>}/svmb_steps_out.txt"
