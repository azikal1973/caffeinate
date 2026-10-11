VERSION = 0.2.0.0

PREFIX  ?= $(HOME)/.local
BINDIR  ?= $(PREFIX)/bin
MANDIR  ?= $(PREFIX)/share/man/man8

CC      ?= cc
CFLAGS  ?= -O2 -g

# override: auch bei CFLAGS=... von der Kommandozeile (Distro-Pakete, asan)
override CFLAGS += -std=c11 -Wall -Wextra -Wpedantic

all: caffeinate

# explizit: bei make -B wuerde sonst die eingebaute .c->.o->Binary-Kette greifen
caffeinate: caffeinate.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

# -B: immer neu bauen, auch wenn schon ein normales Binary daliegt
asan:
	$(MAKE) -B caffeinate CFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer" \
		LDFLAGS="-fsanitize=address,undefined"

# ein Binary fuer jede Distribution; braucht die statische libc (musl oder glibc-static)
static:
	$(MAKE) -B caffeinate CFLAGS="-O2" LDFLAGS="-static -s"

test: caffeinate
	sh tests/smoke.sh ./caffeinate
	sh tests/dbus.sh ./caffeinate

install: caffeinate
	install -Dm755 caffeinate $(DESTDIR)$(BINDIR)/caffeinate
	install -Dm644 caffeinate.8 $(DESTDIR)$(MANDIR)/caffeinate.8

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/caffeinate $(DESTDIR)$(MANDIR)/caffeinate.8

clean:
	rm -f caffeinate

.PHONY: all asan static test install uninstall clean
