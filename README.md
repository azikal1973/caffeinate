# caffeinate für Linux

Version 0.2.0.0

Nachbau von macOS-`caffeinate(8)` in C. Parameter und Verhalten folgen Apples
Quellcode (PowerManagement/caffeinate), sodass man zwischen Mac und Linux keinen
Unterschied merkt.

Eine einzelne C-Datei, die außer der libc nichts braucht. Statisch gelinkt läuft
das Binary auf jeder Distribution, ohne dass etwas installiert werden muss.

```
caffeinate [-disu] [-t timeout] [-w Process ID] [command arguments...]
```

## Benutzung

```sh
caffeinate                 # kein Ruhezustand, bis Strg+C
caffeinate -t 3600         # eine Stunde lang, Strg+C beendet vorher
caffeinate -d              # auch der Bildschirm bleibt an
caffeinate -i make         # solange make läuft
caffeinate -w 4711         # bis Prozess 4711 endet
```

**Beenden:** Strg+C im Terminal, in dem caffeinate läuft. Alle Sperren sind dann weg.

## Installieren

### Fertiges Binary

Statisch gelinkt (musl), für x86_64 und aarch64. Es läuft auf jeder Distribution:

```sh
curl -fLo caffeinate https://github.com/azikal1973/caffeinate/releases/latest/download/caffeinate-$(uname -m)-linux
chmod +x caffeinate
sudo mv caffeinate /usr/local/bin/    # oder nach ~/.local/bin
```

### Selbst bauen

Gebraucht werden nur ein C-Compiler und make, keine Bibliotheken und kein pkg-config.

```sh
make
sudo make install PREFIX=/usr/local   # systemweit: /usr/local/bin + Manpage
make install                          # nur für dich: ~/.local/bin
```

Ohne make geht es auch: `cc -O2 -o caffeinate caffeinate.c`

Erst `make` als normaler Nutzer, dann `sudo make install`, sonst gehört das Binary im Projektordner root.
Eine laufende Instanz stört die Installation nicht. Sie arbeitet mit der alten Datei weiter, bis sie neu
gestartet wird.

```sh
make static                           # statisches Binary, am besten mit musl (z. B. unter Alpine)
make test                             # Smoke-Test und D-Bus-Test
make asan                             # Build mit Address/UB/LeakSanitizer
```

Mit glibc statisch gelinkt, verlangt das Binary mindestens den Kernel, für den die glibc der
Distribution gebaut wurde. Mit musl gibt es diese Grenze nicht, deshalb entstehen die Binaries der
Releases unter Alpine.

## Linux-Umsetzung

| Flag | macOS-Assertion | Linux |
|---|---|---|
| `-i` | PreventUserIdleSystemSleep (Standard) | `org.freedesktop.PowerManagement.Inhibit` (KDE, XFCE), sonst GNOME/MATE-SessionManager (Flag suspend) |
| `-d` | PreventUserIdleDisplaySleep | `org.freedesktop.ScreenSaver.Inhibit`, sonst `org.xfce.ScreenSaver` oder GNOME/MATE-SessionManager (Flag idle) |
| `-s` | PreventSystemSleep (nur am Netzteil) | logind-Inhibitor `sleep`, nur am Netz |
| `-u` | UserIsActive (Standard-Timeout 5 s) | `ScreenSaver.SimulateUserActivity` + wie `-d` |
| `-m` | PreventDiskIdle | kein Gegenstück, ohne Wirkung |

Ohne Session-Bus (TTY, SSH) oder ohne passenden Desktop-Dienst fallen `-i`, `-d` und `-u` auf den
logind-Inhibitor `idle` zurück. Desktop-Dienste werden nie per D-Bus-Aktivierung gestartet, es werden
nur laufende genutzt.

Startet ein Desktop-Dienst neu (etwa PowerDevil nach einem Absturz), ist die Sperre dort verloren.
caffeinate bemerkt das und legt sie neu an. Solange der Dienst fehlt, hält logind eine Ersatzsperre.
Dasselbe gilt, wenn der Dienst erst nach caffeinate startet oder der Session-Bus endet.

Ob der Rechner am Netz hängt, entscheidet `-s` wie systemd: Eine Stromquelle meldet `online`, oder
kein Akku entlädt sich. Der Status wird alle 5 Sekunden geprüft.

D-Bus spricht caffeinate direkt auf dem Socket, libdbus wird nicht gebraucht.

## Verhalten wie auf macOS

Wo Apples Manpage und Apples Code sich widersprechen, folgt dieser Nachbau dem Code:

- Ohne Assertion-Flag gilt `-i`. `-h` gibt die Usage aus (Exit 0).
- `-t` und `-w` werden mit `strtol(…, 0)` gelesen: `0x10` = 16, `10abc` = 10, `abc` = Fehler.
- `-t 0` heißt „kein Timeout“.
- `-t` mit command: Die Sperren enden nach dem Timeout, das command läuft weiter. Die Manpage sagt dagegen „ignoriert“.
- `-u` ohne `-t`: Die Sperre endet nach 5 s, caffeinate läuft aber weiter, bis es beendet wird.
- `-w` mit nicht existierender PID beendet sofort. `-w` wird ignoriert, wenn ein command angegeben ist.
- Mit command wird caffeinate per exec selbst zum command. PID, Exit-Status und Signale sind die des commands. Ein Hilfsprozess hält die Sperren, ignoriert SIGINT und SIGQUIT und endet mit dem command.
- Lässt sich eine Sperre nicht anlegen: `Failed to create <Typ> assertion`, Exit 1. Davor steht eine Zeile mit dem Grund.
- Die Meldungen sind im BSD-Wortlaut: `illegal option -- x`, `option requires an argument -- t`.

## Desktops und Systeme

| Umgebung | Status |
|---|---|
| KDE Plasma | getestet |
| GNOME, Cinnamon | über `org.gnome.SessionManager` (gegen Mock-Dienst getestet) |
| MATE | über `org.mate.SessionManager` (gegen Mock-Dienst getestet) |
| XFCE | über xfce4-power-manager und xfce4-screensaver (gegen Mock-Dienst getestet) |
| Hyprland (hypridle) | über ScreenSaver-D-Bus bzw. logind |
| sway (swayidle) | wirkungslos, swayidle kennt keine Inhibitoren ohne Fenster |
| TTY / SSH | logind `idle` bzw. `sleep` |
| systemd oder elogind | beide (Artix, Devuan, Void, Alpine, Gentoo/OpenRC) |
| ohne logind | ConsoleKit2, gleiche Schnittstelle (gegen Mock-Dienst getestet) |
| glibc oder musl | beide |
| Architektur | beliebig, auch big-endian; x86_64 und aarch64 laufen in der CI |
| Container, User-Namespace | Anmeldung am Bus auch mit abweichender uid |
| Kernel | ab 2.6.27; unter 5.3 ohne pidfd, dann mit PDEATHSIG und Polling |

`-u` weckt den Bildschirm nur dort, wo `SimulateUserActivity` existiert (KDE, XFCE). Auf GNOME wirkt es
nur als Display-Sperre.

`-s` braucht die Erlaubnis von logind (polkit-Aktion `org.freedesktop.login1.inhibit-block-sleep`). In
der Standardeinstellung haben sie root und Nutzer mit lokaler Sitzung. Wer nur über SSH angemeldet ist,
müsste ein Admin-Passwort eingeben, `-s` schlägt dann fehl. `-i`, `-d` und `-u` sind davon nicht
betroffen.

## Tests

`make test` startet zwei Skripte:

- `tests/smoke.sh` prüft Optionen, Meldungen und Exit-Codes. Es braucht keinen D-Bus.
- `tests/dbus.sh` startet einen privaten dbus-daemon mit Mock-Diensten und prüft, welche Sperre wo
  angelegt und wieder freigegeben wird: KDE, XFCE, GNOME, MATE, logind, ConsoleKit2, Neustart eines Dienstes,
  Ende des Session-Busses, Netzteil-Erkennung. Es braucht `dbus-daemon` und python3 mit PyGObject, für
  die Netzteil-Tests außerdem `unshare`. Was fehlt, wird übersprungen. Der echte Session- und System-Bus
  bleiben unberührt.

Die CI (`.github/workflows/build.yml`) baut und testet auf Arch, Debian stable/oldstable, Ubuntu
22.04, Fedora (clang), Rocky Linux 8, openSUSE Tumbleweed und Alpine (musl). Zusätzlich baut sie das
statische Binary für x86_64 und aarch64 und startet es auf weiteren Distributionen. Bei einem Tag `v*`
hängt sie die Binaries an das Release.

Zur Fehlersuche: `CAFFEINATE_DEBUG=1 caffeinate …`

## Lizenz

MIT, siehe [LICENSE](LICENSE). Der Code ist eine eigene Implementierung. Apples caffeinate.c
(APSL 2.0) diente als Verhaltensreferenz, daraus wurde kein Code übernommen.
