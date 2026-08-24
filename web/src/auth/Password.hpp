// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef TGLOGGERD_WEB_AUTH_PASSWORD_HPP
#define TGLOGGERD_WEB_AUTH_PASSWORD_HPP

#include <string>

namespace tgweb::auth {

/*
 * Initialize libsodium. Must be called once before any hashing or
 * verification. Returns false if the library fails to initialize.
 */
bool initCrypto(void);

/*
 * Hash a password into a self-describing argon2id string (libsodium
 * crypto_pwhash_str: algorithm, parameters, and salt are all embedded).
 * Returns an empty string on failure. The result fits web_users.password_hash
 * (VARCHAR(255)); argon2id strings are at most 128 bytes.
 */
std::string hashPassword(const std::string &password);

/*
 * Verify a plaintext password against a stored argon2id hash. Returns false
 * on mismatch or malformed hash. Runs in constant time relative to the hash.
 */
bool verifyPassword(const std::string &hash, const std::string &password);

} /* namespace tgweb::auth */

#endif /* TGLOGGERD_WEB_AUTH_PASSWORD_HPP */
