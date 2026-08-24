// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 */
#include "common.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

int mkdir_recursive(const char *path, unsigned int mode)
{
	char *p = strdup(path);
	int err;

	if (!p)
		return -EINVAL;

	for (char *q = p + 1; *q; q++) {
		if (*q == '/') {
			*q = '\0';
			if (mkdir(p, mode) != 0 && errno != EEXIST) {
				err = errno;
				free(p);
				return -err;
			}
			*q = '/';
		}
	}

	if (mkdir(p, mode) != 0) {
		err = errno;
		free(p);
		return -err;
	}

	free(p);
	return 0;
}
