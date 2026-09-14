#!/bin/bash
# r58 host-side melt watcher: probe every 20s, snapshot the newest vmware.log
# Tools/heartbeat lines on state change; append everything to a timeline.
# usage: melt_host_watch.sh <minutes> [config-label]
MIN=${1:-45}; LABEL=${2:-default}
TL=${WINDBG_TEST:-/c/Users/<user>/AiCode/windbg-test}/logs/melt_host_timeline.txt
VLOG="${VM_DIR:-.}/vmware.log"
echo "=== host watch start $(date -u +%H:%M:%S)Z label=$LABEL ===" >> "$TL"
T0=$(date +%s); END=$((T0 + MIN*60)); LASTHB=""
while [ $(date +%s) -lt $END ]; do
  sleep 20
  timeout 25 cmd.exe //c "build\vm_ctl_stats_only.bat" >/dev/null 2>&1
  RC=$?; EL=$(( $(date +%s) - T0 ))
  NEWHB=$(grep -a "heartbeat timeout\|Changing running status\|CPU reset" "$VLOG" 2>/dev/null | tail -2 | cut -c1-60 | tr '\n' ' | ')
  ST="alive"
  [ $RC -ne 0 ] && [ $RC -ne 1 ] && ST="WEDGED"
  echo "+${EL}s rc=$RC $ST hb=[${NEWHB}]" >> "$TL"
  if [ "$ST" = "WEDGED" ]; then echo "=== WEDGE detected ===" >> "$TL"; break; fi
done
echo "=== host watch end $(date -u +%H:%M:%S)Z ===" >> "$TL"
