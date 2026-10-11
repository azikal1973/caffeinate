#!/usr/bin/env python3
# Mocks of the D-Bus services that caffeinate talks to (for tests/dbus.sh).
#
#   mockbus.py LOGFILE SERVICE...
#
# Connects to the bus named by DBUS_SESSION_BUS_ADDRESS and writes every call
# as one line to the log file. Services:
#   pm         org.freedesktop.PowerManagement (PowerDevil of KDE)
#   xfpm       the same, also under org.xfce.PowerManager (xfce4-power-manager)
#   ss         org.freedesktop.ScreenSaver at both paths
#   ss-legacy  org.freedesktop.ScreenSaver at /ScreenSaver only
#   ss-broken  like ss, but Inhibit answers with an error
#   xfss       org.xfce.ScreenSaver
#   gsm        org.gnome.SessionManager (GNOME, Cinnamon, MATE)
#   login1     org.freedesktop.login1 (Inhibit returns an fd)
#   login1-denied  like login1, but Inhibit is refused (logind without polkit)
#   ck         org.freedesktop.ConsoleKit (likewise)
#   noisy      not a service: ahead of every reply, unrelated signals to the caller, a
#              small one and an oversized one, both carrying fds
# MOCK_COOKIE sets the first cookie that is handed out (default 1).
import itertools
import os
import sys
import warnings

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib  # noqa: E402

# newer PyGObject versions deprecate register_object, older ones know nothing else
warnings.simplefilter("ignore", DeprecationWarning)

FDO_XML = """<node><interface name='%s'>
  <method name='Inhibit'>
    <arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='u' direction='out'/>
  </method>
  <method name='UnInhibit'><arg type='u' direction='in'/></method>
  <method name='SimulateUserActivity'/>
</interface></node>"""

PM_XML = """<node><interface name='%s'>
  <method name='Inhibit'>
    <arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='u' direction='out'/>
  </method>
  <method name='UnInhibit'><arg type='u' direction='in'/></method>
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

# service -> (bus names, interface, XML, paths)
SERVICES = {
    "pm": (FDO_PM, FDO_PM + ".Inhibit", PM_XML, ["/org/freedesktop/PowerManagement/Inhibit"]),
    "xfpm": ("org.xfce.PowerManager " + FDO_PM, FDO_PM + ".Inhibit", PM_XML,
             ["/org/freedesktop/PowerManagement/Inhibit"]),
    "ss": (FDO_SS, FDO_SS, FDO_XML, ["/org/freedesktop/ScreenSaver", "/ScreenSaver"]),
    "ss-legacy": (FDO_SS, FDO_SS, FDO_XML, ["/ScreenSaver"]),
    "ss-broken": (FDO_SS, FDO_SS, FDO_XML, ["/org/freedesktop/ScreenSaver", "/ScreenSaver"]),
    "xfss": (XFCE_SS, XFCE_SS, FDO_XML, ["/org/xfce/ScreenSaver"]),
    "gsm": ("org.gnome.SessionManager", "org.gnome.SessionManager", GSM_XML,
            ["/org/gnome/SessionManager"]),
    "login1": ("org.freedesktop.login1", "org.freedesktop.login1.Manager", SEAT_XML,
               ["/org/freedesktop/login1"]),
    "login1-denied": ("org.freedesktop.login1", "org.freedesktop.login1.Manager", SEAT_XML,
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


def noise(conn, destination, text, fd_count):
    message = Gio.DBusMessage.new_signal("/mock", "org.example.Mock", "Noise")
    message.set_destination(destination)
    message.set_body(GLib.Variant("(s)", (text,)))
    fds = Gio.UnixFDList.new()
    for _ in range(fd_count):
        r, w = os.pipe()
        fds.append(w)
        os.close(r)
        os.close(w)
    message.set_unix_fd_list(fds)
    conn.send_message(message, Gio.DBusSendMessageFlags.NONE)


def make_handler(tag):
    def handler(conn, sender, path, _iface, method, params, invocation):
        args = params.unpack()
        if noisy:
            noise(conn, sender, "x", 2)
            noise(conn, sender, "x" * 20000, 4)
        if tag == "login1-denied":
            out(tag, "Inhibit-abgelehnt", args[0])
            invocation.return_dbus_error("org.freedesktop.DBus.Error.AccessDenied", "Permission denied")
        elif tag in ("login1", "ck"):
            # like logind: the caller gets the write end, and we see when it goes away
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
    names, iface, xml, paths = SERVICES[tag]
    info = Gio.DBusNodeInfo.new_for_xml(xml % iface).interfaces[0]
    for path in paths:
        conn.register_object(path, info, make_handler(tag), None, None)
    names = names.split()
    for name in names:
        # "ready" only once the last name has been acquired
        Gio.bus_own_name_on_connection(
            conn, name, Gio.BusNameOwnerFlags.NONE,
            (lambda _c, _n, tag=tag: out("ready", tag)) if name == names[-1] else None, None)
GLib.MainLoop().run()
