// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 */
#include <tgloggerd/tgloggerd_extern.h>
#include <tgloggerd/TgLoggerd.hpp>
#include <new>

using tgloggerd::TgLoggerd;

extern "C" {

tgloggerd_t *tgld_create(uint32_t api_id, const char *api_hash,
			 const char *data_dir)
{
	TgLoggerd *tgld;

	tgld = new (std::nothrow) TgLoggerd(api_id, api_hash, data_dir);
	if (!tgld)
		return nullptr;

	return reinterpret_cast<tgloggerd_t *>(tgld);
}

void tgld_start(tgloggerd_t *tgld)
{
	TgLoggerd *tgld_ = reinterpret_cast<TgLoggerd *>(tgld);
	tgld_->start();
}

void tgld_stop(tgloggerd_t *tgld)
{
	TgLoggerd *tgld_ = reinterpret_cast<TgLoggerd *>(tgld);
	tgld_->stop();
}

void tgld_set_logger(tgloggerd_t *tgld, log_hd_t *h)
{
	TgLoggerd *tgld_ = reinterpret_cast<TgLoggerd *>(tgld);
	tgld_->setLogger(h);
}

void tgld_set_logger_level(tgloggerd_t *tgld, int8_t log_level)
{
	TgLoggerd *tgld_ = reinterpret_cast<TgLoggerd *>(tgld);
	tgld_->setLoggerLevel(log_level);
}

void tgld_free(tgloggerd_t *tgld)
{
	TgLoggerd *tgld_ = reinterpret_cast<TgLoggerd *>(tgld);
	delete tgld_;
}

} /* extern "C" */
