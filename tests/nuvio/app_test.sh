#!/bin/bash
# tests/nuvio/app_test.sh BIN ROOT OUT: the whole app against fake_nuvio.py, driven with the remote's keys. Software rendering, about
# 45 frames a second; the frame numbers leave room for a slow machine.
BIN="$1"; ROOT="$2"; O="$3"; PORT=18765; H="$O/home"; D="$H/Library/Application Support/StremioOrigin"; LOG="$O/server.log"; FAILS=0
mkdir -p "$O"; rm -rf "$H" "$O"/*.log "$O"/*.png; mkdir -p "$D"; : > "$LOG"
seed() { echo '{"guideSeen":"1","whatsNewSeen":"9.9.9","introSound":"false"}' > "$D/state.json"; }
seed
python3 -I "$ROOT/tests/nuvio/fake_nuvio.py" $PORT "$LOG" & SRV=$!; trap 'kill $SRV 2>/dev/null' EXIT; sleep 0.6
ctl() { python3 -I -c "import urllib.request,sys; urllib.request.urlopen('http://127.0.0.1:$PORT/ctl?'+sys.argv[1]).read()" "$1"; }
run() { # name script exit-frame
  : > "$LOG.part"; local n0=$(wc -l < "$LOG")
  ( cd "$ROOT" && env HOME="$H" SB_ASSETS=assets SB_NV_URL=http://127.0.0.1:$PORT SB_NV_URL2=http://127.0.0.1:$PORT SB_NV_KEY=k SB_SCRIPT="$2" SB_SHOT="$O/$1.png" SB_SHOT_FRAME="$3" SB_SHOT_EXIT=1 SB_ALARM=240 LIBGL_ALWAYS_SOFTWARE=1 ASAN_OPTIONS=detect_leaks=0:abort_on_error=0 \
    xvfb-run -a -s "-screen 0 1920x1080x24" "$BIN" > "$O/$1.log" 2>&1; rc=$?; [ $rc = 0 ] || { echo "FAIL $1: exit $rc"; } )
  tail -n +$((n0 + 1)) "$LOG" > "$LOG.part"
  grep -E 'AddressSanitizer|SUMMARY' "$O/$1.log" | cut -c1-200 | head -5
  if grep -q AddressSanitizer "$O/$1.log"; then echo "FAIL $1: sanitizer"; grep -E '^    #[0-9] ' "$O/$1.log" | head -14 | cut -c1-150; FAILS=$((FAILS + 1)); fi
}
# check "what" python-expression over s (state.json), acct (files in acct/), log (rows the server saw in this run)
check() { python3 -I - "$D" "$LOG.part" "$1" "$2" <<'PY' || FAILS=$((FAILS + 1))
import json,sys,os
d,lp,what,expr=sys.argv[1:5]
s=json.load(open(d+'/state.json')); acct=os.listdir(d+'/acct') if os.path.isdir(d+'/acct') else []
log=[json.loads(l) for l in open(lp) if l.strip()]; k=[r['k'] for r in log]
lib=s.get('lib') or {}; shown=sorted(i for i,v in lib.items() if not v.get('removed')); nadd=len(s.get('addons') or [])
ok=eval(expr)
print(('ok   ' if ok else 'FAIL ')+what+('' if ok else '   [profile %s, acct %s, shown %s, addons %d, files %s, server %s]'%(s.get('nvProfile'),s.get('acctKind'),shown,nadd,acct,sorted(set(k)))))
sys.exit(0 if ok else 1)
PY
}
# 1. a Stremio user: the sign-in as it always was; Nuvio's server is not called
run stremio "" 300; check "Stremio sign-in never calls Nuvio" "len(log)==0 and not s.get('acctKind')"
# 2. Nuvio tab, code approved by the phone, profiles, Right + OK = Kids (their addons and library come down)
run signin "100:up,130:right,620:right,660:ok" 1300
check "signed in as Kids" "s.get('nvProfile')=='2' and s.get('acctKind')=='nuvio'"
check "Kids' library and watched came down" "'tt0317219' in shown and lib.get('tt0114709',{}).get('state',{}).get('timesWatched')==1"
check "Kids' addons" "nadd==1"
check "nothing pushed back from a fresh sign-in" "not any(x.startswith('sync_push') or x.startswith('sync_delete') for x in k)"
# 3. start again: the picker over Home; Left + OK = Sam, new on this device: the loading screen
ctl "dead=1"
run sam1 "340:left,380:ok" 1200
check "Sam chosen" "s.get('nvProfile')=='1'"
check "Sam's library, none of Kids'" "'tt0111161' in shown and 'tt0317219' not in shown"
check "Sam's progress came down" "lib.get('tt0468569',{}).get('state',{}).get('timeOffset')==600000"
check "a dead manifest with no copy: the other addons stay" "nadd==2"
check "Kids' data set aside" "any(f.endswith('-p2.json') for f in acct)"
ctl "dead=0"
# 4. a manifest that dies after we have its copy keeps its addon (6 Nov fault)
python3 -I - "$D" <<'PY'
import json,sys
p=sys.argv[1]+'/state.json'; s=json.load(open(p))
s['addons'].append({"transportUrl":"http://127.0.0.1:18765/dead/manifest.json","manifest":{"id":"fake.dead","name":"Fake dead","version":"1.0.0","resources":["stream"],"types":["movie"],"catalogs":[]},"flags":{},"nvAt":0})
json.dump(s,open(p,'w'))
PY
ctl "dead=1"
run deadkeep "340:ok" 500
check "a manifest that fails keeps its addon" "nadd==3"
check "young manifests are not asked for again" "sum(1 for r in log if r['k']=='manifest' and r['b']!='dead')==0"
ctl "dead=0"
# 5. switching back to a kept profile downloads nothing before Home; the sync follows
run kids2 "340:right,380:ok" 800
check "back on Kids from the kept copy" "s.get('nvProfile')=='2' and 'tt0317219' in shown and nadd==1"
# 6. the PIN: Locked (Right), wrong then right
PIN="340:right,370:ok,420:ok,450:down,480:down,510:down,540:right,570:right,600:ok,  660:left,690:left,720:up,750:up,780:up,810:ok,840:right,870:ok,900:right,930:ok,960:down,990:left,1020:left,1050:ok,1080:down,1110:down,1140:right,1170:right,1200:ok"
run pin "${PIN// /}" 1700
check "a wrong PIN then the right one" "s.get('nvProfile')=='3' and [r['b'] for r in log if r['k']=='pin']==['1','1234']"
check "uses the primary's addons" "any(r['k']=='addons' and r['b']==1 for r in log)"
# the question switched off: a locked last profile still asks; an open one goes straight in
python3 -I -c "import json,sys; p=sys.argv[1]+'/state.json'; s=json.load(open(p)); s['askProfile']='false'; json.dump(s,open(p,'w'))" "$D"
run asklocked "" 300; if grep -q '\[profiles\] open, mode 0' "$O/asklocked.log"; then echo "ok   a locked profile asks even with the question off"; else echo "FAIL locked profile opened without asking"; FAILS=$((FAILS + 1)); fi
python3 -I -c "import json,sys; p=sys.argv[1]+'/state.json'; s=json.load(open(p)); s['askProfile']='true'; json.dump(s,open(p,'w'))" "$D"
# 7. the session: 401 once, refreshed; 500 once, the second address
ctl "expire=1"; run refresh "340:left,370:left,400:ok" 500; check "401: refreshed and carried on" "'refresh' in k and 'pull_lib' in k"
ctl "fail=1"; run fallback "340:ok" 500; check "500: the second address answers" "'pull_wat' in k"
# 8. the server ends the session: back to sign-in, signed out, nothing left behind
ctl "expire=1"; ctl "refuse=1"; run lost "340:ok" 500; ctl "refuse=0"
check "session refused: signed out and forgotten" "not s.get('acctKind') and not s.get('nvRefresh') and not [f for f in acct if f.startswith('nv-')] and not lib"
# 9. the first sign-in with one profile still asks; Add profile: one push, the whole list, every field kept
fresh() { rm -rf "$D"; mkdir -p "$D"; seed; }
PUSHED="[r['b'] for r in log if r['k']=='sync_push_profiles']"
# the name "Ab" on the QWERTY keyboard (Shift starts on): down twice to A, OK; down to Z, right 4 to B, OK; down to the last row, right twice to Done, OK
TYPE="750:down,780:down,810:ok,840:down,870:right,900:right,930:right,960:right,990:ok,1020:down,1050:right,1080:right,1110:ok"
fresh; ctl "list=1"; run oneprof "100:up,130:right" 800; if grep -q '\[profiles\] open, mode 1' "$O/oneprof.log"; then echo "ok   one profile: the picker still shows on the first sign-in"; else echo "FAIL one profile went straight in"; FAILS=$((FAILS + 1)); fi
fresh; ctl "list=3"; run addp "100:up,130:right,620:right,650:right,680:right,710:ok,$TYPE" 1500
check "add: pushed once, the whole list plus the new one" "(lambda P: len(P)==1 and [x['profile_index'] for x in P[0]['p_profiles']]==[1,2,3,4] and P[0]['p_client_max_profiles']==6)($PUSHED)"
check "add: the rows that were there keep every field" "(lambda p: p['avatar_id']=='av1' and p['profile_background_id']=='bg1' and p['profile_background_url']=='https://example.test/b.png' and p['uses_primary_plugins'] is True and p['name']=='Sam')($PUSHED[0]['p_profiles'][0])"
check "add: the new profile shares the primary's addons" "(lambda p: p['name']=='Ab' and p['uses_primary_addons'] is True and p['uses_primary_plugins'] is False)($PUSHED[0]['p_profiles'][3])"
check "add: the picker holds the new profile" "len(s.get('nvProfiles') or [])==4"
fresh; ctl "list=0"; run adde "100:up,130:right,620:right,650:ok,$TYPE" 1500
check "add on an account with no saved profile: its first profile goes too" "(lambda P: len(P)==1 and [x['profile_index'] for x in P[0]['p_profiles']]==[1,2])($PUSHED)"
ctl "list=3"
# 10. two services on one TV: each keeps its own data, and a half-finished add comes back
seed2() { rm -rf "$D"; mkdir -p "$D/acct"; python3 -I - "$D" <<'PY'
import json,sys
d=sys.argv[1]; item=lambda i:{"_id":i,"name":i,"type":"movie","removed":False,"state":{}}
json.dump({"guideSeen":"1","whatsNewSeen":"9.9.9","authKey":"fakeauth","user":{"_id":"u1","email":"s@example.test"},"addons":[],"lib":{"ttS":item("ttS")},"nvAccess":"user-token","nvRefresh":"r1","nvExp":"9999999999","nvUser":{"email":"n@example.test"},"nvProfiles":[{"i":1,"name":"Sam","color":1,"pin":False,"pa":False}],"nvProfile":"1","nvOwner":"owner-123"},open(d+'/state.json','w'))
json.dump({"lib":{"ttN":item("ttN")},"addons":[]},open(d+'/acct/nv-owner-123-p1.json','w'))
PY
}
SF="json.load(open(d+'/acct/stremio.json'))"; NF="json.load(open(d+'/acct/nv-owner-123-p1.json'))"
seed2; SB_ACCT_ACTION=switch run sw1 "" 300
check "to Nuvio: Stremio's key and library are set aside" "s.get('acctKind')=='nuvio' and not s.get('authKey') and 'ttS' not in lib and $SF['authKey']=='fakeauth' and 'ttS' in $SF['lib']"
check "to Nuvio: its data came back and it synced" "'pull_lib' in k"
SB_ACCT_ACTION=switch run sw2 "" 300
check "back to Stremio: its key and library, none of Nuvio's" "not s.get('acctKind') and s.get('authKey')=='fakeauth' and s['user']['_id']=='u1' and 'ttS' in lib and 'tt0111161' not in lib"
check "back to Stremio: Nuvio stays signed in, its data set aside" "s.get('nvRefresh')=='r1' and s.get('nvProfile')=='1' and 'tt0111161' in $NF['lib']"
check "back to Stremio: Nuvio's server is left alone" "not any(x.startswith('pull') for x in k)"
SB_ACCT_ACTION=switch run sw3 "" 300; SB_ACCT_ACTION=logout run sw4 "" 300
check "log out of Nuvio: Stremio carries on" "not s.get('acctKind') and s.get('authKey')=='fakeauth' and 'ttS' in lib and not s.get('nvRefresh')"
SB_ACCT_ACTION=logout run sw5 "" 300
check "log out of Stremio too: nothing left" "not s.get('authKey') and not lib and not any(f.startswith('stremio') for f in acct)"
seed2; python3 -I -c "import json,sys; p=sys.argv[1]+'/state.json'; s=json.load(open(p)); s['acctKind']='nuvio'; a=s.pop('authKey'); u=s.pop('user'); l=s.pop('lib'); json.dump(s,open(p,'w')); json.dump({'authKey':a,'user':u,'lib':l,'addons':[]},open(sys.argv[1]+'/acct/stremio.json','w'))" "$D"
SB_ACCT_ACTION=logout run sw6 "" 300; SB_ACCT_ACTION=logout run sw7 "" 300
check "a kept Stremio account logged out from the other side goes" "not s.get('authKey') and not any(f.startswith('stremio') for f in acct)"
seed2; python3 -I -c "import json,sys; p=sys.argv[1]+'/state.json'; s=json.load(open(p)); s['nvRefresh']=None; del s['nvRefresh']; del s['nvProfile']; json.dump(s,open(p,'w'))" "$D"
SB_ACCT_ACTION=add run add1 "" 200
check "add: the Stremio account is set aside" "not s.get('authKey') and 'ttS' in $SF['lib']"
run add2 "" 200
check "add left half way: back on Stremio at the next start" "s.get('authKey')=='fakeauth' and 'ttS' in lib and not s.get('acctKind')"
echo "fails: $FAILS"; [ $FAILS = 0 ] && echo "APP TEST PASSED"
