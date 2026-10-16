/*
 * Minimal logging layer for atophttpd.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef _LOG_H_
#define _LOG_H_

#include <syslog.h>

/*
 * Initialise the logging backend. Call once, early in main().
 *
 *   daemon_mode != 0 -> route messages to syslog(3). After daemon(3) the
 *                       standard streams are redirected to /dev/null, so
 *                       anything written there would be lost.
 *   daemon_mode == 0 -> write to stderr. When stderr is not a terminal
 *                       (typically a pipe to systemd-journald) each line is
 *                       prefixed with a "<N>" syslog level marker so the
 *                       journal records the correct severity; interactive
 *                       terminals get clean, unprefixed output.
 */
void log_open(int daemon_mode);

void log_msg(int priority, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

#define log_err(fmt, ...)  log_msg(LOG_ERR, fmt, ##__VA_ARGS__)
#define log_warn(fmt, ...) log_msg(LOG_WARNING, fmt, ##__VA_ARGS__)
#define log_info(fmt, ...) log_msg(LOG_INFO, fmt, ##__VA_ARGS__)

#endif
