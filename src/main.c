// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 */
#include <tgloggerd/tgloggerd_extern.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>

/*
 * Set by the SIGINT/SIGTERM handler to request a graceful shutdown. The
 * event loop in TgLoggerd::start() polls it, exits, and drains the
 * background workers before returning. The handler only touches this
 * flag, which is async-signal-safe.
 */
volatile sig_atomic_t g_tgld_stop = 0;

static void tgld_signal_handler(int sig)
{
	(void)sig;
	g_tgld_stop = 1;
}

static void tgld_install_signal_handlers(void)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = tgld_signal_handler;
	sigemptyset(&sa.sa_mask);
	/* No SA_RESTART: the loop re-checks the flag each receive() tick. */
	sa.sa_flags = 0;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
}

int main(void)
{
	char *api_id, *api_hash, *data_dir, *log_file, *log_level_str;
	int log_level = PR_LOG_INFO;
	tgloggerd_t *tgld;
	log_hd_t *h = NULL;

	api_id = getenv("TG_API_ID");
	if (!api_id) {
		fprintf(stderr, "TG_API_ID is not set\n");
		return 1;
	}

	api_hash = getenv("TG_API_HASH");
	if (!api_hash) {
		fprintf(stderr, "TG_API_HASH is not set\n");
		return 1;
	}

	data_dir = getenv("TG_DATA_DIR");
	if (!data_dir) {
		fprintf(stderr, "TG_DATA_DIR is not set\n");
		return 1;
	}

	tgld = tgld_create(atoi(api_id), api_hash, data_dir);
	if (!tgld) {
		fprintf(stderr, "tgld_create failed\n");
		return 1;
	}

	log_file = getenv("TG_LOG_FILE");
	if (log_file && *log_file != '\0') {
		log_level_str = getenv("TG_LOG_LEVEL");
		if (log_level_str) {
			if (strcmp(log_level_str, "error") == 0) {
				log_level = PR_LOG_ERROR;
			} else if (strcmp(log_level_str, "warning") == 0) {
				log_level = PR_LOG_WARNING;
			} else if (strcmp(log_level_str, "info") == 0) {
				log_level = PR_LOG_INFO;
			} else if (strcmp(log_level_str, "debug") == 0) {
				log_level = PR_LOG_DEBUG;
			} else {
				fprintf(stderr, "Invalid log level: %s\n", log_level_str);
				tgld_free(tgld);
				return 1;
			}
		}
	}

	if (log_file && *log_file != '\0') {
		h = pr_create(log_file, log_level);
		if (!h) {
			fprintf(stderr, "pr_create failed\n");
			tgld_free(tgld);
			return 1;
		}
	} else {
		h = pr_create_from_fp(stdout, log_level, false);
		if (!h) {
			fprintf(stderr, "pr_create_from_fp failed\n");
			tgld_free(tgld);
			return 1;
		}
	}

	tgld_set_logger(tgld, h);
	tgld_install_signal_handlers();
	tgld_start(tgld);
	tgld_stop(tgld);
	tgld_free(tgld);
	return 0;
}
