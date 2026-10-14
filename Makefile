# caffeinate - build, test, install. Needs a C compiler and GNU make, nothing else.
#
#   make             build ./caffeinate (hardened, dynamically linked)
#   make static      static binary that runs on every distribution
#   make test        smoke, D-Bus and X11 tests (see tests/)
#   make asan        build with AddressSanitizer and UBSan
#   make windows     caffeinate-{x86_64,aarch64}-windows.exe, cross-compiled with zig
#   make test-windows Windows test (Git Bash on Windows; elevated: everything)
#   make install     install binary and man page under PREFIX (default ~/.local)
#   make uninstall   remove them again

VERSION = 0.3.0.0

PREFIX  ?= $(HOME)/.local
BINDIR  ?= $(PREFIX)/bin
MANDIR  ?= $(PREFIX)/share/man/man8

CC      ?= cc

# Hardening by default. Whoever sets CFLAGS and LDFLAGS (distro packages)
# brings their own. _FORTIFY_SOURCE is set inside caffeinate.c.
# gcc only knows -fstack-protector-strong since 4.9, hence the probe.
HARDEN   = $(shell $(CC) -fstack-protector-strong -E -x c /dev/null >/dev/null 2>&1 && echo -fstack-protector-strong)
CFLAGS  ?= -O2 -g $(HARDEN)
LDFLAGS ?= -pie -Wl,-z,relro,-z,now

# override: added even to CFLAGS=... from the command line (distro packages, asan).
# -fPIE belongs with -pie, even when only CFLAGS were replaced.
override CFLAGS += -std=c11 -Wall -Wextra -Wpedantic $(if $(filter -pie -static-pie,$(LDFLAGS)),-fPIE)

all: caffeinate

# explicit rule: with make -B the built-in .c -> .o -> binary chain would apply otherwise
caffeinate: caffeinate.c caffeinate.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

# -B: always rebuild, even if a regular binary is already there
asan:
	$(MAKE) -B caffeinate CFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
		-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0" LDFLAGS="-fsanitize=address,undefined"

# One binary for every distribution; needs the static libc (musl or glibc-static).
# Built as static-pie where the toolchain can do that, plain static otherwise.
static:
	$(MAKE) -B caffeinate CFLAGS="-O2 $(HARDEN)" LDFLAGS="-static-pie -s -Wl,-z,relro,-z,now" || \
	$(MAKE) -B caffeinate CFLAGS="-O2" LDFLAGS="-static -s"

test: caffeinate
	sh tests/smoke.sh ./caffeinate
	sh tests/dbus.sh ./caffeinate
	sh tests/x11.sh ./caffeinate

# Windows, from any host: zig brings compiler, linker and mingw-w64 along.
# lld sets ASLR (high entropy) and DEP by default. -s: no debug info, no .pdb.
ZIG        ?= zig
WIN_ARCHS   = x86_64 aarch64
WIN_CFLAGS ?= -O2

windows: $(WIN_ARCHS:%=caffeinate-%-windows.exe)

caffeinate-%-windows.exe: caffeinate_win.c caffeinate.h
	$(ZIG) cc -target $*-windows-gnu $(WIN_CFLAGS) -std=c11 -Wall -Wextra -Wpedantic -s \
		-o $@ caffeinate_win.c -luser32 -ladvapi32

test-windows: caffeinate-x86_64-windows.exe
	sh tests/windows.sh ./caffeinate-x86_64-windows.exe

install: caffeinate
	install -Dm755 caffeinate $(DESTDIR)$(BINDIR)/caffeinate
	install -Dm644 caffeinate.8 $(DESTDIR)$(MANDIR)/caffeinate.8

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/caffeinate $(DESTDIR)$(MANDIR)/caffeinate.8

clean:
	rm -f caffeinate caffeinate-*-windows.exe caffeinate-*-windows.pdb

.PHONY: all asan static test windows test-windows install uninstall clean
