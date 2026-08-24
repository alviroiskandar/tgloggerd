// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 */
#ifndef TGLOGGERD__HELPERS__LOG_H
#define TGLOGGERD__HELPERS__LOG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

struct log_handle {
	FILE		*fp;
	int8_t		log_level;
	bool		close_fp_on_free;
};

typedef struct log_handle log_hd_t;

enum {
	PR_LOG_ERROR   = 0,
	PR_LOG_WARNING = 1,
	PR_LOG_INFO    = 2,
	PR_LOG_DEBUG   = 3,
};

extern void __pr_log(log_hd_t *h, int level, const char *fmt, ...)
__attribute__((format(printf, 3, 4)));

static inline void pr_set_log_level(log_hd_t *h, int level)
{
	h->log_level = level;
}

static inline void pr_set_close_fp_on_free(log_hd_t *h, bool close)
{
	h->close_fp_on_free = close;
}

log_hd_t *pr_create(const char *filename, int log_level);
log_hd_t *pr_create_from_fp(FILE *fp, int log_level,
			    bool close_fp_on_free);
void pr_free(log_hd_t *h);

#define pr_err(h, fmt, ...)   __pr_log(h, PR_LOG_ERROR, fmt, ##__VA_ARGS__)
#define pr_error(h, fmt, ...) __pr_log(h, PR_LOG_ERROR, fmt, ##__VA_ARGS__)
#define pr_warn(h, fmt, ...)  __pr_log(h, PR_LOG_WARNING, fmt, ##__VA_ARGS__)
#define pr_info(h, fmt, ...)  __pr_log(h, PR_LOG_INFO, fmt, ##__VA_ARGS__)
#define pr_debug(h, fmt, ...) __pr_log(h, PR_LOG_DEBUG, fmt, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* #ifndef TGLOGGERD__HELPERS__LOG_H */
