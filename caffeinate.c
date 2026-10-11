/*
 * caffeinate - Nachbau von macOS caffeinate(8) fuer Linux
 *
 *   caffeinate [-disu] [-t timeout] [-w Process ID] [command arguments...]
 *
 * Verhalten wie Apples caffeinate.c (PowerManagement), nicht nur wie die
 * Manpage - wo sich beide widersprechen, zaehlt der Code:
 *   - ohne Assertion-Flag gilt -i; -h gibt die Usage aus (Exit 0)
 *   - -t/-w werden mit strtol(..., 0) gelesen; -t 0 heisst "kein Timeout"
 *   - -t gilt pro Assertion; mit command laeuft caffeinate trotzdem bis
 *     zum Ende des command weiter
 *   - -u ohne -t: Assertion 5 s (die danach folgende -m ebenso), der
 *     Prozess selbst laeuft weiter
 *   - mit command wird der Elternprozess per exec zum command, ein Kind
 *     haelt die Assertions und ignoriert SIGINT/SIGQUIT
 *
 * Abbildung der macOS-Assertions:
 *   -i  PreventUserIdleSystemSleep  -> org.freedesktop.PowerManagement.Inhibit
 *                                      (GNOME/MATE: SessionManager, Flag suspend)
 *   -d  PreventUserIdleDisplaySleep -> org.freedesktop.ScreenSaver.Inhibit
 *                                      (XFCE: org.xfce.ScreenSaver,
 *                                      GNOME/MATE: SessionManager, Flag idle)
 *   -s  PreventSystemSleep (nur AC) -> logind-Inhibitor "sleep"
 *   -u  UserIsActive                -> ScreenSaver.SimulateUserActivity + wie -d
 *   -m  PreventDiskIdle             -> kein Gegenstueck, wird angenommen
 * Ohne Session-Bus oder ohne passenden Desktop-Dienst fallen -i/-d/-u auf
 * den logind-Inhibitor "idle" zurueck. logind heisst hier auch elogind;
 * ConsoleKit2 hat dieselbe Schnittstelle und wird danach versucht.
 *
 * Alle Sperren haengen an Bus-Verbindungen bzw. am logind-fd und fallen
 * weg, sobald der Prozess endet - auch bei kill -9. Startet ein
 * Desktop-Dienst neu und vergisst damit die Sperre, wird sie neu angelegt.
 *
 * Gebraucht wird nur die libc: D-Bus wird direkt auf dem Socket gesprochen.
 * Statisch gelinkt laeuft das Binary damit auf jeder Distribution.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* aeltere Kernel-Header (musl, RHEL 8) kennen die Nummern noch nicht */
#ifndef SYS_pidfd_open
# ifdef __alpha__
#  define SYS_pidfd_open 544
# else
#  define SYS_pidfd_open 434
# endif
#endif
#ifndef SYS_close_range
# ifdef __alpha__
#  define SYS_close_range 546
# else
#  define SYS_close_range 436
# endif
#endif

#define NAME            "caffeinate"
#define POWER_DIR       "/sys/class/power_supply"
#define AC_POLL_MS      5000	/* Netzteil-Abfrage fuer -s */
#define PID_POLL_MS     1000	/* -w ohne pidfd (Kernel < 5.3) */
#define CALL_TIMEOUT_MS 5000
#define SETTLE_MS       500	/* neu gestarteter Dienst: warten, bis er Aufrufe annimmt */
#define RETRY_MS        5000	/* ... und einmal nachfassen, falls er dann noch nicht so weit war */
#define FOREVER         (-1LL)
#define MAX_TIMEOUT_S   3155760000UL	/* 100 Jahre; mehr gilt als unendlich */

static bool debug;

#define DBG(...) do { if (debug) { fprintf(stderr, NAME ": " __VA_ARGS__); fputc('\n', stderr); } } while (0)

/* ---------------------------------------------------------------- Assertions */

enum { A_IDLE, A_DISPLAY, A_SYSTEM, A_USER, A_DISK, A_COUNT };

/* Reihenfolge wie assertionMap bei Apple - wichtig fuer den -u-Timeout */
static const char *const type_name[A_COUNT] = {
	"PreventUserIdleSystemSleep",
	"PreventUserIdleDisplaySleep",
	"PreventSystemSleep",
	"UserIsActive",
	"PreventDiskIdle",
};

struct backend {
	const char *dest, *path, *iface, *uninhibit;
	uint32_t gsm_flags;	/* 0: freedesktop-Signatur (ss), sonst SessionManager (susu) */
};

#define GSM_SUSPEND 4
#define GSM_IDLE    8
#define FDO_SS      "org.freedesktop.ScreenSaver"
#define XFCE_SS     "org.xfce.ScreenSaver"
#define FDO_PM      "org.freedesktop.PowerManagement"
#define GNOME_SM    "org.gnome.SessionManager", "/org/gnome/SessionManager", "org.gnome.SessionManager", "Uninhibit"
#define MATE_SM     "org.mate.SessionManager", "/org/mate/SessionManager", "org.mate.SessionManager", "Uninhibit"

static const struct backend display_backends[] = {
	{ FDO_SS, "/org/freedesktop/ScreenSaver", FDO_SS, "UnInhibit", 0 },
	{ FDO_SS, "/ScreenSaver", FDO_SS, "UnInhibit", 0 },	/* manche Dienste kennen nur diesen Pfad */
	{ XFCE_SS, "/org/xfce/ScreenSaver", XFCE_SS, "UnInhibit", 0 },
	{ GNOME_SM, GSM_IDLE },
	{ MATE_SM, GSM_IDLE },
	{ NULL, NULL, NULL, NULL, 0 }
};

/* PowerDevil (KDE) und xfce4-power-manager; nur KDE besitzt auch den Namen "...Inhibit" */
static const struct backend system_backends[] = {
	{ FDO_PM, "/org/freedesktop/PowerManagement/Inhibit", FDO_PM ".Inhibit", "UnInhibit", 0 },
	{ GNOME_SM, GSM_SUSPEND },
	{ MATE_SM, GSM_SUSPEND },
	{ NULL, NULL, NULL, NULL, 0 }
};

/* logind, elogind und ConsoleKit2: Inhibit(what, who, why, mode) -> fd */
static const struct {
	const char *dest, *path, *iface;
} seat_managers[] = {
	{ "org.freedesktop.login1", "/org/freedesktop/login1", "org.freedesktop.login1.Manager" },
	{ "org.freedesktop.ConsoleKit", "/org/freedesktop/ConsoleKit/Manager",
	  "org.freedesktop.ConsoleKit.Manager" },
};

struct assertion {
	bool active;
	bool stale;			/* Dienst neu gestartet: Sperre neu anlegen */
	long long expires;		/* CLOCK_MONOTONIC in ms oder FOREVER */
	const struct backend *be;	/* Session-Bus-Sperre */
	uint32_t cookie;
	int fd;				/* logind-Sperre */
};

static struct assertion as[A_COUNT];
static long long refresh_at = FOREVER;	/* wann stale-Sperren neu angelegt werden */
static bool retry;			/* danach noch einmal nachfassen */
static char details[160];
static char reason[256];		/* warum der letzte Aufruf scheiterte */

/* ---------------------------------------------------------------- Hilfen */

static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Ablaufzeitpunkt fuer einen Timeout in Sekunden; 0 = kein Timeout */
static long long expiry_after(long secs)
{
	if (secs <= 0 || (unsigned long)secs > MAX_TIMEOUT_S)
		return FOREVER;
	return now_ms() + (long long)secs * 1000;
}

static int sys_pidfd_open(pid_t pid)
{
	return (int)syscall(SYS_pidfd_open, pid, 0);
}

static void usage(void)
{
	fprintf(stderr, "usage: caffeinate [-disu] [-t timeout] [-w Process ID] [command arguments...]\n");
}

/* Grund fuer einen Fehlschlag merken; Text von Diensten kommt ungeprueft herein */
__attribute__((format(printf, 1, 2)))
static void fail(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(reason, sizeof reason, fmt, ap);
	va_end(ap);
	for (unsigned char *p = (unsigned char *)reason; *p; p++)
		if (*p < 0x20 || *p == 0x7F)
			*p = '?';
	DBG("%s", reason);
}

/*
 * strtol(s, NULL, 0) wie auf macOS: dort setzt strtol bei "keine Ziffer"
 * errno = EINVAL, glibc und musl nicht. Liefert false, wenn Apple hier
 * usage() aufruft.
 */
static bool mac_strtol(const char *s, long *out)
{
	char *end;

	errno = 0;
	*out = strtol(s, &end, 0);
	if (end == s)
		errno = EINVAL;
	return !(*out == 0 && errno != 0);
}

/*
 * D-Bus verlangt gueltiges UTF-8, der Bus trennt sonst die Verbindung. Der
 * Befehlsname kann beliebige Bytes enthalten (Latin-1-Dateinamen) und wird
 * in details ggf. mitten in einem Zeichen abgeschnitten: ungueltige Bytes
 * werden zu '?', ein abgeschnittenes Zeichen am Ende faellt weg. Geprueft
 * wird so streng wie aeltere libdbus (keine Surrogates, keine Nichtzeichen).
 */
static void utf8_sanitize(char *s)
{
	unsigned char *p = (unsigned char *)s;

	while (*p) {
		int n = *p < 0x80 ? 1 : (*p & 0xE0) == 0xC0 ? 2 :
		        (*p & 0xF0) == 0xE0 ? 3 : (*p & 0xF8) == 0xF0 ? 4 : 0;
		uint32_t cp = n == 1 ? *p : n == 2 ? *p & 0x1Fu : n == 3 ? *p & 0x0Fu : *p & 0x07u;
		int i;

		for (i = 1; i < n && (p[i] & 0xC0) == 0x80; i++)
			cp = (cp << 6) | (p[i] & 0x3Fu);
		if (n > 1 && i < n && p[i] == '\0') {
			*p = '\0';		/* abgeschnittenes Zeichen am Ende */
			return;
		}
		bool ok = n > 0 && i == n &&
		          (n == 1 || cp >= (n == 2 ? 0x80u : n == 3 ? 0x800u : 0x10000u)) &&
		          cp < 0x110000 && (cp & 0xFFFFF800u) != 0xD800 &&
		          (cp < 0xFDD0 || cp > 0xFDEF) && (cp & 0xFFFE) != 0xFFFE;
		if (ok) {
			p += n;
		} else {
			*p++ = '?';
		}
	}
}

/* Alle geerbten fds ausser 0-2 und keep schliessen (wie Apple im Kind). */
static int close_inherited_fds(int keep)
{
	if (keep >= 0 && keep != 3) {
		int moved = dup2(keep, 3);
		close(keep);
		if (moved < 0)
			return -1;
		keep = moved;
	}
	int first = keep >= 0 ? 4 : 3;

	if (syscall(SYS_close_range, first, ~0U, 0) == 0)
		return keep;

	DIR *d = opendir("/proc/self/fd");
	if (d) {
		int dfd = dirfd(d);
		struct dirent *e;
		while ((e = readdir(d))) {
			char *end;
			long fd = strtol(e->d_name, &end, 10);
			if (*end == '\0' && end != e->d_name && fd >= first && fd <= INT_MAX && fd != dfd)
				close((int)fd);
		}
		closedir(d);
	} else {
		long max = sysconf(_SC_OPEN_MAX);
		if (max < 0 || max > 65536)
			max = 65536;
		for (int fd = first; fd < max; fd++)
			close(fd);
	}
	return keep;
}

/* ---------------------------------------------------------------- D-Bus */

/*
 * Minimaler D-Bus-Client statt libdbus. Er kann genau das, was hier noetig
 * ist: Anmeldung per EXTERNAL, Methodenaufrufe mit Argumenten vom Typ s und
 * u, Antworten mit u oder h (fd per SCM_RIGHTS) und das Signal
 * NameOwnerChanged. Gesendet wird little-endian, gelesen beides.
 */

#define DBUS_NAME     "org.freedesktop.DBus"
#define DBUS_PATH     "/org/freedesktop/DBus"
#define NO_AUTO_START 2
#define MAX_FIELDS    (1u << 26)	/* Grenzen der Spezifikation */
#define MAX_BODY      (1u << 27)

enum { M_CALL = 1, M_RETURN, M_ERROR, M_SIGNAL };
enum { F_PATH = 1, F_IFACE, F_MEMBER, F_ERROR, F_REPLY_SERIAL, F_DEST, F_SENDER, F_SIGNATURE, F_UNIX_FDS };

struct bus {
	const char *what;		/* fuer Meldungen */
	int fd;				/* -1: nicht verbunden */
	bool tried;
	uint32_t serial;
	unsigned char in[4096];
	size_t have;			/* Bytes in in[] */
	size_t done;			/* Laenge der zuletzt gelieferten Nachricht */
	size_t skip;			/* Rest einer zu grossen Nachricht, die ueberlesen wird */
	int fds[4];			/* per SCM_RIGHTS empfangen, noch nicht abgeholt */
	int nfds;
};

static struct bus user_bus = { .what = "Session-Bus", .fd = -1 };
static struct bus sys_bus = { .what = "System-Bus", .fd = -1 };

/* empfangene Nachricht; die Zeiger gelten bis zum naechsten bus_next() */
struct message {
	int type;
	bool big;			/* big-endian */
	uint32_t reply_serial, unix_fds;
	const char *iface, *member, *error, *sender, *sig;
	const unsigned char *body;
	size_t body_len;
};

/* ausgehende Nachricht */
struct out {
	unsigned char buf[1024];
	size_t len;
	bool overflow;
};

static void name_changed(const char *name);

static uint32_t rd32(const unsigned char *p, bool big)
{
	return big ? (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]
	           : (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
}

static void set32(unsigned char *p, size_t v)
{
	p[0] = (unsigned char)v;
	p[1] = (unsigned char)(v >> 8);
	p[2] = (unsigned char)(v >> 16);
	p[3] = (unsigned char)(v >> 24);
}

static void put(struct out *o, const void *data, size_t n)
{
	if (n > sizeof o->buf - o->len) {
		o->overflow = true;
		return;
	}
	memcpy(o->buf + o->len, data, n);
	o->len += n;
}

static void put_align(struct out *o, size_t a)
{
	static const unsigned char zero[8];

	put(o, zero, (a - o->len % a) % a);
}

static void put_byte(struct out *o, unsigned char c)
{
	put(o, &c, 1);
}

static void put_u32(struct out *o, uint32_t v)
{
	unsigned char b[4];

	set32(b, v);
	put_align(o, 4);
	put(o, b, 4);
}

static void put_str(struct out *o, const char *s)	/* Typ s und o */
{
	size_t n = strlen(s);

	put_u32(o, (uint32_t)n);
	put(o, s, n + 1);
}

static void put_sig(struct out *o, const char *s)	/* Typ g */
{
	size_t n = strlen(s);

	put_byte(o, (unsigned char)n);
	put(o, s, n + 1);
}

static void put_field(struct out *o, unsigned char code, char type, const char *val)
{
	const char sig[2] = { type, '\0' };

	put_align(o, 8);
	put_byte(o, code);
	put_sig(o, sig);
	if (type == 'g')
		put_sig(o, val);
	else
		put_str(o, val);
}

static void bus_kill(struct bus *b)
{
	if (b->fd >= 0)
		close(b->fd);
	b->fd = -1;
	while (b->nfds > 0)
		if (b->fds[--b->nfds] >= 0)
			close(b->fds[b->nfds]);
	b->have = b->done = b->skip = 0;
}

/* Die n fds der eben verarbeiteten Nachricht aus der Schlange nehmen und schliessen */
static void bus_drop_fds(struct bus *b, uint32_t n)
{
	int k = n < (uint32_t)b->nfds ? (int)n : b->nfds;

	for (int i = 0; i < k; i++)
		if (b->fds[i] >= 0)
			close(b->fds[i]);
	memmove(b->fds, b->fds + k, (size_t)(b->nfds - k) * sizeof *b->fds);
	b->nfds -= k;
}

/* Auf fd warten, laengstens bis deadline; false bei Timeout */
static bool wait_fd(int fd, short events, long long deadline)
{
	for (;;) {
		long long left = deadline - now_ms();
		struct pollfd p = { fd, events, 0 };
		int r;

		if (left <= 0)
			return false;
		r = poll(&p, 1, left > INT_MAX ? INT_MAX : (int)left);
		if (r > 0)
			return true;
		if (r < 0 && errno != EINTR)
			return false;
	}
}

static bool bus_send(struct bus *b, const void *data, size_t len)
{
	long long deadline = now_ms() + CALL_TIMEOUT_MS;
	const unsigned char *p = data;

	while (len > 0) {
		ssize_t n = send(b->fd, p, len, MSG_NOSIGNAL | MSG_DONTWAIT);

		if (n >= 0) {
			p += n;
			len -= (size_t)n;
		} else if (errno != EINTR &&
		           ((errno != EAGAIN && errno != EWOULDBLOCK) || !wait_fd(b->fd, POLLOUT, deadline))) {
			DBG("%s: Senden fehlgeschlagen", b->what);
			bus_kill(b);	/* halb gesendete Nachricht: Verbindung unbrauchbar */
			return false;
		}
	}
	return true;
}

/* Mehr Daten in den Puffer holen: 1 = gelesen, 0 = Timeout, -1 = Verbindung weg */
static int bus_recv(struct bus *b, long long deadline)
{
	union {
		char buf[CMSG_SPACE(sizeof b->fds)];
		struct cmsghdr align;
	} ctl;

	if (b->fd < 0)
		return -1;
	for (;;) {
		struct iovec iov = { b->in + b->have, sizeof b->in - b->have };
		struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1,
		                     .msg_control = ctl.buf, .msg_controllen = sizeof ctl.buf };
		ssize_t n = recvmsg(b->fd, &mh, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);

		if (n > 0) {
			/* je recvmsg() kommt hoechstens eine Kontrollnachricht, die mit den fds */
			struct cmsghdr *c = CMSG_FIRSTHDR(&mh);

			if (c && c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
				size_t count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);

				for (size_t i = 0; i < count; i++) {
					int fd;
					memcpy(&fd, CMSG_DATA(c) + i * sizeof fd, sizeof fd);
					if (b->nfds < (int)(sizeof b->fds / sizeof *b->fds))
						b->fds[b->nfds++] = fd;
					else
						close(fd);
				}
			}
			b->have += (size_t)n;
			return 1;
		}
		if (n < 0 && errno == EINTR)
			continue;
		if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
			DBG("%s: Verbindung getrennt", b->what);
			bus_kill(b);
			return -1;
		}
		if (!wait_fd(b->fd, POLLIN, deadline))
			return 0;
	}
}

/* Headerfelder einer vollstaendig gelesenen Nachricht auswerten */
static void parse_fields(const unsigned char *p, size_t fields_len, struct message *m)
{
	size_t i = 16, end = 16 + fields_len;

	for (;;) {
		const char *s = NULL;
		uint32_t u = 0, n;
		unsigned char code, type;

		/* Feld = Code, Signatur aus einem Zeichen, Wert. Die definierten
		 * Felder haben nur die Typen u, s, o und g; bei allem anderen ist Schluss. */
		i = (i + 7) & ~(size_t)7;
		if (i + 4 > end || p[i + 1] != 1 || p[i + 3] != '\0')
			return;
		code = p[i];
		type = p[i + 2];
		i += 4;
		if (type == 'u') {
			if (end - i < 4)
				return;
			u = rd32(p + i, m->big);
			i += 4;
		} else if (type == 's' || type == 'o') {
			if (end - i < 4)
				return;
			n = rd32(p + i, m->big);
			i += 4;
			if (n >= end - i || p[i + n] != '\0')
				return;
			s = (const char *)p + i;
			i += (size_t)n + 1;
		} else if (type == 'g') {
			if (end - i < 1)
				return;
			n = p[i++];
			if (n >= end - i || p[i + n] != '\0')
				return;
			s = (const char *)p + i;
			i += (size_t)n + 1;
		} else {
			return;
		}
		switch (code) {
		case F_IFACE:        m->iface = s; break;
		case F_MEMBER:       m->member = s; break;
		case F_ERROR:        m->error = s; break;
		case F_SENDER:       m->sender = s; break;
		case F_SIGNATURE:    m->sig = s; break;
		case F_REPLY_SERIAL: m->reply_serial = u; break;
		case F_UNIX_FDS:     m->unix_fds = u; break;
		}
	}
}

/* Naechste Nachricht liefern: 1 = da, 0 = Timeout, -1 = Verbindung weg */
static int bus_next(struct bus *b, long long deadline, struct message *m)
{
	for (;;) {
		int r;

		/* zuletzt gelieferte bzw. zu grosse Nachricht aus dem Puffer nehmen */
		if (b->done || b->skip) {
			size_t n = b->done ? b->done : b->have < b->skip ? b->have : b->skip;

			memmove(b->in, b->in + n, b->have - n);
			b->have -= n;
			if (b->done)
				b->done = 0;
			else
				b->skip -= n;
		}
		if (b->skip || b->have < 16) {
			if ((r = bus_recv(b, deadline)) <= 0)
				return r;
			continue;
		}

		bool big = b->in[0] == 'B';
		uint32_t body_len = rd32(b->in + 4, big), fields_len = rd32(b->in + 12, big);

		if ((!big && b->in[0] != 'l') || b->in[3] != 1 || fields_len > MAX_FIELDS || body_len > MAX_BODY) {
			DBG("%s: Protokollfehler", b->what);
			bus_kill(b);
			return -1;
		}

		size_t body_off = 16 + (((size_t)fields_len + 7) & ~(size_t)7);
		size_t total = body_off + body_len;

		if (total > sizeof b->in) {
			/* erwartet hier niemand; fremde Nachricht ueberlesen */
			DBG("%s: Nachricht mit %zu Bytes uebergangen", b->what, total);
			b->skip = total;
			continue;
		}
		if (b->have < total) {
			if ((r = bus_recv(b, deadline)) <= 0)
				return r;
			continue;
		}
		memset(m, 0, sizeof *m);
		m->type = b->in[1];
		m->big = big;
		m->body = b->in + body_off;
		m->body_len = body_len;
		parse_fields(b->in, fields_len, m);
		b->done = total;
		return 1;
	}
}

/* Naechstes Argument vom Typ s aus dem Body; NULL, wenn keins mehr da ist */
static const char *body_str(const struct message *m, size_t *off)
{
	size_t i = (*off + 3) & ~(size_t)3;
	uint32_t n;

	if (i + 4 > m->body_len)
		return NULL;
	n = rd32(m->body + i, m->big);
	i += 4;
	if (n >= m->body_len - i || m->body[i + n] != '\0')
		return NULL;
	*off = i + n + 1;
	return (const char *)m->body + i;
}

/* Alles, was keine Antwort auf den laufenden Aufruf ist */
static void bus_other(struct bus *b, const struct message *m)
{
	size_t off = 0;
	const char *name;

	if (b == &user_bus && m->type == M_SIGNAL &&
	    m->sender && !strcmp(m->sender, DBUS_NAME) &&
	    m->member && !strcmp(m->member, "NameOwnerChanged") &&
	    m->sig && !strncmp(m->sig, "sss", 3) && (name = body_str(m, &off)))
		name_changed(name);
	bus_drop_fds(b, m->unix_fds);
}

/*
 * Methodenaufruf, blockierend bis zur Antwort. sig beschreibt die Argumente:
 * 's' = const char *, 'u' = uint32_t. want ist der Typ des erwarteten
 * Rueckgabewerts: 'u' (out = uint32_t *), 'h' (out = int *, fd mit CLOEXEC)
 * oder 0 = keiner. autostart = false: keine Desktop-Dienste per
 * D-Bus-Aktivierung hochfahren, nur laufende nutzen.
 */
static bool call(struct bus *b, bool autostart, const char *dest, const char *path,
                 const char *iface, const char *member, char want, void *out,
                 const char *sig, ...)
{
	struct out o = { .len = 0 };
	struct message m;
	va_list ap;

	if (!b || b->fd < 0)
		return false;

	uint32_t serial = ++b->serial;

	put_byte(&o, 'l');
	put_byte(&o, M_CALL);
	put_byte(&o, autostart ? 0 : NO_AUTO_START);
	put_byte(&o, 1);		/* Protokollversion */
	put_u32(&o, 0);			/* Laenge des Body, siehe unten */
	put_u32(&o, serial);
	put_u32(&o, 0);			/* Laenge der Headerfelder, siehe unten */
	put_field(&o, F_PATH, 'o', path);
	put_field(&o, F_DEST, 's', dest);
	put_field(&o, F_IFACE, 's', iface);
	put_field(&o, F_MEMBER, 's', member);
	if (*sig)
		put_field(&o, F_SIGNATURE, 'g', sig);
	set32(o.buf + 12, o.len - 16);
	put_align(&o, 8);

	size_t body = o.len;

	va_start(ap, sig);
	for (const char *t = sig; *t; t++) {
		if (*t == 's')
			put_str(&o, va_arg(ap, const char *));
		else
			put_u32(&o, va_arg(ap, uint32_t));
	}
	va_end(ap);
	set32(o.buf + 4, o.len - body);

	if (o.overflow || !bus_send(b, o.buf, o.len)) {
		fail("%s.%s: nicht gesendet", iface, member);
		return false;
	}

	long long deadline = now_ms() + CALL_TIMEOUT_MS;

	for (;;) {
		int r = bus_next(b, deadline, &m);

		if (r <= 0) {
			fail("%s.%s: %s", iface, member, r ? "Verbindung getrennt" : "keine Antwort");
			return false;
		}
		if ((m.type == M_RETURN || m.type == M_ERROR) && m.reply_serial == serial)
			break;
		bus_other(b, &m);
	}

	bool ok = m.type == M_RETURN;

	if (!ok) {
		size_t off = 0;
		const char *text = m.sig && m.sig[0] == 's' ? body_str(&m, &off) : NULL;

		fail("%s.%s: %s", iface, member, text ? text : m.error ? m.error : "Fehler");
	} else if (want) {
		uint32_t v = m.body_len >= 4 ? rd32(m.body, m.big) : 0;

		ok = m.sig && m.sig[0] == want && m.body_len >= 4;
		if (ok && want == 'h') {
			/* der Wert ist der Index in die mitgeschickten fds */
			ok = v < m.unix_fds && v < (uint32_t)b->nfds && b->fds[v] >= 0;
			if (ok) {
				*(int *)out = b->fds[v];
				b->fds[v] = -1;
			}
		} else if (ok) {
			*(uint32_t *)out = v;
		}
		if (!ok)
			fail("%s.%s: unerwartete Antwort", iface, member);
	}
	bus_drop_fds(b, m.unix_fds);
	return ok;
}

static int unix_connect(const char *name, size_t len, bool abstract)
{
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	int fd;

	/* Pfad mit abschliessender, abstrakter Name mit fuehrender Null */
	if (len == 0 || len + 1 > sizeof sa.sun_path)
		return -1;
	memcpy(sa.sun_path + abstract, name, len);
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	if (connect(fd, (struct sockaddr *)&sa,
	            (socklen_t)(offsetof(struct sockaddr_un, sun_path) + len + 1)) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static int hexval(int c)
{
	return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
	       c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* Wert eines Adress-Schluessels dekodieren (%XX); liefert die Laenge, 0 bei Fehler */
static size_t addr_value(const char *s, const char *end, char *out, size_t max)
{
	size_t len = 0;

	while (s < end) {
		int c = (unsigned char)*s++;

		if (c == '%') {
			if (end - s < 2 || hexval(s[0]) < 0 || hexval(s[1]) < 0)
				return 0;
			c = hexval(s[0]) << 4 | hexval(s[1]);
			s += 2;
		}
		if (len == max)
			return 0;
		out[len++] = (char)c;
	}
	return len;
}

/*
 * D-Bus-Adresse wie "unix:path=/run/user/1000/bus" oder
 * "unix:abstract=/tmp/dbus-X,guid=...;unix:path=..." durchgehen und den
 * ersten erreichbaren Unix-Socket nehmen. Andere Transporte werden
 * uebergangen - autolaunch wuerde per SSH ggf. einen dbus-daemon starten.
 */
static int addr_connect(const char *addr)
{
	while (*addr) {
		const char *end = addr + strcspn(addr, ";");
		const char *kv = strncmp(addr, "unix:", 5) ? end : addr + 5;

		while (kv < end) {
			const char *kend = memchr(kv, ',', (size_t)(end - kv));
			bool abstract = !strncmp(kv, "abstract=", 9);

			if (!kend)
				kend = end;
			if (abstract || !strncmp(kv, "path=", 5)) {
				char name[sizeof ((struct sockaddr_un *)0)->sun_path];
				size_t len = addr_value(kv + (abstract ? 9 : 5), kend, name, sizeof name);
				int fd = unix_connect(name, len, abstract);

				if (fd >= 0)
					return fd;
			}
			kv = kend < end ? kend + 1 : end;
		}
		addr = *end ? end + 1 : end;
	}
	return -1;
}

/* Eine Zeile der Anmeldung senden und, wenn reply gesetzt ist, die Antwortzeile lesen */
static bool sasl(struct bus *b, const char *cmd, size_t len, char *reply, size_t max)
{
	long long deadline = now_ms() + CALL_TIMEOUT_MS;
	size_t n = 0;

	if (!bus_send(b, cmd, len))
		return false;
	if (!reply)
		return true;
	/* byteweise: hinter der Zeile darf nichts vom Nachrichtenstrom verloren gehen */
	for (;;) {
		char c;
		ssize_t r = recv(b->fd, &c, 1, MSG_DONTWAIT);

		if (r == 1) {
			if (c == '\n')
				break;
			if (n + 1 >= max)
				return false;
			reply[n++] = c;
		} else if (r == 0 || (errno != EINTR &&
		           ((errno != EAGAIN && errno != EWOULDBLOCK) || !wait_fd(b->fd, POLLIN, deadline)))) {
			return false;
		}
	}
	if (n && reply[n - 1] == '\r')
		n--;
	reply[n] = '\0';
	return true;
}

/* Verbindung auf fd anmelden; bei Fehlschlag ist fd geschlossen */
static bool bus_open(struct bus *b, int fd)
{
	char uid[16], cmd[64], line[256];
	int digits = snprintf(uid, sizeof uid, "%lu", (unsigned long)geteuid());
	size_t len = 0;

	b->fd = fd;
	if (fd < 0)
		return false;

	/* Null-Byte, dann EXTERNAL mit der uid als Hex-Text */
	cmd[len++] = '\0';
	len += (size_t)snprintf(cmd + len, sizeof cmd - len, "AUTH EXTERNAL ");
	for (int i = 0; i < digits; i++)
		len += (size_t)snprintf(cmd + len, sizeof cmd - len, "%02x", (unsigned char)uid[i]);
	len += (size_t)snprintf(cmd + len, sizeof cmd - len, "\r\n");

	if (!sasl(b, cmd, len, line, sizeof line))
		goto fail;
	if (strncmp(line, "OK", 2) != 0) {
		/* im User-Namespace (Container) kennt uns der Bus unter anderer uid:
		 * ohne Angabe nimmt er die des Sockets */
		if (!sasl(b, "AUTH EXTERNAL\r\n", 15, line, sizeof line) || strncmp(line, "DATA", 4) != 0 ||
		    !sasl(b, "DATA\r\n", 6, line, sizeof line) || strncmp(line, "OK", 2) != 0)
			goto fail;
	}
	/* fd-Uebergabe fuer logind; lehnt der Bus ab, scheitert spaeter nur dessen Aufruf */
	if (!sasl(b, "NEGOTIATE_UNIX_FD\r\n", 19, line, sizeof line) ||
	    !sasl(b, "BEGIN\r\n", 7, NULL, 0) ||
	    !call(b, true, DBUS_NAME, DBUS_PATH, DBUS_NAME, "Hello", 0, NULL, ""))
		goto fail;
	return true;
fail:
	DBG("%s: Anmeldung fehlgeschlagen", b->what);
	bus_kill(b);
	return false;
}

/*
 * Session-Bus ohne X11-Autolaunch: DBUS_SESSION_BUS_ADDRESS, sonst der Bus
 * des systemd-Nutzerdienstes. Der hilft auch, wenn die Variable veraltet
 * ist (tmux, su) oder XDG_RUNTIME_DIR fehlt (cron).
 */
static struct bus *get_user_bus(void)
{
	static const struct backend *const lists[] = { display_backends, system_backends };
	struct bus *b = &user_bus;
	const char *env = getenv("DBUS_SESSION_BUS_ADDRESS");
	int fd = -1;

	if (b->tried)
		return b->fd >= 0 ? b : NULL;
	b->tried = true;

	if (env && *env)
		fd = addr_connect(env);
	if (fd < 0) {
		char path[sizeof ((struct sockaddr_un *)0)->sun_path];
		int n;

		env = getenv("XDG_RUNTIME_DIR");
		if (env && *env)
			n = snprintf(path, sizeof path, "%s/bus", env);
		else
			n = snprintf(path, sizeof path, "/run/user/%lu/bus", (unsigned long)geteuid());
		if (n > 0 && (size_t)n < sizeof path)
			fd = unix_connect(path, (size_t)n, false);
	}
	if (!bus_open(b, fd)) {
		DBG("Session-Bus: nicht erreichbar");
		return NULL;
	}

	/* mitbekommen, wenn einer der Desktop-Dienste geht oder (neu) kommt */
	const char *seen[16];
	size_t n = 0;

	for (size_t l = 0; l < sizeof lists / sizeof *lists; l++) {
		for (const struct backend *be = lists[l]; be->dest; be++) {
			char rule[256];
			size_t k = 0;

			while (k < n && strcmp(seen[k], be->dest) != 0)
				k++;
			if (k < n || n == sizeof seen / sizeof *seen)
				continue;	/* Name kommt in den Listen mehrfach vor */
			seen[n++] = be->dest;
			snprintf(rule, sizeof rule, "type='signal',sender='" DBUS_NAME "',path='" DBUS_PATH "',"
			         "interface='" DBUS_NAME "',member='NameOwnerChanged',arg0='%s'", be->dest);
			call(b, true, DBUS_NAME, DBUS_PATH, DBUS_NAME, "AddMatch", 0, NULL, "s", rule);
		}
	}
	return b->fd >= 0 ? b : NULL;
}

static struct bus *get_sys_bus(void)
{
	static const char *const paths[] = { "/run/dbus/system_bus_socket", "/var/run/dbus/system_bus_socket" };
	struct bus *b = &sys_bus;
	const char *env = getenv("DBUS_SYSTEM_BUS_ADDRESS");
	int fd = -1;

	if (b->tried)
		return b->fd >= 0 ? b : NULL;
	b->tried = true;

	if (env && *env)
		fd = addr_connect(env);
	else
		for (size_t i = 0; fd < 0 && i < sizeof paths / sizeof *paths; i++)
			fd = unix_connect(paths[i], strlen(paths[i]), false);
	if (!bus_open(b, fd)) {
		DBG("System-Bus: nicht erreichbar");
		return NULL;
	}
	return b;
}

/* logind-Inhibitor; liefert einen fd (CLOEXEC) oder -1. */
static int logind_inhibit(const char *what)
{
	struct bus *b = get_sys_bus();
	int fd = -1;

	if (!b) {
		fail("System-Bus nicht erreichbar");
		return -1;
	}
	for (size_t i = 0; i < sizeof seat_managers / sizeof *seat_managers; i++) {
		if (call(b, true, seat_managers[i].dest, seat_managers[i].path, seat_managers[i].iface,
		         "Inhibit", 'h', &fd, "ssss", what, NAME, details, "block")) {
			DBG("%s.Inhibit(%s) -> fd %d", seat_managers[i].iface, what, fd);
			return fd;
		}
	}
	return -1;
}

/* Erstes laufende Desktop-Backend der Liste nehmen. */
static bool sess_inhibit(const struct backend *list, struct assertion *a)
{
	struct bus *b = get_user_bus();
	uint32_t xid = 0;

	for (const struct backend *be = list; b && be->dest; be++) {
		bool ok;

		if (be->gsm_flags)
			ok = call(b, false, be->dest, be->path, be->iface, "Inhibit", 'u', &a->cookie,
			          "susu", NAME, xid, details, be->gsm_flags);
		else
			ok = call(b, false, be->dest, be->path, be->iface, "Inhibit", 'u', &a->cookie,
			          "ss", NAME, details);
		if (ok) {
			a->be = be;
			DBG("%s.Inhibit -> cookie %u", be->iface, a->cookie);
			return true;
		}
	}
	return false;
}

static void sess_uninhibit(struct assertion *a)
{
	if (!a->be)
		return;
	call(&user_bus, false, a->be->dest, a->be->path, a->be->iface, a->be->uninhibit,
	     0, NULL, "u", a->cookie);
	DBG("%s.%s(%u)", a->be->iface, a->be->uninhibit, a->cookie);
	a->be = NULL;
}

/* -u: Bildschirm wecken, wo der Dienst das kann (KDE, XFCE) */
static void simulate_user_activity(void)
{
	struct bus *b = get_user_bus();

	for (const struct backend *be = display_backends; b && be->dest; be++)
		if (!be->gsm_flags &&
		    call(b, false, be->dest, be->path, be->iface, "SimulateUserActivity", 0, NULL, ""))
			break;
}

/* ---------------------------------------------------------------- Netzteil */

static bool read_sysfs(const char *dev, const char *attr, char *buf, size_t len)
{
	char path[PATH_MAX];
	FILE *f;
	bool ok;

	snprintf(path, sizeof path, POWER_DIR "/%s/%s", dev, attr);
	f = fopen(path, "re");
	if (!f)
		return false;
	ok = fgets(buf, (int)len, f) != NULL;
	fclose(f);
	if (ok)
		buf[strcspn(buf, "\n")] = '\0';
	return ok;
}

/*
 * Wie on_ac_power() von systemd: am Netz, sobald eine Stromquelle online
 * meldet; sonst nur dann nicht, wenn ein Akku entlaedt. Ein Rechner ohne
 * Akku (Desktop) gilt also als "am Netz", auch wenn ein USB-C-Port als
 * Stromquelle offline meldet. Akkus von Peripherie (Maus, Tastatur) zaehlen
 * nicht.
 */
static bool on_ac_power(void)
{
	DIR *d = opendir(POWER_DIR);
	struct dirent *e;
	bool online = false, discharging = false;
	char buf[32];

	if (!d)
		return true;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.' || !read_sysfs(e->d_name, "type", buf, sizeof buf))
			continue;
		if (!strcmp(buf, "Battery")) {
			if (read_sysfs(e->d_name, "scope", buf, sizeof buf) && !strcmp(buf, "Device"))
				continue;
			if (read_sysfs(e->d_name, "status", buf, sizeof buf) && !strcmp(buf, "Discharging"))
				discharging = true;
		} else if (read_sysfs(e->d_name, "online", buf, sizeof buf) && strtoul(buf, NULL, 10) > 0) {
			online = true;	/* 1 = fest, 2 = programmierbar (USB-PD) */
		}
	}
	closedir(d);
	return online || !discharging;
}

/* -s: Sperre nur am Netzteil halten; false, wenn sie sich dort nicht anlegen laesst */
static bool update_system_assertion(void)
{
	struct assertion *a = &as[A_SYSTEM];

	if (!on_ac_power()) {
		if (a->fd >= 0) {
			DBG("Akkubetrieb: sleep-Sperre freigegeben");
			close(a->fd);
			a->fd = -1;
		}
		return true;
	}
	if (a->fd < 0)
		a->fd = logind_inhibit("sleep");
	return a->fd >= 0;
}

/* ---------------------------------------------------------------- anlegen/freigeben */

static const struct backend *backends_for(int type)
{
	return type == A_IDLE ? system_backends :
	       type == A_DISPLAY || type == A_USER ? display_backends : NULL;
}

/* first = false: Sperre nach einem Dienst-Neustart erneut anlegen */
static bool create_one(int type, bool first)
{
	struct assertion *a = &as[type];

	a->fd = -1;
	a->be = NULL;
	reason[0] = '\0';
	switch (type) {
	case A_USER:
		if (first)
			simulate_user_activity();
		/* fall through */
	case A_IDLE:
	case A_DISPLAY:
		if (sess_inhibit(backends_for(type), a))
			return true;
		a->fd = logind_inhibit("idle");
		return a->fd >= 0;
	case A_SYSTEM:
		/* Akkubetrieb: Assertion gilt, wirkt aber erst am Netzteil */
		if (!get_sys_bus()) {
			fail("System-Bus nicht erreichbar");
			return false;
		}
		return update_system_assertion();
	case A_DISK:
		return true;
	default:
		return false;
	}
}

static void release_one(int type)
{
	struct assertion *a = &as[type];

	if (!a->active)
		return;
	if (a->fd >= 0)
		close(a->fd);
	a->fd = -1;
	sess_uninhibit(a);
	a->active = a->stale = false;
	DBG("%s freigegeben", type_name[type]);
}

static void release_all(void)
{
	for (int i = 0; i < A_COUNT; i++)
		release_one(i);
	bus_kill(&user_bus);
	bus_kill(&sys_bus);
}

/* wie createAssertions() bei Apple; false = Fehler, Aufrufer beendet mit 1 */
static bool create_assertions(const bool want[A_COUNT], long timeout)
{
	for (int i = 0; i < A_COUNT; i++) {
		if (!want[i])
			continue;
		if (i == A_USER && timeout == 0)
			timeout = 5;	/* gilt bei Apple auch fuer alle folgenden */
		if (!create_one(i, true)) {
			/* halb angelegte Sperre (z. B. -s) trotzdem sauber freigeben */
			as[i].active = true;
			if (*reason && !debug)
				fprintf(stderr, NAME ": %s\n", reason);
			fprintf(stderr, "Failed to create %s assertion\n", type_name[i]);
			return false;
		}
		as[i].active = true;
		as[i].expires = expiry_after(timeout);
		DBG("%s angelegt%s", type_name[i], as[i].expires == FOREVER ? "" : " (mit Timeout)");
	}
	return true;
}

/*
 * Der Besitzer eines Dienstnamens hat gewechselt: Der Dienst ist beendet
 * oder (neu) gestartet. Eine Sperre bei ihm ist damit weg; eine
 * Ersatzsperre bei logind kann jetzt ggf. zum Dienst wechseln.
 */
static void name_changed(const char *name)
{
	for (int i = 0; i < A_COUNT; i++) {
		struct assertion *a = &as[i];
		const struct backend *be = backends_for(i);

		if (!a->active || !be)
			continue;
		if (a->be) {
			if (strcmp(a->be->dest, name) != 0)
				continue;
			/* das Cookie gehoert dem alten Besitzer; beim neuen koennte
			 * dieselbe Nummer die Sperre eines anderen Programms sein */
			a->be = NULL;
		} else {
			while (be->dest && strcmp(be->dest, name) != 0)
				be++;
			if (!be->dest)
				continue;
		}
		DBG("%s: Besitzer von %s hat gewechselt", type_name[i], name);
		a->stale = true;
		retry = true;
		refresh_at = now_ms() + SETTLE_MS;
	}
}

/* Aufrufer hat refresh_at zurueckgesetzt; kommt waehrenddessen ein Signal, plant name_changed() neu */
static void refresh_assertions(void)
{
	bool follow_up = retry, again = false;

	retry = false;
	for (int i = 0; i < A_COUNT; i++) {
		struct assertion *a = &as[i];
		int old_fd = a->fd;

		if (!a->active || !a->stale)
			continue;
		a->stale = false;
		if (!create_one(i, false))
			DBG("%s: zurzeit keine Sperre moeglich", type_name[i]);
		/* Ersatzsperre erst jetzt freigeben, damit keine Luecke entsteht */
		if (old_fd >= 0)
			close(old_fd);
		if (!a->be && follow_up)
			a->stale = again = true;
	}
	if (again && refresh_at == FOREVER)
		refresh_at = now_ms() + RETRY_MS;
}

/* Eingegangenes vom Session-Bus abarbeiten, ohne zu warten */
static void sess_dispatch(void)
{
	struct message m;
	int r;

	while ((r = bus_next(&user_bus, 0, &m)) > 0)
		bus_other(&user_bus, &m);
	if (r == 0)
		return;
	/* Bus weg (abgemeldet): die Sperren dort gibt es nicht mehr */
	for (int i = 0; i < A_COUNT; i++) {
		if (as[i].active && as[i].be) {
			as[i].be = NULL;
			as[i].stale = true;
			retry = false;
			refresh_at = 0;
		}
	}
}

/* ---------------------------------------------------------------- main */

static int die_by_signal(int sig, const sigset_t *oldmask)
{
	sigset_t one;

	signal(sig, SIG_DFL);
	sigprocmask(SIG_SETMASK, oldmask, NULL);
	sigemptyset(&one);
	sigaddset(&one, sig);
	sigprocmask(SIG_UNBLOCK, &one, NULL);
	raise(sig);
	return 128 + sig;
}

int main(int argc, char **argv)
{
	bool want[A_COUNT] = { false };
	long timeout = 0, waitforpid = 0;
	const char *env = getenv("CAFFEINATE_DEBUG");
	int ch;

	debug = env && *env && strcmp(env, "0") != 0;

	/* '+': wie BSD-getopt beim ersten Nicht-Flag aufhoeren; ':' + opterr = 0:
	 * Meldungen im BSD-Wortlaut statt glibc/musl */
	opterr = 0;
	while ((ch = getopt(argc, argv, "+:mdhisut:w:")) != -1) {
		switch (ch) {
		case 'm': want[A_DISK] = true; break;
		case 'd': want[A_DISPLAY] = true; break;
		case 'h': usage(); return EXIT_SUCCESS;
		case 'i': want[A_IDLE] = true; break;
		case 's': want[A_SYSTEM] = true; break;
		case 'u': want[A_USER] = true; break;
		case 'w':
			if (!mac_strtol(optarg, &waitforpid)) {
				usage();
				return EXIT_FAILURE;
			}
			waitforpid = (pid_t)waitforpid;
			break;
		case 't':
			if (!mac_strtol(optarg, &timeout)) {
				usage();
				return EXIT_FAILURE;
			}
			if (timeout < 0)	/* bei Apple unsigned: praktisch unendlich */
				timeout = LONG_MAX;
			break;
		case ':':
			fprintf(stderr, NAME ": option requires an argument -- %c\n", optopt);
			usage();
			return EXIT_FAILURE;
		default:
			fprintf(stderr, NAME ": illegal option -- %c\n", optopt);
			usage();
			return EXIT_FAILURE;
		}
	}

	bool any = false;
	for (int i = 0; i < A_COUNT; i++)
		any |= want[i];
	if (!any)
		want[A_IDLE] = true;

	sigset_t mask, oldmask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGHUP);

	int pfd = -1;		/* Prozess, auf dessen Ende gewartet wird */
	pid_t pid_poll = 0;	/* -w ohne pidfd: per kill(pid, 0) abfragen */
	long long deadline = FOREVER;
	bool have_cmd = argc - optind > 0;

	if (have_cmd) {
		/*
		 * Wie Apple: frueh forken, der Elternprozess wird per exec zum
		 * command (PID, Exit-Status und Signale bleiben dessen eigene),
		 * das Kind haelt die Assertions und wacht ueber den Elternprozess.
		 */
		argv += optind;
		pid_t parent = getpid();
		int self_fd = sys_pidfd_open(parent);	/* von Haus aus CLOEXEC */

		switch (fork()) {
		case 0:
			break;
		case -1:
			/* kein Hilfsprozess, also keine Sperren - das command laeuft trotzdem */
			perror(NAME ": fork");
			/* fall through */
		default:
			execvp(*argv, argv);
			int saved_errno = errno;
			perror(*argv);
			_exit(saved_errno == ENOENT ? 127 : 126);
		}

		pfd = close_inherited_fds(self_fd);
		signal(SIGINT, SIG_IGN);
		signal(SIGQUIT, SIG_IGN);
		sigprocmask(SIG_BLOCK, &mask, &oldmask);
		if (pfd < 0)	/* kein pidfd (Kernel < 5.3): Signal beim Tod des Elternprozesses */
			prctl(PR_SET_PDEATHSIG, SIGTERM);
		if (getppid() != parent)
			return EXIT_SUCCESS;
		snprintf(details, sizeof details,
		         "caffeinate asserting on behalf of '%s' (pid %d)", *argv, (int)parent);
	} else {
		sigaddset(&mask, SIGINT);
		sigaddset(&mask, SIGQUIT);
		sigprocmask(SIG_BLOCK, &mask, &oldmask);
		deadline = expiry_after(timeout);
		if (waitforpid) {
			if (timeout)
				snprintf(details, sizeof details,
				         "caffeinate asserting on behalf of Process ID %ld for %ld secs",
				         waitforpid, timeout);
			else
				snprintf(details, sizeof details,
				         "caffeinate asserting on behalf of Process ID %ld", waitforpid);
			if (waitforpid > 0)
				pfd = sys_pidfd_open((pid_t)waitforpid);
			if (pfd < 0) {
				if (waitforpid < 0 || errno == ESRCH || errno == EINVAL)
					deadline = 0;	/* gibt es nicht: libdispatch meldet sofort Exit */
				else	/* kein pidfd (Kernel < 5.3, seccomp): abfragen */
					pid_poll = (pid_t)waitforpid;
			}
		} else if (timeout) {
			snprintf(details, sizeof details, "caffeinate asserting for %ld secs", timeout);
		} else {
			snprintf(details, sizeof details, "caffeinate asserting forever");
		}
	}

	int sfd = signalfd(-1, &mask, SFD_CLOEXEC);
	if (sfd < 0) {
		perror(NAME ": signalfd");
		return EXIT_FAILURE;
	}

	utf8_sanitize(details);
	if (!create_assertions(want, timeout)) {
		release_all();
		return EXIT_FAILURE;
	}

	int exit_code = EXIT_SUCCESS, term_sig = 0;

	for (;;) {
		/* auch das, was beim letzten Aufruf schon mit im Puffer gelandet ist */
		if (user_bus.fd >= 0)
			sess_dispatch();
		if (refresh_at != FOREVER && refresh_at <= now_ms()) {
			refresh_at = FOREVER;
			refresh_assertions();
		}

		long long now = now_ms(), next = deadline;

		if (deadline != FOREVER && deadline <= now)
			break;
		for (int i = 0; i < A_COUNT; i++) {
			if (!as[i].active || as[i].expires == FOREVER)
				continue;
			if (as[i].expires <= now) {
				release_one(i);
				continue;
			}
			if (next == FOREVER || as[i].expires < next)
				next = as[i].expires;
		}
		if (as[A_SYSTEM].active) {
			update_system_assertion();
			if (next == FOREVER || now + AC_POLL_MS < next)
				next = now + AC_POLL_MS;
		}
		if (pid_poll) {
			if (kill(pid_poll, 0) < 0 && errno == ESRCH)
				break;
			if (next == FOREVER || now + PID_POLL_MS < next)
				next = now + PID_POLL_MS;
		}
		if (refresh_at != FOREVER && (next == FOREVER || refresh_at < next))
			next = refresh_at;

		long long wait = next == FOREVER ? -1 : next < now ? 0 : next - now;
		struct pollfd p[3] = { { sfd, POLLIN, 0 }, { pfd, POLLIN, 0 } };
		nfds_t n = pfd >= 0 ? 2 : 1;

		/* Session-Bus: weckt nur, gelesen wird oben */
		if (user_bus.fd >= 0)
			p[n++] = (struct pollfd){ user_bus.fd, POLLIN, 0 };

		if (poll(p, n, wait > INT_MAX ? INT_MAX : (int)wait) < 0) {
			if (errno == EINTR)
				continue;
			perror(NAME ": poll");
			exit_code = EXIT_FAILURE;
			break;
		}
		if (p[0].revents & POLLIN) {
			struct signalfd_siginfo si;
			if (read(sfd, &si, sizeof si) == sizeof si) {
				term_sig = (int)si.ssi_signo;
				break;
			}
		}
		if (pfd >= 0 && (p[1].revents & (POLLIN | POLLHUP | POLLERR)))
			break;
	}

	release_all();
	if (pfd >= 0)
		close(pfd);
	close(sfd);
	if (term_sig)
		exit_code = die_by_signal(term_sig, &oldmask);
	return exit_code;
}
