#!/bin/bash
# CPU an idle Home costs: the app started with NAME's data, left alone, its CPU time measured over SECS seconds once
# Home has settled (from second 15). Prints percent of one core.   tools/windows/test/idlecpu.sh NAME [SECS]
NAME=$1; SECS=${2:-20}
T=$(cd "$(dirname "$0")" && pwd); STAGE=$(cd "$T/../../../.." && pwd)/windows-run; EXE=$(cygpath -w "$STAGE/Origin.exe")
IDLE=${SB_IDLE_OFF:+}1 NOTOP=1 SB_POST="${SB_POST:-250:click 470 340}" TIMEOUT=$((SECS + 25)) bash "$T/../run.sh" "$NAME" >/dev/null 2>&1 &
sleep 15
cpu() { powershell -NoProfile -Command "(Get-Process Origin -EA SilentlyContinue | Where-Object { \$_.Path -eq '$EXE' } | Select-Object -First 1).TotalProcessorTime.TotalMilliseconds"; }
a=$(cpu | tr -d '\r'); sleep "$SECS"; b=$(cpu | tr -d '\r'); wait
python -c "print('idle Home: %.1f%% of one core over $SECS s' % ((float('$b') - float('$a')) / ($SECS * 10)))"
grep -E "^\[pace\] monitor" "$STAGE/$NAME.log" | head -1
