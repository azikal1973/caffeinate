#!/bin/sh
# Windows test, run with Git Bash (also on the GitHub Windows runner):
# options, messages, exit codes, command start, -w, -t. In an elevated shell
# also the power requests (powercfg /requests) and the -r lease in the
# registry, including a killed caffeinate and a handover between two.
C=${1:-./caffeinate-x86_64-windows.exe}
case $C in */*) ;; *) C=./$C ;; esac
C=$(cd "$(dirname "$C")" && pwd)/$(basename "$C")	# some tests run in another directory
USAGE='usage: caffeinate [-dirsu] [-t timeout] [-w Process ID] [command arguments...]'
# Git Bash would turn /c into C:/ in the arguments of Windows programs
export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'
fail=0
TMP=$(mktemp -d)
restore=:	# set once the elevated part has taken its snapshot
cleanup() {
	# stray caffeinate from an interrupted test, then the registry as it was
	for pid in $(jobs -p); do
		taskkill /F /PID "$(cat /proc/$pid/winpid 2>/dev/null)" >/dev/null 2>&1
	done
	wait 2>/dev/null	# none may still write while the values go back
	$restore
	rm -rf "$TMP"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

check() {	# description, expected, actual
	if [ "$2" = "$3" ]; then
		echo "ok   $1"
	else
		echo "FAIL $1: erwartet '$2', bekommen '$3'"
		fail=1
	fi
}

# drop the \r of Windows programs
crlf() { tr -d '\r'; }

out=$("$C" -h 2>&1 | crlf)
check "-h: Usage-Text" "$USAGE" "$out"
"$C" -h 2>/dev/null; check "-h: Exit 0" 0 "$?"

out=$("$C" -x 2>&1 | crlf | head -n 1)
check "-x: Meldung" "caffeinate: illegal option -- x" "$out"
"$C" -x 2>/dev/null; check "-x: Exit 1" 1 "$?"

out=$("$C" -t 2>&1 | crlf | head -n 1)
check "-t ohne Wert: Meldung" "caffeinate: option requires an argument -- t" "$out"
"$C" -t 2>/dev/null; check "-t ohne Wert: Exit 1" 1 "$?"

"$C" -t abc 2>/dev/null; check "-t abc: Exit 1" 1 "$?"
"$C" -w abc 2>/dev/null; check "-w abc: Exit 1" 1 "$?"
"$C" -t 0x cmd /c "exit 0" 2>/dev/null; check "-t 0x: wie auf macOS und Linux 0" 0 "$?"

"$C" cmd /c "exit 7" 2>/dev/null; check "command: Exit-Status durchgereicht" 7 "$?"
"$C" -i -t 5 cmd /c "exit 0" 2>/dev/null; check "command mit -t: Exit 0" 0 "$?"
"$C" nonexistent-command-4711 2>/dev/null; check "command fehlt: Exit 127" 127 "$?"

# the command line reaches the command as typed: quotes, double blanks, its own flags
out=$("$C" cmd /c echo "a  b" -t 2>/dev/null | crlf)
check "Befehlszeile bleibt unberuehrt" '"a  b" -t' "$out"

# a name without extension is found along PATH with PATHEXT, like npm.cmd
mkdir "$TMP/bin"
printf '@exit /b 5\r\n' > "$TMP/bin/caffeinate-test-tool.cmd"
PATH="$TMP/bin:$PATH" "$C" caffeinate-test-tool 2>/dev/null
check "command.cmd ueber PATHEXT gefunden" 5 "$?"

# the batch file that was found runs, not one of the same name in the current directory
printf '@echo %%~1\r\n@exit /b 6\r\n' > "$TMP/bin/caffeinate-test-echo.cmd"
mkdir "$TMP/cwd"
printf '@echo falsch\r\n@exit /b 9\r\n' > "$TMP/cwd/caffeinate-test-echo.bat"
printf '@exit /b 9\r\n' > "$TMP/cwd/caffeinate-test-cwd.bat"
out=$(cd "$TMP/cwd" && PATH="$TMP/bin:$PATH" "$C" caffeinate-test-echo "a b" 2>/dev/null | crlf)
check "Batchdatei aus PATH, nicht aus dem aktuellen Verzeichnis" "a b" "$out"
(cd "$TMP/cwd" && PATH="$TMP/bin:$PATH" "$C" caffeinate-test-echo x >/dev/null 2>&1)
check "Batchdatei: Exit-Status durchgereicht" 6 "$?"
(cd "$TMP/bin" && "$C" ./caffeinate-test-echo x >/dev/null 2>&1)
check "Batchdatei mit ./ davor" 6 "$?"
(cd "$TMP/cwd" && "$C" caffeinate-test-cwd 2>/dev/null)
check "nur im aktuellen Verzeichnis: nicht gefunden, Exit 127" 127 "$?"

# -t without command: ends after the timeout
start=$(date +%s)
"$C" -t 2 2>/dev/null; rc=$?
took=$(( $(date +%s) - start ))
check "-t 2: Exit 0" 0 "$rc"
check "-t 2: endet nach 2-3 s" 1 "$([ "$took" -ge 2 ] && [ "$took" -le 3 ] && echo 1)"

# -w: a process that does not exist ends caffeinate at once
start=$(date +%s)
"$C" -w 2147483644 -t 3 2>/dev/null; rc=$?
check "-w ohne Prozess: Exit 0" 0 "$rc"
check "-w ohne Prozess: sofort" 1 "$([ $(( $(date +%s) - start )) -le 1 ] && echo 1)"

# -w: Windows ignores the low two bits of a pid; 5 is no process, not System (4)
start=$(date +%s)
"$C" -w 5 -t 3 2>/dev/null; rc=$?
check "-w 5: Exit 0" 0 "$rc"
check "-w 5: sofort" 1 "$([ $(( $(date +%s) - start )) -le 1 ] && echo 1)"

# -w: ends with the process it watches
ping -n 3 127.0.0.1 >/dev/null &
winpid=$(cat /proc/$!/winpid)
start=$(date +%s)
"$C" -w "$winpid" 2>/dev/null; rc=$?
took=$(( $(date +%s) - start ))
check "-w: Exit 0" 0 "$rc"
check "-w: endet mit dem Prozess (1-3 s)" 1 "$([ "$took" -ge 1 ] && [ "$took" -le 3 ] && echo 1)"
wait

if ! net session >/dev/null 2>&1; then
	out=$("$C" -r -t 1 2>&1 | crlf | head -n 1)
	check "-r ohne Admin: Meldung" "caffeinate: Windows Update lässt sich nur mit Administratorrechten aufhalten (Terminal als Administrator starten)" "$out"
	"$C" -r -t 1 2>/dev/null; check "-r ohne Admin: Exit 1" 1 "$?"
	echo "skip Power Requests und -r: brauchen ein Terminal mit Administratorrechten"
	exit $fail
fi

# ---- elevated from here on

# the request is listed under the right type, with the reason, and gone afterwards
requests() { powercfg /requests | crlf; }
section() {	# type: the lines of that section of powercfg /requests
	requests | awk -v t="$1:" '$0 == t { on = 1; next } /^[A-Z]+:$/ { on = 0 } on'
}
"$C" -i -t 3 2>/dev/null &
sleep 1
check "-i: SYSTEM-Request mit Grund" 1 "$(section SYSTEM | grep -c 'caffeinate asserting for 3 secs')"
wait
check "-i: Request nach dem Ende weg" 0 "$(requests | grep -c 'caffeinate asserting')"

"$C" -d -t 3 2>/dev/null &
sleep 1
check "-d: DISPLAY-Request" 1 "$(section DISPLAY | grep -c 'caffeinate asserting for 3 secs')"
wait

# killed: Windows drops the request with the process
"$C" -t 30 2>/dev/null &
sleep 1
taskkill /F /PID "$(cat /proc/$!/winpid)" >/dev/null 2>&1
wait
sleep 1
check "kill: Request weg" 0 "$(requests | grep -c 'caffeinate asserting')"

# ---- -r: the lease in the registry
POL='HKLM\SOFTWARE\Policies\Microsoft\Windows\WindowsUpdate'
UX='HKLM\SOFTWARE\Microsoft\WindowsUpdate\UX\Settings'
STORE='HKLM\SOFTWARE\caffeinate\UpdateLease'
snapshot() {	# the values -r touches, one per line
	for v in SetActiveHours ActiveHoursStart ActiveHoursEnd; do
		echo "$v=$(reg query "$POL" /v $v 2>/dev/null | crlf | awk -v v=$v '$1 == v { print $3 }')"
	done
	for v in PauseUpdatesStartTime PauseUpdatesExpiryTime PauseFeatureUpdatesStartTime \
	         PauseFeatureUpdatesEndTime PauseQualityUpdatesStartTime PauseQualityUpdatesEndTime; do
		echo "$v=$(reg query "$UX" /v $v 2>/dev/null | crlf | awk -v v=$v '$1 == v { print $3 }')"
	done
}
value() { snapshot | sed -n "s/^$1=//p"; }
has_store() { reg query "$STORE" >/dev/null 2>&1 && echo 1 || echo 0; }
put_back() {	# every value as in $before
	echo "$before" | while IFS== read -r v d; do
		case $v in Set*|Active*) key=$POL t=REG_DWORD ;; *) key=$UX t=REG_SZ ;; esac
		if [ -n "$d" ]; then
			reg add "$key" /v "$v" /t $t /d "$d" /f >/dev/null
		else
			reg delete "$key" /v "$v" /f >/dev/null 2>&1
		fi
	done
	reg delete 'HKLM\SOFTWARE\caffeinate' /f >/dev/null 2>&1
}

# a record left by an earlier run that was torn down: give its values back
# first, or they would pass for the originals here
if [ "$(has_store)" = 1 ]; then
	echo "Sicherung eines frueheren Laufs gefunden, wird zurueckgesetzt"
	"$C" -r -t 1 2>/dev/null
fi
before=$(snapshot)
restore=put_back
"$C" -r -t 3 2>/dev/null &
sleep 1
check "-r: SetActiveHours gesetzt" 0x1 "$(value SetActiveHours)"
hour=$(date +%H | sed 's/^0//')
check "-r: Nutzungszeit beginnt eine Stunde vor jetzt" "0x$(printf %x $(( (hour + 23) % 24 )))" "$(value ActiveHoursStart)"
check "-r: Pause bis morgen" "$(date -u -d tomorrow +%Y-%m-%d)" "$(value PauseUpdatesExpiryTime | cut -c1-10)"
check "-r: Sicherung angelegt" 1 "$(has_store)"
wait
check "-r: alte Werte wieder da" "$before" "$(snapshot)"
check "-r: Sicherung geloescht" 0 "$(has_store)"

# killed: the next caffeinate -r puts the old values back
"$C" -r -t 30 2>/dev/null &
sleep 1
taskkill /F /PID "$(cat /proc/$!/winpid)" >/dev/null 2>&1
wait
check "-r kill: Sicherung bleibt" 1 "$(has_store)"
"$C" -r -t 1 2>/dev/null
check "-r kill: naechstes -r setzt zurueck" "$before" "$(snapshot)"
check "-r kill: Sicherung danach geloescht" 0 "$(has_store)"

# two at once: the second takes over when the first ends
"$C" -r -t 2 2>/dev/null &
first=$!
sleep 1
"$C" -r -t 4 2>/dev/null &
second=$!
wait $first
sleep 1
# the first one has dropped its record; only a second that took over has one
check "-r Uebergabe: der zweite haelt die Sperre" 1 "$(has_store)"
check "-r Uebergabe: Nutzungszeit gesetzt" 0x1 "$(value SetActiveHours)"
wait $second
check "-r Uebergabe: danach alles zurueck" "$before" "$(snapshot)"

# the owner killed while a second one waits: the second takes over
"$C" -r -t 30 2>/dev/null &
first=$!
sleep 1
"$C" -r -t 4 2>/dev/null &
second=$!
sleep 1
first_start=$(value PauseUpdatesStartTime)
taskkill /F /PID "$(cat /proc/$first/winpid)" >/dev/null 2>&1
wait $first
sleep 1
# the second one renews on taking over: a later start of the pause
check "-r Uebernahme nach kill: der zweite hat erneuert" 1 \
	"$([ -n "$(value PauseUpdatesStartTime)" ] && [ "$(value PauseUpdatesStartTime)" != "$first_start" ] && echo 1)"
wait $second
check "-r Uebernahme nach kill: danach alles zurueck" "$before" "$(snapshot)"

# active hours changed during the run: the whole group stays as the user left it
"$C" -r -t 3 2>/dev/null &
sleep 1
reg add "$POL" /v ActiveHoursStart /t REG_DWORD /d 8 /f >/dev/null
wait
check "-r: geaenderte Nutzungszeit bleibt" 0x8 "$(value ActiveHoursStart)"
check "-r: mit ihrer Gruppe" 0x1 "$(value SetActiveHours)"
check "-r: Sicherung trotzdem geloescht" 0 "$(has_store)"
put_back

# a longer pause of the user's own stays
reg add "$UX" /v PauseUpdatesExpiryTime /t REG_SZ /d 2099-01-01T00:00:00Z /f >/dev/null
"$C" -r -t 2 2>/dev/null &
sleep 1
check "-r: laengere Pause des Nutzers bleibt waehrend des Laufs" 2099-01-01T00:00:00Z "$(value PauseUpdatesExpiryTime)"
check "-r: ihr Beginn auch" "$(echo "$before" | sed -n 's/^PauseUpdatesStartTime=//p')" "$(value PauseUpdatesStartTime)"
wait
check "-r: laengere Pause des Nutzers bleibt danach" 2099-01-01T00:00:00Z "$(value PauseUpdatesExpiryTime)"
put_back
check "-r: Testwerte aufgeraeumt" "$before" "$(snapshot)"

exit $fail
