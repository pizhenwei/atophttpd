/*
 * Copyright 2022 zhenwei pi
 *
 * Authors:
 *   zhenwei pi <pizhenwei@bytedance.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef _OUTPUT_H_
#define _OUTPUT_H_

enum {
        OUTPUT_STDOUT,
        OUTPUT_FD,
        OUTPUT_BUF
};

struct output {
        int output_type;
        union {
                /* OUTPUT_STDOUT needs no more argument */
                int fd; /* for OUTPUT_FD */
                struct output_buf {
                        char *buf;
                        int size; /* size of buf, auto grow if not enough */
                        int offset; /* offset of buf, reset to 0 for next record */
                } ob; /* OUTPUT_BUF */
        };
        void    (*done)(struct output *op);
};

void output_samp(struct output *op, char *buf, int size);
void output_samp_done(struct output *op);

#endif
