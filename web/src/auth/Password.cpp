// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include "auth/Password.hpp"

#include <sodium.h>

namespace tgweb::auth {

bool initCrypto(void)
{
	/* sodium_init() is idempotent and returns 1 if already initialized. */
	return sodium_init() >= 0;
}

std::string hashPassword(const std::string &password)
{
	char out[crypto_pwhash_STRBYTES];

	/*
	 * INTERACTIVE limits target roughly the argon2id cost recommended for
	 * online logins: strong, yet fast enough to not stall the login path.
	 */
	if (crypto_pwhash_str(out, password.c_str(), password.size(),
			      crypto_pwhash_OPSLIMIT_INTERACTIVE,
			      crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0) {
		/* Out of memory. */
		return std::string();
	}

	return std::string(out);
}

bool verifyPassword(const std::string &hash, const std::string &password)
{
	return crypto_pwhash_str_verify(hash.c_str(), password.c_str(),
					password.size()) == 0;
}

} /* namespace tgweb::auth */
