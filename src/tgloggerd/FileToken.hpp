// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef TGLOGGERD__FILE_TOKEN_HPP
#define TGLOGGERD__FILE_TOKEN_HPP

#include <cstdint>
#include <string>

namespace tgloggerd {

/*
 * Daemon-side minter for the web's public /files/<token> URLs. It reproduces
 * web/src/auth/FileToken exactly (same WEB_APP_KEY, same libsodium SIV
 * construction and domain labels), so a token minted here decrypts on the web's
 * MediaController. Used to give the Discord forwarder public avatar/media URLs.
 */
class FileToken {
public:
	/*
	 * Derive the subkeys from base64 WEB_APP_KEY. Returns false when the key
	 * is unset or too short (then encrypt() yields ""). Call once at startup.
	 */
	bool init(const std::string &web_app_key_b64);

	/* The 64-hex-char token for a file id, or "" if not initialized. */
	std::string encrypt(uint64_t file_id) const;

	bool ready(void) const { return ready_; }

private:
	unsigned char mac_[32] = {};
	unsigned char enc_[32] = {};
	bool ready_ = false;
};

} /* namespace tgloggerd */

#endif /* #ifndef TGLOGGERD__FILE_TOKEN_HPP */
