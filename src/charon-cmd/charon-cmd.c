/*
 * Copyright (C) 2006-2012 Tobias Brunner
 * Copyright (C) 2005-2013 Martin Willi
 * Copyright (C) 2006 Daniel Roethlisberger
 * Copyright (C) 2005 Jan Hutter
 * Hochschule fuer Technik Rapperswil
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <http://www.fsf.org/copyleft/gpl.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#include <stdio.h>
#define _POSIX_PTHREAD_SEMANTICS /* for two param sigwait on OpenSolaris */
#include <signal.h>
#undef _POSIX_PTHREAD_SEMANTICS
#include <pthread.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <getopt.h>

#include <library.h>
#include <hydra.h>
#include <daemon.h>
#include <utils/backtrace.h>
#include <threading/thread.h>

#include "cmd/cmd_options.h"

/**
 * Loglevel configuration
 */
static level_t levels[DBG_MAX];

/**
 * hook in library for debugging messages
 */
extern void (*dbg) (debug_t group, level_t level, char *fmt, ...);

/**
 * Logging hook for library logs, using stderr output
 */
static void dbg_stderr(debug_t group, level_t level, char *fmt, ...)
{
	va_list args;

	if (level <= 1)
	{
		va_start(args, fmt);
		fprintf(stderr, "00[%N] ", debug_names, group);
		vfprintf(stderr, fmt, args);
		fprintf(stderr, "\n");
		va_end(args);
	}
}

/**
 * Run the daemon and handle unix signals
 */
static void run()
{
	sigset_t set;

	/* handle SIGINT, SIGHUP ans SIGTERM in this handler */
	sigemptyset(&set);
	sigaddset(&set, SIGINT);
	sigaddset(&set, SIGHUP);
	sigaddset(&set, SIGTERM);
	sigprocmask(SIG_BLOCK, &set, NULL);

	while (TRUE)
	{
		int sig;
		int error;

		error = sigwait(&set, &sig);
		if (error)
		{
			DBG1(DBG_DMN, "error %d while waiting for a signal", error);
			return;
		}
		switch (sig)
		{
			case SIGHUP:
			{
				DBG1(DBG_DMN, "signal of type SIGHUP received. Reloading "
					 "configuration");
				if (lib->settings->load_files(lib->settings, NULL, FALSE))
				{
					charon->load_loggers(charon, levels, TRUE);
					lib->plugins->reload(lib->plugins, NULL);
				}
				else
				{
					DBG1(DBG_DMN, "reloading config failed, keeping old");
				}
				break;
			}
			case SIGINT:
			{
				DBG1(DBG_DMN, "signal of type SIGINT received. Shutting down");
				charon->bus->alert(charon->bus, ALERT_SHUTDOWN_SIGNAL, sig);
				return;
			}
			case SIGTERM:
			{
				DBG1(DBG_DMN, "signal of type SIGTERM received. Shutting down");
				charon->bus->alert(charon->bus, ALERT_SHUTDOWN_SIGNAL, sig);
				return;
			}
			default:
			{
				DBG1(DBG_DMN, "unknown signal %d received. Ignored", sig);
				break;
			}
		}
	}
}

/**
 * lookup UID and GID
 */
static bool lookup_uid_gid()
{
#ifdef IPSEC_USER
	if (!charon->caps->resolve_uid(charon->caps, IPSEC_USER))
	{
		return FALSE;
	}
#endif
#ifdef IPSEC_GROUP
	if (!charon->caps->resolve_gid(charon->caps, IPSEC_GROUP))
	{
		return FALSE;
	}
#endif
	return TRUE;
}

/**
 * Handle SIGSEGV/SIGILL signals raised by threads
 */
static void segv_handler(int signal)
{
	backtrace_t *backtrace;

	DBG1(DBG_DMN, "thread %u received %d", thread_current_id(), signal);
	backtrace = backtrace_create(2);
	backtrace->log(backtrace, stderr, TRUE);
	backtrace->destroy(backtrace);

	DBG1(DBG_DMN, "killing ourself, received critical signal");
	abort();
}

/**
 * Print command line usage and exit
 */
static void usage(FILE *out, char *msg, char *binary)
{
	int i, pre, post, padto = 0, spacing = 2;

	for (i = 0; i < CMD_OPT_COUNT; i++)
	{
		padto = max(padto, strlen(cmd_options[i].name) +
						   strlen(cmd_options[i].arg));
	}
	padto += spacing;

	if (msg)
	{
		fprintf(out, "%s\n", msg);
	}
	fprintf(out, "Usage: %s\n", binary);
	for (i = 0; i < CMD_OPT_COUNT; i++)
	{
		switch (cmd_options[i].has_arg)
		{
			case required_argument:
				pre = '<';
				post = '>';
				break;
			case optional_argument:
				pre = '[';
				post = ']';
				break;
			case no_argument:
			default:
				pre = post = ' ';
				break;
		}
		fprintf(out, "  --%s %c%s%c %-*s%s\n",
			cmd_options[i].name,
			pre, cmd_options[i].arg, post,
			padto - strlen(cmd_options[i].name) - strlen(cmd_options[i].arg), "",
			cmd_options[i].desc);
	}
}

/**
 * Handle command line options
 */
static void handle_arguments(int argc, char *argv[])
{
	while (TRUE)
	{
		struct option long_opts[CMD_OPT_COUNT + 1] = {};
		int i;

		for (i = 0; i < CMD_OPT_COUNT; i++)
		{
			long_opts[i].name = cmd_options[i].name;
			long_opts[i].val = cmd_options[i].id;
			long_opts[i].has_arg = cmd_options[i].has_arg;
		}

		switch (getopt_long(argc, argv, "", long_opts, NULL))
		{
			case EOF:
				break;
			case CMD_OPT_HELP:
				usage(stdout, NULL, argv[0]);
				exit(0);
			case CMD_OPT_VERSION:
				printf("%s, strongSwan %s\n", "charon-cmd", VERSION);
				exit(0);
			default:
				usage(stderr, NULL, argv[0]);
				exit(1);
		}
		break;
	}
}

/**
 * Main function, starts the daemon.
 */
int main(int argc, char *argv[])
{
	struct sigaction action;
	struct utsname utsname;
	int group;

	dbg = dbg_stderr;
	atexit(library_deinit);
	if (!library_init(NULL))
	{
		exit(SS_RC_LIBSTRONGSWAN_INTEGRITY);
	}
	if (lib->integrity)
	{
		if (!lib->integrity->check_file(lib->integrity, "charon-cmd", argv[0]))
		{
			exit(SS_RC_DAEMON_INTEGRITY);
		}
	}
	atexit(libhydra_deinit);
	if (!libhydra_init("charon-cmd"))
	{
		exit(SS_RC_INITIALIZATION_FAILED);
	}
	atexit(libcharon_deinit);
	if (!libcharon_init("charon-cmd"))
	{
		exit(SS_RC_INITIALIZATION_FAILED);
	}
	for (group = 0; group < DBG_MAX; group++)
	{
		levels[group] = LEVEL_CTRL;
	}

	handle_arguments(argc, argv);

	if (!lookup_uid_gid())
	{
		exit(SS_RC_INITIALIZATION_FAILED);
	}
	charon->load_loggers(charon, levels, TRUE);

	if (uname(&utsname) != 0)
	{
		memset(&utsname, 0, sizeof(utsname));
	}
	DBG1(DBG_DMN, "Starting charon-cmd IKE client (strongSwan %s, %s %s, %s)",
		 VERSION, utsname.sysname, utsname.release, utsname.machine);

	if (!charon->initialize(charon,
			lib->settings->get_str(lib->settings, "charon-cmd.load", PLUGINS)))
	{
		exit(SS_RC_INITIALIZATION_FAILED);
	}
	if (!charon->caps->drop(charon->caps))
	{
		exit(SS_RC_INITIALIZATION_FAILED);
	}

	/* add handler for SEGV and ILL,
	 * INT, TERM and HUP are handled by sigwait() in run() */
	action.sa_handler = segv_handler;
	action.sa_flags = 0;
	sigemptyset(&action.sa_mask);
	sigaddset(&action.sa_mask, SIGINT);
	sigaddset(&action.sa_mask, SIGTERM);
	sigaddset(&action.sa_mask, SIGHUP);
	sigaction(SIGSEGV, &action, NULL);
	sigaction(SIGILL, &action, NULL);
	sigaction(SIGBUS, &action, NULL);
	action.sa_handler = SIG_IGN;
	sigaction(SIGPIPE, &action, NULL);

	pthread_sigmask(SIG_SETMASK, &action.sa_mask, NULL);

	/* start daemon with thread-pool */
	charon->start(charon);
	/* wait for signal */
	run();

	return 0;
}