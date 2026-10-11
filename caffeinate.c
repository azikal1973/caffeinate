/*
 * caffeinate - a Linux clone of macOS caffeinate(8)
 *
 *   caffeinate [-disu] [-t timeout] [-w Process ID] [command arguments...]
 *
 * WHAT IT DOES
 *
 * Keeps the machine awake for as long as it runs: forever, for -t seconds,
 * until process -w exits, or until the given command finishes. macOS calls
 * such a request a "power assertion", Linux an "inhibitor lock". This program
 * takes Apple's five assertion types and maps each one to whatever the
 * running system offers:
 *
 *   -i  PreventUserIdleSystemSleep   -> org.freedesktop.PowerManagement.Inhibit
 *                                       (else SessionManager, flag "suspend")
 *   -d  PreventUserIdleDisplaySleep  -> org.freedesktop.ScreenSaver.Inhibit
 *                                       (XFCE: xfce4-power-manager or
 *                                       org.xfce.ScreenSaver; else
 *                                       SessionManager, flag "idle")
 *   -s  PreventSystemSleep (AC only) -> logind inhibitor "sleep"
 *   -u  UserIsActive                 -> ScreenSaver.SimulateUserActivity,
 *                                       then the same as -d
 *   -m  PreventDiskIdle              -> no counterpart; accepted, does nothing
 *
 * Without a session bus, or without a desktop service that answers, -i/-d/-u
 * fall back to the logind inhibitor "idle". "logind" here always includes
 * elogind; ConsoleKit2 has the same interface and is tried after it. In an
 * X11 session -d/-u additionally suspend the X server's own screen saver and
 * DPMS, which works without any desktop service at all (i3, dwm ...).
 *
 * DESIGN
 *
 * In one sentence: a single-threaded reactor (one poll() loop) that holds a
 * set of connection-scoped leases and keeps reconciling them against a table
 * of backends. The patterns in detail:
 *
 * 1. A lock is a lease tied to an open connection.
 *    Nothing is ever switched off globally. Every lock taken here belongs to
 *    a connection: the cookie from a desktop service to our session bus
 *    connection, the logind lock to a file descriptor, the X11 suspend to
 *    our X connection. The other side drops the lock when that connection
 *    closes, and the kernel closes it when the process ends - even after
 *    kill -9. A lock can therefore never be left behind, and correctness
 *    does not depend on cleanup code. The price: a service that restarts
 *    forgets its locks; see 4.
 *
 * 2. Backends are data, tried in order (chain of responsibility).
 *    display_backends[] and system_backends[] list the known desktop
 *    services by priority. sess_inhibit() walks a list and takes the first
 *    service that is running; logind is the fallback behind every list.
 *    Supporting another desktop means adding a table row, not code.
 *
 * 3. One thread, one poll() loop (reactor).
 *    main() waits on a signalfd (termination signals), a pidfd (the watched
 *    process), the session bus socket and the X sockets. All timers - -t,
 *    per-assertion expiry, the AC check for -s, pending re-acquisition - are
 *    absolute CLOCK_MONOTONIC deadlines from which the poll timeout is
 *    derived. There are no threads, no signal handlers and no callbacks.
 *    Remote calls are synchronous: call() blocks until the reply or a 5 s
 *    deadline and hands every unrelated message to bus_other().
 *
 * 4. Desired state versus actual state (reconciliation).
 *    struct assertion records what the user asked for (active, expires) and
 *    what currently enforces it (be + cookie, fd, xfd). Events never repair
 *    anything themselves: name_changed() merely marks an assertion stale and
 *    sets refresh_at; refresh_assertions() later re-acquires it, moves it to
 *    a better backend that has appeared, or reports it as lost and keeps
 *    retrying.
 *
 * 5. With a command, parent and child swap roles.
 *    As in Apple's tool, the parent exec()s the command - PID, exit status
 *    and signals are then the command's own - while a forked child holds the
 *    assertions and watches the parent (pidfd, or PDEATHSIG before Linux 5.3).
 *
 * 6. No dependencies beyond libc.
 *    The D-Bus and X11 wire protocols are spoken directly on their unix
 *    sockets, limited to the few messages needed. Linked statically, the
 *    binary therefore runs on every distribution.
 *
 * COMPATIBILITY WITH APPLE
 *
 * Behaviour follows Apple's caffeinate.c (PowerManagement project), not just
 * its man page; where the two disagree, the code wins:
 *   - with no assertion flag, -i is assumed; -h prints the usage (exit 0)
 *   - -t/-w are parsed with strtol(..., 0); -t 0 means "no timeout"
 *   - -t applies per assertion; with a command, caffeinate still runs until
 *     the command ends
 *   - -u without -t: the assertion lasts 5 s (as does a -m after it), the
 *     process itself keeps running
 *   - with a command, the child that holds the assertions ignores
 *     SIGINT/SIGQUIT
 *
 * READING GUIDE
 *
 *   Assertions           types, backend tables, per-assertion state
 *   Helpers              time, option parsing, UTF-8, fd hygiene
 *   D-Bus                wire format, connection, authentication, call()
 *   X11                  Xauthority, connection setup, screen saver suspend
 *   Power supply         on_ac_power() for -s
 *   Acquire and release  the state machine described in 4.
 *   main                 options, fork/exec, event loop
 *
 * Messages for the user are in German, except those Apple's tool prints
 * (usage, getopt errors, "Failed to create ... assertion"), which are kept
 * verbatim. CAFFEINATE_DEBUG=1 traces every step on stderr.
 */
#define _GNU_SOURCE
/*
 * Harden libc calls unless the compiler or the distribution already asks for
 * it. Has to come before the first #include.
 */
#if !defined(_FORTIFY_SOURCE) && defined(__OPTIMIZE__)
# define _FORTIFY_SOURCE 2
#endif
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

/*
 * Older kernel headers (musl, RHEL 8) lack these syscall numbers. They are the
 * same on every architecture except alpha.
 */
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
#define AC_POLL_MS      5000	/* -s: interval of the power supply check */
#define PID_POLL_MS     1000	/* -w without pidfd (kernel < 5.3): interval of the pid check */
#define CALL_TIMEOUT_MS 5000	/* longest wait for a D-Bus reply or for the X server */
#define SETTLE_MS       500	/* a service was just (re)started: wait until it accepts calls */
#define RETRY_MS        5000	/* ... and try once more in case it was not ready by then */
#define LOST_RETRY_MS   60000	/* no lock at all: retry this often */
#define FOREVER         (-1LL)	/* "no deadline", for every timestamp below */
#define MAX_TIMEOUT_S   3155760000UL	/* 100 years; anything longer counts as infinite */

/* Set by CAFFEINATE_DEBUG: trace every step on stderr */
static bool debug;

/* Debug trace in printf style; the newline is added here */
#define DBG(...) do { if (debug) { fprintf(stderr, NAME ": " __VA_ARGS__); fputc('\n', stderr); } } while (0)

/* ---------------------------------------------------------------- Assertions */

/*
 * An "assertion" (Apple's term) is one request to keep something awake. There
 * is one slot per type, that is per command line flag: -i, -d, -s, -u, -m.
 */
enum { A_IDLE, A_DISPLAY, A_SYSTEM, A_USER, A_DISK, A_COUNT };

/*
 * Names as Apple prints them, in the order of Apple's assertionMap. The order
 * matters: assertions are created in it, and the implicit 5 s timeout of -u
 * carries over to those after it (see create_assertions()).
 */
static const char *const type_name[A_COUNT] = {
	"PreventUserIdleSystemSleep",
	"PreventUserIdleDisplaySleep",
	"PreventSystemSleep",
	"UserIsActive",
	"PreventDiskIdle",
};

/*
 * A desktop service on the session bus that hands out inhibitor locks: calling
 * iface.Inhibit on object path of bus name dest returns a cookie, and
 * iface.<uninhibit>(cookie) gives the lock back. Inhibit comes in two flavours:
 * the freedesktop services take (application, reason), signature "ss";
 * org.gnome.SessionManager takes (application, window id, reason, flags),
 * signature "susu".
 */
struct backend {
	const char *dest, *path, *iface, *uninhibit;
	uint32_t gsm_flags;	/* 0: freedesktop flavour; else SessionManager with these flags */
};

/* What to inhibit: flags of org.gnome.SessionManager.Inhibit */
#define GSM_SUSPEND 4
#define GSM_IDLE    8
#define FDO_SS      "org.freedesktop.ScreenSaver"
#define XFCE_SS     "org.xfce.ScreenSaver"
#define FDO_PM      "org.freedesktop.PowerManagement"
/* Object path, interface and release method, common to both power managers */
#define PM_INHIBIT  "/org/freedesktop/PowerManagement/Inhibit", FDO_PM ".Inhibit", "UnInhibit"
/* Also Cinnamon and MATE: their session managers answer to the GNOME name */
#define GNOME_SM    "org.gnome.SessionManager", "/org/gnome/SessionManager", "org.gnome.SessionManager", "Uninhibit"

/*
 * Backends for -d and -u. Order is priority: the first one running wins. XFCE
 * comes first because xfce4-power-manager is what blanks the screen there (via
 * DPMS), and while it is inhibited it also holds off the screen saver, be it
 * xfce4-screensaver or light-locker. A lock taken from those screen savers
 * themselves would leave DPMS and suspend-on-idle running, so it only counts
 * when the power manager is absent.
 */
static const struct backend display_backends[] = {
	{ "org.xfce.PowerManager", PM_INHIBIT, 0 },
	{ FDO_SS, "/org/freedesktop/ScreenSaver", FDO_SS, "UnInhibit", 0 },
	{ FDO_SS, "/ScreenSaver", FDO_SS, "UnInhibit", 0 },	/* some services only export this path */
	{ XFCE_SS, "/org/xfce/ScreenSaver", XFCE_SS, "UnInhibit", 0 },
	{ GNOME_SM, GSM_IDLE },
	{ NULL, NULL, NULL, NULL, 0 }
};

/*
 * Backends for -i. org.freedesktop.PowerManagement is owned by PowerDevil (KDE)
 * and by xfce4-power-manager; only KDE also owns the name "...Inhibit".
 */
static const struct backend system_backends[] = {
	{ FDO_PM, PM_INHIBIT, 0 },
	{ GNOME_SM, GSM_SUSPEND },
	{ NULL, NULL, NULL, NULL, 0 }
};

/*
 * Login managers on the system bus, tried in this order; systemd-logind and
 * elogind share the first name. Inhibit(what, who, why, mode) returns a file
 * descriptor, and the lock lasts exactly as long as that descriptor is open.
 */
static const struct {
	const char *dest, *path, *iface;
} seat_managers[] = {
	{ "org.freedesktop.login1", "/org/freedesktop/login1", "org.freedesktop.login1.Manager" },
	{ "org.freedesktop.ConsoleKit", "/org/freedesktop/ConsoleKit/Manager",
	  "org.freedesktop.ConsoleKit.Manager" },
};

/*
 * State of one assertion. "active" is what the user wants; be/cookie, fd and
 * xfd say what currently enforces it. More than one can be set: -d under a
 * bare X11 window manager, for example, holds both fd (logind) and xfd.
 */
struct assertion {
	bool active;			/* requested, and neither expired nor released */
	bool stale;			/* service gone, or a better one came: refresh is due */
	bool lost;			/* nothing enforces it right now; already reported */
	long long expires;		/* CLOCK_MONOTONIC in ms, or FOREVER */
	const struct backend *be;	/* desktop service that holds the lock, or NULL */
	uint32_t cookie;		/* ... and the cookie that identifies the lock there */
	int fd;				/* logind inhibitor fd, or -1 */
	int xfd;			/* X connection that suspends the screen saver, or -1 */
};

static struct assertion as[A_COUNT];	/* indexed by A_* */
static long long refresh_at = FOREVER;	/* when refresh_assertions() is due, or FOREVER */
static bool retry;			/* that refresh gets one follow-up attempt */
static unsigned owner_changes;		/* counts every time a lock loses its service */
static const char *inflight_dest;	/* service with an Inhibit call in flight, ... */
static char inflight_owner[64];		/* ... its new owner if the name changed hands, ... */
static bool inflight_hit;		/* ... and whether that happened */
static char last_sender[64];		/* sender of the last reply (unique bus name) */
static char details[160];		/* the "why" text services show in their lock lists */
static char reason[256];		/* why the last call failed, set by fail() */
static enum { E_OTHER, E_ABSENT, E_DENIED } failure;	/* ... and what kind of failure it was */

/* ---------------------------------------------------------------- Helpers */

/* Monotonic clock in milliseconds: the time base of every deadline in this file */
static long long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Deadline for a timeout given in seconds; 0 (or absurdly long) means none */
static long long expiry_after(long secs)
{
	if (secs <= 0 || (unsigned long)secs > MAX_TIMEOUT_S)
		return FOREVER;
	return now_ms() + (long long)secs * 1000;
}

/* pidfd_open(2), called directly because glibc only wraps it since 2.36 */
static int sys_pidfd_open(pid_t pid)
{
	return (int)syscall(SYS_pidfd_open, pid, 0);
}

/* Apple's usage line, verbatim */
static void usage(void)
{
	fprintf(stderr, "usage: caffeinate [-disu] [-t timeout] [-w Process ID] [command arguments...]\n");
}

/*
 * Remember why something failed, for the message to the user. Text from
 * services arrives unchecked, so control characters are replaced before it can
 * reach a terminal.
 */
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
 * strtol(s, NULL, 0) with macOS semantics: there strtol sets errno = EINVAL
 * when it finds no digit; glibc and musl do not. Returns false in exactly the
 * cases where Apple's caffeinate calls usage().
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
 * D-Bus requires valid UTF-8; the bus disconnects a client that sends anything
 * else. The command name may contain arbitrary bytes (Latin-1 file names) and
 * may have been cut off in the middle of a character when it was copied into
 * details[]. Invalid bytes become '?', a truncated character at the end is
 * dropped. The check is as strict as that of older libdbus versions (no
 * surrogates, no noncharacters).
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
			*p = '\0';		/* truncated character at the end */
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

/*
 * Close every inherited fd except 0-2 and keep, as Apple does in the child.
 * keep is moved to fd 3 if necessary; returns its new number, or -1 if there is
 * none. Uses close_range(2), else /proc/self/fd, else a plain loop.
 */
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
 * A minimal D-Bus client in place of libdbus. It does exactly what this program
 * needs: EXTERNAL authentication, method calls with arguments of type s
 * (string) and u (uint32), replies carrying a u or an h (a file descriptor,
 * passed via SCM_RIGHTS), and the NameOwnerChanged signal. Messages are sent
 * little-endian and read in either byte order.
 *
 * Layout of a message (D-Bus specification, "Message Protocol"):
 *
 *   offset 0   byte    byte order: 'l' or 'B'
 *          1   byte    message type (M_*)
 *          2   byte    flags
 *          3   byte    protocol version, 1
 *          4   uint32  length of the body
 *          8   uint32  serial number of this message
 *         12   uint32  length of the header fields
 *         16   ...     header fields: pairs of a code (F_*) and a variant,
 *                      each pair aligned to 8 bytes
 *              ...     padding to 8 bytes, then the body
 *
 * Every value is aligned to its own size. A string is a uint32 length, the
 * bytes and a terminating NUL; a signature is the same with a one-byte length.
 *
 * All traffic is synchronous: call() sends a request and reads messages until
 * the matching reply arrives. Whatever else comes in meanwhile, and whatever
 * the main loop finds between calls, goes to bus_other().
 */

#define DBUS_NAME     "org.freedesktop.DBus"
#define DBUS_PATH     "/org/freedesktop/DBus"
#define NO_AUTO_START 2			/* message flag: do not start the service */
#define MAX_FIELDS    (1u << 26)	/* limits set by the specification */
#define MAX_BODY      (1u << 27)

/* Message types and header field codes, numbered as in the specification */
enum { M_CALL = 1, M_RETURN, M_ERROR, M_SIGNAL };
enum { F_PATH = 1, F_IFACE, F_MEMBER, F_ERROR, F_REPLY_SERIAL, F_DEST, F_SENDER, F_SIGNATURE, F_UNIX_FDS };

/* One bus connection, together with the state of its incremental reader */
struct bus {
	const char *what;		/* name used in messages */
	int fd;				/* socket, or -1 when not connected */
	bool tried;			/* a connection attempt was made; it is not repeated */
	uint32_t serial;		/* serial number of the last message sent */
	unsigned char in[4096];		/* the message that is being read */
	size_t have;			/* bytes of it already in in[] */
	size_t skip;			/* rest of an oversized message that is being skipped */
	bool done;			/* in[] was handed out; the next read starts afresh */
	int fds[4];			/* fds that arrived with this message (SCM_RIGHTS) */
	int nfds;
};

static struct bus user_bus = { .what = "Session-Bus", .fd = -1 };
static struct bus sys_bus = { .what = "System-Bus", .fd = -1 };

/*
 * A received message. The pointers refer into bus.in[] and stay valid until the
 * next bus_next().
 */
struct message {
	int type;
	bool big;			/* big-endian */
	uint32_t reply_serial;
	const char *iface, *member, *error, *sender, *sig;
	const unsigned char *body;
	size_t body_len;
};

/*
 * An outgoing message under construction. Instead of writing past buf, put()
 * sets overflow; call() checks it once before sending.
 */
struct out {
	unsigned char buf[1024];
	size_t len;
	bool overflow;
};

/* Part of the state machine further down; called for every NameOwnerChanged */
static void name_changed(const char *name, const char *new_owner);

/* Read a uint32 in the byte order of the message it comes from */
static uint32_t rd32(const unsigned char *p, bool big)
{
	return big ? (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]
	           : (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
}

/* Store a uint32 little-endian */
static void set32(unsigned char *p, size_t v)
{
	p[0] = (unsigned char)v;
	p[1] = (unsigned char)(v >> 8);
	p[2] = (unsigned char)(v >> 16);
	p[3] = (unsigned char)(v >> 24);
}

/* put*(): append to an outgoing message, with the alignment D-Bus requires */
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

static void put_str(struct out *o, const char *s)	/* types s and o */
{
	size_t n = strlen(s);

	put_u32(o, (uint32_t)n);
	put(o, s, n + 1);
}

static void put_sig(struct out *o, const char *s)	/* type g (signature) */
{
	size_t n = strlen(s);

	put_byte(o, (unsigned char)n);
	put(o, s, n + 1);
}

/* One header field: its code, then a variant holding val (type 's', 'o' or 'g') */
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

/* Close those fds of the current message that nobody has taken over */
static void bus_drop_fds(struct bus *b)
{
	while (b->nfds > 0)
		if (b->fds[--b->nfds] >= 0)
			close(b->fds[b->nfds]);
}

/* Drop the connection and reset the reader. Locks held through it are gone. */
static void bus_kill(struct bus *b)
{
	if (b->fd >= 0)
		close(b->fd);
	b->fd = -1;
	bus_drop_fds(b);
	b->have = b->skip = 0;
	b->done = false;
}

/* Wait for events on fd, until deadline at most; false on timeout or error */
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

/* Send everything, until deadline at most */
static bool send_all(int fd, const void *data, size_t len, long long deadline)
{
	const unsigned char *p = data;

	while (len > 0) {
		ssize_t n = send(fd, p, len, MSG_NOSIGNAL | MSG_DONTWAIT);

		if (n >= 0) {
			p += n;
			len -= (size_t)n;
		} else if (errno != EINTR &&
		           ((errno != EAGAIN && errno != EWOULDBLOCK) || !wait_fd(fd, POLLOUT, deadline))) {
			return false;
		}
	}
	return true;
}

/* Read exactly len bytes, until deadline at most; buf == NULL discards them */
static bool recv_all(int fd, void *buf, size_t len, long long deadline)
{
	unsigned char junk[256], *p = buf;

	while (len > 0) {
		ssize_t n = recv(fd, p ? p : junk, p || len < sizeof junk ? len : sizeof junk, MSG_DONTWAIT);

		if (n > 0) {
			if (p)
				p += n;
			len -= (size_t)n;
		} else if (n == 0 || (errno != EINTR &&
		           ((errno != EAGAIN && errno != EWOULDBLOCK) || !wait_fd(fd, POLLIN, deadline)))) {
			return false;
		}
	}
	return true;
}

/* Send one complete message or authentication line */
static bool bus_send(struct bus *b, const void *data, size_t len)
{
	if (send_all(b->fd, data, len, now_ms() + CALL_TIMEOUT_MS))
		return true;
	DBG("%s: Senden fehlgeschlagen", b->what);
	bus_kill(b);	/* a half-sent message leaves the stream unusable */
	return false;
}

/*
 * Fetch at most want more bytes of the current message: 1 = got some,
 * 0 = timeout, -1 = connection lost. Callers never ask for more than the
 * current message has left. That is what makes fd passing safe: fds travel
 * with the first byte of their message, so whatever arrives here always
 * belongs to the message being read, never to the next one.
 */
static int bus_recv(struct bus *b, size_t want, long long deadline)
{
	union {
		char buf[CMSG_SPACE(sizeof b->fds)];
		struct cmsghdr align;
	} ctl;

	if (b->fd < 0)
		return -1;
	for (;;) {
		struct iovec iov = { b->in + b->have, want };
		struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1,
		                     .msg_control = ctl.buf, .msg_controllen = sizeof ctl.buf };
		ssize_t n = recvmsg(b->fd, &mh, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);

		if (n > 0) {
			/* a recvmsg() returns at most one control message: the one with the fds */
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

/*
 * Alignment of a D-Bus type; for the fixed-size types this is also their size.
 * 0 = unknown type.
 */
static size_t type_align(int type)
{
	switch (type) {
	case 'y': case 'g': case 'v':
		return 1;
	case 'n': case 'q':
		return 2;
	case 'b': case 'i': case 'u': case 'h': case 's': case 'o': case 'a':
		return 4;
	case 'x': case 't': case 'd': case '(': case '{':
		return 8;
	default:
		return 0;
	}
}

/*
 * Decode the header fields of a completely read message into m. Fields that
 * nobody here needs are skipped as long as their type is basic or an array; a
 * struct or variant (used by no field the specification defines) ends the
 * parse. So does malformed input: what was decoded up to that point stays, the
 * rest of m remains zero.
 */
static void parse_fields(const unsigned char *p, size_t fields_len, struct message *m)
{
	size_t i = 16, end = 16 + fields_len;

	for (;;) {
		const unsigned char *sig;
		const char *s = NULL;
		uint32_t u = 0, n, sig_len;
		size_t align;
		unsigned char code, type;

		/* field = code, signature, value; each one starts on an 8-byte boundary */
		i = (i + 7) & ~(size_t)7;
		if (i + 4 > end)
			return;
		code = p[i];
		sig_len = p[i + 1];
		sig = p + i + 2;
		if (sig_len == 0 || sig_len > end - i - 3 || sig[sig_len] != '\0')
			return;
		type = sig[0];
		align = type_align(type);
		i += 3 + sig_len;
		if (!align || type == 'v' || type == '(' || type == '{')
			return;
		i = (i + align - 1) & ~(align - 1);
		if (i > end)
			return;
		if (type == 's' || type == 'o' || type == 'a') {
			if (end - i < 4)
				return;
			n = rd32(p + i, m->big);
			i += 4;
			if (type == 'a') {
				/* length in bytes, counted from the first (aligned) element */
				align = type_align(sig[1]);
				if (!align)
					return;
				i = (i + align - 1) & ~(align - 1);
				if (i > end || n > end - i)
					return;
				i += n;
			} else {
				if (n >= end - i || p[i + n] != '\0')
					return;
				s = (const char *)p + i;
				i += (size_t)n + 1;
			}
		} else if (type == 'g') {
			if (end - i < 1)
				return;
			n = p[i++];
			if (n >= end - i || p[i + n] != '\0')
				return;
			s = (const char *)p + i;
			i += (size_t)n + 1;
		} else {
			if (end - i < align)
				return;
			if (type == 'u')
				u = rd32(p + i, m->big);
			i += align;
		}
		if (s) {
			switch (code) {
			case F_IFACE:     m->iface = s; break;
			case F_MEMBER:    m->member = s; break;
			case F_ERROR:     m->error = s; break;
			case F_SENDER:    m->sender = s; break;
			case F_SIGNATURE: m->sig = s; break;
			}
		} else if (code == F_REPLY_SERIAL && type == 'u') {
			m->reply_serial = u;
		}
	}
}

/*
 * Deliver the next message: 1 = m is filled, 0 = timeout, -1 = connection lost.
 * Reading resumes where the previous call stopped, so a deadline of 0 makes
 * this a non-blocking poll. A message is read in two steps - the fixed 16-byte
 * header, then exactly the rest - and never beyond its end.
 */
static int bus_next(struct bus *b, long long deadline, struct message *m)
{
	for (;;) {
		int r;

		if (b->done) {
			/* the previous message is done with; fds nobody took over are closed */
			b->done = false;
			b->have = 0;
			bus_drop_fds(b);
		}
		if (b->skip) {
			/* discard the rest of an oversized message, along with its fds */
			if ((r = bus_recv(b, b->skip < sizeof b->in ? b->skip : sizeof b->in, deadline)) <= 0)
				return r;
			b->skip -= b->have;
			b->have = 0;
			bus_drop_fds(b);
			/* not even an endless stream may hold the caller past its deadline */
			if (b->skip && now_ms() >= deadline)
				return 0;
			continue;
		}
		if (b->have < 16) {
			if ((r = bus_recv(b, 16 - b->have, deadline)) <= 0)
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
			/* no reply awaited here is that large: skip the message */
			DBG("%s: Nachricht mit %zu Bytes uebergangen", b->what, total);
			b->skip = total - b->have;
			b->have = 0;
			bus_drop_fds(b);
			continue;
		}
		if (b->have < total) {
			if ((r = bus_recv(b, total - b->have, deadline)) <= 0)
				return r;
			continue;
		}
		memset(m, 0, sizeof *m);
		m->type = b->in[1];
		m->big = big;
		m->body = b->in + body_off;
		m->body_len = body_len;
		parse_fields(b->in, fields_len, m);
		b->done = true;
		return 1;
	}
}

/* Next argument of type s from the body, advancing *off; NULL if there is none */
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

/*
 * Handle a message that is not the reply to the call in progress. Only one
 * kind matters: NameOwnerChanged from the bus itself.
 */
static void bus_other(struct bus *b, const struct message *m)
{
	size_t off = 0;
	const char *name, *new_owner;

	/* NameOwnerChanged(name, old owner, new owner) */
	if (b == &user_bus && m->type == M_SIGNAL &&
	    m->sender && !strcmp(m->sender, DBUS_NAME) &&
	    m->member && !strcmp(m->member, "NameOwnerChanged") &&
	    m->sig && !strncmp(m->sig, "sss", 3) &&
	    (name = body_str(m, &off)) && body_str(m, &off) && (new_owner = body_str(m, &off)))
		name_changed(name, new_owner);
	bus_drop_fds(b);
}

/*
 * Method call, blocking until the reply arrives or CALL_TIMEOUT_MS has passed.
 *
 * sig describes the arguments that follow it: 's' = const char *,
 * 'u' = uint32_t. want is the type of the expected return value: 'u'
 * (out = uint32_t *), 'h' (out = int *; receives an fd with CLOEXEC set, which
 * the caller then owns) or 0 for none. autostart = false: do not let the bus
 * launch the service (D-Bus activation), only talk to it if it is running.
 *
 * Returns false on any failure; "reason" then says why and "failure" what
 * kind it was. Messages that arrive ahead of the reply go to bus_other(),
 * which may run name_changed() - the assertion state can change during any
 * call().
 */
static bool call(struct bus *b, bool autostart, const char *dest, const char *path,
                 const char *iface, const char *member, char want, void *out,
                 const char *sig, ...)
{
	struct out o = { .len = 0 };
	struct message m;
	va_list ap;

	failure = E_OTHER;
	if (!b || b->fd < 0)
		return false;

	uint32_t serial = ++b->serial;

	put_byte(&o, 'l');
	put_byte(&o, M_CALL);
	put_byte(&o, autostart ? 0 : NO_AUTO_START);
	put_byte(&o, 1);		/* protocol version */
	put_u32(&o, 0);			/* length of the body, filled in below */
	put_u32(&o, serial);
	put_u32(&o, 0);			/* length of the header fields, filled in below */
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
		/* the deadline also holds while unrelated messages keep arriving */
		int r = now_ms() < deadline ? bus_next(b, deadline, &m) : 0;

		if (r <= 0) {
			fail("%s.%s: %s", iface, member, r ? "Verbindung getrennt" : "keine Antwort");
			return false;
		}
		if ((m.type == M_RETURN || m.type == M_ERROR) && m.reply_serial == serial)
			break;
		bus_other(b, &m);
	}

	bool ok = m.type == M_RETURN;

	snprintf(last_sender, sizeof last_sender, "%s", m.sender ? m.sender : "");

	if (!ok) {
		size_t off = 0;
		const char *text = m.sig && m.sig[0] == 's' ? body_str(&m, &off) : NULL;

		if (m.error && (!strcmp(m.error, DBUS_NAME ".Error.ServiceUnknown") ||
		                !strcmp(m.error, DBUS_NAME ".Error.NameHasNoOwner")))
			failure = E_ABSENT;	/* there is no such service */
		else if (m.error && !strcmp(m.error, DBUS_NAME ".Error.AccessDenied"))
			failure = E_DENIED;
		fail("%s.%s: %s", iface, member, text ? text : m.error ? m.error : "Fehler");
	} else if (want) {
		uint32_t v = m.body_len >= 4 ? rd32(m.body, m.big) : 0;

		ok = m.sig && m.sig[0] == want && m.body_len >= 4;
		if (ok && want == 'h') {
			/* the value is an index into the fds that came with this message */
			ok = v < (uint32_t)b->nfds && b->fds[v] >= 0;
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
	bus_drop_fds(b);
	return ok;
}

/* Connect to a unix stream socket (CLOEXEC); returns the fd or -1 */
static int unix_connect(const char *name, size_t len, bool abstract)
{
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	int fd;

	/* a path ends with a NUL byte, an abstract name starts with one */
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

/* Value of a hex digit, or -1 */
static int hexval(int c)
{
	return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
	       c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/*
 * Decode the value of a bus address key (%XX escapes) into out; returns its
 * length, 0 on error.
 */
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
 * Walk a D-Bus address such as "unix:path=/run/user/1000/bus" or
 * "unix:abstract=/tmp/dbus-X,guid=...;unix:path=..." and connect to the first
 * unix socket that accepts. Other transports are skipped on purpose:
 * "autolaunch:", for one, might start a dbus-daemon in an SSH session.
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

/*
 * Send one line of the authentication handshake and, if reply is set, read the
 * server's answer line into it.
 */
static bool sasl(struct bus *b, const char *cmd, size_t len, char *reply, size_t max)
{
	long long deadline = now_ms() + CALL_TIMEOUT_MS;
	size_t n = 0;

	if (!bus_send(b, cmd, len))
		return false;
	if (!reply)
		return true;
	/* byte by byte: nothing of the message stream behind the line may be swallowed */
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

/*
 * Authenticate on the freshly connected socket fd and say Hello, which makes
 * the bus assign our unique name. The handshake is the line-based SASL exchange
 * of the specification. On failure fd is closed and b->fd is -1.
 */
static bool bus_open(struct bus *b, int fd)
{
	char uid[16], cmd[64], line[256];
	int digits = snprintf(uid, sizeof uid, "%lu", (unsigned long)geteuid());
	size_t len = 0;

	b->fd = fd;
	if (fd < 0)
		return false;

	/* a NUL byte, then EXTERNAL with our uid as hex-encoded text */
	cmd[len++] = '\0';
	len += (size_t)snprintf(cmd + len, sizeof cmd - len, "AUTH EXTERNAL ");
	for (int i = 0; i < digits; i++)
		len += (size_t)snprintf(cmd + len, sizeof cmd - len, "%02x", (unsigned char)uid[i]);
	len += (size_t)snprintf(cmd + len, sizeof cmd - len, "\r\n");

	if (!sasl(b, cmd, len, line, sizeof line))
		goto fail;
	if (strncmp(line, "OK", 2) != 0) {
		/* in a user namespace (container) the bus knows us under a different
		 * uid: asked without one, it takes the uid from the socket */
		if (!sasl(b, "AUTH EXTERNAL\r\n", 15, line, sizeof line) || strncmp(line, "DATA", 4) != 0 ||
		    !sasl(b, "DATA\r\n", 6, line, sizeof line) || strncmp(line, "OK", 2) != 0)
			goto fail;
	}
	/* fd passing, for logind; if the bus refuses, only that call fails later */
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
 * The session bus, connected on first use; NULL if there is none. Never via X11
 * autolaunch: DBUS_SESSION_BUS_ADDRESS first, then the bus of the systemd user
 * instance at $XDG_RUNTIME_DIR/bus. The latter also helps when the variable is
 * stale (tmux, su), when the bus named there rejects the login, or - as
 * /run/user/<uid>/bus - when XDG_RUNTIME_DIR is missing (cron).
 *
 * Also subscribes to NameOwnerChanged for every backend name. That is how the
 * program learns that a desktop service stopped, started or restarted.
 */
static struct bus *get_user_bus(void)
{
	static const struct backend *const lists[] = { display_backends, system_backends };
	struct bus *b = &user_bus;
	const char *env = getenv("DBUS_SESSION_BUS_ADDRESS");
	bool ok = false;

	if (b->tried)
		return b->fd >= 0 ? b : NULL;
	b->tried = true;

	if (env && *env)
		ok = bus_open(b, addr_connect(env));
	if (!ok) {
		char path[sizeof ((struct sockaddr_un *)0)->sun_path];
		int n;

		env = getenv("XDG_RUNTIME_DIR");
		if (env && *env)
			n = snprintf(path, sizeof path, "%s/bus", env);
		else
			n = snprintf(path, sizeof path, "/run/user/%lu/bus", (unsigned long)geteuid());
		if (n > 0 && (size_t)n < sizeof path)
			ok = bus_open(b, unix_connect(path, (size_t)n, false));
	}
	if (!ok) {
		DBG("Session-Bus: nicht erreichbar");
		return NULL;
	}

	/* get told when one of the desktop services goes away or (re)appears */
	const char *seen[16];
	size_t n = 0;

	for (size_t l = 0; l < sizeof lists / sizeof *lists; l++) {
		for (const struct backend *be = lists[l]; be->dest; be++) {
			char rule[256];
			size_t k = 0;

			while (k < n && strcmp(seen[k], be->dest) != 0)
				k++;
			if (k < n || n == sizeof seen / sizeof *seen)
				continue;	/* the name occurs more than once in the lists */
			seen[n++] = be->dest;
			snprintf(rule, sizeof rule, "type='signal',sender='" DBUS_NAME "',path='" DBUS_PATH "',"
			         "interface='" DBUS_NAME "',member='NameOwnerChanged',arg0='%s'", be->dest);
			call(b, true, DBUS_NAME, DBUS_PATH, DBUS_NAME, "AddMatch", 0, NULL, "s", rule);
		}
	}
	return b->fd >= 0 ? b : NULL;
}

/* The system bus, connected on first use; NULL if it cannot be reached */
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

/*
 * Take an inhibitor lock ("idle" or "sleep") from logind, elogind or
 * ConsoleKit2. Returns the fd that holds the lock (CLOEXEC), or -1 with the
 * cause in "reason".
 */
static int logind_inhibit(const char *what)
{
	struct bus *b = get_sys_bus();
	char why[sizeof reason + 64] = "";
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
		/* report the cause given by the manager that exists - not that the
		 * other one is missing. Without polkit, logind refuses everyone but root. */
		if (failure != E_ABSENT && !*why)
			snprintf(why, sizeof why, "%s%s", reason,
			         failure == E_DENIED ? " (polkit fehlt oder erlaubt die Sperre nicht)" : "");
	}
	if (*why)
		fail("%s", why);
	else
		fail("weder logind noch elogind oder ConsoleKit2 am System-Bus");
	return -1;
}

/*
 * Take a lock from the first running desktop service in list and record it in
 * a (be, cookie). stop limits the search to the entries before it (NULL = all);
 * that is how a move to a better backend is attempted.
 */
static bool sess_inhibit(const struct backend *list, const struct backend *stop, struct assertion *a)
{
	struct bus *b = get_user_bus();
	uint32_t xid = 0, cookie;

	for (const struct backend *be = list; b && be->dest && be != stop; be++) {
		bool ok;

		inflight_dest = be->dest;
		inflight_hit = false;
		if (be->gsm_flags)
			ok = call(b, false, be->dest, be->path, be->iface, "Inhibit", 'u', &cookie,
			          "susu", NAME, xid, details, be->gsm_flags);
		else
			ok = call(b, false, be->dest, be->path, be->iface, "Inhibit", 'u', &cookie,
			          "ss", NAME, details);
		inflight_dest = NULL;
		if (!ok)
			continue;
		if (inflight_hit && strcmp(inflight_owner, last_sender) != 0) {
			/* The name changed hands during the call: the grant does not come
			 * from its present owner. It lapses with our connection;
			 * name_changed() has already scheduled a new attempt. */
			DBG("%s: Besitzer hat waehrend des Aufrufs gewechselt", be->dest);
			continue;
		}
		a->be = be;
		a->cookie = cookie;
		DBG("%s.Inhibit -> cookie %u", be->iface, a->cookie);
		return true;
	}
	return false;
}

/* Give a desktop service its lock back, if a holds one */
static void sess_uninhibit(struct assertion *a)
{
	const struct backend *be = a->be;	/* name_changed() may clear a->be during the call */

	if (!be)
		return;
	call(&user_bus, false, be->dest, be->path, be->iface, be->uninhibit, 0, NULL, "u", a->cookie);
	DBG("%s.%s(%u)", be->iface, be->uninhibit, a->cookie);
	a->be = NULL;
}

/* -u: wake the screen where the service can do that (KDE, XFCE) */
static void simulate_user_activity(void)
{
	struct bus *b = get_user_bus();

	for (const struct backend *be = display_backends; b && be->dest; be++)
		if (!be->gsm_flags &&
		    call(b, false, be->dest, be->path, be->iface, "SimulateUserActivity", 0, NULL, ""))
			break;
}

/* ---------------------------------------------------------------- X11 */

/*
 * Without a desktop service (i3, dwm, bspwm, Openbox ...) it is the X server
 * itself that blanks the screen: screen saver timeout and DPMS. Both stay off
 * for as long as some client demands it with the MIT-SCREEN-SAVER request
 * "Suspend", and resume when that client's connection ends - the same pattern
 * as the logind fd. This, too, without a library: connection setup and a
 * handful of requests, all little-endian.
 *
 * X11 in brief: a request starts with its opcode, one data byte (for an
 * extension: the minor opcode) and its length in 4-byte units. The server
 * numbers the requests of a connection in the order they arrive. What it sends
 * comes in 32-byte packets - an error (first byte 0), a reply (1, possibly
 * followed by extra data) or an event - and errors and replies carry the low
 * 16 bits of the number of the request they belong to.
 */

#define X_COOKIE "MIT-MAGIC-COOKIE-1"

/* State of an X connection while x11_setup() talks to the server */
struct xconn {
	int fd;
	long long deadline;		/* for the setup as a whole */
	unsigned seq;			/* number of the last request sent */
	unsigned watch;			/* a request without reply whose error matters */
	bool failed;			/* ... and that error has arrived */
};

/*
 * One field of an Xauthority file: length (2 bytes, big-endian), then the data.
 * A field longer than max is skipped and reported as *len = SIZE_MAX, which
 * matches nothing. false = end of file or read error.
 */
static bool xau_field(FILE *f, unsigned char *buf, size_t max, size_t *len)
{
	int hi = getc(f), lo = getc(f);

	if (hi == EOF || lo == EOF)
		return false;
	*len = (size_t)hi << 8 | (size_t)lo;
	if (*len > max) {
		/* does not fit: skip it, the entry then counts as "no match" */
		if (fseek(f, (long)*len, SEEK_CUR) < 0)
			return false;
		*len = SIZE_MAX;
		return true;
	}
	return fread(buf, 1, *len, f) == *len;
}

/*
 * Look up the MIT-MAGIC-COOKIE-1 for display num in $XAUTHORITY or
 * ~/.Xauthority; returns its length, or 0 if there is none. An entry is a
 * 2-byte family followed by four fields: address, display number, name of the
 * authorization scheme, and its data (the cookie).
 */
static size_t x11_cookie(const char *num, unsigned char *cookie, size_t max)
{
	const char *file = getenv("XAUTHORITY"), *home = getenv("HOME");
	char path[PATH_MAX], host[256] = "";
	unsigned char addr[256], number[16], name[32];
	size_t alen, nlen, tlen, clen, found = 0;
	FILE *f;

	if (!file || !*file) {
		if (!home || snprintf(path, sizeof path, "%s/.Xauthority", home) >= (int)sizeof path)
			return 0;
		file = path;
	}
	f = fopen(file, "re");
	if (!f)
		return 0;
	gethostname(host, sizeof host - 1);
	for (;;) {
		int hi = getc(f), lo = getc(f);
		unsigned family = (unsigned)hi << 8 | (unsigned)lo;

		if (hi == EOF || lo == EOF ||
		    !xau_field(f, addr, sizeof addr, &alen) || !xau_field(f, number, sizeof number, &nlen) ||
		    !xau_field(f, name, sizeof name, &tlen) || !xau_field(f, cookie, max, &clen))
			break;
		/* entry for this host (256 = local) or any (65535), for this display or any */
		if (tlen == strlen(X_COOKIE) && !memcmp(name, X_COOKIE, tlen) && clen > 0 && clen <= max &&
		    (family == 65535 || (family == 256 && alen == strlen(host) && !memcmp(addr, host, alen))) &&
		    (nlen == 0 || (nlen == strlen(num) && !memcmp(number, num, nlen)))) {
			found = clen;
			break;
		}
	}
	fclose(f);
	return found;
}

/* Send one request and count it */
static bool x11_send(struct xconn *x, const void *req, size_t len)
{
	x->seq++;
	return send_all(x->fd, req, len, x->deadline);
}

/*
 * Read packets until the server reacts to the last request: 1 = reply (in
 * pkt), 0 = error, -1 = connection lost or timeout. Along the way, note
 * whether an error for request x->watch comes in.
 */
static int x11_wait(struct xconn *x, unsigned char pkt[32])
{
	for (;;) {
		/* the deadline also holds while the server keeps sending events */
		if (now_ms() >= x->deadline || !recv_all(x->fd, pkt, 32, x->deadline))
			return -1;

		unsigned type = pkt[0], seq = pkt[2] | (unsigned)pkt[3] << 8;

		if (type == 1 || (type & 0x7F) == 35) {
			/* reply or GenericEvent: extra data follows, counted in 4-byte units */
			uint32_t extra = rd32(pkt + 4, false);

			if (extra > 65536 || !recv_all(x->fd, NULL, (size_t)extra * 4, x->deadline))
				return -1;
		}
		if (type == 0 && seq == (x->watch & 0xFFFF))
			x->failed = true;
		if (type <= 1 && seq == (x->seq & 0xFFFF))
			return (int)type;
	}
}

/* Major opcode of an extension; 0 if the server does not have it */
static unsigned x11_extension(struct xconn *x, const char *name)
{
	unsigned char req[8 + 32] = { 98 /* QueryExtension */ }, pkt[32];
	size_t n = strlen(name), padded = (n + 3) & ~(size_t)3;

	req[2] = (unsigned char)(2 + padded / 4);
	req[4] = (unsigned char)n;
	memcpy(req + 8, name, n);
	if (!x11_send(x, req, 8 + padded) || x11_wait(x, pkt) != 1 || !pkt[8])
		return 0;
	return pkt[9];
}

/*
 * Log in on the fresh connection fd (with the cookie for display num) and
 * suspend the screen saver together with DPMS. wake (-u): also wake the screen,
 * as user input would.
 */
static bool x11_setup(int fd, const char *num, bool wake)
{
	static const unsigned char reset[4] = { 115 /* ForceScreenSaver */, 0 /* Reset */, 1 };
	static const unsigned char sync[4] = { 43 /* GetInputFocus */, 0, 1 };
	struct xconn x = { .fd = fd, .deadline = now_ms() + CALL_TIMEOUT_MS };
	unsigned char setup[12 + 20 + 64] = { 'l', 0, 11 /* protocol 11.0 */ }, cookie[64], pkt[32];
	size_t clen = x11_cookie(num, cookie, sizeof cookie), nlen = clen ? strlen(X_COOKIE) : 0, len = 12;
	unsigned saver, dpms;

	/* without a cookie the server's access control list decides */
	setup[6] = (unsigned char)nlen;
	setup[8] = (unsigned char)clen;
	memcpy(setup + len, X_COOKIE, nlen);
	len += (nlen + 3) & ~(size_t)3;
	memcpy(setup + len, cookie, clen);
	len += (clen + 3) & ~(size_t)3;
	/* answer: status, then extra data in 4-byte units (on success the server's
	 * description of itself) */
	if (!send_all(fd, setup, len, x.deadline) || !recv_all(fd, pkt, 8, x.deadline) ||
	    !recv_all(fd, NULL, (size_t)(pkt[6] | pkt[7] << 8) * 4, x.deadline) || pkt[0] != 1) {
		DBG("X11: Display :%s antwortet nicht oder lehnt die Verbindung ab", num);
		return false;
	}

	saver = x11_extension(&x, "MIT-SCREEN-SAVER");
	if (!saver) {
		DBG("X11: Server ohne MIT-SCREEN-SAVER");
		return false;
	}

	const unsigned char suspend[8] = { (unsigned char)saver, 5 /* Suspend */, 2, 0, 1 /* True */ };

	if (!x11_send(&x, suspend, sizeof suspend))
		return false;
	x.watch = x.seq;
	if (wake && x11_send(&x, reset, sizeof reset) && (dpms = x11_extension(&x, "DPMS")) != 0) {
		const unsigned char on[8] = { (unsigned char)dpms, 6 /* ForceLevel */, 2, 0, 0 /* On */ };

		x11_send(&x, on, sizeof on);
	}
	/* Suspend has no reply: wait until the server has processed a request that
	 * has one. An error for Suspend would arrive before that reply. */
	if (!x11_send(&x, sync, sizeof sync) || x11_wait(&x, pkt) != 1 || x.failed) {
		DBG("X11: Bildschirmschoner laesst sich nicht aussetzen");
		return false;
	}
	DBG("X11: Bildschirmschoner und DPMS auf Display :%s ausgesetzt", num);
	return true;
}

/*
 * Returns the fd of a connection that keeps screen saver and DPMS suspended
 * for as long as it stays open, or -1.
 */
static int x11_suspend(bool wake)
{
	const char *dpy = getenv("DISPLAY"), *wayland = getenv("WAYLAND_DISPLAY");
	char num[8], path[32];
	size_t n;
	int fd;

	/* Under Wayland this would be Xwayland: its screen saver blanks nothing,
	 * and the connection might be what starts it in the first place. */
	if (!dpy || (wayland && *wayland))
		return -1;
	/* local displays ":N" and ":N.S" only; with ssh -X the screen would be elsewhere */
	if (!strncmp(dpy, "unix", 4))
		dpy += 4;
	n = *dpy == ':' ? strspn(dpy + 1, "0123456789") : 0;
	if (n == 0 || (dpy[1 + n] != '\0' && dpy[1 + n] != '.'))
		return -1;
	/* ":007" means display 7 */
	for (dpy++; n > 1 && *dpy == '0'; dpy++)
		n--;
	if (n >= sizeof num)
		return -1;
	memcpy(num, dpy, n);
	num[n] = '\0';
	n = (size_t)snprintf(path, sizeof path, "/tmp/.X11-unix/X%s", num);
	if ((fd = unix_connect(path, n, false)) < 0 && (fd = unix_connect(path, n, true)) < 0) {
		DBG("X11: Display :%s nicht erreichbar", num);
		return -1;
	}
	if (!x11_setup(fd, num, wake)) {
		close(fd);
		return -1;
	}
	return fd;
}

/*
 * Discard whatever the X server sends unasked (every client gets MappingNotify,
 * for example), so that nothing piles up on its side. If the server is gone,
 * close the connection.
 */
static void x11_drain(struct assertion *a)
{
	unsigned char junk[512];
	size_t total = 0;
	ssize_t n = 1;

	/* in portions, so that the main loop gets its turn even during a flood */
	for (int i = 0; i < 64 && (n = recv(a->xfd, junk, sizeof junk, MSG_DONTWAIT)) > 0; i++)
		total += (size_t)n;
	if (total)
		DBG("X11: %zu Bytes vom Server verworfen", total);
	if (n > 0 || (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)))
		return;
	DBG("X11: Verbindung beendet");
	close(a->xfd);
	a->xfd = -1;
	if (!a->be && a->fd < 0 && !a->lost) {
		fprintf(stderr, NAME ": %s: Sperre verloren: X-Server beendet\n", type_name[a - as]);
		a->lost = a->stale = true;
		if (refresh_at == FOREVER)
			refresh_at = now_ms() + LOST_RETRY_MS;
	}
}

/* ---------------------------------------------------------------- Power supply */

/* First line of /sys/class/power_supply/<dev>/<attr>, without the newline */
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
 * USB-C: a port that is currently powering a device (role "source") is not a
 * power source for us.
 */
static bool usb_is_source(const char *dev)
{
	char path[PATH_MAX], buf[64];
	bool source = false, sink = false;
	struct dirent *e;
	DIR *d;

	snprintf(path, sizeof path, POWER_DIR "/%s/device/typec", dev);
	d = opendir(path);
	if (!d)
		return false;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		snprintf(path, sizeof path, "%s/device/typec/%s", dev, e->d_name);
		if (!read_sysfs(path, "power_role", buf, sizeof buf))
			continue;
		/* the active role is the one in brackets: "[source] sink" */
		if (strstr(buf, "[source]"))
			source = true;
		else if (strstr(buf, "[sink]"))
			sink = true;
	}
	closedir(d);
	return source && !sink;
}

/*
 * The rules of systemd's on_ac_power(): on mains as soon as one power source
 * reports online; otherwise off mains only if a battery is discharging. A
 * machine without battery (desktop) therefore counts as "on mains" even when a
 * USB-C port reports offline as a power source. Batteries of peripherals
 * (mouse, keyboard) and empty battery bays do not count.
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
			if (read_sysfs(e->d_name, "present", buf, sizeof buf) && !strcmp(buf, "0"))
				continue;
			/* no readable status: like systemd, assume "discharging" to be safe */
			if (!read_sysfs(e->d_name, "status", buf, sizeof buf) || !strcmp(buf, "Discharging"))
				discharging = true;
		} else if (!(strncmp(buf, "USB", 3) == 0 && usb_is_source(e->d_name)) &&
		           read_sysfs(e->d_name, "online", buf, sizeof buf) && strtoul(buf, NULL, 10) > 0) {
			online = true;	/* 1 = fixed, 2 = programmable (USB PD) */
		}
	}
	closedir(d);
	return online || !discharging;
}

/*
 * -s: hold the "sleep" lock only while on mains power, as Apple does. The main
 * loop calls this every AC_POLL_MS. false = on mains, but the lock could not be
 * taken.
 */
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

/* ---------------------------------------------------------------- Acquire and release */

/*
 * How an assertion is enforced, and how that is kept true over time:
 *
 *   create_one()          First choice is a desktop service on the session
 *                         bus (a->be, a->cookie), else the logind "idle"
 *                         inhibitor (a->fd). -d and -u additionally suspend
 *                         the X screen saver (a->xfd). -s only ever uses
 *                         the logind "sleep" inhibitor.
 *
 *   name_changed()        A backend service stopped, restarted or appeared:
 *                         mark the affected assertions stale and schedule
 *                         refresh_assertions() through refresh_at.
 *
 *   refresh_assertions()  Re-acquire the stale ones: from the same service
 *                         once it is back, from a better one that appeared,
 *                         or from the fallback. An old lock is only dropped
 *                         after the new one is held, so there is no gap.
 *
 * After a service restart the first attempt follows SETTLE_MS later. If no
 * desktop service took the lock, there is one follow-up RETRY_MS after that.
 * An assertion that then has no lock at all is reported on stderr and retried
 * every LOST_RETRY_MS until it is back.
 */

/* The desktop services that can enforce an assertion type; NULL for -s and -m */
static const struct backend *backends_for(int type)
{
	return type == A_IDLE ? system_backends :
	       type == A_DISPLAY || type == A_USER ? display_backends : NULL;
}

/*
 * Acquire whatever enforces assertion type on this system (see above).
 * first = false: re-acquire after a service restart; the screen is not woken
 * again, and the X connection, which outlives such restarts, is kept. Returns
 * false if nothing could be acquired; "reason" then says why.
 */
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
	case A_DISPLAY:
		/* the X11 connection outlives a restart of the desktop services */
		if (first)
			a->xfd = x11_suspend(type == A_USER);
		/* fall through */
	case A_IDLE:
		if (sess_inhibit(backends_for(type), NULL, a))
			return true;
		a->fd = logind_inhibit("idle");
		return a->fd >= 0 || a->xfd >= 0;
	case A_SYSTEM:
		/* on battery the assertion is accepted, but only takes effect on mains */
		if (!get_sys_bus()) {
			fail("System-Bus nicht erreichbar");
			return false;
		}
		return update_system_assertion();
	case A_DISK:
		/* no counterpart on Linux: accepted, nothing to do */
		return true;
	default:
		return false;
	}
}

/* Release one assertion: close its fds and hand its cookie back */
static void release_one(int type)
{
	struct assertion *a = &as[type];

	if (!a->active)
		return;
	if (a->fd >= 0)
		close(a->fd);
	if (a->xfd >= 0)
		close(a->xfd);
	a->fd = a->xfd = -1;
	sess_uninhibit(a);
	a->active = a->stale = false;
	DBG("%s freigegeben", type_name[type]);
}

/* Release everything and disconnect from both buses */
static void release_all(void)
{
	for (int i = 0; i < A_COUNT; i++)
		release_one(i);
	bus_kill(&user_bus);
	bus_kill(&sys_bus);
}

/*
 * Like createAssertions() in Apple's source: create the requested assertions in
 * type order. false = failure; the caller then exits with status 1.
 */
static bool create_assertions(const bool want[A_COUNT], long timeout)
{
	for (int i = 0; i < A_COUNT; i++) {
		if (!want[i])
			continue;
		if (i == A_USER && timeout == 0)
			timeout = 5;	/* as with Apple, this then applies to all later ones too */
		if (!create_one(i, true)) {
			/* mark it active so a half-created lock (e.g. -s) is released cleanly */
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
 * The owner of a service name has changed: the service exited (new_owner is
 * empty) or was (re)started. A lock held there is gone with it. If a service
 * appears that precedes the current one in the list - or any service at all
 * while only the fallback lock holds - the assertion moves over to it.
 *
 * This runs in the middle of call() or sess_dispatch(). It therefore only
 * records what happened and schedules refresh_assertions(); it never makes a
 * call itself.
 */
static void name_changed(const char *name, const char *new_owner)
{
	if (inflight_dest && !strcmp(name, inflight_dest)) {
		inflight_hit = true;
		snprintf(inflight_owner, sizeof inflight_owner, "%s", new_owner);
	}
	for (int i = 0; i < A_COUNT; i++) {
		struct assertion *a = &as[i];
		const struct backend *be = backends_for(i);

		if (!a->active || !be)
			continue;
		if (a->be && !strcmp(a->be->dest, name)) {
			/* the cookie belonged to the old owner; with the new one the
			 * same number could be the lock of another program */
			a->be = NULL;
			owner_changes++;
		} else {
			/* otherwise this only matters if the service came up and ranks
			 * before the one currently used */
			while (be->dest && be != a->be && strcmp(be->dest, name) != 0)
				be++;
			if (!*new_owner || !be->dest || be == a->be)
				continue;
		}
		DBG("%s: Besitzer von %s hat gewechselt", type_name[i], name);
		a->stale = true;
		retry = true;
		refresh_at = now_ms() + SETTLE_MS;
	}
}

/*
 * Bring every stale assertion back in line with what the system offers now (see
 * the overview above). The caller has reset refresh_at; if a signal arrives
 * while this runs, name_changed() schedules the next round.
 */
static void refresh_assertions(void)
{
	bool follow_up = retry, again = false, lost = false;

	retry = false;
	for (int i = 0; i < A_COUNT; i++) {
		/* with no lock at all, even broken bus connections get another try */
		if (as[i].active && as[i].lost) {
			if (user_bus.fd < 0)
				user_bus.tried = false;
			if (sys_bus.fd < 0)
				sys_bus.tried = false;
		}
	}
	for (int i = 0; i < A_COUNT; i++) {
		struct assertion *a = &as[i], old = as[i];

		if (!a->active || !a->stale)
			continue;
		a->stale = false;
		if (old.be) {
			/* A service further up the list has appeared: lock there first, then
			 * give up the previous lock - unless its service has changed owner in
			 * the meantime, in which case that lock no longer exists. */
			unsigned before = owner_changes;

			if (sess_inhibit(backends_for(i), old.be, a) && before == owner_changes)
				sess_uninhibit(&old);
			continue;
		}
		create_one(i, false);
		if (a->be || a->fd >= 0) {
			/* release the old fallback lock only now, so that there is no gap */
			if (old.fd >= 0)
				close(old.fd);
			if (a->lost)
				fprintf(stderr, NAME ": %s: Sperre wieder angelegt\n", type_name[i]);
			a->lost = false;
		} else {
			/* got nothing new: the old fallback lock stays in force */
			a->fd = old.fd;
			if (old.fd < 0 && a->xfd < 0) {
				/* no lock at all: report it once the follow-up has failed as
				 * well, and keep trying at longer intervals */
				if (!follow_up && !a->lost) {
					fprintf(stderr, NAME ": %s: Sperre verloren%s%s\n", type_name[i],
					        *reason ? ": " : "", reason);
					a->lost = true;
				}
				a->stale = lost = true;
			}
		}
		/* no desktop service yet - it may still be starting: look once more */
		if (!a->be && follow_up)
			a->stale = again = true;
	}
	if (refresh_at == FOREVER && (again || lost))
		refresh_at = now_ms() + (again ? RETRY_MS : LOST_RETRY_MS);
}

/*
 * The session bus is gone (logout): the locks held there no longer exist.
 * Refresh at once, which now leads to the logind fallback.
 */
static void sess_gone(void)
{
	for (int i = 0; i < A_COUNT; i++) {
		if (as[i].active && as[i].be) {
			as[i].be = NULL;
			as[i].stale = true;
			retry = false;
			refresh_at = 0;
		}
	}
}

/* Process what has arrived on the session bus, without waiting */
static void sess_dispatch(void)
{
	struct message m;
	int r = 0;

	/* in portions: signals and deadlines of the main loop get their turn even during a flood */
	for (int n = 0; n < 32 && (r = bus_next(&user_bus, 0, &m)) > 0; n++)
		bus_other(&user_bus, &m);
	if (r < 0)
		sess_gone();
}

/* -w without pidfd: does the process still exist? A zombie counts as gone. */
static bool pid_alive(pid_t pid)
{
	char path[64], buf[512];
	const char *end;
	size_t n;
	FILE *f;

	if (kill(pid, 0) < 0 && errno == ESRCH)
		return false;
	snprintf(path, sizeof path, "/proc/%ld/stat", (long)pid);
	f = fopen(path, "re");
	if (!f)
		return true;
	n = fread(buf, 1, sizeof buf - 1, f);
	fclose(f);
	buf[n] = '\0';
	/* "pid (name) state ..."; the name may itself contain parentheses */
	end = strrchr(buf, ')');
	return !(end && end[1] == ' ' && (end[2] == 'Z' || end[2] == 'X'));
}

/*
 * Have the signal delivered through the signalfd - unless it is being ignored
 * (nohup, background job of a shell without job control). Blocked, it would
 * arrive there all the same, and caffeinate would exit although the command
 * keeps running.
 */
static void watch_signal(sigset_t *mask, int sig)
{
	struct sigaction sa;

	if (sigaction(sig, NULL, &sa) < 0 || sa.sa_handler != SIG_IGN)
		sigaddset(mask, sig);
}

/* ---------------------------------------------------------------- main */

/*
 * End the way a process killed by sig ends, so that the parent sees the true
 * cause: restore the default action, unblock the signal and raise it again.
 * Returns the shell's exit status for that signal in case this does not kill.
 */
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

/*
 * 1. parse the options the way Apple's getopt loop does
 * 2. with a command: fork; the parent becomes the command, the child goes on
 * 3. create the assertions
 * 4. event loop, until a timeout, a signal or the end of the watched process
 * 5. release everything
 */
int main(int argc, char **argv)
{
	bool want[A_COUNT] = { false };
	long timeout = 0, waitforpid = 0;
	const char *env = getenv("CAFFEINATE_DEBUG");
	int ch;

	debug = env && *env && strcmp(env, "0") != 0;
	for (int i = 0; i < A_COUNT; i++)
		as[i].fd = as[i].xfd = -1;

	/* '+': stop at the first non-option like BSD getopt, which leaves the
	 * command's own flags alone; ':' and opterr = 0: print the messages in BSD
	 * wording instead of glibc's or musl's */
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
			if (timeout < 0)	/* unsigned in Apple's code: practically infinite */
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

	/* no assertion flag at all: -i is the default */
	bool any = false;
	for (int i = 0; i < A_COUNT; i++)
		any |= want[i];
	if (!any)
		want[A_IDLE] = true;

	/* the signals that end caffeinate are read from a signalfd in the event loop */
	sigset_t mask, oldmask;
	sigemptyset(&mask);
	watch_signal(&mask, SIGTERM);
	watch_signal(&mask, SIGHUP);

	int pfd = -1;		/* pidfd of the process whose end is awaited */
	int death_sig = 0;	/* command without pidfd: signal sent when the parent dies */
	pid_t pid_poll = 0;	/* -w without pidfd: pid to check with kill(pid, 0) */
	long long deadline = FOREVER;	/* when caffeinate itself ends (-t without command) */
	bool have_cmd = argc - optind > 0;

	if (have_cmd) {
		/* Like Apple: fork early. The parent exec()s the command, so PID,
		 * exit status and signals remain the command's own; the child
		 * holds the assertions and watches over the parent. */
		argv += optind;
		pid_t parent = getpid();
		int self_fd = sys_pidfd_open(parent);	/* CLOEXEC by default */

		switch (fork()) {
		case 0:
			break;
		case -1:
			/* no helper process, hence no locks - the command runs all the same */
			perror(NAME ": fork");
			/* fall through */
		default:
			execvp(*argv, argv);
			int saved_errno = errno;
			perror(*argv);
			_exit(saved_errno == ENOENT ? 127 : 126);
		}

		/* child: keep only the pidfd; SIGINT and SIGQUIT from the terminal are
		 * meant for the command */
		pfd = close_inherited_fds(self_fd);
		signal(SIGINT, SIG_IGN);
		signal(SIGQUIT, SIG_IGN);
		if (pfd < 0) {
			/* no pidfd (kernel < 5.3): let the kernel signal us when the parent
			 * dies. With a signal of our own, since SIGTERM may be ignored
			 * (nohup, trap). */
			death_sig = SIGUSR1;
			sigaddset(&mask, death_sig);
		}
		sigprocmask(SIG_BLOCK, &mask, &oldmask);
		if (death_sig)
			prctl(PR_SET_PDEATHSIG, death_sig);
		/* the parent was gone before we could start watching it */
		if (getppid() != parent)
			return EXIT_SUCCESS;
		snprintf(details, sizeof details,
		         "caffeinate asserting on behalf of '%s' (pid %d)", *argv, (int)parent);
	} else {
		/* no command: SIGINT and SIGQUIT from the terminal end caffeinate itself */
		watch_signal(&mask, SIGINT);
		watch_signal(&mask, SIGQUIT);
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
					deadline = 0;	/* no such process: exit at once, like Apple */
				else	/* no pidfd (kernel < 5.3, seccomp): check periodically */
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

	/* details[] travels over D-Bus and has to be valid UTF-8 */
	utf8_sanitize(details);
	if (!create_assertions(want, timeout)) {
		release_all();
		return EXIT_FAILURE;
	}

	int exit_code = EXIT_SUCCESS, term_sig = 0;

	/* Event loop. Each round: handle bus traffic, run a refresh that is due,
	 * expire assertions, do the periodic checks, then sleep in poll() until
	 * the earliest deadline or until one of the fds has something. */
	for (;;) {
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
			if (!pid_alive(pid_poll))
				break;
			if (next == FOREVER || now + PID_POLL_MS < next)
				next = now + PID_POLL_MS;
		}
		/* the session bus may have died in any of the calls above */
		if (user_bus.fd < 0)
			sess_gone();
		if (refresh_at != FOREVER && (next == FOREVER || refresh_at < next))
			next = refresh_at;

		/* poll set: signalfd, pidfd if there is one, session bus, X connections */
		long long wait = next == FOREVER ? -1 : next < now ? 0 : next - now;
		struct pollfd p[3 + A_COUNT] = { { sfd, POLLIN, 0 }, { pfd, POLLIN, 0 } };
		nfds_t n = pfd >= 0 ? 2 : 1, first_x;

		/* session bus: this only wakes us; reading happens at the top of the loop */
		if (user_bus.fd >= 0)
			p[n++] = (struct pollfd){ user_bus.fd, POLLIN, 0 };
		first_x = n;
		for (int i = 0; i < A_COUNT; i++)
			if (as[i].active && as[i].xfd >= 0)
				p[n++] = (struct pollfd){ as[i].xfd, POLLIN, 0 };

		if (poll(p, n, wait > INT_MAX ? INT_MAX : (int)wait) < 0) {
			if (errno == EINTR)
				continue;
			perror(NAME ": poll");
			exit_code = EXIT_FAILURE;
			break;
		}
		/* a signal: leave the loop; death_sig only says that the parent is gone */
		if (p[0].revents & POLLIN) {
			struct signalfd_siginfo si;
			if (read(sfd, &si, sizeof si) == sizeof si) {
				if ((int)si.ssi_signo != death_sig)
					term_sig = (int)si.ssi_signo;
				break;
			}
		}
		/* the watched process (-w, or the command) has ended */
		if (pfd >= 0 && (p[1].revents & (POLLIN | POLLHUP | POLLERR)))
			break;
		for (int i = 0; i < A_COUNT; i++)
			if (as[i].active && as[i].xfd >= 0 && p[first_x++].revents)
				x11_drain(&as[i]);
	}

	release_all();
	if (pfd >= 0)
		close(pfd);
	close(sfd);
	/* everything is released: now end through the signal that stopped us */
	if (term_sig)
		exit_code = die_by_signal(term_sig, &oldmask);
	return exit_code;
}
