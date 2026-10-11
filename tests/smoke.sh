#!/bin/sh
# Smoke-Test ohne D-Bus (laeuft auch in CI-Containern):
# Optionen, Meldungen und Exit-Codes wie bei Apples caffeinate.
C=${1:-./caffeinate}
# auch auf einem Desktop keine echten Sperren anlegen
export DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent DBUS_SYSTEM_BUS_ADDRESS=unix:path=/nonexistent
export XDG_RUNTIME_DIR=/nonexistent
unset DISPLAY WAYLAND_DISPLAY
USAGE='usage: caffeinate [-disu] [-t timeout] [-w Process ID] [command arguments...]'
fail=0

check() {	# Beschreibung, erwartet, tatsaechlich
	if [ "$2" = "$3" ]; then
		echo "ok   $1"
	else
		echo "FAIL $1: erwartet '$2', bekommen '$3'"
		fail=1
	fi
}

out=$("$C" -h 2>&1); rc=$?
check "-h: Exit 0" 0 "$rc"
check "-h: Usage-Text" "$USAGE" "$out"

out=$("$C" -x 2>&1); rc=$?
check "-x: Exit 1" 1 "$rc"
check "-x: Meldung" "caffeinate: illegal option -- x" "$(echo "$out" | head -n 1)"

out=$("$C" -t 2>&1); rc=$?
check "-t ohne Wert: Exit 1" 1 "$rc"
check "-t ohne Wert: Meldung" "caffeinate: option requires an argument -- t" "$(echo "$out" | head -n 1)"

"$C" -t abc 2>/dev/null; check "-t abc: Exit 1" 1 "$?"
"$C" -w abc 2>/dev/null; check "-w abc: Exit 1" 1 "$?"

"$C" sh -c 'exit 7' 2>/dev/null; check "command: Exit-Status durchgereicht" 7 "$?"
"$C" -i -t 5 sh -c 'exit 0' 2>/dev/null; check "command mit -t: Exit 0" 0 "$?"
"$C" /nonexistent/cmd 2>/dev/null; check "command fehlt: Exit 127" 127 "$?"
"$C" sh -c 'kill -TERM $$' 2>/dev/null; check "command stirbt an SIGTERM: 143" 143 "$?"

# wie Apple: der Elternprozess wird per exec zum command, $! ist dessen PID
pids=$(C="$C" sh -c '"$C" sh -c "echo \$\$" 2>/dev/null & echo $!; wait' | sort -u | wc -l)
check "command behaelt die PID von caffeinate" 1 "$pids"

out=$("$C" ls -d / 2>/dev/null)
check "Flags des command bleiben unberuehrt" "/" "$out"

exit $fail
