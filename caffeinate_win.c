/*
 * caffeinate_win.c - macOS caffeinate(8) for Windows
 *
 *   caffeinate [-dirsu] [-t timeout] [-w Process ID] [command arguments...]
 *
 * WHAT IT DOES
 *
 * The Windows counterpart of caffeinate.c: same options, same behaviour,
 * built with zig cc (or any mingw-w64 gcc/clang) into one .exe without DLLs
 * of its own. Option parsing and Apple's assertion types come from
 * caffeinate.h. Each assertion becomes a power request, the mechanism behind
 * "powercfg /requests":
 *
 *   -i  PreventUserIdleSystemSleep   -> PowerRequestSystemRequired
 *   -d  PreventUserIdleDisplaySleep  -> PowerRequestDisplayRequired: the
 *                                       screen stays on
 *   -s  PreventSystemSleep (AC only) -> PowerRequestSystemRequired, set only
 *                                       while on mains power
 *   -u  UserIsActive                 -> one injected input event (resets the
 *                                       idle timers, wakes the screen), then
 *                                       the same as -d
 *   -m  PreventDiskIdle              -> no counterpart; accepted, does nothing
 *
 * and one that Apple's tool does not have:
 *
 *   -r  PreventUpdateRestart         -> pauses Windows Update and keeps its
 *                                       active hours around the current time,
 *                                       so that it neither installs updates
 *                                       nor restarts; needs an elevated prompt
 *
 * DESIGN
 *
 * 1. A lock is a lease tied to a handle.
 *    A power request belongs to a kernel handle, and Windows drops it when
 *    the handle closes - at the latest when the process ends, even when it
 *    is killed. Correctness does not depend on cleanup code.
 *
 * 2. -r is turned into a lease although Windows offers none.
 *    Windows Update has no API to hold off a restart, only settings in
 *    HKLM, which outlive the process. Three things give them lease
 *    semantics (details in the section Windows Update):
 *      - the pause expires on its own, PAUSE_HOURS after the last renewal
 *      - the previous values are saved before the first change and put
 *        back when the lease ends, or by the next caffeinate -r if this
 *        one was killed
 *      - of several caffeinate -r, only the owner of a named mutex touches
 *        the registry; Windows hands the mutex to the next one waiting when
 *        the owner ends, even when it is killed
 *
 * 3. One thread, one wait (reactor).
 *    main() waits in MsgWaitForMultipleObjects on an event that the console
 *    handler sets (Ctrl+C, window closed), on the watched process (-w, or
 *    the command), on the lease mutex while another caffeinate owns it, and
 *    on window messages (session end). All timers - -t, per-assertion
 *    expiry, the AC check for -s, the lease renewal - are absolute
 *    GetTickCount64() deadlines from which the wait timeout is derived. The
 *    console handler runs on a thread of its own, but only sets the event.
 *
 * 4. With a command, caffeinate starts it and waits for it.
 *    Windows has no exec(). The command gets the rest of caffeinate's command
 *    line exactly as typed, and caffeinate ends with its exit code. Ctrl+C
 *    and Ctrl+Break reach the command directly; caffeinate ignores them, as
 *    Apple's helper process ignores SIGINT and SIGQUIT.
 *
 * COMPATIBILITY WITH APPLE AND WITH THE LINUX VERSION
 *
 *   - options, messages and exit codes as in caffeinate.c, see caffeinate.h
 *   - ended by Ctrl+C or by closing its window, caffeinate exits with
 *     STATUS_CONTROL_C_EXIT, like any console program that Ctrl+C kills
 *   - a command that cannot be started: exit 127 if it does not exist, 126
 *     otherwise; a name without directory is looked up along PATH only, as
 *     PowerShell does, and without extension with those from PATHEXT, so
 *     "caffeinate npm run build" finds npm.cmd
 *
 * READING GUIDE
 *
 *   Assertions           per-assertion state
 *   Helpers              time, messages, error texts
 *   Power requests       create_one() and friends for -i -d -s -u
 *   Windows Update       the -r lease
 *   Command              command line tail, program lookup, start
 *   main                 options, console events, event loop
 *
 * Messages for the user are in German, except those Apple's tool prints,
 * which are kept verbatim. CAFFEINATE_DEBUG=1 traces every step on stderr.
 */
#ifndef _WIN32_WINNT
# define _WIN32_WINNT 0x0A00	/* Windows 10 */
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sddl.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>

#include "caffeinate.h"

#define AC_POLL_MS      5000	/* -s: interval of the power supply check */
#define PID_POLL_MS     1000	/* -w on a process we may not wait for: interval of the check */
#define LOST_RETRY_MS   60000	/* -r could not be renewed: retry this often */
#define FOREVER         (-1LL)	/* "no deadline", for every timestamp below */
#define MAX_TIMEOUT_S   3155760000ULL	/* 100 years; anything longer counts as infinite */
#define CMDLINE_MAX     32768	/* longest command line Windows accepts, in UTF-16 units */

/* Set by CAFFEINATE_DEBUG: trace every step on stderr */
static bool debug;

/* Debug trace in printf style; the newline is added by say() */
#define DBG(...) do { if (debug) say(__VA_ARGS__); } while (0)

/* ---------------------------------------------------------------- Assertions */

/*
 * The assertion types (A_*) and their names are in caffeinate.h. Each active
 * one owns a power request; -s sets it only while on mains power.
 */
struct assertion {
	bool active;		/* requested and not yet expired */
	long long expires;	/* now_ms() deadline, or FOREVER */
	HANDLE req;		/* power request, or NULL */
	POWER_REQUEST_TYPE type;	/* what req asks for */
	bool set;		/* the request is in force (always, except -s on battery) */
};

static struct assertion as[A_COUNT];	/* indexed by A_* */
static char details[160];		/* the "why" text powercfg /requests shows, UTF-8 */
static char reason[256];		/* why the last step failed, set by fail() */

/* ---------------------------------------------------------------- Helpers */

/* Monotonic clock in milliseconds: the time base of every deadline in this file */
static long long now_ms(void)
{
	return (long long)GetTickCount64();
}

/* Deadline for a timeout given in seconds; 0 (or absurdly long) means none */
static long long expiry_after(long secs)
{
	if (secs <= 0 || (unsigned long long)secs > MAX_TIMEOUT_S)
		return FOREVER;
	return now_ms() + (long long)secs * 1000;
}

/* The earlier of two deadlines */
static long long earlier(long long a, long long b)
{
	return a == FOREVER ? b : b == FOREVER || a < b ? a : b;
}

/*
 * One line on stderr, "caffeinate: " in front. The text is UTF-8. A console
 * gets it as UTF-16, so umlauts come out right whatever its code page; a file
 * or pipe gets the UTF-8 bytes.
 */
__attribute__((format(printf, 1, 2)))
static void say(const char *fmt, ...)
{
	char buf[1024];
	wchar_t wide[1024];
	HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
	DWORD mode, done;
	va_list ap;
	int n = snprintf(buf, sizeof buf, NAME ": ");

	va_start(ap, fmt);
	vsnprintf(buf + n, sizeof buf - (size_t)n - 1, fmt, ap);
	va_end(ap);
	strcat(buf, "\n");	/* the -1 above left room for it */

	fflush(stderr);
	if (h && h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode)) {
		int len = MultiByteToWideChar(CP_UTF8, 0, buf, -1, wide, 1024);
		if (len > 0 && WriteConsoleW(h, wide, (DWORD)len - 1, &done, NULL))
			return;
	}
	fputs(buf, stderr);
}

/*
 * Windows' own text for error code err, in the user's language, as UTF-8
 * without the full stop and line break at the end. The buffer is static: use
 * the result before the next call.
 */
static const char *win_error(DWORD err)
{
	static char text[512];
	wchar_t wide[256];
	DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
	                         NULL, err, 0, wide, 256, NULL);

	while (n > 0 && wcschr(L"\r\n. ", wide[n - 1]))
		n--;
	wide[n] = L'\0';
	if (n == 0 || !WideCharToMultiByte(CP_UTF8, 0, wide, -1, text, sizeof text, NULL, NULL))
		snprintf(text, sizeof text, "Fehler %lu", (unsigned long)err);
	return text;
}

/* Remember why something failed, for the message to the user */
__attribute__((format(printf, 1, 2)))
static void fail(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(reason, sizeof reason, fmt, ap);
	va_end(ap);
	DBG("%s", reason);
}

/* UTF-16 to UTF-8, cut off if out is too small (the cut may split a character) */
static void to_utf8(const wchar_t *in, char *out, size_t max)
{
	int n = WideCharToMultiByte(CP_UTF8, 0, in, -1, out, (int)max, NULL, NULL);

	if (n == 0) {	/* too long: take what fits */
		n = WideCharToMultiByte(CP_UTF8, 0, in, (int)wcsnlen(in, max / 4), out, (int)max - 1, NULL, NULL);
		out[n] = '\0';
	}
}

/* ---------------------------------------------------------------- Power requests */

/* -s: on mains power, or unknown (a desktop without battery reports 1 or 255) */
static bool on_ac_power(void)
{
	SYSTEM_POWER_STATUS s;

	return !GetSystemPowerStatus(&s) || s.ACLineStatus != 0;
}

/*
 * -u: tell Windows that the user is active. A mouse move by 0 pixels counts
 * as input and moves nothing: it resets the idle timers that screen saver
 * and lock follow, and wakes the screen. Where input cannot be injected (lock
 * screen, an elevated window in front), SetThreadExecutionState at least
 * wakes the screen and resets the sleep timers.
 */
static void simulate_user_activity(void)
{
	INPUT in = { .type = INPUT_MOUSE };

	in.mi.dwFlags = MOUSEEVENTF_MOVE;
	if (SendInput(1, &in, sizeof in) != 1)
		DBG("SendInput: %s", win_error(GetLastError()));
	SetThreadExecutionState(ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED);
}

/* Put a's request in force (on = true) or suspend it; false with reason set on failure */
static bool request_set(struct assertion *a, bool on)
{
	if (on ? PowerSetRequest(a->req, a->type) : PowerClearRequest(a->req, a->type)) {
		a->set = on;
		return true;
	}
	fail("%s: %s", on ? "PowerSetRequest" : "PowerClearRequest", win_error(GetLastError()));
	return false;
}

/*
 * -s: follow the power supply. Called every AC_POLL_MS; false if the request
 * could not be switched.
 */
static bool update_system_assertion(void)
{
	struct assertion *a = &as[A_SYSTEM];
	bool ac = on_ac_power();

	if (ac == a->set)
		return true;
	if (!request_set(a, ac))
		return false;
	DBG("%s: %s", type_name[A_SYSTEM], ac ? "am Netz, Sperre aktiv" : "Akkubetrieb, Sperre ausgesetzt");
	return true;
}

/*
 * Acquire what enforces assertion type: a power request with details[] as its
 * reason. Returns false if that failed; "reason" then says why.
 */
static bool create_one(int type)
{
	struct assertion *a = &as[type];
	wchar_t why[sizeof details];
	REASON_CONTEXT rc = {
		.Version = POWER_REQUEST_CONTEXT_VERSION,
		.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING,
	};

	if (type == A_DISK)
		return true;	/* no counterpart on Windows: accepted, nothing to do */
	if (type == A_USER)
		simulate_user_activity();

	if (!MultiByteToWideChar(CP_UTF8, 0, details, -1, why, (int)(sizeof why / sizeof *why)))
		wcscpy(why, L"caffeinate");
	rc.Reason.SimpleReasonString = why;	/* copied by Windows */
	a->req = PowerCreateRequest(&rc);
	if (a->req == INVALID_HANDLE_VALUE) {
		a->req = NULL;
		fail("PowerCreateRequest: %s", win_error(GetLastError()));
		return false;
	}
	a->type = type == A_DISPLAY || type == A_USER ? PowerRequestDisplayRequired : PowerRequestSystemRequired;
	if (type == A_SYSTEM)
		return update_system_assertion();	/* on battery: accepted, takes effect on mains */
	return request_set(a, true);
}

/* Release one assertion: closing the handle ends its request */
static void release_one(int type)
{
	struct assertion *a = &as[type];

	if (!a->active)
		return;
	if (a->req) {
		if (a->set)
			PowerClearRequest(a->req, a->type);
		CloseHandle(a->req);
	}
	a->req = NULL;
	a->active = a->set = false;
	DBG("%s freigegeben", type_name[type]);
}

/* ---------------------------------------------------------------- Windows Update */

/*
 * -r: hold off restarts by Windows Update. There is no lock for that, only
 * settings in HKLM, and it takes two of them to cover both ways a restart
 * comes about:
 *
 *   pause         The pause from Settings > Windows Update, up to
 *                 PAUSE_HOURS ahead and renewed every RENEW_MS. No new
 *                 update is installed, so none asks for a restart. The pause
 *                 runs out by itself: should caffeinate be killed, updates
 *                 resume after PAUSE_HOURS at the latest. A longer pause the
 *                 user set is left alone.
 *   active hours  An update that was installed before caffeinate started
 *                 restarts the machine outside the active hours. The policy
 *                 values keep them from the hour before now to 17 hours
 *                 ahead (Windows allows 18 at most), renewed with the pause,
 *                 so that now is never outside.
 *
 * Every change is recorded. Before caffeinate writes a value for the first
 * time, its previous value goes to ORIG_KEY; each write is announced under
 * PENDING_KEY and confirmed under WRITTEN_KEY, so that a kill at any point
 * leaves the value either untouched or known as caffeinate's. When the lease
 * ends, the values get their previous ones back, a group at a time (active
 * hours, pause), and only if every value of the group still holds what
 * caffeinate wrote; a group that someone else has changed since is left
 * alone. Such a change during the lease is noticed at the next renewal, and
 * caffeinate stops writing that group. The record survives a killed
 * caffeinate, and the next caffeinate -r gives those values back first.
 *
 * Several caffeinate -r share the lease through the named mutex LEASE_MUTEX:
 * its owner manages the registry, the others wait for it in the event loop.
 * When the owner ends - even when it is killed - Windows hands the mutex to
 * one of them, which takes over.
 *
 * At logoff, shutdown or restart, Windows ends a console program that uses
 * user32.dll (this one does, for SendInput) without a console event. A hidden
 * window receives WM_ENDSESSION instead, in time to give the values back. It
 * also learns of a resume from sleep and of a new time or time zone, after
 * which the lease is renewed at once.
 */

#define PAUSE_HOURS     24
#define RENEW_MS        (30 * 60 * 1000)
#define POLICY_KEY      L"SOFTWARE\\Policies\\Microsoft\\Windows\\WindowsUpdate"
#define UX_KEY          L"SOFTWARE\\Microsoft\\WindowsUpdate\\UX\\Settings"
#define OWN_KEY         L"SOFTWARE\\caffeinate"
#define STORE_KEY       OWN_KEY L"\\UpdateLease"
#define ORIG_KEY        STORE_KEY L"\\Original"
#define WRITTEN_KEY     STORE_KEY L"\\Written"
#define PENDING_KEY     STORE_KEY L"\\Pending"
#define LEASE_MUTEX     L"Global\\caffeinate-update-lease"
#define LEASE_MUTEX_SD  L"D:P(A;;GA;;;BA)(A;;GA;;;SY)"	/* only administrators and SYSTEM may open it */
#define LEASE_NAME      "PreventUpdateRestart"

/* The values -r changes; their names are distinct, so the record uses them as they are */
enum { V_SET_ACTIVE, V_ACTIVE_START, V_ACTIVE_END,
       V_PAUSE_START, V_PAUSE_EXPIRY, V_FEATURE_START, V_FEATURE_END,
       V_QUALITY_START, V_QUALITY_END, V_COUNT };

static const struct {
	const wchar_t *key, *name;
} wu_value[V_COUNT] = {
	{ POLICY_KEY, L"SetActiveHours" },
	{ POLICY_KEY, L"ActiveHoursStart" },
	{ POLICY_KEY, L"ActiveHoursEnd" },
	{ UX_KEY, L"PauseUpdatesStartTime" },
	{ UX_KEY, L"PauseUpdatesExpiryTime" },
	{ UX_KEY, L"PauseFeatureUpdatesStartTime" },
	{ UX_KEY, L"PauseFeatureUpdatesEndTime" },
	{ UX_KEY, L"PauseQualityUpdatesStartTime" },
	{ UX_KEY, L"PauseQualityUpdatesEndTime" },
};

/* Values that only work together; each group is given back as a whole */
enum { G_ACTIVE_HOURS, G_PAUSE, G_COUNT };

static const struct {
	int first, last;
	const char *what;	/* for messages */
} wu_group[G_COUNT] = {
	{ V_SET_ACTIVE, V_ACTIVE_END, "Die Nutzungszeit" },
	{ V_PAUSE_START, V_QUALITY_END, "Die Pause" },
};

/* A registry value, or its absence */
struct reg_value {
	bool present;
	DWORD type, size;
	BYTE data[256];
};

static struct {
	bool want;		/* -r was given and has not expired */
	bool owner;		/* this process owns LEASE_MUTEX ... */
	bool taken;		/* ... and has cleared up after its predecessor */
	bool saved[V_COUNT];	/* the original of the value is in ORIG_KEY */
	bool left[G_COUNT];	/* the group was changed from outside: no longer written */
	bool lost;		/* the last renewal failed; reported once */
	long long expires;	/* end of the lease (-t), or FOREVER */
	long long renew_at;	/* next renewal, while owner */
	HANDLE mutex;
} lease;

/* Read HKLM\sub\name; a missing key or value is no error, just absent */
static bool reg_get(const wchar_t *sub, const wchar_t *name, struct reg_value *v)
{
	v->size = sizeof v->data;
	LSTATUS e = RegGetValueW(HKEY_LOCAL_MACHINE, sub, name,
	                         RRF_RT_ANY | RRF_NOEXPAND | RRF_SUBKEY_WOW6464KEY,
	                         &v->type, v->data, &v->size);

	v->present = e == ERROR_SUCCESS;
	if (e == ERROR_SUCCESS || e == ERROR_FILE_NOT_FOUND)
		return true;
	fail("Registry %ls\\%ls: %s", sub, name, win_error((DWORD)e));
	return false;
}

/* Write HKLM\sub\name, or delete it if v is absent */
static bool reg_put(const wchar_t *sub, const wchar_t *name, const struct reg_value *v)
{
	HKEY k;
	LSTATUS e;

	if (v->present) {
		e = RegCreateKeyExW(HKEY_LOCAL_MACHINE, sub, 0, NULL, 0,
		                    KEY_SET_VALUE | KEY_WOW64_64KEY, NULL, &k, NULL);
		if (e == ERROR_SUCCESS) {
			e = RegSetValueExW(k, name, 0, v->type, v->data, v->size);
			RegCloseKey(k);
		}
	} else {
		e = RegOpenKeyExW(HKEY_LOCAL_MACHINE, sub, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, &k);
		if (e == ERROR_SUCCESS) {
			e = RegDeleteValueW(k, name);
			RegCloseKey(k);
		}
		if (e == ERROR_FILE_NOT_FOUND)
			e = ERROR_SUCCESS;	/* already gone */
	}
	if (e == ERROR_SUCCESS)
		return true;
	fail("Registry %ls\\%ls: %s", sub, name, win_error((DWORD)e));
	return false;
}

static bool reg_equal(const struct reg_value *a, const struct reg_value *b)
{
	if (!a->present || !b->present)
		return a->present == b->present;
	return a->type == b->type && a->size == b->size && !memcmp(a->data, b->data, a->size);
}

static void reg_dword(struct reg_value *v, DWORD d)
{
	v->present = true;
	v->type = REG_DWORD;
	v->size = sizeof d;
	memcpy(v->data, &d, sizeof d);
}

/* A REG_SZ from ASCII text */
static void reg_ascii(struct reg_value *v, const char *s)
{
	size_t n = strlen(s);
	wchar_t *w = (wchar_t *)(void *)v->data;

	if (n >= sizeof v->data / sizeof *w)
		n = sizeof v->data / sizeof *w - 1;
	for (size_t i = 0; i < n; i++)
		w[i] = (unsigned char)s[i];
	w[n] = L'\0';
	v->present = true;
	v->type = REG_SZ;
	v->size = (DWORD)((n + 1) * sizeof *w);
}

/* Delete a key of our own with everything below it */
static bool reg_drop(const wchar_t *sub)
{
	HKEY k;
	LSTATUS e = RegOpenKeyExW(HKEY_LOCAL_MACHINE, sub, 0, DELETE | KEY_ALL_ACCESS | KEY_WOW64_64KEY, &k);

	if (e == ERROR_FILE_NOT_FOUND)
		return true;
	if (e == ERROR_SUCCESS) {
		e = RegDeleteTreeW(k, NULL);
		RegCloseKey(k);
	}
	if (e == ERROR_SUCCESS)
		e = RegDeleteKeyExW(HKEY_LOCAL_MACHINE, sub, KEY_WOW64_64KEY, 0);
	if (e == ERROR_SUCCESS || e == ERROR_FILE_NOT_FOUND)
		return true;
	fail("Registry %ls: %s", sub, win_error((DWORD)e));
	return false;
}

/* Delete sub if nothing is left in it; anything else stays as it is */
static void reg_drop_if_empty(const wchar_t *sub)
{
	HKEY k;
	DWORD subkeys, values;

	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, sub, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS)
		return;
	LSTATUS e = RegQueryInfoKeyW(k, NULL, NULL, NULL, &subkeys, NULL, NULL, &values,
	                             NULL, NULL, NULL, NULL);
	RegCloseKey(k);
	if (e == ERROR_SUCCESS && subkeys == 0 && values == 0)
		RegDeleteKeyExW(HKEY_LOCAL_MACHINE, sub, KEY_WOW64_64KEY, 0);
}

/*
 * Set value i and record it. Before the first write, the original goes to
 * ORIG_KEY, a value that did not exist as REG_NONE without data. The write
 * itself is announced under PENDING_KEY and confirmed under WRITTEN_KEY:
 * killed at any point in between, the value holds either what it held
 * before or what the record names.
 */
static bool lease_put(int i, const struct reg_value *v)
{
	const wchar_t *key = wu_value[i].key, *name = wu_value[i].name;
	struct reg_value orig, gone = { .present = false };

	if (!lease.saved[i]) {
		if (!reg_get(key, name, &orig))
			return false;
		if (!orig.present) {
			orig.present = true;
			orig.type = REG_NONE;
			orig.size = 0;
		}
		if (!reg_put(ORIG_KEY, name, &orig))
			return false;
		lease.saved[i] = true;
	}
	return reg_put(PENDING_KEY, name, v) && reg_put(key, name, v) &&
	       reg_put(WRITTEN_KEY, name, v) && reg_put(PENDING_KEY, name, &gone);
}

/* The original of value i from ORIG_KEY, absent if it did not exist; *recorded false if there is none */
static bool orig_get(int i, struct reg_value *v, bool *recorded)
{
	if (!reg_get(ORIG_KEY, wu_value[i].name, v))
		return false;
	*recorded = v->present;
	if (v->present && v->type == REG_NONE && v->size == 0)
		v->present = false;
	return true;
}

/*
 * What the record says about value i: *touched if caffeinate wrote it (or
 * was about to), *ours if it still holds what caffeinate wrote or was about
 * to write. When giving back (restoring), its original counts as well: a
 * give-back that broke off. Without a recorded original it is never ours:
 * there is nothing to give back.
 */
static bool value_state(int i, bool restoring, bool *touched, bool *ours)
{
	struct reg_value written, pending, orig, now;
	const wchar_t *name = wu_value[i].name;
	bool recorded;

	*touched = *ours = false;
	if (!reg_get(WRITTEN_KEY, name, &written) || !reg_get(PENDING_KEY, name, &pending))
		return false;
	*touched = written.present || pending.present;
	if (!*touched)
		return true;
	if (!reg_get(wu_value[i].key, name, &now) || !orig_get(i, &orig, &recorded))
		return false;
	*ours = recorded && ((written.present && reg_equal(&now, &written)) ||
	                     (pending.present && reg_equal(&now, &pending)) ||
	                     (restoring && reg_equal(&now, &orig)));
	return true;
}

/*
 * The state of group g: *any if caffeinate wrote one of its values, *changed
 * if one of those no longer holds what caffeinate wrote - someone else has
 * changed the group since.
 */
static bool group_state(int g, bool restoring, bool touched[V_COUNT], bool ours[V_COUNT], bool *any,
                        bool *changed)
{
	*any = *changed = false;
	for (int i = wu_group[g].first; i <= wu_group[g].last; i++) {
		if (!value_state(i, restoring, &touched[i], &ours[i]))
			return false;
		*any |= touched[i];
		*changed |= touched[i] && !ours[i];
	}
	return true;
}

/*
 * Give group g its originals back if every value of it that caffeinate wrote
 * still holds what it wrote; a group that someone else has changed since
 * stays as it is. Either way the record of the group is cleared. restoring:
 * see value_state(); false while the lease runs.
 */
static bool give_back_group(int g, bool restoring)
{
	int first = wu_group[g].first, last = wu_group[g].last;
	bool touched[V_COUNT], ours[V_COUNT], any, changed, recorded;
	struct reg_value orig, gone = { .present = false };

	if (!group_state(g, restoring, touched, ours, &any, &changed))
		return false;
	if (!any)
		return true;
	if (changed)
		DBG("%s wurde inzwischen geändert und bleibt so", wu_group[g].what);
	for (int i = first; i <= last && !changed; i++) {
		if (!touched[i])
			continue;
		if (!orig_get(i, &orig, &recorded) || !reg_put(wu_value[i].key, wu_value[i].name, &orig))
			return false;
	}
	/* Only now, and the changed values last: until then a kill leaves a
	 * record that still shows the group as changed */
	for (int pass = 0; pass < 2; pass++) {
		for (int i = first; i <= last; i++) {
			if ((!touched[i] || ours[i]) != (pass == 0))
				continue;
			if (!reg_put(WRITTEN_KEY, wu_value[i].name, &gone) ||
			    !reg_put(PENDING_KEY, wu_value[i].name, &gone))
				return false;
		}
	}
	return true;
}

/*
 * Put back everything the record says caffeinate changed, then delete the
 * record. If a group fails, the record stays for the next caffeinate -r.
 */
static bool give_back_all(void)
{
	bool ok = true;

	for (int g = 0; g < G_COUNT; g++)
		ok &= give_back_group(g, true);
	if (!ok)
		return false;
	reg_drop_if_empty(POLICY_KEY);	/* caffeinate may have created it */
	if (!reg_drop(STORE_KEY))
		return false;
	reg_drop_if_empty(OWN_KEY);
	memset(lease.saved, 0, sizeof lease.saved);
	return true;
}

/*
 * A group changed from outside while the lease runs - an administrator set
 * the active hours, the user resumed updates - is left to whoever changed
 * it: caffeinate clears its record of the group and writes it no more.
 */
static bool leave_changed_groups(void)
{
	bool touched[V_COUNT], ours[V_COUNT], any, changed;

	for (int g = 0; g < G_COUNT; g++) {
		if (lease.left[g])
			continue;
		if (!group_state(g, false, touched, ours, &any, &changed))
			return false;
		if (!changed)
			continue;
		if (!give_back_group(g, false))
			return false;
		lease.left[g] = true;
		say("%s: %s wurde von außen geändert, caffeinate lässt sie ab jetzt so", LEASE_NAME,
		    wu_group[g].what);
	}
	return true;
}

/* Comparable number from a timestamp as Settings writes it (2026-02-18T18:04:54Z); 0 if unreadable */
static unsigned long long stamp_value(const struct reg_value *v)
{
	char text[32];
	const wchar_t *w = (const wchar_t *)(const void *)v->data;
	unsigned y, mo, d, h, mi, s;
	size_t n = 0;

	if (!v->present || v->type != REG_SZ)
		return 0;
	for (; n < sizeof text - 1 && n < v->size / sizeof *w && w[n] && w[n] < 0x80; n++)
		text[n] = (char)w[n];
	text[n] = '\0';
	if (sscanf(text, "%4u-%2u-%2uT%2u:%2u:%2u", &y, &mo, &d, &h, &mi, &s) != 6)
		return 0;
	return ((((y * 100ULL + mo) * 100 + d) * 100 + h) * 100 + mi) * 100 + s;
}

/* UTC now plus hours, in the form Settings writes */
static void utc_stamp(unsigned hours, char *out, size_t max)
{
	FILETIME ft;
	ULARGE_INTEGER t;
	SYSTEMTIME st;

	GetSystemTimeAsFileTime(&ft);
	t.LowPart = ft.dwLowDateTime;
	t.HighPart = ft.dwHighDateTime;
	t.QuadPart += hours * 36000000000ULL;	/* in 100 ns */
	ft.dwLowDateTime = t.LowPart;
	ft.dwHighDateTime = t.HighPart;
	FileTimeToSystemTime(&ft, &st);
	snprintf(out, max, "%04u-%02u-%02uT%02u:%02u:%02uZ", (unsigned)st.wYear, (unsigned)st.wMonth,
	         (unsigned)st.wDay, (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond);
}

/* Active hours from the hour before now to 17 hours ahead */
static bool renew_active_hours(void)
{
	struct reg_value v;
	SYSTEMTIME local;

	GetLocalTime(&local);
	reg_dword(&v, 1);
	if (!lease_put(V_SET_ACTIVE, &v))
		return false;
	reg_dword(&v, (local.wHour + 23u) % 24);
	if (!lease_put(V_ACTIVE_START, &v))
		return false;
	reg_dword(&v, (local.wHour + 17u) % 24);
	if (!lease_put(V_ACTIVE_END, &v))
		return false;
	DBG("%s: Nutzungszeit %u-%u Uhr", LEASE_NAME, (local.wHour + 23u) % 24, (local.wHour + 17u) % 24);
	return true;
}

/* The pause until PAUSE_HOURS from now, unless the user's own lasts longer */
static bool renew_pause(void)
{
	struct reg_value start, end, current;
	char text[32];

	utc_stamp(0, text, sizeof text);
	reg_ascii(&start, text);
	utc_stamp(PAUSE_HOURS, text, sizeof text);
	reg_ascii(&end, text);
	if (!reg_get(UX_KEY, wu_value[V_PAUSE_EXPIRY].name, &current))
		return false;
	if (stamp_value(&current) > stamp_value(&end)) {
		DBG("Windows Update ist schon länger pausiert, die Pause bleibt so");
		return true;
	}
	for (int i = V_PAUSE_START; i <= V_QUALITY_END; i += 2)
		if (!lease_put(i, &start) || !lease_put(i + 1, &end))
			return false;
	DBG("%s: Pause bis %s", LEASE_NAME, text);
	return true;
}

/* Apply the lease once more, to the groups nobody else has taken over */
static bool lease_renew(void)
{
	if (!leave_changed_groups())
		return false;
	if (!lease.left[G_ACTIVE_HOURS] && !renew_active_hours())
		return false;
	return lease.left[G_PAUSE] || renew_pause();
}

/*
 * Become the one that manages the registry: first give back what a killed
 * caffeinate left behind, then apply the lease.
 */
static bool lease_take(void)
{
	if (!give_back_all())
		return false;
	lease.taken = true;
	return lease_renew();
}

/* Renew the lease when due; a failure is reported once and retried every LOST_RETRY_MS */
static void lease_tick(void)
{
	bool ok = lease.taken ? lease_renew() : lease_take();

	if (ok) {
		if (lease.lost)
			say("%s: Sperre wieder angelegt", LEASE_NAME);
		lease.lost = false;
		lease.renew_at = now_ms() + RENEW_MS;
		return;
	}
	if (!lease.lost)
		say("%s: Sperre verloren: %s; neuer Versuch jede Minute", LEASE_NAME, reason);
	lease.lost = true;
	lease.renew_at = now_ms() + LOST_RETRY_MS;
}

/* The mutex became ours: another caffeinate -r ended (or there was none) */
static void lease_owned(void)
{
	lease.owner = true;
	lease.renew_at = now_ms();
	DBG("%s: Windows Update wird von diesem caffeinate aufgehalten", LEASE_NAME);
}

/*
 * Start -r. Fails without administrator rights. If another caffeinate -r
 * holds the lease already, this one waits in the event loop to take over.
 */
static bool lease_create(long long expires)
{
	HKEY k;
	LSTATUS e = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE", 0, KEY_CREATE_SUB_KEY | KEY_WOW64_64KEY, &k);

	if (e == ERROR_ACCESS_DENIED) {
		fail("Windows Update lässt sich nur mit Administratorrechten aufhalten (Terminal als Administrator starten)");
		return false;
	}
	if (e != ERROR_SUCCESS) {
		fail("Registry HKLM\\SOFTWARE: %s", win_error((DWORD)e));
		return false;
	}
	RegCloseKey(k);

	/* not open to everyone: a user without rights could hold it otherwise */
	SECURITY_ATTRIBUTES sa = { .nLength = sizeof sa };
	if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(LEASE_MUTEX_SD, SDDL_REVISION_1,
	                                                          &sa.lpSecurityDescriptor, NULL)) {
		fail("Sicherheitsbeschreibung: %s", win_error(GetLastError()));
		return false;
	}
	lease.mutex = CreateMutexW(&sa, FALSE, LEASE_MUTEX);
	DWORD err = GetLastError();
	LocalFree(sa.lpSecurityDescriptor);
	if (!lease.mutex) {
		fail("CreateMutex: %s", win_error(err));
		return false;
	}
	lease.want = true;
	lease.expires = expires;
	switch (WaitForSingleObject(lease.mutex, 0)) {
	case WAIT_OBJECT_0:
	case WAIT_ABANDONED:	/* its last owner was killed */
		lease_owned();
		if (!lease_take())
			return false;
		lease.renew_at = now_ms() + RENEW_MS;
		return true;
	case WAIT_TIMEOUT:
		say("%s: ein anderes caffeinate -r hält Windows Update schon auf; übernehme, wenn es endet",
		    LEASE_NAME);
		return true;
	default:
		fail("WaitForSingleObject: %s", win_error(GetLastError()));
		return false;
	}
}

/* End -r: give the values back and hand the mutex on. Safe to call twice. */
static void lease_end(void)
{
	if (lease.owner) {
		/* even if not yet taken: what a killed predecessor left behind */
		if (!give_back_all())
			say("%s: alte Einstellungen nicht vollständig zurückgesetzt: %s; "
			    "das nächste caffeinate -r holt es nach", LEASE_NAME, reason);
		ReleaseMutex(lease.mutex);
	}
	if (lease.mutex)
		CloseHandle(lease.mutex);
	if (lease.want)
		DBG("%s freigegeben", LEASE_NAME);
	lease.mutex = NULL;
	lease.want = lease.owner = lease.taken = lease.lost = false;
	memset(lease.left, 0, sizeof lease.left);
}

static bool session_over;	/* WM_ENDSESSION: the session ends, so does caffeinate */

static LRESULT CALLBACK session_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
	case WM_ENDSESSION:
		if (!wp)
			break;
		/* Windows may end the process as soon as this returns */
		lease_end();
		session_over = true;
		return 0;
	case WM_POWERBROADCAST:
		/* back from sleep: the waits did not count the time asleep, the
		 * renewal may be overdue */
		if (wp == PBT_APMRESUMEAUTOMATIC && lease.owner)
			lease.renew_at = now_ms();
		return TRUE;
	case WM_TIMECHANGE:
		/* another time zone moves the active hours */
		if (lease.owner)
			lease.renew_at = now_ms();
		return 0;
	}
	return DefWindowProcW(w, msg, wp, lp);
}

/* The hidden window that learns about logoff, shutdown, resume and clock changes (see above) */
static void watch_session_end(void)
{
	WNDCLASSW wc = {
		.lpfnWndProc = session_proc,
		.hInstance = GetModuleHandleW(NULL),
		.lpszClassName = L"caffeinate",
	};

	if (!RegisterClassW(&wc) ||
	    !CreateWindowExW(0, wc.lpszClassName, L"caffeinate", WS_OVERLAPPED, 0, 0, 0, 0,
	                     NULL, NULL, wc.hInstance, NULL))
		DBG("Fenster für das Sitzungsende: %s", win_error(GetLastError()));
}

/* ---------------------------------------------------------------- Acquire and release */

/* Release everything */
static void release_all(void)
{
	for (int i = 0; i < A_COUNT; i++)
		release_one(i);
	lease_end();
}

/*
 * Like createAssertions() in Apple's source: create the requested assertions in
 * type order, then -r. false = failure, with the message printed.
 */
static bool create_assertions(const struct options *o)
{
	long timeout = o->timeout;

	for (int i = 0; i < A_COUNT; i++) {
		if (!o->want[i])
			continue;
		if (i == A_USER && timeout == 0)
			timeout = 5;	/* as with Apple, this then applies to all later ones too */
		/* mark it active first so a half-created one is released cleanly */
		as[i].active = true;
		if (!create_one(i)) {
			if (*reason && !debug)
				say("%s", reason);
			fprintf(stderr, "Failed to create %s assertion\n", type_name[i]);
			return false;
		}
		as[i].expires = expiry_after(timeout);
		DBG("%s angelegt%s", type_name[i], as[i].expires == FOREVER ? "" : " (mit Timeout)");
	}
	/* -r follows -t only, not the implicit 5 s of -u */
	if (o->update && !lease_create(expiry_after(o->timeout))) {
		if (*reason && !debug)
			say("%s", reason);
		fprintf(stderr, "Failed to create %s assertion\n", LEASE_NAME);
		return false;
	}
	return true;
}

/* ---------------------------------------------------------------- Command */

/*
 * Take one argument off the command line at *p, with the rules of the
 * Windows C runtime: backslashes are literal unless they precede a quote,
 * 2n backslashes and a quote give n backslashes and toggle quoting, 2n+1 give
 * n backslashes and a literal quote, and "" inside quotes is a literal quote.
 * The unquoted value goes to out (if not NULL, cut off at max); *p moves past
 * the argument and the blanks after it.
 */
static void next_arg(const wchar_t **p, wchar_t *out, size_t max)
{
	const wchar_t *s = *p;
	size_t n = 0;
	bool quoted = false;

#define EMIT(c) do { if (out && n + 1 < max) out[n++] = (c); } while (0)
	while (*s && (quoted || (*s != L' ' && *s != L'\t'))) {
		if (*s == L'\\') {
			size_t slashes = 0;
			while (*s == L'\\') {
				slashes++;
				s++;
			}
			for (size_t i = 0; i < (*s == L'"' ? slashes / 2 : slashes); i++)
				EMIT(L'\\');
			if (*s == L'"' && slashes % 2) {
				EMIT(L'"');
				s++;
			}
			continue;
		}
		if (*s == L'"') {
			if (quoted && s[1] == L'"') {
				EMIT(L'"');
				s++;
			} else {
				quoted = !quoted;
			}
			s++;
			continue;
		}
		EMIT(*s);
		s++;
	}
#undef EMIT
	if (out && max)
		out[n] = L'\0';
	while (*s == L' ' || *s == L'\t')
		s++;
	*p = s;
}

/* GetCommandLineW() past the program name, whose quotes group but do not escape */
static const wchar_t *after_program(void)
{
	const wchar_t *p = GetCommandLineW();
	bool quoted = false;

	for (; *p && (quoted || (*p != L' ' && *p != L'\t')); p++)
		if (*p == L'"')
			quoted = !quoted;
	while (*p == L' ' || *p == L'\t')
		p++;
	return p;
}

/*
 * argv for the option parser, split from GetCommandLineW() with next_arg()
 * and in UTF-8. The C runtime's argv comes from the ANSI command line, where
 * best-fit mapping can turn a character into a blank or a quote (U+3000 into
 * ' ' with code page 1252); its argument count could then differ from what
 * command_tail() counts, and the wrong word would start the command.
 */
static char **utf8_argv(int *argc)
{
	static char store[CMDLINE_MAX * 4];	/* at most 3 bytes per UTF-16 unit, plus the NULs */
	static char *args[CMDLINE_MAX / 2 + 2];	/* an argument takes 2 units at least, with its blank */
	static wchar_t one[CMDLINE_MAX];
	static char program[] = NAME;
	const wchar_t *p = after_program();
	size_t used = 0;
	int n = 0;

	args[n++] = program;
	while (*p && n < (int)(sizeof args / sizeof *args) - 1) {
		next_arg(&p, one, CMDLINE_MAX);
		int len = WideCharToMultiByte(CP_UTF8, 0, one, -1, store + used, (int)(sizeof store - used), NULL, NULL);
		if (len <= 0)
			break;	/* cannot happen with these sizes */
		args[n++] = store + used;
		used += (size_t)len;
	}
	args[n] = NULL;
	*argc = n;
	return args;
}

/*
 * The command line from argument number first on, exactly as it was typed:
 * GetCommandLineW() past the program name and the options. Passed on
 * unchanged, it keeps its quoting and characters outside the ANSI code page.
 */
static const wchar_t *command_tail(int first)
{
	const wchar_t *p = after_program();

	for (int i = 1; i < first && *p; i++)
		next_arg(&p, NULL, 0);
	return p;
}

/* Does file exist and is not a directory? */
static bool is_file(const wchar_t *path)
{
	DWORD a = GetFileAttributesW(path);

	return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/* Is ext (len units, with the dot) one of exts, a list like PATHEXT? */
static bool ext_in(const wchar_t *ext, size_t len, const wchar_t *exts)
{
	for (const wchar_t *e = exts; *e; ) {
		size_t n = wcscspn(e, L";");
		if (n && n == len && !_wcsnicmp(ext, e, len))
			return true;
		e += n + (e[n] == L';');
	}
	return false;
}

/* The file name in path, after the last directory */
static const wchar_t *base_name(const wchar_t *path)
{
	const wchar_t *base = path + wcslen(path);

	while (base > path && !wcschr(L"\\/:", base[-1]))
		base--;
	return base;
}

/* The extension of file name base, with the dot; "" if it has none */
static const wchar_t *extension(const wchar_t *base)
{
	const wchar_t *dot = wcsrchr(base, L'.');

	return dot ? dot : base + wcslen(base);
}

/*
 * Try dir\name (dir may be empty): as it is if complete, otherwise with each
 * extension from exts that CreateProcessW can start. .vbs, .js and the like
 * from PATHEXT need a file association, which it does not follow. Returns
 * true with the hit in path.
 */
static bool try_dir(const wchar_t *dir, size_t dir_len, const wchar_t *name, bool complete,
                    const wchar_t *exts, wchar_t *path, size_t max)
{
	const wchar_t *sep = dir_len && !wcschr(L"\\/", dir[dir_len - 1]) ? L"\\" : L"";

	if (complete) {
		int n = _snwprintf(path, max, L"%.*ls%ls%ls", (int)dir_len, dir, sep, name);
		return n > 0 && (size_t)n < max && is_file(path);
	}
	for (const wchar_t *e = exts; *e; ) {
		size_t len = wcscspn(e, L";");
		if (ext_in(e, len, L".COM;.EXE;.BAT;.CMD")) {
			int n = _snwprintf(path, max, L"%.*ls%ls%ls%.*ls", (int)dir_len, dir, sep, name, (int)len, e);
			if (n > 0 && (size_t)n < max && is_file(path))
				return true;
		}
		e += len + (e[len] == L';');
	}
	return false;
}

/*
 * Find the program the way PowerShell does: a name with a directory only
 * there, any other along PATH alone - not first in the current directory, as
 * cmd.exe and CreateProcessW would, so that an npm.bat planted in a checkout
 * does not run instead of npm.cmd. A name that does not end in one of the
 * extensions from PATHEXT is tried with each of them: "npm" finds npm.cmd,
 * "python3.12" finds python3.12.exe. Returns true with the file in path;
 * false means not found, and nothing else is searched.
 */
static bool find_program(const wchar_t *name, wchar_t *path, size_t max)
{
	static wchar_t dirs[CMDLINE_MAX];
	wchar_t exts[1024];
	const wchar_t *base = base_name(name);

	if (!*base)
		return false;
	DWORD n = GetEnvironmentVariableW(L"PATHEXT", exts, (DWORD)(sizeof exts / sizeof *exts));
	if (n == 0 || n >= sizeof exts / sizeof *exts)
		wcscpy(exts, L".COM;.EXE;.BAT;.CMD");
	const wchar_t *ext = extension(base);
	bool complete = *ext && ext_in(ext, wcslen(ext), exts);
	if (base != name)	/* also as it is: CreateProcessW starts a program whatever its extension */
		return try_dir(L"", 0, name, complete, exts, path, max) ||
		       try_dir(L"", 0, name, true, exts, path, max);

	n = GetEnvironmentVariableW(L"PATH", dirs, CMDLINE_MAX);
	if (n == 0 || n >= CMDLINE_MAX)
		return false;
	for (const wchar_t *d = dirs; *d; ) {
		size_t len = wcscspn(d, L";");
		const wchar_t *dir = d;
		size_t dir_len = len;
		if (dir_len >= 2 && dir[0] == L'"' && dir[dir_len - 1] == L'"') {
			dir++;
			dir_len -= 2;
		}
		if (dir_len && try_dir(dir, dir_len, name, complete, exts, path, max))
			return true;
		d += len + (d[len] == L';');
	}
	return false;
}

/*
 * A batch file runs in cmd.exe. CreateProcessW would start it as
 * "cmd /c <line>", and cmd would look up the first word once more - in the
 * current directory first. So cmd.exe is started here directly, with exactly
 * the file that was found: cmd /d /s /c ""path" rest". /s takes the outer
 * quotes off and leaves the rest alone, /d skips AutoRun commands.
 */
static bool batch_line(wchar_t *path, const wchar_t *rest, wchar_t *app, size_t app_max,
                       wchar_t *line, size_t max)
{
	UINT n = GetSystemDirectoryW(app, (UINT)app_max);

	if (n == 0 || n >= app_max)
		return false;
	int len = _snwprintf(app + n, app_max - n, L"\\cmd.exe");
	if (len <= 0 || (size_t)len >= app_max - n)
		return false;
	for (wchar_t *s = path; *s; s++)	/* ./tool.cmd: cmd takes / for a switch */
		if (*s == L'/')
			*s = L'\\';
	len = _snwprintf(line, max, L"\"%ls\" /d /s /c \"\"%ls\"%ls%ls\"", app, path, *rest ? L" " : L"", rest);
	return len > 0 && (size_t)len < max;
}

static DWORD child_pid;	/* the command's process ID, for ctrl_handler() */

/*
 * Start the command, the rest of the command line from argument first on.
 * Returns its process handle; NULL if it could not be started, with
 * *status set to 127 (not found) or 126, as a shell would.
 */
static HANDLE start_command(int first, int *status)
{
	static wchar_t line[CMDLINE_MAX], program[MAX_PATH * 4], app[MAX_PATH * 4];
	wchar_t name[MAX_PATH * 4];
	const wchar_t *tail = command_tail(first), *rest = tail;
	STARTUPINFOW si = { .cb = sizeof si };
	PROCESS_INFORMATION pi;
	char shown[sizeof details];
	const wchar_t *start = NULL;	/* lpApplicationName */

	next_arg(&rest, name, sizeof name / sizeof *name);
	to_utf8(name, shown, sizeof shown);
	*status = 126;
	if (wcslen(name) >= sizeof name / sizeof *name - 1) {	/* may have been cut off */
		say("Befehlsname zu lang");
		return NULL;
	}
	if (!find_program(name, program, sizeof program / sizeof *program)) {
		say("%s: %s", shown, win_error(ERROR_FILE_NOT_FOUND));
		*status = 127;
		return NULL;
	}

	const wchar_t *ext = extension(base_name(program));
	bool ok;
	if (ext_in(ext, wcslen(ext), L".BAT;.CMD")) {
		if (wcschr(program, L'%')) {	/* cmd would expand %...% in it */
			say("%s: eine Batchdatei mit %% im Pfad lässt sich nicht sicher starten", shown);
			return NULL;
		}
		ok = batch_line(program, rest, app, sizeof app / sizeof *app, line, CMDLINE_MAX);
		start = app;
	} else {
		ok = wcslen(tail) < CMDLINE_MAX;
		if (ok)
			wcscpy(line, tail);	/* CreateProcessW may write to it */
		start = program;
	}
	if (!ok) {
		say("Befehlszeile zu lang");
		return NULL;
	}
	DBG("starte %s%s", shown, start == app ? " mit cmd.exe" : "");
	/* inherit handles: a redirected stdout of caffeinate is the command's
	 * as well. caffeinate's own handles are not inheritable. */
	if (!CreateProcessW(start, line, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
		DWORD e = GetLastError();
		say("%s: %s", shown, win_error(e));
		*status = e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ||
		          e == ERROR_INVALID_NAME ? 127 : 126;
		return NULL;
	}
	CloseHandle(pi.hThread);
	child_pid = pi.dwProcessId;
	snprintf(details, sizeof details, "caffeinate asserting on behalf of '%s' (pid %lu)",
	         shown, (unsigned long)pi.dwProcessId);
	return pi.hProcess;
}

/* Has the process of h ended? Needs only the right to query it. */
static bool has_exited(HANDLE h)
{
	FILETIME created, exited, kernel, user;

	return GetProcessTimes(h, &created, &exited, &kernel, &user) &&
	       (exited.dwLowDateTime | exited.dwHighDateTime) != 0;
}

/*
 * -w on a process we may not even query: does pid still run? Opened anew
 * each time, so a reused pid counts as the same process; nothing better is
 * possible without a handle.
 */
static bool pid_alive(DWORD pid)
{
	HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);

	if (!h)		/* access denied means it exists */
		return GetLastError() != ERROR_INVALID_PARAMETER;
	bool alive = !has_exited(h);
	CloseHandle(h);
	return alive;
}

/* ---------------------------------------------------------------- main */

static HANDLE stop_event;	/* set by ctrl_handler(): end caffeinate */
static bool ignore_ctrl_c;	/* with a command: Ctrl+C and Ctrl+Break are meant for it */

/*
 * Does the command share our console, so that Ctrl+C reaches it? Not if it is
 * a program with windows of its own, notepad for example.
 */
static bool child_on_console(void)
{
	DWORD list[64];
	DWORD n = GetConsoleProcessList(list, 64);

	if (n > 64)
		return true;	/* more than fit: assume it is among them */
	for (DWORD i = 0; i < n; i++)
		if (list[i] == child_pid)
			return true;
	return false;
}

/*
 * Runs on a thread of its own. Ctrl+C, Ctrl+Break and closing the window end
 * caffeinate; with a command, Ctrl+C and Ctrl+Break only if the command does
 * not share the console and would never see them. For a closing window
 * Windows ends the process as soon as this returns, so it waits instead:
 * main() releases everything and exits within the 5 s Windows grants.
 */
static BOOL WINAPI ctrl_handler(DWORD type)
{
	if ((type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) && ignore_ctrl_c && child_on_console())
		return TRUE;
	SetEvent(stop_event);
	if (type == CTRL_CLOSE_EVENT || type == CTRL_LOGOFF_EVENT || type == CTRL_SHUTDOWN_EVENT)
		Sleep(INFINITE);
	return TRUE;
}

/*
 * The wait of one loop round: until the earliest deadline, or until the stop
 * event, the watched process, the lease mutex or a window message. Returns
 * the index of the handle that fired, n for a message, -1 for the deadline,
 * -2 on error.
 */
static int wait_any(HANDLE *h, DWORD n, long long next)
{
	long long now = now_ms();
	long long wait = next == FOREVER ? INFINITE : next < now ? 0 : next - now;
	DWORD r = MsgWaitForMultipleObjects(n, h, FALSE, wait >= INFINITE ? INFINITE - 1 : (DWORD)wait,
	                                    QS_ALLINPUT);

	if (r == WAIT_TIMEOUT)
		return -1;
	if (r < WAIT_OBJECT_0 + n + 1)
		return (int)(r - WAIT_OBJECT_0);
	if (r >= WAIT_ABANDONED_0 && r < WAIT_ABANDONED_0 + n)
		return (int)(r - WAIT_ABANDONED_0);	/* the lease mutex, from a killed owner */
	say("MsgWaitForMultipleObjects: %s", win_error(GetLastError()));
	return -2;
}

/* Expire assertions and the lease, run the periodic checks; returns the next deadline */
static long long run_timers(long long next)
{
	long long now = now_ms();

	for (int i = 0; i < A_COUNT; i++) {
		if (!as[i].active || as[i].expires == FOREVER)
			continue;
		if (as[i].expires <= now) {
			release_one(i);
			continue;
		}
		next = earlier(next, as[i].expires);
	}
	if (as[A_SYSTEM].active) {
		static bool reported;	/* a failure is reported once, retried every AC_POLL_MS */
		bool ok = update_system_assertion();
		if (!ok && !reported)
			say("%s: %s", type_name[A_SYSTEM], reason);
		reported = !ok;
		next = earlier(next, now + AC_POLL_MS);
	}
	if (lease.want && lease.expires != FOREVER && lease.expires <= now)
		lease_end();
	if (lease.want) {
		next = earlier(next, lease.expires);
		if (lease.owner && lease.renew_at <= now)
			lease_tick();
		if (lease.owner)
			next = earlier(next, lease.renew_at);
	}
	return next;
}

/*
 * -w: how to learn of the end of process pid. Usually by waiting on its
 * handle (*proc); if we may not, by querying a handle (*query), which also
 * keeps the pid from being reused; if not even that, by opening it anew each
 * time (*pid_poll). No such process: *deadline = 0, end at once, like Apple.
 * Windows ignores the two low bits of a pid (5 opens process 4), so a pid
 * that is no multiple of 4 is none.
 */
static void watch_pid(DWORD pid, bool invalid, HANDLE *proc, HANDLE *query, DWORD *pid_poll,
                      long long *deadline)
{
	if (invalid) {
		*deadline = 0;
		return;
	}
	*proc = OpenProcess(SYNCHRONIZE, FALSE, pid);
	if (*proc) {
		DBG("warte auf Prozess %lu", (unsigned long)pid);
		return;
	}
	if (GetLastError() == ERROR_INVALID_PARAMETER) {
		*deadline = 0;
		return;
	}
	*query = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!*query)
		*pid_poll = pid;
	DBG("prüfe jede Sekunde, ob Prozess %lu noch läuft", (unsigned long)pid);
}

/*
 * 1. parse the options the way Apple's getopt loop does
 * 2. with a command: start it; caffeinate waits for it
 * 3. create the assertions
 * 4. event loop, until a timeout, Ctrl+C, a closed window or the end of the
 *    watched process
 * 5. release everything
 */
int main(void)
{
	struct options opt = { .want = { false } };
	const char *env = getenv("CAFFEINATE_DEBUG");
	int argc;
	char **argv = utf8_argv(&argc);

	debug = env && *env && strcmp(env, "0") != 0;
	int status = parse_options(argc, argv, &opt);
	if (status >= 0)
		return status;

	bool have_cmd = argc - opt.cmd > 0;
	HANDLE proc = NULL;		/* the process whose end is awaited */
	HANDLE query = NULL;		/* -w on a process we may only query: polled */
	DWORD pid_poll = 0;		/* -w on a process we may not even query: polled by pid */
	long long deadline = FOREVER;	/* when caffeinate itself ends (-t without command) */

	stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!stop_event) {
		say("CreateEvent: %s", win_error(GetLastError()));
		return EXIT_FAILURE;
	}
	ignore_ctrl_c = have_cmd;
	SetConsoleCtrlHandler(ctrl_handler, TRUE);

	if (have_cmd) {
		proc = start_command(opt.cmd, &status);
		if (!proc)
			return status;
	} else {
		deadline = expiry_after(opt.timeout);
		if (opt.waitforpid) {
			if (opt.timeout)
				snprintf(details, sizeof details,
				         "caffeinate asserting on behalf of Process ID %ld for %ld secs",
				         opt.waitforpid, opt.timeout);
			else
				snprintf(details, sizeof details,
				         "caffeinate asserting on behalf of Process ID %ld", opt.waitforpid);
			watch_pid((DWORD)opt.waitforpid, opt.waitforpid < 0 || opt.waitforpid % 4,
			          &proc, &query, &pid_poll, &deadline);
		} else if (opt.timeout) {
			snprintf(details, sizeof details, "caffeinate asserting for %ld secs", opt.timeout);
		} else {
			snprintf(details, sizeof details, "caffeinate asserting forever");
		}
	}

	if (opt.update)
		watch_session_end();
	if (!create_assertions(&opt)) {
		release_all();
		/* with a command, caffeinate still waits for it, only without locks,
		 * as on Linux, where the command does not notice the failure either */
		if (!have_cmd)
			return EXIT_FAILURE;
	}

	int exit_code = EXIT_SUCCESS;
	bool stopped = false;

	for (;;) {
		long long next = deadline;

		if (deadline != FOREVER && deadline <= now_ms())
			break;
		next = run_timers(next);
		if (query || pid_poll) {
			if (query ? has_exited(query) : !pid_alive(pid_poll))
				break;
			next = earlier(next, now_ms() + PID_POLL_MS);
		}

		/* wait set: stop event, the process if there is one, the lease mutex
		 * while another caffeinate owns it */
		HANDLE h[3] = { stop_event };
		DWORD n = 1, i_proc = 0, i_mutex = 0;
		if (proc)
			h[i_proc = n++] = proc;
		if (lease.want && !lease.owner)
			h[i_mutex = n++] = lease.mutex;

		int r = wait_any(h, n, next);
		if (r == -2) {
			exit_code = EXIT_FAILURE;
			break;
		}
		if (r == 0) {
			stopped = true;
			break;
		}
		if (r > 0 && (DWORD)r == i_proc)
			break;
		if (r > 0 && (DWORD)r == i_mutex)
			lease_owned();
		if ((DWORD)r == n) {	/* window messages: only the session end matters */
			MSG m;
			while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE))
				DispatchMessageW(&m);
			if (session_over) {
				stopped = true;	/* the command is still running: no exit code of its own */
				break;
			}
		}
	}

	release_all();
	if (have_cmd && !stopped && exit_code == EXIT_SUCCESS) {
		DWORD code;
		exit_code = GetExitCodeProcess(proc, &code) ? (int)code : EXIT_FAILURE;
	}
	if (proc)
		CloseHandle(proc);
	if (query)
		CloseHandle(query);
	/* everything is released: now end the way Ctrl+C would have ended us */
	if (stopped)
		exit_code = (int)STATUS_CONTROL_C_EXIT;
	return exit_code;
}
