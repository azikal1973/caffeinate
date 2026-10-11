#!/bin/sh
# D-Bus test: a private dbus-daemon plays session and system bus, joined by
# mock services (tests/mockbus.py). Checks which lock caffeinate takes from
# which service, and when that lock is gone again.
# The real session bus and system bus are not touched.
#
# Needs dbus-daemon and python3 with PyGObject, and for the power supply
# tests also unshare (mount namespace); whatever is missing gets skipped.
C=${1:-./caffeinate}
case $C in /*) ;; *) C=$PWD/$C ;; esac
HERE=$(cd "$(dirname "$0")" && pwd)

if ! command -v dbus-daemon >/dev/null 2>&1 ||
   ! python3 -c 'import gi; gi.require_version("Gio", "2.0"); from gi.repository import Gio' 2>/dev/null; then
	echo "skip D-Bus-Test: dbus-daemon oder python3 mit PyGObject fehlt"
	exit 0
fi

# keep it short: paths of unix sockets may only be about 100 characters long
T=$(mktemp -d /tmp/caffeinate-test.XXXXXX) || exit 1
LOG=$T/log
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

lines() {	# pattern -> number of matching log lines
	grep -c -- "$1" "$LOG" || true
}

waitlog() {	# pattern, minimum count (default 1), wait in tenths of a second (default 50)
	i=0
	while [ "$(lines "$1")" -lt "${2:-1}" ]; do
		i=$((i + 1))
		[ "$i" -gt "${3:-50}" ] && return 1
		sleep 0.1
	done
}

has() {	# description, pattern, minimum count
	if waitlog "$2" "${3:-1}"; then
		echo "ok   $1"
	else
		echo "FAIL $1: '$2' fehlt im Protokoll:"
		sed 's/^/     | /' "$LOG"
		fail=1
	fi
}

hasnt() {	# description, pattern
	check "$1" 0 "$(lines "$2")"
}

start_bus() {	# name -> socket $T/name, PID in $bus_pid
	dbus-daemon --config-file="$T/bus.conf" --address="unix:path=$T/$1" --nofork --nopidfile 2>/dev/null &
	bus_pid=$!
	pids="$pids $bus_pid"
	i=0
	while [ ! -S "$T/$1" ]; do
		i=$((i + 1))
		[ "$i" -gt 50 ] && { echo "FAIL dbus-daemon startet nicht"; exit 1; }
		sleep 0.1
	done
}

mock() {	# bus, services... -> PID in $mock_pid
	bus=$1
	shift
	DBUS_SESSION_BUS_ADDRESS="unix:path=$T/$bus" python3 "$HERE/mockbus.py" "$LOG" "$@" &
	mock_pid=$!
	pids="$pids $mock_pid"
	for s in "$@"; do
		[ "$s" = noisy ] || waitlog "^ready $s\$" || { echo "FAIL Mock $s startet nicht"; exit 1; }
	done
}

stop() {	# PID
	kill "$1" 2>/dev/null
	wait "$1" 2>/dev/null
}

cat > "$T/bus.conf" <<EOF
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>session</type>
  <listen>unix:tmpdir=/tmp</listen>
  <policy context="default">
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
</busconfig>
EOF
: > "$LOG"
start_bus session
start_bus system
export DBUS_SESSION_BUS_ADDRESS="unix:path=$T/session"
export DBUS_SYSTEM_BUS_ADDRESS="unix:path=$T/system"
export XDG_RUNTIME_DIR="$T"	# no $T/bus there: the real session bus is out of reach
unset CAFFEINATE_DEBUG

# ---------------------------------------------------------------- KDE
mock session pm ss
m=$mock_pid

"$C" -i -t 1
check "-i: Exit 0" 0 "$?"
has "-i: PowerManagement.Inhibit" "^pm /org/freedesktop/PowerManagement/Inhibit Inhibit cookie=1 app=caffeinate reason=caffeinate asserting for 1 secs\$"
has "-i: UnInhibit am Ende" "^pm .* UnInhibit 1\$"
hasnt "-i: ScreenSaver bleibt unberuehrt" "^ss "

: > "$LOG"
"$C" -d -u -t 1
has "-d: ScreenSaver.Inhibit unter dem Pfad der Spezifikation" "^ss /org/freedesktop/ScreenSaver Inhibit cookie=[0-9]* app=caffeinate "
has "-u: SimulateUserActivity" "^ss /org/freedesktop/ScreenSaver SimulateUserActivity"
check "-d -u: zwei Sperren" 2 "$(lines ' Inhibit ')"
check "-d -u: beide freigegeben" 2 "$(lines ' UnInhibit ')"

: > "$LOG"
"$C" -i sh -c 'sleep 0.5; exit 3'
check "command: Exit-Status durchgereicht" 3 "$?"
has "command: Sperre im Namen des command" "^pm .* Inhibit .* reason=caffeinate asserting on behalf of 'sh' (pid [0-9]*)\$"
has "command: Freigabe nach dessen Ende" "^pm .* UnInhibit "

: > "$LOG"
"$C" -i -t 1 sleep 3 &
cpid=$!
pids="$pids $cpid"
if waitlog "^pm .* UnInhibit " 1 25 && kill -0 "$cpid" 2>/dev/null; then
	echo "ok   command mit -t: Sperre endet, command laeuft weiter"
else
	echo "FAIL command mit -t: Sperre endet, command laeuft weiter"
	fail=1
fi
kill "$cpid" 2>/dev/null
wait "$cpid" 2>/dev/null

: > "$LOG"
sleep 1 &
spid=$!
"$C" -w "$spid"
check "-w: endet mit dem Prozess" 0 "$?"
has "-w: Sperre im Namen der PID" "^pm .* reason=caffeinate asserting on behalf of Process ID $spid\$"
has "-w: UnInhibit am Ende" "^pm .* UnInhibit "
wait "$spid" 2>/dev/null
"$C" -w 2147483646
check "-w mit unbekannter PID: sofort Exit 0" 0 "$?"

# Latin-1 file name: the reason has to stay valid UTF-8, or the bus disconnects
: > "$LOG"
latin=$T/$(printf 'x\377y')
if printf '#!/bin/sh\nexit 0\n' > "$latin" 2>/dev/null && chmod +x "$latin"; then
	"$C" -i "$latin"
	has "command mit ungueltigem UTF-8 im Namen" "^pm .* Inhibit .*x?y' (pid "
fi
stop "$m"

# unrelated messages ahead of the reply, one of them oversized
: > "$LOG"
mock session pm noisy
m=$mock_pid
"$C" -i -t 1
check "fremde Signale vor der Antwort: Exit 0" 0 "$?"
has "fremde Signale vor der Antwort: Sperre trotzdem angelegt" "^pm .* Inhibit cookie=1 "
has "fremde Signale vor der Antwort: und freigegeben" "^pm .* UnInhibit 1\$"
stop "$m"

: > "$LOG"
mock session ss-legacy
m=$mock_pid
"$C" -d -t 1
has "-d: Dienst nur unter /ScreenSaver" "^ss-legacy /ScreenSaver Inhibit "
stop "$m"

# ---------------------------------------------------------------- XFCE
# while it is inhibited, xfce4-power-manager itself holds off DPMS and the screen
# saver; org.xfce.ScreenSaver directly only counts when the power manager is absent
: > "$LOG"
mock session xfpm xfss
m=$mock_pid
"$C" -i -d -u -t 1
check "XFCE -i, -d, -u: drei Sperren bei xfce4-power-manager" 3 "$(lines '^xfpm /org/freedesktop/PowerManagement/Inhibit Inhibit ')"
hasnt "XFCE -d: nicht nur der Bildschirmschoner" "^xfss .* Inhibit "
has "XFCE -u: SimulateUserActivity beim Bildschirmschoner" "^xfss /org/xfce/ScreenSaver SimulateUserActivity"
check "XFCE: alle drei freigegeben" 3 "$(lines ' UnInhibit ')"
stop "$m"

: > "$LOG"
mock session xfss
m=$mock_pid
"$C" -d -t 1
has "XFCE ohne Power-Manager -d: xfce4-screensaver" "^xfss /org/xfce/ScreenSaver Inhibit cookie=[0-9]* app=caffeinate "
stop "$m"

# xfce4-power-manager starts after caffeinate: the lock moves over to it
: > "$LOG"
mock session xfss
m=$mock_pid
"$C" -d &
cpid=$!
pids="$pids $cpid"
has "XFCE, Power-Manager kommt spaeter: zunaechst xfce4-screensaver" "^xfss .* Inhibit cookie=1 "
mock session xfpm
m2=$mock_pid
has "XFCE, Power-Manager kommt spaeter: Sperre wechselt dorthin" "^xfpm .* Inhibit "
has "XFCE, Power-Manager kommt spaeter: die alte wird freigegeben" "^xfss .* UnInhibit 1\$"
kill -TERM "$cpid"
wait "$cpid"
has "XFCE, Power-Manager kommt spaeter: Freigabe am Ende" "^xfpm .* UnInhibit "
stop "$m"
stop "$m2"

# ---------------------------------------------------------------- GNOME, Cinnamon, MATE
# all three register their session manager as org.gnome.SessionManager
: > "$LOG"
mock session gsm
m=$mock_pid
"$C" -i -d -t 1
has "GNOME -i: SessionManager, Flag suspend" "^gsm /org/gnome/SessionManager Inhibit cookie=[0-9]* app=caffeinate xid=0 flags=4 reason=caffeinate asserting for 1 secs\$"
has "GNOME -d: SessionManager, Flag idle" "^gsm .* Inhibit .* flags=8 "
check "GNOME: beide freigegeben" 2 "$(lines ' Uninhibit ')"
stop "$m"

: > "$LOG"
mock session ss-broken gsm
m=$mock_pid
"$C" -d -t 1
has "Dienst antwortet mit Fehler: naechster wird genommen" "^gsm .* Inhibit .* flags=8 "
stop "$m"

# ---------------------------------------------------------------- logind
: > "$LOG"
mock system login1
ml=$mock_pid
"$C" -i -t 1
check "ohne Desktop-Dienst: Exit 0" 0 "$?"
has "ohne Desktop-Dienst: logind idle" "^login1 Inhibit what=idle who=caffeinate mode=block why=caffeinate asserting for 1 secs\$"
has "ohne Desktop-Dienst: fd am Ende geschlossen" "^login1 released idle\$"

: > "$LOG"
DBUS_SESSION_BUS_ADDRESS=unix:path=/nonexistent "$C" -d -t 1
has "ohne Session-Bus: logind idle" "^login1 Inhibit what=idle "
has "ohne Session-Bus: fd am Ende geschlossen" "^login1 released idle\$"

# ---------------------------------------------------------------- ignored signals (nohup)
: > "$LOG"
mock session pm
m=$mock_pid
(trap '' HUP; exec "$C" -i -t 3) &
cpid=$!
pids="$pids $cpid"
has "nohup: Sperre angelegt" "^pm .* Inhibit "
kill -HUP "$cpid"
sleep 0.5
hasnt "nohup: SIGHUP gibt die Sperre nicht frei" " UnInhibit "
wait "$cpid"
check "nohup: laeuft bis zum Timeout, Exit 0" 0 "$?"

# with a command: the helper process has to survive the SIGHUP just like the command
: > "$LOG"
(trap '' HUP; exec "$C" -i sleep 3) &
cpid=$!
pids="$pids $cpid"
has "nohup mit command: Sperre angelegt" "^pm .* Inhibit "
helper=
for stat in /proc/[0-9]*/stat; do
	read -r pid _ _ ppid _ < "$stat" 2>/dev/null || continue
	[ "$ppid" = "$cpid" ] && helper=$pid
done
kill -HUP "$helper" "$cpid" 2>/dev/null
sleep 0.5
hasnt "nohup mit command: SIGHUP gibt die Sperre nicht frei" " UnInhibit "
wait "$cpid"
has "nohup mit command: Freigabe erst mit dem Ende des command" " UnInhibit "
stop "$m"

# ---------------------------------------------------------------- a service restarts
: > "$LOG"
# not as "VAR=x mock ...": in front of a function call that stays set in some shells
MOCK_COOKIE=7
export MOCK_COOKIE
mock session pm
unset MOCK_COOKIE
m=$mock_pid
"$C" -i &
cpid=$!
pids="$pids $cpid"
has "Neustart: Sperre beim Dienst" "^pm .* Inhibit cookie=7 "
stop "$m"
has "Neustart: Dienst weg, Ersatzsperre bei logind" "^login1 Inhibit what=idle "
mock session pm
m=$mock_pid
has "Neustart: Dienst wieder da, neue Sperre" "^pm .* Inhibit cookie=1 "
has "Neustart: Ersatzsperre freigegeben" "^login1 released idle\$"
kill -TERM "$cpid"
wait "$cpid"
check "SIGTERM: Exit durch das Signal" 143 "$?"
has "SIGTERM: Sperre vorher freigegeben" "^pm .* UnInhibit 1\$"
hasnt "Neustart: altes Cookie wird nicht mehr benutzt" " UnInhibit 7\$"
stop "$m"

# the service exits right before caffeinate (logout): no crash, not even with debug output
: > "$LOG"
mock session pm
m=$mock_pid
CAFFEINATE_DEBUG=1 "$C" -i 2>/dev/null &
cpid=$!
pids="$pids $cpid"
has "Dienst endet kurz vor caffeinate: Sperre beim Dienst" "^pm .* Inhibit "
kill "$m"
kill -TERM "$cpid"
wait "$cpid"
check "Dienst endet kurz vor caffeinate: Exit durch SIGTERM" 143 "$?"
wait "$m" 2>/dev/null

# the session bus itself exits (logout)
: > "$LOG"
start_bus session2
sb=$bus_pid
mock session2 pm
DBUS_SESSION_BUS_ADDRESS="unix:path=$T/session2" "$C" -i &
cpid=$!
pids="$pids $cpid"
has "Abmeldung: Sperre beim Dienst" "^pm .* Inhibit "
stop "$sb"
has "Abmeldung: Session-Bus weg, Ersatzsperre bei logind" "^login1 Inhibit what=idle "
kill -TERM "$cpid"
wait "$cpid"
check "Abmeldung: laeuft weiter bis zum Signal" 143 "$?"
has "Abmeldung: Ersatzsperre am Ende freigegeben" "^login1 released idle\$"

# ---------------------------------------------------------------- -s and the power supply
if unshare -rm true 2>/dev/null; then
	UNSHARE="unshare -rm"
elif unshare -m true 2>/dev/null; then
	UNSHARE="unshare -m"
else
	UNSHARE=
fi
P=$T/power
mkdir "$P"
# shellcheck disable=SC2016
if [ -n "$UNSHARE" ] && $UNSHARE sh -c 'mount --bind "$1" /sys/class/power_supply' sh "$P" 2>/dev/null; then
	dev() {	# name, attribute=value... (the attribute may include a subdirectory)
		d=$P/$1
		shift
		for kv; do
			mkdir -p "$(dirname "$d/${kv%%=*}")"
			printf '%s\n' "${kv#*=}" > "$d/${kv%%=*}"
		done
	}
	power() {	# description, expected number of sleep locks; the devices are in $P
		: > "$LOG"
		# shellcheck disable=SC2016
		$UNSHARE sh -c 'mount --bind "$1" /sys/class/power_supply && shift && exec "$@"' sh "$P" "$C" -s -t 1
		rc=$?
		waitlog "^login1 released sleep\$" "$2" 20
		check "-s, $1" "0 $2" "$rc $(lines '^login1 Inhibit what=sleep ')"
		rm -rf "$P"/*
	}

	power "Rechner ohne Stromquelle im sysfs: Sperre" 1
	dev AC type=Mains online=1
	dev BAT0 type=Battery status=Charging
	power "Netzteil online: Sperre" 1
	dev AC type=Mains online=0
	dev BAT0 type=Battery status=Discharging
	power "Akkubetrieb: keine Sperre" 0
	dev ucsi-source-psy-USBC000:001 type=USB online=0
	power "Desktop mit freiem USB-C-Port: Sperre" 1
	dev ucsi-source-psy-USBC000:001 type=USB online=2
	dev BAT0 type=Battery status=Charging
	power "USB-PD-Netzteil (online=2): Sperre" 1
	dev AC type=Mains online=0
	dev hidpp_battery_0 type=Battery scope=Device status=Discharging
	power "nur der Akku einer Maus entlaedt: Sperre" 1
	dev AC type=Mains online=0
	dev BAT1 type=Battery present=0 status=Discharging
	power "leerer Akkuschacht: Sperre" 1
	dev ucsi-source-psy-USBC000:001 type=USB online=1 'device/typec/port0/power_role=[source] sink'
	dev BAT0 type=Battery status=Discharging
	power "Akkubetrieb, der USB-C-Port speist ein Geraet: keine Sperre" 0
	dev ucsi-source-psy-USBC000:001 type=USB online=1 'device/typec/port0/power_role=source [sink]'
	dev BAT0 type=Battery status=Charging
	power "USB-C-Port wird gespeist: Sperre" 1

	# the power supply is unplugged while caffeinate runs
	: > "$LOG"
	dev AC type=Mains online=1
	dev BAT0 type=Battery status=Charging
	# shellcheck disable=SC2016
	$UNSHARE sh -c 'mount --bind "$1" /sys/class/power_supply && shift && exec "$@"' sh "$P" "$C" -s &
	cpid=$!
	pids="$pids $cpid"
	has "-s: Sperre am Netzteil" "^login1 Inhibit what=sleep who=caffeinate mode=block "
	dev AC type=Mains online=0
	dev BAT0 type=Battery status=Discharging
	if waitlog "^login1 released sleep\$" 1 70; then
		echo "ok   -s: Netzteil gezogen, Sperre freigegeben"
	else
		echo "FAIL -s: Netzteil gezogen, Sperre freigegeben"
		fail=1
	fi
	kill -TERM "$cpid" 2>/dev/null
	wait "$cpid" 2>/dev/null
else
	echo "skip -s und das Netzteil: unshare mit Mount-Namespace nicht moeglich"
fi
stop "$ml"

# unrelated signals carrying fds right before the reply from logind: caffeinate
# has to hold the fd of the lock, not one of the others
: > "$LOG"
mock system login1 noisy
m=$mock_pid
"$C" -i -t 2 &
cpid=$!
pids="$pids $cpid"
has "fremde fds vor der Antwort: Sperre bei logind" "^login1 Inhibit what=idle "
sleep 1
hasnt "fremde fds vor der Antwort: Sperre haelt, solange caffeinate laeuft" "^login1 released"
wait "$cpid"
has "fremde fds vor der Antwort: am Ende freigegeben" "^login1 released idle\$"
stop "$m"

# service gone and nothing to replace it: that is reported only once the
# follow-up after 5 s has failed too, and again when the lock is back
: > "$LOG"
mock session pm
m=$mock_pid
"$C" -i 2> "$T/err" &
cpid=$!
pids="$pids $cpid"
has "Sperre verloren: zunaechst beim Dienst" "^pm .* Inhibit "
stop "$m"
sleep 2
check "Sperre verloren: noch keine Meldung vor dem Nachfassen" "" "$(cat "$T/err")"
i=0
while [ ! -s "$T/err" ] && [ "$i" -lt 60 ]; do
	i=$((i + 1))
	sleep 0.1
done
check "Sperre verloren: Meldung" "caffeinate: PreventUserIdleSystemSleep: Sperre verloren: weder logind noch elogind oder ConsoleKit2 am System-Bus" "$(cat "$T/err")"
mock session pm
m=$mock_pid
has "Sperre verloren: Dienst wieder da, neue Sperre" "^pm .* Inhibit " 2
sleep 0.3
check "Sperre verloren: Meldung, dass sie wieder da ist" "caffeinate: PreventUserIdleSystemSleep: Sperre wieder angelegt" "$(tail -n 1 "$T/err")"
kill -TERM "$cpid"
wait "$cpid" 2>/dev/null
stop "$m"

# ---------------------------------------------------------------- ConsoleKit2, nothing at all
: > "$LOG"
mock system ck
m=$mock_pid
"$C" -i -t 1
has "ohne logind: ConsoleKit2" "^ck Inhibit what=idle who=caffeinate mode=block "
has "ohne logind: fd am Ende geschlossen" "^ck released idle\$"
stop "$m"

out=$("$C" -i -t 1 2>&1)
check "keine Sperre moeglich: Exit 1" 1 "$?"
check "keine Sperre moeglich: Grund" "caffeinate: weder logind noch elogind oder ConsoleKit2 am System-Bus" "$(echo "$out" | head -n 1)"
check "keine Sperre moeglich: Meldung" "Failed to create PreventUserIdleSystemSleep assertion" "$(echo "$out" | tail -n 1)"

# logind without polkit refuses everyone but root: the message has to say that, not that ConsoleKit is missing
mock system login1-denied
m=$mock_pid
out=$("$C" -i -t 1 2>&1)
check "logind lehnt ab: Exit 1" 1 "$?"
check "logind lehnt ab: Grund" "caffeinate: org.freedesktop.login1.Manager.Inhibit: Permission denied (polkit fehlt oder erlaubt die Sperre nicht)" "$(echo "$out" | head -n 1)"
stop "$m"

out=$(DBUS_SYSTEM_BUS_ADDRESS=unix:path=/nonexistent "$C" -s -t 1 2>&1)
check "-s ohne System-Bus: Exit 1" 1 "$?"
check "-s ohne System-Bus: Meldung" "Failed to create PreventSystemSleep assertion" "$(echo "$out" | tail -n 1)"

exit $fail
