#!/bin/sh
# X11 test: Xvfb with cookie authentication, no D-Bus. Checks that -d
# suspends the screen saver of the X server for as long as caffeinate runs,
# and that -u ends a running one. tests/x11check.py sets the timeout for
# this and queries the state.
#
# Needs Xvfb and python3; if either is missing, the test is skipped.
C=${1:-./caffeinate}
case $C in /*) ;; *) C=$PWD/$C ;; esac
HERE=$(cd "$(dirname "$0")" && pwd)

if ! command -v Xvfb >/dev/null 2>&1 || ! command -v python3 >/dev/null 2>&1; then
	echo "skip X11-Test: Xvfb oder python3 fehlt"
	exit 0
fi

T=$(mktemp -d /tmp/caffeinate-x11.XXXXXX) || exit 1
fail=0
pids=

cleanup() {
	# shellcheck disable=SC2086
	[ -n "$pids" ] && kill $pids 2>/dev/null
	rm -rf "$T"
}
trap cleanup EXIT
trap 'exit 1' INT TERM

check() {	# description, expected, actual
	if [ "$2" = "$3" ]; then
		echo "ok   $1"
	else
		echo "FAIL $1: erwartet '$2', bekommen '$3'"
		fail=1
	fi
}

python3 "$HERE/x11check.py" "$T/xauth" mkauth
# -noreset: otherwise the server resets once the last client leaves, and forgets the timeout
Xvfb -displayfd 3 -auth "$T/xauth" -noreset -nolisten tcp -screen 0 640x480x8 3> "$T/display" 2>/dev/null &
xvfb=$!
pids=$xvfb
i=0
while [ ! -s "$T/display" ]; do
	i=$((i + 1))
	[ "$i" -gt 100 ] && { echo "FAIL Xvfb startet nicht"; exit 1; }
	sleep 0.1
done
num=$(cat "$T/display")

x() {	# command [value]
	python3 "$HERE/x11check.py" "$T/xauth" "$num" "$@"
}

export DISPLAY=":$num" XAUTHORITY="$T/xauth" XDG_RUNTIME_DIR="$T"
export DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent DBUS_SYSTEM_BUS_ADDRESS=unix:path=/nonexistent
unset WAYLAND_DISPLAY CAFFEINATE_DEBUG

# cross-check: without caffeinate the screen saver kicks in after 1 s
x timeout 1
sleep 2.5
check "ohne caffeinate: Bildschirmschoner nach Timeout an" on "$(x state)"
x reset

"$C" -d -t 4 &
cpid=$!
pids="$pids $cpid"
sleep 2.5
check "-d: Bildschirmschoner bleibt aus" off "$(x state)"
wait "$cpid"
check "-d: Exit 0, auch ganz ohne D-Bus" 0 "$?"
sleep 2.5
check "-d: nach dem Ende laeuft der Timeout wieder" on "$(x state)"
x reset

"$C" -d sleep 4 &
cpid=$!
pids="$pids $cpid"
sleep 2.5
check "-d mit command: Bildschirmschoner bleibt aus" off "$(x state)"
wait "$cpid"
sleep 2.5
check "-d mit command: danach laeuft der Timeout wieder" on "$(x state)"

x timeout 30
x activate
check "vor -u: Bildschirmschoner ist an" on "$(x state)"
"$C" -u -t 1
check "-u: Exit 0" 0 "$?"
check "-u: Bildschirmschoner beendet" off "$(x state)"

out=$(XAUTHORITY=/dev/null "$C" -d -t 1 2>&1)
check "ohne Cookie: Exit 1" 1 "$?"
check "ohne Cookie: Meldung" "Failed to create PreventUserIdleDisplaySleep assertion" "$(echo "$out" | tail -n 1)"

WAYLAND_DISPLAY=wayland-0 "$C" -d -t 1 2>/dev/null
check "unter Wayland wird der X-Server nicht benutzt: Exit 1" 1 "$?"

"$C" -i -t 1 2>/dev/null
check "-i benutzt den X-Server nicht: Exit 1" 1 "$?"

# the X server exits: caffeinate reports the lost lock and keeps running
"$C" -d 2> "$T/err" &
cpid=$!
pids="$pids $cpid"
sleep 1
kill "$xvfb"
wait "$xvfb" 2>/dev/null
i=0
while [ ! -s "$T/err" ] && [ "$i" -lt 30 ]; do
	i=$((i + 1))
	sleep 0.1
done
check "X-Server endet: Meldung" "caffeinate: PreventUserIdleDisplaySleep: Sperre verloren: X-Server beendet" "$(cat "$T/err")"
kill -0 "$cpid" 2>/dev/null
check "X-Server endet: caffeinate laeuft weiter" 0 "$?"
kill "$cpid" 2>/dev/null
wait "$cpid" 2>/dev/null

exit $fail
