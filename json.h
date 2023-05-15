/*
 * Copyright 2022 zhenwei pi
 *
 * Authors:
 *   zhenwei pi <pizhenwei@bytedance.com>
 *   Fei Li <lifei.shirley@bytedance.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef _JSON_H_
#define _JSON_H_

#include "photosyst.h"
#include "photoproc.h"
#include "connection.h"

int jsonout(int, char *, time_t, int, struct devtstat *, struct sstat *, int, unsigned int, char, connection* connection);

#endif
