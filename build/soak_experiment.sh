#!/bin/bash
# r58 melt bisection: one boot + probe loop per experiment configuration.
# usage: soak_experiment.sh <TlbMode> <GuestAsid> <NptEnable> <minutes>
#   rc timeline: 0=alive(driver ok) 1=alive(driver ioctl fail - expected
#   when NptEnable=0) 124/other=wedged/dead
TLB=${1:-0}; ASID=${2:-1}; NPT=${3:-1}; MIN=${4:-10}
cd "$(dirname "$0")/.." || exit 1
run(){ local b="build\\$1"; shift; [ $# -gt 0 ] && b="$b $*"; \
  timeout 30 cmd.exe //c "$b" >/dev/null 2>&1; RC=$?; }
echo "=== soak: TlbMode=$TLB Asid=$ASID NptEnable=$NPT for ${MIN}min $(date -u +%H:%M:%S)Z ==="
run vm_reset_hard.bat; [ $RC -ne 0 ] && { echo RESET_FAIL; exit 1; }
Q=0
for i in 1 2 3 4 5 6; do sleep 8; run vm_quarantine_now.bat; \
  [ $RC -eq 0 ] && { echo "QUARANTINED $i"; Q=1; break; }; done
[ $Q -eq 0 ] && echo "RACE_LOST (continuing anyway)"
run vm_unquarantine_now.bat
run vm_push_driver.bat; run vm_push_ctl.bat
run vm_svc_create.bat
run vm_reg_param.bat TlbMode $TLB
run vm_reg_param.bat GuestAsid $ASID
run vm_reg_param.bat NptEnable $NPT
START_T=90
timeout $START_T cmd.exe //c "build\\vm_svc_start.bat" >/dev/null 2>&1
RC=$?
[ $RC -ne 0 ] && { echo "START_FAIL rc=$RC"; exit 1; }
echo "STARTED $(date -u +%H:%M:%S)Z"
T0=$(date +%s); END=$((T0 + MIN*60)); DEAD=0
while [ $(date +%s) -lt $END ]; do
  sleep 15
  timeout 25 cmd.exe //c "build\\vm_ctl_stats_only.bat" >/dev/null 2>&1
  RC=$?; EL=$(( $(date +%s) - T0 ))
  if [ $RC -ne 0 ] && [ $RC -ne 1 ]; then
    echo "WEDGED at +${EL}s rc=$RC"; DEAD=1; break
  fi
  echo "alive +${EL}s"
done
[ $DEAD -eq 0 ] && echo "SURVIVED ${MIN}min"
