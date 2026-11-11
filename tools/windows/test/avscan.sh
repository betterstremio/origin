#!/bin/bash
# Windows Defender's verdict on files, without it removing them (-DisableRemediation): an unsigned exe can be taken
# for malware by its machine-learning rules (2026-11-11: Origin.exe as "Trojan:Win32/Bearfoos.A!ml").
#   tools/windows/test/avscan.sh FILE...   (Git Bash)
MP="$(ls -d "/c/ProgramData/Microsoft/Windows Defender/Platform/"*/ 2>/dev/null | sort | tail -1)MpCmdRun.exe"
[ -f "$MP" ] || MP="/c/Program Files/Windows Defender/MpCmdRun.exe"
rc=0
for f in "$@"; do
  out=$(MSYS_NO_PATHCONV=1 "$MP" -Scan -ScanType 3 -File "$(cygpath -w "$f")" -DisableRemediation 2>&1 | tr -d '\r')
  if [ ! -f "$f" ]; then rc=2; echo "MISSING  $f"; elif echo "$out" | grep -q "found no threats"; then echo "clean    $f"; elif ! echo "$out" | grep -q "found [0-9]* threat"; then rc=2; echo "NO SCAN  $f: $(echo "$out" | tail -2 | tr "
" " ")"; else rc=1; echo "FLAGGED  $f: $(echo "$out" | grep -iE "threat|name" | head -2 | tr '\n' ' ')"; fi
done
exit $rc
