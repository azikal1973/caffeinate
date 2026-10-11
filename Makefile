VERSION = 0.2.0.0

PREFIX  ?= $(HOME)/.local
BINDIR  ?= $(PREFIX)/bin
MANDIR  ?= $(PREFIX)/share/man/man8

CC      ?= cc

# Haertung als Vorgabe. Wer CFLAGS und LDFLAGS selbst setzt (Distro-Pakete),
# bringt seine eigene mit. _FORTIFY_SOURCE setzt caffeinate.c.
# -fstack-protector-strong kennt gcc erst ab 4.9, deshalb die Probe.
HARDEN   = $(shell $(CC) -fstack-protector-strong -E -x c /dev/null >/dev/null 2>&1 && echo -fstack-protector-strong)
CFLAGS  ?= -O2 -g $(HARDEN)
LDFLAGS ?= -pie -Wl,-z,relro,-z,now

# override: auch bei CFLAGS=... von der Kommandozeile (Distro-Pakete, asan).
# -fPIE gehoert zu -pie, auch wenn nur CFLAGS ersetzt wurden.
override CFLAGS += -std=c11 -Wall -Wextra -Wpedantic $(if $(filter -pie -static-pie,$(LDFLAGS)),-fPIE)

all: caffeinate

# explizit: bei make -B wuerde sonst die eingebaute .c->.o->Binary-Kette greifen
caffeinate: caffeinate.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

# -B: immer neu bauen, auch wenn schon ein normales Binary daliegt
asan:
	$(MAKE) -B caffeinate CFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
		-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0" LDFLAGS="-fsanitize=address,undefined"

# ein Binary fuer jede Distribution; braucht die statische libc (musl oder glibc-static).
# Als static-pie, wo die Toolchain das kann, sonst klassisch statisch.
static:
	$(MAKE) -B caffeinate CFLAGS="-O2 $(HARDEN)" LDFLAGS="-static-pie -s -Wl,-z,relro,-z,now" || \
	$(MAKE) -B caffeinate CFLAGS="-O2" LDFLAGS="-static -s"

test: caffeinate
	sh tests/smoke.sh ./caffeinate
	sh tests/dbus.sh ./caffeinate
	sh tests/x11.sh ./caffeinate

install: caffeinate
	install -Dm755 caffeinate $(DESTDIR)$(BINDIR)/caffeinate
	install -Dm644 caffeinate.8 $(DESTDIR)$(MANDIR)/caffeinate.8

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/caffeinate $(DESTDIR)$(MANDIR)/caffeinate.8

clean:
	rm -f caffeinate

.PHONY: all asan static test install uninstall clean
