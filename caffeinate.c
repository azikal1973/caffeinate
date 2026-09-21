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
 *                                      (GNOME/MATE: SessionManager, Flag idle)
 *   -s  PreventSystemSleep (nur AC) -> logind-Inhibitor "sleep"
 *   -u  UserIsActive                -> ScreenSaver.SimulateUserActivity + wie -d
 *   -m  PreventDiskIdle             -> kein Gegenstueck, wird angenommen
 * Ohne Session-Bus oder ohne passenden Desktop-Dienst fallen -i/-d/-u auf
 * den logind-Inhibitor "idle" zurueck. logind heisst hier auch elogind.
 *
 * Alle Sperren haengen an Bus-Verbindungen bzw. am logind-fd und fallen
 * weg, sobald der Prozess endet - auch bei kill -9.
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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <dbus/dbus.h>

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
#define AC_POLL_MS      5000	/* Netzteil-Abfrage fuer -s */
#define PID_POLL_MS     1000	/* -w ohne pidfd (Kernel < 5.3) */
#define CALL_TIMEOUT_MS 5000
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
#define SS_DEST     "org.freedesktop.ScreenSaver"
#define SS_PATH     "/ScreenSaver"
#define GNOME_SM    "org.gnome.SessionManager", "/org/gnome/SessionManager", "org.gnome.SessionManager", "Uninhibit"
#define MATE_SM     "org.mate.SessionManager", "/org/mate/SessionManager", "org.mate.SessionManager", "Uninhibit"

static const struct backend display_backends[] = {
	{ SS_DEST, SS_PATH, SS_DEST, "UnInhibit", 0 },
	{ GNOME_SM, GSM_IDLE },
	{ MATE_SM, GSM_IDLE },
	{ NULL, NULL, NULL, NULL, 0 }
};

static const struct backend system_backends[] = {
	{ "org.freedesktop.PowerManagement.Inhibit", "/org/freedesktop/PowerManagement/Inhibit",
	  "org.freedesktop.PowerManagement.Inhibit", "UnInhibit", 0 },
	{ GNOME_SM, GSM_SUSPEND },
	{ MATE_SM, GSM_SUSPEND },
	{ NULL, NULL, NULL, NULL, 0 }
};

struct assertion {
	bool active;
	long long expires;		/* CLOCK_MONOTONIC in ms oder FOREVER */
	const struct backend *be;	/* Session-Bus-Sperre */
	uint32_t cookie;
	int fd;				/* logind-Sperre */
};

static struct assertion as[A_COUNT];
static DBusConnection *user_bus, *sys_bus;
static bool user_tried, sys_tried;
static char details[160];

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
 * D-Bus verlangt gueltiges UTF-8, libdbus bricht sonst ab. Der Befehlsname
 * kann beliebige Bytes enthalten (Latin-1-Dateinamen) und wird in details
 * ggf. mitten in einem Zeichen abgeschnitten: ungueltige Bytes werden zu
 * '?', ein abgeschnittenes Zeichen am Ende faellt weg. Geprueft wird so
 * streng wie aeltere libdbus (keine Surrogates, keine Nichtzeichen).
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

static DBusConnection *bus_finish(DBusConnection *c, DBusError *e, const char *what)
{
	if (c && !dbus_bus_register(c, e)) {
		dbus_connection_close(c);
		dbus_connection_unref(c);
		c = NULL;
	}
	if (!c) {
		DBG("%s: %s", what, dbus_error_is_set(e) ? e->message : "nicht erreichbar");
		dbus_error_free(e);
		return NULL;
	}
	dbus_connection_set_exit_on_disconnect(c, FALSE);
	return c;
}

/*
 * Session-Bus ohne X11-Autolaunch: nur DBUS_SESSION_BUS_ADDRESS oder
 * $XDG_RUNTIME_DIR/bus - sonst wuerde libdbus per SSH ggf. einen
 * dbus-daemon starten.
 */
static DBusConnection *get_user_bus(void)
{
	if (user_tried)
		return user_bus;
	user_tried = true;

	DBusError e;
	dbus_error_init(&e);
	const char *addr = getenv("DBUS_SESSION_BUS_ADDRESS");
	char *owned = NULL;

	if (!addr || !*addr) {
		const char *rt = getenv("XDG_RUNTIME_DIR");
		char path[PATH_MAX];
		struct stat st;

		if (!rt || snprintf(path, sizeof path, "%s/bus", rt) >= (int)sizeof path ||
		    stat(path, &st) < 0 || !S_ISSOCK(st.st_mode)) {
			DBG("Session-Bus: keine Adresse");
			return NULL;
		}
		char *esc = dbus_address_escape_value(path);
		if (!esc)
			return NULL;
		size_t len = strlen(esc) + sizeof "unix:path=";
		owned = malloc(len);
		if (owned)
			snprintf(owned, len, "unix:path=%s", esc);
		dbus_free(esc);
		if (!owned)
			return NULL;
		addr = owned;
	}
	user_bus = bus_finish(dbus_connection_open_private(addr, &e), &e, "Session-Bus");
	free(owned);
	return user_bus;
}

static DBusConnection *get_sys_bus(void)
{
	if (sys_tried)
		return sys_bus;
	sys_tried = true;

	DBusError e;
	dbus_error_init(&e);
	DBusConnection *c = dbus_bus_get_private(DBUS_BUS_SYSTEM, &e);
	if (c) {
		/* dbus_bus_get_private meldet sich schon selbst an */
		dbus_connection_set_exit_on_disconnect(c, FALSE);
	} else {
		DBG("System-Bus: %s", dbus_error_is_set(&e) ? e.message : "nicht erreichbar");
		dbus_error_free(&e);
	}
	sys_bus = c;
	return c;
}

static void bus_close(DBusConnection *c)
{
	if (!c)
		return;
	dbus_connection_flush(c);
	dbus_connection_close(c);
	dbus_connection_unref(c);
}

/*
 * Methodenaufruf, blockierend. autostart = false: keine Desktop-Dienste per
 * D-Bus-Aktivierung hochfahren, nur laufende nutzen.
 */
static DBusMessage *call(DBusConnection *c, bool autostart, const char *dest, const char *path,
                         const char *iface, const char *method, int first_type, ...)
{
	DBusMessage *m, *r = NULL;
	DBusError e;
	va_list ap;

	if (!c)
		return NULL;
	m = dbus_message_new_method_call(dest, path, iface, method);
	if (!m)
		return NULL;
	dbus_message_set_auto_start(m, autostart);
	va_start(ap, first_type);
	bool ok = dbus_message_append_args_valist(m, first_type, ap);
	va_end(ap);

	dbus_error_init(&e);
	if (ok)
		r = dbus_connection_send_with_reply_and_block(c, m, CALL_TIMEOUT_MS, &e);
	if (!r)
		DBG("%s.%s: %s", iface, method, dbus_error_is_set(&e) ? e.message : "Fehler");
	dbus_error_free(&e);
	dbus_message_unref(m);
	return r;
}

/* logind-Inhibitor; liefert einen fd (CLOEXEC) oder -1. */
static int logind_inhibit(const char *what)
{
	const char *who = NAME, *why = details, *mode = "block";
	DBusMessage *r;
	DBusError e;
	int fd = -1;

	r = call(get_sys_bus(), true, "org.freedesktop.login1", "/org/freedesktop/login1",
	         "org.freedesktop.login1.Manager", "Inhibit",
	         DBUS_TYPE_STRING, &what, DBUS_TYPE_STRING, &who,
	         DBUS_TYPE_STRING, &why, DBUS_TYPE_STRING, &mode, DBUS_TYPE_INVALID);
	if (!r)
		return -1;
	dbus_error_init(&e);
	if (!dbus_message_get_args(r, &e, DBUS_TYPE_UNIX_FD, &fd, DBUS_TYPE_INVALID)) {
		DBG("logind Inhibit: %s", e.message);
		dbus_error_free(&e);
		fd = -1;
	} else {
		fcntl(fd, F_SETFD, FD_CLOEXEC);
		DBG("logind Inhibit(%s) -> fd %d", what, fd);
	}
	dbus_message_unref(r);
	return fd;
}

/* Erstes laufende Desktop-Backend der Liste nehmen. */
static bool sess_inhibit(const struct backend *list, struct assertion *a)
{
	DBusConnection *c = get_user_bus();
	const char *app = NAME, *why = details;
	uint32_t xid = 0;

	for (const struct backend *be = list; c && be->dest; be++) {
		DBusMessage *r;

		if (be->gsm_flags)
			r = call(c, false, be->dest, be->path, be->iface, "Inhibit",
			         DBUS_TYPE_STRING, &app, DBUS_TYPE_UINT32, &xid,
			         DBUS_TYPE_STRING, &why, DBUS_TYPE_UINT32, &be->gsm_flags,
			         DBUS_TYPE_INVALID);
		else
			r = call(c, false, be->dest, be->path, be->iface, "Inhibit",
			         DBUS_TYPE_STRING, &app, DBUS_TYPE_STRING, &why, DBUS_TYPE_INVALID);
		if (!r)
			continue;
		bool ok = dbus_message_get_args(r, NULL, DBUS_TYPE_UINT32, &a->cookie, DBUS_TYPE_INVALID);
		dbus_message_unref(r);
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
	DBusMessage *r;

	if (!a->be)
		return;
	r = call(user_bus, false, a->be->dest, a->be->path, a->be->iface, a->be->uninhibit,
	         DBUS_TYPE_UINT32, &a->cookie, DBUS_TYPE_INVALID);
	if (r)
		dbus_message_unref(r);
	DBG("%s.%s(%u)", a->be->iface, a->be->uninhibit, a->cookie);
	a->be = NULL;
}

/* ---------------------------------------------------------------- Netzteil */

static bool read_sysfs(const char *dev, const char *attr, char *buf, size_t len)
{
	char path[PATH_MAX];
	FILE *f;
	bool ok;

	snprintf(path, sizeof path, "/sys/class/power_supply/%s/%s", dev, attr);
	f = fopen(path, "re");
	if (!f)
		return false;
	ok = fgets(buf, (int)len, f) != NULL;
	fclose(f);
	if (ok)
		buf[strcspn(buf, "\n")] = '\0';
	return ok;
}

/* Ein Rechner ganz ohne Netzteil-Eintrag (Desktop) gilt als "am Netz". */
static bool on_ac_power(void)
{
	DIR *d = opendir("/sys/class/power_supply");
	struct dirent *e;
	bool have_mains = false, online = false;
	char buf[32];

	if (!d)
		return true;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		if (!read_sysfs(e->d_name, "type", buf, sizeof buf))
			continue;
		if (strcmp(buf, "Mains") != 0 && strncmp(buf, "USB", 3) != 0)
			continue;
		have_mains = true;
		if (read_sysfs(e->d_name, "online", buf, sizeof buf) && !strcmp(buf, "1"))
			online = true;
	}
	closedir(d);
	return !have_mains || online;
}

/* -s: Sperre nur am Netzteil halten */
static void update_system_assertion(void)
{
	struct assertion *a = &as[A_SYSTEM];
	bool ac = on_ac_power();

	if (ac && a->fd < 0) {
		a->fd = logind_inhibit("sleep");
	} else if (!ac && a->fd >= 0) {
		DBG("Akkubetrieb: sleep-Sperre freigegeben");
		close(a->fd);
		a->fd = -1;
	}
}

/* ---------------------------------------------------------------- anlegen/freigeben */

static bool create_one(int type)
{
	struct assertion *a = &as[type];

	a->fd = -1;
	a->be = NULL;
	switch (type) {
	case A_IDLE:
		if (sess_inhibit(system_backends, a))
			return true;
		a->fd = logind_inhibit("idle");
		return a->fd >= 0;
	case A_USER: {
		DBusMessage *r = call(get_user_bus(), false, SS_DEST, SS_PATH, SS_DEST,
		                      "SimulateUserActivity", DBUS_TYPE_INVALID);
		if (r)
			dbus_message_unref(r);
	}
		/* fall through */
	case A_DISPLAY:
		if (sess_inhibit(display_backends, a))
			return true;
		a->fd = logind_inhibit("idle");
		return a->fd >= 0;
	case A_SYSTEM:
		/* Akkubetrieb: Assertion gilt, wirkt aber erst am Netzteil */
		if (!get_sys_bus())
			return false;
		update_system_assertion();
		return !on_ac_power() || a->fd >= 0;
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
	a->active = false;
	DBG("%s freigegeben", type_name[type]);
}

static void release_all(void)
{
	for (int i = 0; i < A_COUNT; i++)
		release_one(i);
	bus_close(user_bus);
	bus_close(sys_bus);
	user_bus = sys_bus = NULL;
}

/* wie createAssertions() bei Apple; false = Fehler, Aufrufer beendet mit 1 */
static bool create_assertions(const bool want[A_COUNT], long timeout)
{
	for (int i = 0; i < A_COUNT; i++) {
		if (!want[i])
			continue;
		if (i == A_USER && timeout == 0)
			timeout = 5;	/* gilt bei Apple auch fuer alle folgenden */
		if (!create_one(i)) {
			/* halb angelegte Sperre (z. B. -s) trotzdem sauber freigeben */
			as[i].active = true;
			fprintf(stderr, "Failed to create %s assertion\n", type_name[i]);
			return false;
		}
		as[i].active = true;
		as[i].expires = expiry_after(timeout);
		DBG("%s angelegt%s", type_name[i], as[i].expires == FOREVER ? "" : " (mit Timeout)");
	}
	return true;
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
			perror("");
			return EXIT_SUCCESS;
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
		if (pfd < 0)	/* Kernel < 5.3: Signal beim Tod des Elternprozesses */
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
			pfd = sys_pidfd_open((pid_t)waitforpid);
			if (pfd < 0) {
				if (errno == ENOSYS)
					pid_poll = (pid_t)waitforpid;
				else	/* gibt es nicht: libdispatch meldet sofort Exit */
					deadline = 0;
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

		long long wait = next == FOREVER ? -1 : next - now;
		struct pollfd p[2] = { { sfd, POLLIN, 0 }, { pfd, POLLIN, 0 } };
		nfds_t n = pfd >= 0 ? 2 : 1;

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
		if (n == 2 && (p[1].revents & (POLLIN | POLLHUP | POLLERR)))
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
