// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include "auth/FileToken.hpp"

#include "Config.hpp"

#include <sodium.h>

#include <cstring>
#include <vector>

namespace tgweb::auth::filetoken {

namespace {

/* Layout of the raw (pre-hex) token: a synthetic nonce/tag followed by the
 * encrypted 8-byte little-endian id. The tag doubles as the XChaCha20 nonce,
 * so it is exactly one nonce wide. */
constexpr size_t kPlain = 8;                                  /* id, LE.       */
constexpr size_t kTag   = crypto_stream_xchacha20_NONCEBYTES; /* 24.           */
constexpr size_t kRaw   = kTag + kPlain;                      /* 32.           */
constexpr size_t kHex   = kRaw * 2;                           /* 64.           */

/* Subkeys derived from WEB_APP_KEY: one for the synthetic nonce (MAC), one for
 * the stream cipher. Kept in-process only, so tokens survive restarts. */
unsigned char g_mac[crypto_generichash_KEYBYTES];             /* 32.           */
unsigned char g_enc[crypto_stream_xchacha20_KEYBYTES];        /* 32.           */
bool g_ready = false;

/* base64-decode trying the common alphabets/padding, like the cookie key. */
std::optional<std::vector<unsigned char>> b64decodeAny(const std::string &s)
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
	return std::nullopt;
}

/* A 32-byte subkey = keyed BLAKE2b of a domain-separation label. */
void deriveSubkey(unsigned char out[32], const std::vector<unsigned char> &key,
		  const char *label)
{
	crypto_generichash(out, 32,
			   reinterpret_cast<const unsigned char *>(label),
			   std::strlen(label), key.data(), key.size());
}

void putLE64(unsigned char b[8], uint64_t v)
{
	for (int i = 0; i < 8; i++) {
		b[i] = (unsigned char)(v & 0xff);
		v >>= 8;
	}
}

uint64_t getLE64(const unsigned char b[8])
{
	uint64_t v = 0;
	for (int i = 7; i >= 0; i--)
		v = (v << 8) | b[i];
	return v;
}

} /* namespace */

bool init(void)
{
	std::string k = tgweb::env("WEB_APP_KEY", "");
	if (k.empty())
		return false;

	auto raw = b64decodeAny(k);
	if (!raw || raw->size() < 16)
		return false;

	deriveSubkey(g_mac, *raw, "tgloggerd:file-token:mac:v1");
	deriveSubkey(g_enc, *raw, "tgloggerd:file-token:enc:v1");
	g_ready = true;
	return true;
}

std::string encrypt(uint64_t file_id)
{
	if (!g_ready)
		return std::string();

	unsigned char plain[kPlain];
	putLE64(plain, file_id);

	unsigned char raw[kRaw];
	/* Synthetic nonce/tag: deterministic in the id, so the token is stable;
	 * BLAKE2b's avalanche makes adjacent ids produce unrelated tags. */
	crypto_generichash(raw, kTag, plain, kPlain, g_mac, sizeof(g_mac));
	/* Encrypt the id under that nonce. */
	crypto_stream_xchacha20_xor(raw + kTag, plain, kPlain, raw, g_enc);

	char hex[kHex + 1];
	sodium_bin2hex(hex, sizeof(hex), raw, kRaw);
	return std::string(hex, kHex);
}

std::optional<uint64_t> decrypt(std::string_view hex)
{
	if (!g_ready || hex.size() != kHex)
		return std::nullopt;

	unsigned char raw[kRaw];
	if (sodium_hex2bin(raw, sizeof(raw), hex.data(), hex.size(),
			   nullptr, nullptr, nullptr) != 0)
		return std::nullopt;

	unsigned char plain[kPlain];
	crypto_stream_xchacha20_xor(plain, raw + kTag, kPlain, raw, g_enc);

	/* Recompute the synthetic nonce and require it to match: a token that
	 * was not produced with our key (a guessed or tampered one) fails here
	 * in constant time. */
	unsigned char tag[kTag];
	crypto_generichash(tag, kTag, plain, kPlain, g_mac, sizeof(g_mac));
	if (sodium_memcmp(tag, raw, kTag) != 0)
		return std::nullopt;

	return getLE64(plain);
}

} /* namespace tgweb::auth::filetoken */
