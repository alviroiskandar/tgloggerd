// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 */
#include "log.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>

log_hd_t *pr_create(const char *filename, int log_level)
{
	log_hd_t *h = malloc(sizeof(log_hd_t));
	if (!h) {
		errno = ENOMEM;
		return NULL;
	}

	h->fp = fopen(filename, "ab+");
	if (!h->fp) {
		free(h);
		return NULL;
	}

	h->log_level = log_level;
	h->close_fp_on_free = true;
	return h;
}

log_hd_t *pr_create_from_fp(FILE *fp, int log_level,
			    bool close_fp_on_free)
{
	log_hd_t *h = malloc(sizeof(log_hd_t));
	if (!h) {
		errno = ENOMEM;
		return NULL;
	}

	h->fp = fp;
	h->log_level = log_level;
	h->close_fp_on_free = close_fp_on_free;
	return h;
}

void pr_free(log_hd_t *h)
{
	if (!h)
		return;

	if (h->fp) {
		if (h->close_fp_on_free)
			fclose(h->fp);
		h->fp = NULL;
	}

	free(h);
}

static void get_date_time(char *buf, size_t buf_size)
{
	struct tm tm_info;
	time_t t = time(NULL);
	localtime_r(&t, &tm_info);
	strftime(buf, buf_size, "%Y-%m-%d %H:%M:%S%z", &tm_info);
}

void __pr_log(log_hd_t *h, int level, const char *fmt, ...)
{
	char *heap_buf = NULL, *buf;
	const char *log_level = NULL;
	char stack_buf[4096];
	char date_time[64];
	va_list ap1, ap2;
	int len;

	if (!h || !h->fp)
		return;

	switch (level) {
	case PR_LOG_ERROR:
		log_level = "[error]";
		break;
	case PR_LOG_WARNING:
		log_level = "[warn ]";
		break;
	case PR_LOG_INFO:
		log_level = "[info ]";
		break;
	case PR_LOG_DEBUG:
		log_level = "[debug]";
		break;
	default:
		log_level = "[?????]";
		break;
	}

	va_start(ap1, fmt);
	va_copy(ap2, ap1);
	buf = stack_buf;
	len = vsnprintf(buf, sizeof(stack_buf), fmt, ap1);
	va_end(ap1);
	if (__builtin_expect(len >= (int)sizeof(stack_buf), 0)) {
		heap_buf = malloc(len + 1);
		if (!heap_buf) {
			va_end(ap2);
			return;
		}
		buf = heap_buf;
		len = vsnprintf(buf, len + 1, fmt, ap2);
	}
	va_end(ap2);

	get_date_time(date_time, sizeof(date_time));
	fprintf(h->fp, "[%s]%s: %s\n", date_time, log_level, buf);
	if (__builtin_expect(heap_buf != NULL, 0))
		free(heap_buf);
}
