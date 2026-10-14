/*
 * caffeinate.h - what the Linux and the Windows version share
 *
 * Apple's assertion types, the usage line and the command line parsing.
 * caffeinate.c (Linux) and caffeinate_win.c (Windows) each include it once,
 * after their own system headers; everything in here is static.
 */
#ifndef CAFFEINATE_H
#define CAFFEINATE_H

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>	/* getopt(); mingw-w64 has it as well */

#define NAME "caffeinate"

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

/* What the command line asks for */
struct options {
	bool want[A_COUNT];	/* assertion types, indexed by A_* */
	bool update;		/* -r, Windows only: hold off Windows Update restarts */
	long timeout;		/* -t in seconds; 0 = none */
	long waitforpid;	/* -w; 0 = none */
	int cmd;		/* argv index of the command; argc if there is none */
};

/*
 * Apple's usage line, verbatim. Windows adds -r, which Apple does not have.
 */
static void usage(void)
{
#ifdef _WIN32
	fprintf(stderr, "usage: caffeinate [-dirsu] [-t timeout] [-w Process ID] [command arguments...]\n");
#else
	fprintf(stderr, "usage: caffeinate [-disu] [-t timeout] [-w Process ID] [command arguments...]\n");
#endif
}

/*
 * strtol(s, NULL, 0) with macOS semantics: there strtol sets errno = EINVAL
 * when it finds no digit; glibc, musl and the Windows CRT do not. Returns
 * false in exactly the cases where Apple's caffeinate calls usage().
 */
static bool mac_strtol(const char *s, long *out)
{
	char *end;

	errno = 0;
	*out = strtol(s, &end, 0);
	if (end == s) {
		/* "0x" without a hex digit: macOS, glibc and musl read the 0 and
		 * stop at the x, the Windows CRT reads nothing */
		const char *p = s;
		while (isspace((unsigned char)*p))
			p++;
		if (*p == '+' || *p == '-')
			p++;
		if (*p == '0')
			return true;
		errno = EINVAL;
	}
	return !(*out == 0 && errno != 0);
}

/*
 * Parse the command line the way Apple's getopt loop does. Returns -1 when
 * caffeinate goes on, otherwise the exit status to end with right away.
 */
static int parse_options(int argc, char **argv, struct options *o)
{
#ifdef _WIN32
	static const char optstring[] = "+:mdhirsut:w:";
#else
	static const char optstring[] = "+:mdhisut:w:";
#endif
	int ch;

	/* '+': stop at the first non-option like BSD getopt, which leaves the
	 * command's own flags alone; ':' and opterr = 0: print the messages in BSD
	 * wording instead of glibc's, musl's or mingw's */
	opterr = 0;
	while ((ch = getopt(argc, argv, optstring)) != -1) {
		switch (ch) {
		case 'm': o->want[A_DISK] = true; break;
		case 'd': o->want[A_DISPLAY] = true; break;
		case 'h': usage(); return EXIT_SUCCESS;
		case 'i': o->want[A_IDLE] = true; break;
		case 'r': o->update = true; break;	/* only in the Windows optstring */
		case 's': o->want[A_SYSTEM] = true; break;
		case 'u': o->want[A_USER] = true; break;
		case 'w':
			if (!mac_strtol(optarg, &o->waitforpid)) {
				usage();
				return EXIT_FAILURE;
			}
			o->waitforpid = (int)o->waitforpid;	/* pid_t in Apple's code */
			break;
		case 't':
			if (!mac_strtol(optarg, &o->timeout)) {
				usage();
				return EXIT_FAILURE;
			}
			if (o->timeout < 0)	/* unsigned in Apple's code: practically infinite */
				o->timeout = LONG_MAX;
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
	o->cmd = optind;

	/* no assertion flag at all: -i is the default. -r is no assertion of
	 * Apple's and does not count. */
	bool any = false;
	for (int i = 0; i < A_COUNT; i++)
		any |= o->want[i];
	if (!any)
		o->want[A_IDLE] = true;
	return -1;
}

#endif
