// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef TGLOGGERD_WEB_AUTH_TOKEN_HPP
#define TGLOGGERD_WEB_AUTH_TOKEN_HPP

#include <optional>
#include <string>
#include <string_view>

namespace tgweb::auth::token {

/*
 * Keyed signing primitives for stateless, tamper-proof cookies.
 *
 * A signed token is "<base64url(payload)>.<base64url(HMAC-SHA256(payload))>".
 * Anyone can read the payload, but only a holder of the secret key can produce
 * a valid signature, so a client cannot forge or alter one. The key is the
 * base64-decoded WEB_APP_KEY; because it lives only in the process (not in any
 * datastore), tokens stay valid across restarts.
 */

/*
 * Load the signing key from the base64-encoded WEB_APP_KEY environment
 * variable. Must be called once at startup, after libsodium is initialized and
 * before any token is made or opened. Returns false when WEB_APP_KEY is unset
 * or is not valid base64 of at least 16 bytes.
 */
bool init(void);

/* Sign a payload into "<b64url(payload)>.<b64url(mac)>". */
std::string make(std::string_view payload);

/*
 * Verify a token's signature in constant time and return its payload, or
 * std::nullopt if the token is malformed or the signature does not match.
 */
std::optional<std::string> open(std::string_view tokenStr);

/* base64url(HMAC-SHA256(key, msg)); a keyed, unforgeable tag for msg. */
std::string tag(std::string_view msg);

/* A fresh 256-bit random value, base64url-encoded (e.g. a CSRF nonce). */
std::string randomToken(void);

} /* namespace tgweb::auth::token */

#endif /* TGLOGGERD_WEB_AUTH_TOKEN_HPP */
