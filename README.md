# caffeinate für Linux und Windows

Version 0.3.0.0

Nachbau von macOS-`caffeinate(8)` in C. Parameter und Verhalten folgen Apples
Quellcode (PowerManagement/caffeinate), sodass man zwischen Mac, Linux und Windows
keinen Unterschied merkt.

Für Linux eine C-Datei samt kleinem Header (`caffeinate.c`, `caffeinate.h`), die außer der libc
nichts braucht. Statisch gelinkt
läuft das Binary auf jeder Distribution, ohne dass etwas installiert werden muss.
Die Windows-Fassung wird mit zig zu einer einzelnen `.exe` gebaut, siehe
[Windows](#windows).

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
make test                             # Smoke-, D-Bus- und X11-Test
make asan                             # Build mit Address/UB/LeakSanitizer
```

Mit glibc statisch gelinkt, verlangt das Binary mindestens den Kernel, für den die glibc der
Distribution gebaut wurde. Mit musl gibt es diese Grenze nicht, deshalb entstehen die Binaries der
Releases unter Alpine.

Gebaut wird mit PIE, vollem RELRO, `_FORTIFY_SOURCE` und, wo der Compiler ihn kennt, dem
Stack-Protector. Wer `CFLAGS` und `LDFLAGS` selbst setzt (Distro-Pakete), bestimmt das selbst.

### NixOS

Ein fremdes, dynamisch gelinktes Binary startet auf NixOS nicht, das statische aus den Releases
schon. Als Paket gibt es caffeinate über das Flake dieses Repos:

```sh
nix run github:azikal1973/caffeinate -- -i make     # einmalig ausführen
nix profile install github:azikal1973/caffeinate    # für dich installieren
```

In der Systemkonfiguration: `inputs.caffeinate.url = "github:azikal1973/caffeinate";` und dann
`environment.systemPackages = [ inputs.caffeinate.packages.${pkgs.stdenv.hostPlatform.system}.default ];`.
Ohne Flakes geht `pkgs.callPackage ./package.nix { }`. `packages.static` ist die statisch gelinkte
Variante, `nix develop -c make test` baut und testet mit allem, was die Tests brauchen.

Ohne Desktop (TTY, SSH) und unter Fenstermanagern ohne eigenen Energiedienst hält logind die Sperre.
logind fragt dafür polkit, und ohne polkit lehnt es jeden außer root ab. Die Desktop-Module von NixOS
schalten polkit ein, eine schlanke Konfiguration braucht `security.polkit.enable = true;`.

### Void Linux

```sh
sudo xbps-install -S gcc make
make && sudo make install PREFIX=/usr/local
```

Das fertige Binary läuft unter glibc und musl. Für die Sperren bei elogind müssen die Dienste `dbus`
und `elogind` laufen und das Paket `polkit` installiert sein (D-Bus startet polkitd bei Bedarf). Ohne
polkit lehnt elogind jeden außer root ab, caffeinate meldet dann
`Permission denied (polkit fehlt oder erlaubt die Sperre nicht)`.

## Linux-Umsetzung

| Flag | macOS-Assertion | Linux |
|---|---|---|
| `-i` | PreventUserIdleSystemSleep (Standard) | `org.freedesktop.PowerManagement.Inhibit` (KDE, XFCE), sonst `org.gnome.SessionManager` (Flag suspend) |
| `-d` | PreventUserIdleDisplaySleep | `org.freedesktop.ScreenSaver.Inhibit`, auf XFCE xfce4-power-manager, sonst `org.gnome.SessionManager` (Flag idle) |
| `-s` | PreventSystemSleep (nur am Netzteil) | logind-Inhibitor `sleep`, nur am Netz |
| `-u` | UserIsActive (Standard-Timeout 5 s) | `ScreenSaver.SimulateUserActivity` + wie `-d` |
| `-m` | PreventDiskIdle | kein Gegenstück, ohne Wirkung |

Ohne Session-Bus (TTY, SSH) oder ohne passenden Desktop-Dienst fallen `-i`, `-d` und `-u` auf den
logind-Inhibitor `idle` zurück. Desktop-Dienste werden nie per D-Bus-Aktivierung gestartet, es werden
nur laufende genutzt.

In einer X11-Sitzung setzen `-d` und `-u` zusätzlich den Bildschirmschoner und DPMS des X-Servers aus
(Erweiterung MIT-SCREEN-SAVER). Das wirkt auch ganz ohne Desktop-Dienst, etwa unter i3, dwm oder bspwm,
und endet mit der Sperre. `-u` beendet dabei einen laufenden Bildschirmschoner und schaltet den
Bildschirm ein. Programme, die dem Bildschirmschoner des Servers folgen (xss-lock), bleiben damit
ebenfalls ruhig. Programme, die die Leerlaufzeit selbst messen (xautolock, xidlehook, xscreensaver),
bemerken davon nichts. Unter Wayland wird der X-Server nicht benutzt.

Startet ein Desktop-Dienst neu (etwa PowerDevil nach einem Absturz), ist die Sperre dort verloren.
caffeinate bemerkt das und legt sie neu an. Solange der Dienst fehlt, hält logind eine Ersatzsperre.
Dasselbe gilt, wenn der Dienst erst nach caffeinate startet oder der Session-Bus endet. Lässt sich
eine verlorene Sperre nirgends ersetzen, meldet caffeinate das auf stderr, läuft weiter, versucht es
jede Minute erneut und meldet auch, wenn die Sperre wieder da ist.

Ob der Rechner am Netz hängt, entscheidet `-s` wie systemd: Eine Stromquelle meldet `online`, oder
kein Akku entlädt sich. Ein USB-C-Port, der selbst ein Gerät speist, zählt nicht als Stromquelle. Der
Status wird alle 5 Sekunden geprüft.

D-Bus und X11 spricht caffeinate direkt auf dem Socket, libdbus und Xlib werden nicht gebraucht.

## Verhalten wie auf macOS

Wo Apples Manpage und Apples Code sich widersprechen, folgt dieser Nachbau dem Code:

- Ohne Assertion-Flag gilt `-i`. `-h` gibt die Usage aus (Exit 0).
- `-t` und `-w` werden mit `strtol(…, 0)` gelesen: `0x10` = 16, `10abc` = 10, `abc` = Fehler.
- `-t 0` heißt „kein Timeout“.
- `-t` mit command: Die Sperren enden nach dem Timeout, das command läuft weiter. Die Manpage sagt dagegen „ignoriert“.
- `-u` ohne `-t`: Die Sperre endet nach 5 s, caffeinate läuft aber weiter, bis es beendet wird.
- `-w` mit nicht existierender PID beendet sofort. `-w` wird ignoriert, wenn ein command angegeben ist.
- Mit command wird caffeinate per exec selbst zum command. PID, Exit-Status und Signale sind die des commands. Ein Hilfsprozess hält die Sperren, ignoriert SIGINT und SIGQUIT und endet mit dem command.
- Ignorierte Signale bleiben ignoriert: `nohup caffeinate …` übersteht das Schließen des Terminals.
- Schlägt `fork` fehl, läuft das command trotzdem, nur ohne Sperren.
- Lässt sich eine Sperre nicht anlegen: `Failed to create <Typ> assertion`, Exit 1. Davor steht eine Zeile mit dem Grund.
- Die Meldungen sind im BSD-Wortlaut: `illegal option -- x`, `option requires an argument -- t`.

## Desktops und Systeme

| Umgebung | Status |
|---|---|
| KDE Plasma | getestet; PowerDevil setzt eine Sperre erst 5 s nach der Anforderung durch |
| GNOME, Cinnamon, MATE | über `org.gnome.SessionManager`; `-d` unter GNOME und Cinnamon über den ScreenSaver-Proxy des settings-daemon (gegen Mock-Dienst getestet) |
| XFCE | über xfce4-power-manager, auch wenn er erst später startet; ohne ihn der Bildschirmschoner (gegen Mock-Dienst getestet). Dort hält auch `-i` den Bildschirm an |
| i3, dwm, bspwm, Openbox … (X11 ohne Desktop-Dienst) | `-d` und `-u` über den X-Server (gegen Xvfb getestet), `-i` über logind |
| Hyprland (hypridle) | über ScreenSaver-D-Bus bzw. logind |
| sway, river, niri … (swayidle ab 1.7) | über logind `idle`, sofern swayidle mit logind verbunden ist (eines von `before-sleep`, `after-resume`, `lock`, `unlock`, `idlehint` konfiguriert) |
| TTY / SSH | logind `idle` bzw. `sleep`; das wirkt auf logind selbst und auf Dienste, die dessen Sperren beachten |
| systemd oder elogind | beide (Artix, Devuan, Void, Alpine, Gentoo/OpenRC) |
| ohne logind | ConsoleKit2, gleiche Schnittstelle (gegen Mock-Dienst getestet) |
| glibc oder musl | beide |
| Architektur | beliebig; x86_64 und aarch64 laufen mit allen Tests in der CI, big-endian (s390x, mips, ppc64) wurde unter qemu geprüft |
| Container, User-Namespace | Anmeldung am Bus auch mit abweichender uid |
| Kernel | vorausgesetzt wird nichts, was neuer ist als 2.6.27; pidfd (ab 5.3) wird genutzt, wo vorhanden, sonst PDEATHSIG und Polling (auf so alten Kerneln nicht getestet) |

`-u` weckt den Bildschirm dort, wo `SimulateUserActivity` existiert (KDE, xfce4-screensaver), und in
X11-Sitzungen über den X-Server. Unter GNOME auf Wayland wirkt es nur als Display-Sperre.

`-s` braucht die Erlaubnis von logind (polkit-Aktion `org.freedesktop.login1.inhibit-block-sleep`). In
der Standardeinstellung haben sie root und Nutzer mit lokaler Sitzung. Wer nur über SSH angemeldet ist,
müsste ein Admin-Passwort eingeben, `-s` schlägt dann fehl. Die `idle`-Sperre von `-i`, `-d` und `-u`
darf jeder anlegen, solange polkit überhaupt läuft.

Die `idle`-Sperre bei logind beachten nicht alle: Steht ein Rechner am GNOME-Anmeldebildschirm, legt
ihn dessen Energieverwaltung trotzdem schlafen. Wer dort über SSH arbeitet, braucht `-s` als root.

## Ohne caffeinate: Bordmittel

Linux bringt Werkzeuge mit, die dieselben Sperren anlegen. Auf einem einzelnen Rechner reichen sie oft
aus. caffeinate fasst sie unter einer Befehlszeile zusammen, die auf jedem Desktop und auf dem Mac
gleich ist.

### Sperre, solange ein Befehl läuft

```sh
# systemd; unter elogind heißt das Programm elogind-inhibit
systemd-inhibit --what=idle make                           # die Sperre von caffeinate -i ohne Desktop
systemd-inhibit --what=sleep make                          # wie caffeinate -s, aber auch im Akkubetrieb
systemd-inhibit --what=idle sleep infinity                 # ohne Befehl, bis Strg+C
systemd-inhibit --what=idle sleep 3600                     # wie -t 3600
systemd-inhibit --what=idle tail --pid=4711 -f /dev/null   # wie -w 4711 (GNU tail)
systemd-inhibit --list                                     # zeigt alle Sperren bei logind

# KDE Plasma
kde-inhibit --power make                                   # wie caffeinate -i
kde-inhibit --screenSaver make                             # wie caffeinate -d

# GNOME; unter MATE mate-session-inhibit mit denselben Werten für --inhibit
gnome-session-inhibit --inhibit suspend make               # wie caffeinate -i
gnome-session-inhibit --inhibit idle make                  # wie caffeinate -d
gnome-session-inhibit --inhibit-only                       # ohne Befehl, bis Strg+C
```

Diese Sperren enden wie die von caffeinate mit dem Prozess. Drei Dinge sollte man wissen:

- `--what=idle` wirkt nicht überall gleich. Die Sperre gilt bei logind. KDE behandelt sie wie eine
  Sperre von `-d`, dort bleibt also auch der Bildschirm an; mit `--what=sleep` darf er wie bei `-i`
  abschalten (geprüft mit Plasma 6.7). GNOME beachtet sie am Anmeldebildschirm nicht, siehe oben. Auf
  einem Desktop ist dessen eigenes Werkzeug die sichere Wahl.
- `kde-inhibit` gibt den Exit-Status des Befehls nicht weiter, es endet immer mit 0 (kde-cli-tools 6.7).
  `systemd-inhibit` reicht ihn durch.
- Ein einzelner D-Bus-Aufruf hält nichts. `busctl call`, `gdbus call`, `dbus-send` und `qdbus` bekommen
  zwar ein Cookie zurück, die Sperre hängt aber an der Verbindung des Aufrufers und ist weg, sobald das
  Werkzeug endet (unter KDE mit `busctl` und `gdbus` geprüft).

### Nutzer als aktiv melden

```sh
xdg-screensaver reset                # meldet dem Bildschirmschoner Aktivität, wie caffeinate -u
xset s reset; xset dpms force on     # X11: dasselbe direkt beim X-Server, Bildschirm einschalten
```

### Schalter statt Sperre

Diese Einstellungen hängen an keinem Prozess. Sie gelten, bis man sie zurücknimmt, auch wenn das Skript
dazwischen abbricht.

```sh
xset s off -dpms                     # X11: Bildschirmschoner und DPMS aus, gilt bis zum Abmelden
xset s on +dpms                      # und wieder an
setterm --blank=0 --powerdown=0      # Textkonsole: der Bildschirm bleibt an

# XFCE: Präsentationsmodus von xfce4-power-manager, zurück mit -s false
xfconf-query -c xfce4-power-manager -p /xfce4-power-manager/presentation-mode -n -t bool -s true

# gar kein Ruhezustand mehr, auch kein ausdrücklich angeforderter; zurück mit unmask
sudo systemctl mask sleep.target suspend.target hibernate.target hybrid-sleep.target
```

Soll der Rechner nie von selbst schlafen, gehört das in die Energieeinstellungen des Desktops oder in
`/etc/systemd/logind.conf` (`IdleAction`, `HandleLidSwitch`).

### Was caffeinate dem hinzufügt

caffeinate nimmt einem die Wahl des Werkzeugs ab: Es fragt den Dienst, der auf dem laufenden Desktop
zuständig ist, und nimmt logind, wenn keiner da ist. Dazu kommen `-t` neben einem command, `-w`, `-s`
nur am Netzteil und das Neuanlegen der Sperre, wenn der Dienst neu startet. Skripte laufen unverändert
auf dem Mac.

## Windows

Windows 10 und 11, x64 und ARM64. Die Optionen sind dieselben, dazu kommt `-r`:

```
caffeinate [-dirsu] [-t timeout] [-w Process ID] [command arguments...]
```

```powershell
caffeinate                     # kein Standby, bis Strg+C
caffeinate -r                  # dazu kein Neustart durch Windows Update (Terminal als Administrator)
caffeinate -d -t 3600          # eine Stunde lang bleibt auch der Bildschirm an
caffeinate npm run build       # solange der Befehl läuft; npm.cmd wird über PATHEXT gefunden
caffeinate -w 4711             # bis Prozess 4711 endet
```

Damit Terminals ungestört weiterlaufen, bis man sie selbst beendet, reicht in einem Terminal mit
Administratorrechten `caffeinate -r`. Der Rechner geht nicht von selbst in den Standby, und
Windows Update startet ihn nicht neu. Strg+C oder das Schließen des Fensters beenden caffeinate,
danach gilt alles wieder wie vorher.

### Installieren

Das fertige `caffeinate-x86_64-windows.exe` bzw. `caffeinate-aarch64-windows.exe` aus den Releases
als `caffeinate.exe` in ein Verzeichnis im `PATH` legen. Es braucht keine DLLs außer denen von
Windows.

Selbst bauen, auf Windows, Linux oder macOS, mit [zig](https://ziglang.org) (getestet mit
0.15.2) und GNU make:

```sh
make windows          # caffeinate-x86_64-windows.exe und caffeinate-aarch64-windows.exe
make test-windows     # in Git Bash; als Administrator auch Power Requests und -r
```

Ohne make: `zig cc -target x86_64-windows-gnu -O2 -s -o caffeinate.exe caffeinate_win.c -luser32 -ladvapi32`.
Jeder andere mingw-w64-gcc oder -clang geht ebenso.

### Umsetzung

| Flag | macOS-Assertion | Windows |
|---|---|---|
| `-i` | PreventUserIdleSystemSleep (Standard) | Power Request `SystemRequired` |
| `-d` | PreventUserIdleDisplaySleep | Power Request `DisplayRequired`: der Bildschirm bleibt an |
| `-s` | PreventSystemSleep (nur am Netzteil) | `SystemRequired`, nur am Netz (alle 5 s geprüft) |
| `-u` | UserIsActive (Standard-Timeout 5 s) | eine Mausbewegung um 0 Pixel (setzt die Leerlaufzeit zurück, weckt den Bildschirm), dann wie `-d` |
| `-m` | PreventDiskIdle | kein Gegenstück, ohne Wirkung |
| `-r` | – | Windows Update pausieren und Nutzungszeit mitführen, siehe unten |

Die Power Requests zeigt `powercfg /requests` (als Administrator) mit dem Grund, etwa
`caffeinate asserting forever`. Sie hängen am Prozess und fallen weg, wenn caffeinate endet, auch
wenn es per Task-Manager beendet wird.

Ohne `-r` braucht caffeinate keine Administratorrechte.

Mit command startet caffeinate den Befehl und wartet auf ihn, denn Windows kennt kein `exec`. Der
Befehl bekommt den Rest der Befehlszeile unverändert, caffeinate endet mit seinem Exit-Code. Strg+C
geht an den Befehl, caffeinate selbst ignoriert es. Hat der Befehl keine Konsole (`caffeinate
notepad`), beendet Strg+C caffeinate, und das Programm läuft ohne Sperre weiter. Ohne command endet
caffeinate durch Strg+C mit dem Status, den Windows dafür vorsieht (`0xC000013A`).

Den Befehl sucht caffeinate wie PowerShell: ein Name ohne Verzeichnis nur entlang `PATH`, nicht
zuerst im aktuellen Verzeichnis, wie es cmd.exe täte. Ohne Endung probiert es die aus `PATHEXT`,
von denen aber nur `.exe`, `.com`, `.bat` und `.cmd`. Was dort nicht liegt, gilt als nicht gefunden
(Exit 127). Eine Batchdatei startet es mit genau der gefundenen Datei über `cmd.exe /d /s /c`; liegt
sie in einem Pfad mit `%`, verweigert caffeinate den Start, weil cmd den Pfad sonst verändern würde. cmd liest deren Argumente dabei ein zweites Mal: Wer
caffeinate aus cmd.exe aufruft und in Argumenten `&`, `|`, `<`, `>` oder `^` mit `^` schützt, muss sie
doppelt schützen oder in Anführungszeichen setzen. Aus PowerShell und Git Bash gilt dasselbe wie
beim direkten Aufruf der Batchdatei.

### Windows Update aufhalten (`-r`)

Für Windows Update gibt es keine Sperre, die an einem Prozess hängt. `-r` setzt deshalb zwei
Einstellungen und behandelt sie wie eine Sperre mit Ablaufzeit:

- **Pause:** dieselbe Pause wie unter Einstellungen › Windows Update, jeweils bis 24 Stunden im
  Voraus und alle 30 Minuten verlängert. Es wird kein neues Update installiert, also verlangt auch
  keines einen Neustart. Hat man selbst länger pausiert, bleibt die eigene Pause stehen.
- **Nutzungszeit:** Ein Update, das schon vor dem Start installiert war, startet den Rechner
  außerhalb der Nutzungszeit neu. Die Richtlinie dafür (`SetActiveHours`, `ActiveHoursStart`,
  `ActiveHoursEnd`) legt caffeinate von einer Stunde vor jetzt bis 17 Stunden danach. Mehr als
  18 Stunden erlaubt Windows nicht. Weil caffeinate das Fenster mitschiebt, ist es nie außerhalb der
  Nutzungszeit. Solange `-r` läuft, zeigt die Seite Windows Update „Einige Einstellungen werden von
  Ihrer Organisation verwaltet“.

Bevor caffeinate einen Wert zum ersten Mal ändert, sichert es den alten unter
`HKLM\SOFTWARE\caffeinate`. Am Ende schreibt es die alten Werte zurück, auch beim Abmelden,
Herunterfahren oder Neustart, und zwar je Gruppe (Nutzungszeit, Pause): Hat jemand anderes einen
Wert der Gruppe inzwischen geändert, bleibt die ganze Gruppe so, wie sie ist. Eine solche Änderung
während des Laufs, etwa „Updates fortsetzen“ in den Einstellungen, bemerkt caffeinate bei der nächsten
Verlängerung; es meldet sie und lässt die Gruppe ab dann in Ruhe. Nach dem Aufwachen aus dem Standby
und nach einer Zeitumstellung verlängert caffeinate sofort.

Wird caffeinate gewaltsam beendet, läuft die Pause nach spätestens 24 Stunden aus. Die Richtlinie
zur Nutzungszeit bleibt dagegen stehen, samt Hinweis auf die Verwaltung durch die Organisation, bis
zum nächsten `caffeinate -r`: Das stellt die alten Werte wieder her, ein kurzes `caffeinate -r -t 1`
räumt also auf.

Laufen mehrere `caffeinate -r`, verwaltet nur eines die Einstellungen, die anderen sagen, dass sie
warten. Endet es, auch gewaltsam, übernimmt ein anderes.

Grenzen:

- Microsoft dokumentiert die Richtlinie zur Nutzungszeit, aber keine Schnittstelle für die Pause.
  caffeinate schreibt dieselben Werte wie die Einstellungen-App. Ein Windows-Update könnte das
  ändern.
- Auf verwalteten Rechnern (Intune, Gruppenrichtlinie mit Fristen für Updates und Neustarts) setzt
  sich die Frist durch.
- Nach dem Ende holt Windows Update alles nach und startet außerhalb der Nutzungszeit neu.

### Was caffeinate unter Windows nicht verhindert

- Standby, den man selbst auslöst: Start › Ein/Aus › Energie sparen, den Netzschalter oder das
  Zuklappen des Laptops. Das gilt ebenso für `-i` auf Linux und dem Mac. Dagegen hilft nur eine Einstellung
  unter Systemsteuerung › Energieoptionen.
- Abmelden: Das tut Windows nicht von selbst. Ein gesperrter Bildschirm hält keine Programme an,
  Terminals laufen dahinter weiter. Mit `-d` bleibt der Bildschirm an, eine Sperre, die beim
  Ausschalten des Bildschirms kommt, bleibt damit aus. Ob ein Bildschirmschoner mit Anmeldung oder
  die Richtlinie „Computer: Inaktivitätsgrenze“ trotzdem sperrt, ist nicht geprüft.
- Neustarts, die ein Administrator oder ein Installer auslöst.

### Ohne caffeinate: Bordmittel

Diese Einstellungen hängen an keinem Prozess und gelten, bis man sie zurücknimmt:

```powershell
powercfg /change standby-timeout-ac 0    # am Netz nie Standby; zurück z. B. mit 30 (Minuten)
powercfg /change monitor-timeout-ac 0    # Bildschirm bleibt an
powercfg /requests                       # wer gerade Standby oder Bildschirm-Aus verhindert (Administrator)
```

Updates pausieren (bis zu fünf Wochen) und die Nutzungszeit (bis zu 18 Stunden) stellt man unter
Einstellungen › Windows Update ein. Microsofts PowerToys enthalten mit „Awake“ ein Werkzeug, das
den Rechner wach hält. Neustarts durch Updates hält es nicht auf.

### Tests unter Windows

`tests/windows.sh` läuft in Git Bash. Es prüft Optionen, Meldungen, Exit-Codes, den Start von
Befehlen (Anführungszeichen, `.cmd` über PATHEXT), `-t` und `-w`. Als Administrator prüft es
außerdem die Power Requests in `powercfg /requests` und `-r` in der Registry. Dazu gehören das
Zurücksetzen nach Ende und nach `taskkill /F`, die Übergabe zwischen zwei `caffeinate -r` (auch
wenn das erste gewaltsam endet), eine längere eigene Pause und eine während des Laufs geänderte
Nutzungszeit. Bricht der Test ab, setzt er seine eigenen Änderungen zurück. Die CI baut die Binaries mit zig unter Linux und testet sie auf Windows x64 und
ARM64 mit Administratorrechten.

## Tests

`make test` startet drei Skripte:

- `tests/smoke.sh` prüft Optionen, Meldungen und Exit-Codes. Es braucht keinen D-Bus und legt auch
  auf einem Desktop keine echten Sperren an.
- `tests/dbus.sh` startet einen privaten dbus-daemon mit Mock-Diensten und prüft, welche Sperre wo
  angelegt und wieder freigegeben wird: KDE, XFCE, GNOME, logind, ConsoleKit2, Neustart eines Dienstes,
  Ende des Session-Busses, ignorierte Signale, Netzteil-Erkennung. Es braucht `dbus-daemon` und python3 mit PyGObject, für
  die Netzteil-Tests außerdem `unshare`. Was fehlt, wird übersprungen. Der echte Session- und System-Bus
  bleiben unberührt.
- `tests/x11.sh` startet Xvfb mit Cookie-Anmeldung und prüft, dass `-d` den Bildschirmschoner des
  X-Servers aussetzt und `-u` ihn beendet. Es braucht `Xvfb` und python3.

Die CI (`.github/workflows/build.yml`) baut und testet auf Arch, Debian stable/oldstable, Ubuntu
22.04, Fedora (clang), Rocky Linux 8, openSUSE Tumbleweed und Alpine (musl). Zusätzlich baut sie das
statische Binary für x86_64 und aarch64, lässt alle Tests damit laufen und startet es auf weiteren
Distributionen. Bei einem Tag `v*` hängt sie die Binaries an das Release.

Von Hand geprüft wurden außerdem NixOS 26.05 (Flake-Build, alle Tests, echtes logind ohne polkit)
und Void Linux mit glibc und musl (alle Tests, echtes elogind mit polkit), jeweils im Container.

Zur Fehlersuche: `CAFFEINATE_DEBUG=1 caffeinate …`

## Aufbau des Codes

Der Quelltext ist auf Englisch kommentiert. Am Anfang von `caffeinate.c` steht, was das Programm tut
und nach welchen Mustern es gebaut ist:

- Eine Sperre hängt immer an einer offenen Verbindung (Session-Bus, logind-fd, X-Server) und fällt
  mit ihr weg. Es kann deshalb keine Sperre zurückbleiben, auch nicht nach `kill -9`.
- Die Desktop-Dienste stehen als Tabellen mit Rangfolge im Code. Der erste laufende Dienst bekommt
  die Sperre, logind ist der Ersatz dahinter.
- Ein Thread, eine `poll()`-Schleife: Signale, Prozessende, Bus und X-Server kommen als
  Dateideskriptoren an, alle Fristen sind absolute Zeitpunkte.
- Ereignisse markieren eine Sperre nur als veraltet. Ein eigener Schritt gleicht danach ab, was
  gewünscht ist und was tatsächlich gehalten wird.

Darauf folgen die Abschnitte Assertions, Helpers, D-Bus, X11, Power supply, Acquire and release und
main, jeweils mit eigener Einleitung. Die Meldungen des Programms bleiben deutsch.

`caffeinate_win.c` ist die Windows-Fassung mit demselben Aufbau. Eine Sperre ist ein Power Request,
der an einem Handle hängt. Gewartet wird in einer einzigen `MsgWaitForMultipleObjects`-Schleife.
`-r` ist mit Ablaufzeit, Sicherung und einem benannten Mutex als Sperre nachgebaut. `caffeinate.h`
enthält, was beide teilen: die Assertion-Typen, die Usage-Zeile und das Einlesen der Optionen.

## Lizenz

MIT, siehe [LICENSE](LICENSE). Der Code ist eine eigene Implementierung. Apples caffeinate.c
(APSL 2.0) diente als Verhaltensreferenz, daraus wurde kein Code übernommen.
