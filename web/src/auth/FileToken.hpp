// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef TGLOGGERD_WEB_AUTH_FILETOKEN_HPP
#define TGLOGGERD_WEB_AUTH_FILETOKEN_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace tgweb::auth::filetoken {

/*
 * Reversible, opaque file-id tokens for the public /files/<token> URL.
 *
 * The token is a deterministic authenticated encryption of the 64-bit file id
 * (SIV construction): a synthetic nonce is the keyed BLAKE2b of the id, and the
 * id is then XChaCha20-encrypted under that nonce. Keyed by WEB_APP_KEY, this
 * gives tokens that are
 *   - stable: the same id always maps to the same token (cacheable URLs);
 *   - fixed length: always 32 bytes -> 64 lowercase hex characters;
 *   - non-enumerable: adjacent ids yield unrelated tokens, so the file space
 *     cannot be walked sequentially even when the site is public;
 *   - unforgeable: a token that was not produced with the key is rejected, so
 *     only links the app itself minted resolve.
 * No server-side state is kept; decryption recovers the id directly.
 */

/*
 * Derive the token subkeys from the base64-encoded WEB_APP_KEY. Call once at
 * startup, after libsodium is initialized. Returns false when WEB_APP_KEY is
 * unset or is not valid base64 of at least 16 bytes.
 */
bool init(void);

/* The token for a file id: 64 lowercase hex characters. */
std::string encrypt(uint64_t file_id);

/* The file id a token encodes, or std::nullopt if it is malformed, the wrong
 * length, or fails authentication (not minted with our key). */
std::optional<uint64_t> decrypt(std::string_view hex);

} /* namespace tgweb::auth::filetoken */

#endif /* TGLOGGERD_WEB_AUTH_FILETOKEN_HPP */
