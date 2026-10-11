#!/usr/bin/env python3
# Hilfsprogramm fuer tests/x11.sh. Spricht X11 direkt auf dem Socket, braucht
# also keine Xlib.
#
#   x11check.py XAUTHDATEI mkauth              Cookie erzeugen, Datei schreiben
#   x11check.py XAUTHDATEI NUMMER timeout SEK  Timeout des Bildschirmschoners setzen
#   x11check.py XAUTHDATEI NUMMER activate     Bildschirmschoner sofort einschalten
#   x11check.py XAUTHDATEI NUMMER reset        ... und wieder aus (wie eine Eingabe)
#   x11check.py XAUTHDATEI NUMMER state        gibt on, off oder disabled aus
import os
import socket
import struct
import sys

COOKIE = b"MIT-MAGIC-COOKIE-1"


def pad(data):
    return data + b"\0" * (-len(data) % 4)


def read_cookie(path):
    data = open(path, "rb").read()
    fields, pos = [], 2  # Familie ueberspringen
    for _ in range(4):
        (length,) = struct.unpack_from(">H", data, pos)
        fields.append(data[pos + 2:pos + 2 + length])
        pos += 2 + length
    return fields[3]


class Display:
    def __init__(self, number, cookie):
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.settimeout(5)
        self.sock.connect("/tmp/.X11-unix/X" + number)
        self.sock.sendall(struct.pack("<BxHHHHxx", 0x6C, 11, 0, len(COOKIE), len(cookie))
                          + pad(COOKIE) + pad(cookie))
        status, _, _, _, extra = struct.unpack("<BBHHH", self.recv(8))
        body = self.recv(extra * 4)
        if status != 1:
            sys.exit("X-Server lehnt die Verbindung ab")
        (vendor,) = struct.unpack_from("<H", body, 16)
        formats = body[21]
        (self.root,) = struct.unpack_from("<I", body, 32 + len(pad(b"x" * vendor)) + 8 * formats)

    def recv(self, count):
        data = b""
        while len(data) < count:
            chunk = self.sock.recv(count - len(data))
            if not chunk:
                sys.exit("X-Server hat die Verbindung beendet")
            data += chunk
        return data

    def reply(self):
        while True:
            packet = self.recv(32)
            if packet[0] == 0:
                sys.exit("X-Fehler %d" % packet[1])
            if packet[0] == 1:
                (extra,) = struct.unpack_from("<I", packet, 4)
                return packet + self.recv(extra * 4)

    def sync(self):
        self.sock.sendall(struct.pack("<BxH", 43, 1))  # GetInputFocus
        self.reply()

    def extension(self, name):
        self.sock.sendall(struct.pack("<BxHHxx", 98, 2 + len(pad(name)) // 4, len(name)) + pad(name))
        answer = self.reply()
        if not answer[8]:
            sys.exit("X-Server ohne %s" % name.decode())
        return answer[9]


def main():
    path = sys.argv[1]
    if sys.argv[2] == "mkauth":
        cookie = os.urandom(16)
        # Familie 65535 (jeder Rechner), ohne Adresse und Displaynummer
        with open(path, "wb") as f:
            f.write(struct.pack(">HHHH", 65535, 0, 0, len(COOKIE)) + COOKIE
                    + struct.pack(">H", len(cookie)) + cookie)
        return
    display = Display(sys.argv[2], read_cookie(path))
    command = sys.argv[3]
    if command == "timeout":
        # SetScreenSaver: Timeout, Intervall, prefer-blanking = Default, allow-exposures = Default
        display.sock.sendall(struct.pack("<BxHhhBBxx", 107, 3, int(sys.argv[4]), 0, 2, 2))
        display.sync()
    elif command in ("activate", "reset"):
        display.sock.sendall(struct.pack("<BBH", 115, command == "activate", 1))  # ForceScreenSaver
        display.sync()
    elif command == "state":
        major = display.extension(b"MIT-SCREEN-SAVER")
        display.sock.sendall(struct.pack("<BBHI", major, 1, 2, display.root))  # QueryInfo
        print({0: "off", 1: "on", 2: "cycle", 3: "disabled"}[display.reply()[1]])
    else:
        sys.exit("unbekannter Befehl " + command)


main()
