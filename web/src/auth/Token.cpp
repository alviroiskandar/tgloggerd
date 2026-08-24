// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include "auth/Token.hpp"

#include "Config.hpp"

#include <sodium.h>

#include <cstring>
#include <vector>

namespace tgweb::auth::token {

namespace {

/* The HMAC key: WEB_APP_KEY, base64-decoded. Set once by init(). */
std::vector<unsigned char> g_key;

std::string b64encode(const unsigned char *bin, size_t len, int variant)
{
	size_t max = sodium_base64_encoded_len(len, variant); /* incl. NUL. */
	std::string out(max, '\0');
	sodium_bin2base64(out.data(), max, bin, len, variant);
	out.resize(std::strlen(out.c_str()));
	return out;
}

std::optional<std::vector<unsigned char>> b64decode(std::string_view s,
						    int variant)
{
	std::vector<unsigned char> out(s.size()); /* decoded is never larger. */
	size_t len = 0;
	if (sodium_base642bin(out.data(), out.size(), s.data(), s.size(),
			      nullptr, &len, nullptr, variant) != 0)
		return std::nullopt;
	out.resize(len);
	return out;
}

/* Try the common base64 spellings so WEB_APP_KEY may carry padding or not and
 * use the standard or URL-safe alphabet. */
std::optional<std::vector<unsigned char>> b64decodeAny(std::string_view s)
{
	for (int v : {sodium_base64_VARIANT_ORIGINAL,
		      sodium_base64_VARIANT_ORIGINAL_NO_PADDING,
		      sodium_base64_VARIANT_URLSAFE,
		      sodium_base64_VARIANT_URLSAFE_NO_PADDING}) {
		auto r = b64decode(s, v);
		if (r)
			return r;
	}
	return std::nullopt;
}

std::string hmacRaw(std::string_view msg)
{
	unsigned char mac[crypto_auth_hmacsha256_BYTES];
	crypto_auth_hmacsha256_state st;
	crypto_auth_hmacsha256_init(&st, g_key.data(), g_key.size());
	crypto_auth_hmacsha256_update(
		&st, reinterpret_cast<const unsigned char *>(msg.data()),
		msg.size());
	crypto_auth_hmacsha256_final(&st, mac);
	return std::string(reinterpret_cast<char *>(mac), sizeof(mac));
}

std::string b64url(std::string_view raw)
{
	return b64encode(reinterpret_cast<const unsigned char *>(raw.data()),
			 raw.size(), sodium_base64_VARIANT_URLSAFE_NO_PADDING);
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

	g_key = std::move(*raw);
	return true;
}

std::string tag(std::string_view msg)
{
	return b64url(hmacRaw(msg));
}

std::string make(std::string_view payload)
{
	std::string body = b64url(payload);
	return body + "." + tag(body);
}

std::optional<std::string> open(std::string_view tokenStr)
{
	auto dot = tokenStr.find('.');
	if (dot == std::string_view::npos)
		return std::nullopt;

	std::string_view body = tokenStr.substr(0, dot);
	std::string_view sig = tokenStr.substr(dot + 1);

	std::string expect = tag(body);
	if (expect.size() != sig.size() ||
	    sodium_memcmp(expect.data(), sig.data(), expect.size()) != 0)
		return std::nullopt;

	auto raw = b64decode(body, sodium_base64_VARIANT_URLSAFE_NO_PADDING);
	if (!raw)
		return std::nullopt;
	return std::string(reinterpret_cast<char *>(raw->data()), raw->size());
}

std::string randomToken(void)
{
	unsigned char buf[32];
	randombytes_buf(buf, sizeof(buf));
	return b64encode(buf, sizeof(buf),
			 sodium_base64_VARIANT_URLSAFE_NO_PADDING);
}

} /* namespace tgweb::auth::token */
