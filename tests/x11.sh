#!/bin/sh
# X11-Test: Xvfb mit Cookie-Anmeldung, kein D-Bus. Prueft, dass -d den
# Bildschirmschoner des X-Servers aussetzt, solange caffeinate laeuft, und
# dass -u ihn beendet. tests/x11check.py stellt dazu den Timeout ein und
# fragt den Zustand ab.
#
# Braucht Xvfb und python3; fehlt etwas davon, wird der Test uebersprungen.
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

check() {	# Beschreibung, erwartet, tatsaechlich
	if [ "$2" = "$3" ]; then
		echo "ok   $1"
	else
		echo "FAIL $1: erwartet '$2', bekommen '$3'"
		fail=1
	fi
}

python3 "$HERE/x11check.py" "$T/xauth" mkauth
# -noreset: sonst setzt sich der Server zurueck, sobald der letzte Client geht, und vergisst den Timeout
Xvfb -displayfd 3 -auth "$T/xauth" -noreset -nolisten tcp -screen 0 640x480x8 3> "$T/display" 2>/dev/null &
pids=$!
i=0
while [ ! -s "$T/display" ]; do
	i=$((i + 1))
	[ "$i" -gt 100 ] && { echo "FAIL Xvfb startet nicht"; exit 1; }
	sleep 0.1
done
num=$(cat "$T/display")

x() {	# Befehl [Wert]
	python3 "$HERE/x11check.py" "$T/xauth" "$num" "$@"
}

export DISPLAY=":$num" XAUTHORITY="$T/xauth" XDG_RUNTIME_DIR="$T"
export DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent DBUS_SYSTEM_BUS_ADDRESS=unix:path=/nonexistent
unset WAYLAND_DISPLAY CAFFEINATE_DEBUG

# Gegenprobe: ohne caffeinate springt der Bildschirmschoner nach 1 s an
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

exit $fail
