// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include "FileToken.hpp"

#include <sodium.h>

#include <cstring>
#include <vector>

namespace tgloggerd {

namespace {

/* Raw token layout (mirrors web/src/auth/FileToken.cpp): a 24-byte synthetic
 * nonce/tag, then the XChaCha20-encrypted 8-byte little-endian id -> 32 bytes
 * -> 64 hex chars. */
constexpr size_t kPlain = 8;
constexpr size_t kTag   = crypto_stream_xchacha20_NONCEBYTES; /* 24 */
constexpr size_t kRaw   = kTag + kPlain;                      /* 32 */
constexpr size_t kHex   = kRaw * 2;                           /* 64 */

std::vector<unsigned char> b64decode_any(const std::string &s)
{
	for (int v : {sodium_base64_VARIANT_ORIGINAL,
		      sodium_base64_VARIANT_ORIGINAL_NO_PADDING,
		      sodium_base64_VARIANT_URLSAFE,
		      sodium_base64_VARIANT_URLSAFE_NO_PADDING}) {
		std::vector<unsigned char> out(s.size());
		size_t len = 0;
		if (sodium_base642bin(out.data(), out.size(), s.data(), s.size(),
				      nullptr, &len, nullptr, v) == 0) {
			out.resize(len);
			return out;
		}
	}
	return {};
}

void derive_subkey(unsigned char out[32], const std::vector<unsigned char> &key,
		   const char *label)
{
	crypto_generichash(out, 32,
			   reinterpret_cast<const unsigned char *>(label),
			   std::strlen(label), key.data(), key.size());
}

void put_le64(unsigned char b[8], uint64_t v)
{
	for (int i = 0; i < 8; i++) {
		b[i] = (unsigned char)(v & 0xff);
		v >>= 8;
	}
}

} /* namespace */

bool FileToken::init(const std::string &web_app_key_b64)
{
	if (sodium_init() < 0)
		return false;
	if (web_app_key_b64.empty())
		return false;

	auto raw = b64decode_any(web_app_key_b64);
	if (raw.size() < 16)
		return false;

	derive_subkey(mac_, raw, "tgloggerd:file-token:mac:v1");
	derive_subkey(enc_, raw, "tgloggerd:file-token:enc:v1");
	ready_ = true;
	return true;
}

std::string FileToken::encrypt(uint64_t file_id) const
{
	if (!ready_)
		return std::string();

	unsigned char plain[kPlain];
	put_le64(plain, file_id);

	unsigned char raw[kRaw];
	crypto_generichash(raw, kTag, plain, kPlain, mac_, sizeof(mac_));
	crypto_stream_xchacha20_xor(raw + kTag, plain, kPlain, raw, enc_);

	char hex[kHex + 1];
	sodium_bin2hex(hex, sizeof(hex), raw, kRaw);
	return std::string(hex, kHex);
}

} /* namespace tgloggerd */
