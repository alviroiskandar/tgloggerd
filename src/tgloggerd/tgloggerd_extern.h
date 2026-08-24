// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 */
#ifndef TGLOGGERD__TGLOGGERD_EXTERN_H
#define TGLOGGERD__TGLOGGERD_EXTERN_H

#include "helpers/log.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void tgloggerd_t;

tgloggerd_t *tgld_create(uint32_t api_id, const char *api_hash,
			 const char *data_dir);
void tgld_start(tgloggerd_t *tgld);
void tgld_stop(tgloggerd_t *tgld);
void tgld_set_logger(tgloggerd_t *tgld, log_hd_t *h);
void tgld_set_logger_level(tgloggerd_t *tgld, int8_t log_level);
void tgld_free(tgloggerd_t *tgld);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* #ifndef TGLOGGERD__TGLOGGERD_EXTERN_H */
