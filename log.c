/*
 * Minimal logging layer for atophttpd.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include "log.h"

static int use_syslog;
static int stderr_is_tty;

void log_open(int daemon_mode)
{
	use_syslog = daemon_mode;
	stderr_is_tty = isatty(STDERR_FILENO);
	if (use_syslog)
		openlog("atophttpd", LOG_PID, LOG_DAEMON);
}

void log_msg(int priority, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	if (use_syslog) {
		vsyslog(priority, fmt, ap);
		va_end(ap);
		return;
	}

	char line[1024];
	int n = vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	if (n < 0)
		return;

	size_t len = strlen(line);
	const char *nl = (len == 0 || line[len - 1] != '\n') ? "\n" : "";

	/*
	 * When stderr is captured by systemd-journald (not a terminal) a leading
	 * "<N>" marker makes the journal record the right severity; keep output
	 * clean for interactive use.
	 */
	if (stderr_is_tty)
		fprintf(stderr, "%s%s", line, nl);
	else
		fprintf(stderr, "<%d>%s%s", LOG_PRI(priority), line, nl);
}
