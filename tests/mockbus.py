#!/usr/bin/env python3
# Mock der D-Bus-Dienste, die caffeinate anspricht (fuer tests/dbus.sh).
#
#   mockbus.py PROTOKOLL DIENST...
#
# Verbindet sich mit dem Bus aus DBUS_SESSION_BUS_ADDRESS und schreibt jeden
# Aufruf als Zeile ins Protokoll. Dienste:
#   pm         org.freedesktop.PowerManagement (KDE, XFCE)
#   ss         org.freedesktop.ScreenSaver unter beiden Pfaden
#   ss-legacy  org.freedesktop.ScreenSaver nur unter /ScreenSaver
#   ss-broken  wie ss, aber Inhibit antwortet mit einem Fehler
#   xfss       org.xfce.ScreenSaver
#   gsm        org.gnome.SessionManager
#   mate       org.mate.SessionManager
#   login1     org.freedesktop.login1 (Inhibit liefert einen fd)
#   ck         org.freedesktop.ConsoleKit (ebenso)
#   noisy      kein Dienst: vor jeder Antwort ein uebergrosses Signal an den Aufrufer
# MOCK_COOKIE legt das erste vergebene Cookie fest (Standard 1).
import itertools
import os
import sys

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib  # noqa: E402

FDO_XML = """<node><interface name='%s'>
  <method name='Inhibit'>
    <arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='u' direction='out'/>
  </method>
  <method name='UnInhibit'><arg type='u' direction='in'/></method>
  <method name='SimulateUserActivity'/>
</interface></node>"""

GSM_XML = """<node><interface name='%s'>
  <method name='Inhibit'>
    <arg type='s' direction='in'/><arg type='u' direction='in'/>
    <arg type='s' direction='in'/><arg type='u' direction='in'/><arg type='u' direction='out'/>
  </method>
  <method name='Uninhibit'><arg type='u' direction='in'/></method>
</interface></node>"""

SEAT_XML = """<node><interface name='%s'>
  <method name='Inhibit'>
    <arg type='s' direction='in'/><arg type='s' direction='in'/>
    <arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='h' direction='out'/>
  </method>
</interface></node>"""

FDO_SS = "org.freedesktop.ScreenSaver"
FDO_PM = "org.freedesktop.PowerManagement"
XFCE_SS = "org.xfce.ScreenSaver"

# Dienst -> (Busname, Schnittstelle, XML, Pfade)
SERVICES = {
    "pm": (FDO_PM, FDO_PM + ".Inhibit", FDO_XML, ["/org/freedesktop/PowerManagement/Inhibit"]),
    "ss": (FDO_SS, FDO_SS, FDO_XML, ["/org/freedesktop/ScreenSaver", "/ScreenSaver"]),
    "ss-legacy": (FDO_SS, FDO_SS, FDO_XML, ["/ScreenSaver"]),
    "ss-broken": (FDO_SS, FDO_SS, FDO_XML, ["/org/freedesktop/ScreenSaver", "/ScreenSaver"]),
    "xfss": (XFCE_SS, XFCE_SS, FDO_XML, ["/org/xfce/ScreenSaver"]),
    "gsm": ("org.gnome.SessionManager", "org.gnome.SessionManager", GSM_XML,
            ["/org/gnome/SessionManager"]),
    "mate": ("org.mate.SessionManager", "org.mate.SessionManager", GSM_XML,
             ["/org/mate/SessionManager"]),
    "login1": ("org.freedesktop.login1", "org.freedesktop.login1.Manager", SEAT_XML,
               ["/org/freedesktop/login1"]),
    "ck": ("org.freedesktop.ConsoleKit", "org.freedesktop.ConsoleKit.Manager", SEAT_XML,
           ["/org/freedesktop/ConsoleKit/Manager"]),
}

log = open(sys.argv[1], "a", buffering=1, encoding="utf-8", errors="replace")
wanted = sys.argv[2:]
noisy = "noisy" in wanted
cookies = itertools.count(int(os.environ.get("MOCK_COOKIE", "1")))


def out(*words):
    log.write(" ".join(str(w) for w in words) + "\n")


def released(fd, _condition, tag, what):
    out(tag, "released", what)
    os.close(fd)
    return False


def make_handler(tag):
    def handler(conn, sender, path, _iface, method, params, invocation):
        args = params.unpack()
        if noisy:
            conn.emit_signal(sender, "/mock", "org.example.Mock", "Noise",
                             GLib.Variant("(s)", ("x" * 20000,)))
        if tag in ("login1", "ck"):
            # wie logind: der Aufrufer bekommt das Schreibende, wir sehen sein Ende
            what, who, why, mode = args
            r, w = os.pipe()
            fds = Gio.UnixFDList.new()
            index = fds.append(w)
            os.close(w)
            GLib.io_add_watch(r, GLib.PRIORITY_DEFAULT, GLib.IO_HUP | GLib.IO_ERR, released, tag, what)
            out(tag, "Inhibit", "what=" + what, "who=" + who, "mode=" + mode, "why=" + why)
            invocation.return_value_with_unix_fd_list(GLib.Variant("(h)", (index,)), fds)
        elif method == "Inhibit" and tag == "ss-broken":
            out(tag, path, "Inhibit-Fehler")
            invocation.return_dbus_error("org.example.Mock.Broken", "kaputt")
        elif method == "Inhibit":
            cookie = next(cookies)
            if len(args) == 4:
                app, xid, why, flags = args
                out(tag, path, "Inhibit", "cookie=%d" % cookie, "app=" + app, "xid=%d" % xid,
                    "flags=%d" % flags, "reason=" + why)
            else:
                app, why = args
                out(tag, path, "Inhibit", "cookie=%d" % cookie, "app=" + app, "reason=" + why)
            invocation.return_value(GLib.Variant("(u)", (cookie,)))
        else:
            out(tag, path, method, *args)
            invocation.return_value(None)
    return handler


conn = Gio.bus_get_sync(Gio.BusType.SESSION, None)
for tag in wanted:
    if tag == "noisy":
        continue
    name, iface, xml, paths = SERVICES[tag]
    info = Gio.DBusNodeInfo.new_for_xml(xml % iface).interfaces[0]
    for path in paths:
        conn.register_object(path, info, make_handler(tag), None, None)
    Gio.bus_own_name_on_connection(conn, name, Gio.BusNameOwnerFlags.NONE,
                                   lambda _c, _n, tag=tag: out("ready", tag), None)
GLib.MainLoop().run()
