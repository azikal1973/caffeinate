# caffeinate für Linux

Version 0.1.0.0

Nachbau von macOS-`caffeinate(8)` in C. Parameter und Verhalten folgen Apples
Quellcode (PowerManagement/caffeinate), sodass man zwischen Mac und Linux keinen
Unterschied merkt.

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

## Bauen und installieren

Abhängigkeit: `libdbus-1` (Header und pkg-config). Die Paketnamen:

| Distribution | Paket |
|---|---|
| Arch, Manjaro, CachyOS | `dbus` |
| Debian, Ubuntu, Mint | `libdbus-1-dev` |
| Fedora, RHEL, Rocky, Alma | `dbus-devel` |
| openSUSE | `dbus-1-devel` |
| Alpine | `dbus-dev` |
| Void | `dbus-devel` |
| Gentoo | `sys-apps/dbus` |

```sh
make
sudo make install PREFIX=/usr/local   # systemweit: /usr/local/bin + Manpage
make install                          # nur für dich: ~/.local/bin
make test                             # Smoke-Test (Optionen, Exit-Codes)
make asan                             # Build mit Address/UB/LeakSanitizer
```

Erst `make` als normaler Nutzer, dann `sudo make install`, sonst gehört das Binary im Projektordner root.

## Linux-Umsetzung

| Flag | macOS-Assertion | Linux |
|---|---|---|
| `-i` | PreventUserIdleSystemSleep (Standard) | `org.freedesktop.PowerManagement.Inhibit`, sonst GNOME/MATE-SessionManager (Flag suspend) |
| `-d` | PreventUserIdleDisplaySleep | `org.freedesktop.ScreenSaver.Inhibit`, sonst GNOME/MATE-SessionManager (Flag idle) |
| `-s` | PreventSystemSleep (nur am Netzteil) | logind-Inhibitor `sleep`, nur bei `online=1` |
| `-u` | UserIsActive (Standard-Timeout 5 s) | `ScreenSaver.SimulateUserActivity` + wie `-d` |
| `-m` | PreventDiskIdle | kein Gegenstück, ohne Wirkung |

Ohne Session-Bus (TTY, SSH) oder ohne passenden Desktop-Dienst fallen `-i`, `-d` und `-u` auf den
logind-Inhibitor `idle` zurück. Desktop-Dienste werden nie per D-Bus-Aktivierung gestartet, es werden
nur laufende genutzt.

## Verhalten wie auf macOS

Wo Apples Manpage und Apples Code sich widersprechen, folgt dieser Nachbau dem Code:

- Ohne Assertion-Flag gilt `-i`. `-h` gibt die Usage aus (Exit 0).
- `-t` und `-w` werden mit `strtol(…, 0)` gelesen: `0x10` = 16, `10abc` = 10, `abc` = Fehler.
- `-t 0` heißt „kein Timeout“.
- `-t` mit command: Die Sperren enden nach dem Timeout, das command läuft weiter. Die Manpage sagt dagegen „ignoriert“.
- `-u` ohne `-t`: Die Sperre endet nach 5 s, caffeinate läuft aber weiter, bis es beendet wird.
- `-w` mit nicht existierender PID beendet sofort. `-w` wird ignoriert, wenn ein command angegeben ist.
- Mit command wird caffeinate per exec selbst zum command. PID, Exit-Status und Signale sind die des commands. Ein Hilfsprozess hält die Sperren, ignoriert SIGINT und SIGQUIT und endet mit dem command.
- Lässt sich eine Sperre nicht anlegen: `Failed to create <Typ> assertion`, Exit 1.
- Die Meldungen sind im BSD-Wortlaut: `illegal option -- x`, `option requires an argument -- t`.

## Desktops und Systeme

| Umgebung | Status |
|---|---|
| KDE Plasma | getestet |
| GNOME, Cinnamon | über `org.gnome.SessionManager` (gegen Mock-Dienst getestet) |
| MATE | über `org.mate.SessionManager` |
| XFCE | über xfce4-power-manager / xfce4-screensaver |
| Hyprland (hypridle) | über ScreenSaver-D-Bus bzw. logind |
| sway (swayidle) | wirkungslos, swayidle kennt keine Inhibitoren ohne Fenster |
| TTY / SSH | logind `idle` bzw. `sleep` |
| systemd oder elogind | beide (Artix, Devuan, Void, Alpine, Gentoo/OpenRC) |
| glibc oder musl | beide |
| Kernel | ab 2.6.27; unter 5.3 ohne pidfd, dann mit PDEATHSIG und Polling |

`-u` weckt den Bildschirm nur dort, wo `SimulateUserActivity` existiert (KDE, XFCE). Auf GNOME wirkt es
nur als Display-Sperre.

Die CI (`.github/workflows/build.yml`) baut und testet auf Arch, Debian stable/oldstable, Ubuntu
22.04, Fedora (clang), Rocky Linux 8, openSUSE Tumbleweed und Alpine (musl).

Zur Fehlersuche: `CAFFEINATE_DEBUG=1 caffeinate …`

## Lizenz

MIT, siehe [LICENSE](LICENSE). Der Code ist eine eigene Implementierung. Apples caffeinate.c
(APSL 2.0) diente als Verhaltensreferenz, daraus wurde kein Code übernommen.
