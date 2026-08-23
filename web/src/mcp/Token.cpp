// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include "mcp/Token.hpp"

#include <sodium.h>

#include <cctype>
#include <cstring>

namespace tgweb::mcp::token {

namespace {

constexpr size_t RANDOM_BYTES = 32;

const char *HEX = "0123456789abcdef";

std::string toHex(const unsigned char *p, size_t n)
{
	std::string out;
	out.resize(n * 2);
	for (size_t i = 0; i < n; i++) {
		out[i * 2] = HEX[p[i] >> 4];
		out[i * 2 + 1] = HEX[p[i] & 0x0f];
	}
	return out;
}

} /* namespace */

std::string mint(void)
{
	unsigned char buf[RANDOM_BYTES];
	/* randombytes_buf is the CSPRNG libsodium already initialises for the
	 * session and file-token code; no separate seeding needed here. */
	randombytes_buf(buf, sizeof(buf));

	std::string out = PREFIX;
	out += toHex(buf, sizeof(buf));
	sodium_memzero(buf, sizeof(buf));
	return out;
}

std::string digest(const std::string &plaintext)
{
	unsigned char out[crypto_hash_sha256_BYTES];
	crypto_hash_sha256(out,
			   reinterpret_cast<const unsigned char *>(plaintext.data()),
			   plaintext.size());
	return std::string(reinterpret_cast<const char *>(out), sizeof(out));
}

std::string fromAuthorizationHeader(const std::string &header)
{
	if (header.empty())
		return std::string();

	/* Scheme is case-insensitive per RFC 7235. */
	static const char kBearer[] = "bearer";
	const size_t schemeLen = sizeof(kBearer) - 1;
	if (header.size() <= schemeLen)
		return std::string();

	for (size_t i = 0; i < schemeLen; i++) {
		if (std::tolower(static_cast<unsigned char>(header[i])) !=
		    kBearer[i])
			return std::string();
	}
	if (header[schemeLen] != ' ' && header[schemeLen] != '\t')
		return std::string();

	size_t p = schemeLen;
	while (p < header.size() && (header[p] == ' ' || header[p] == '\t'))
		p++;

	size_t e = header.size();
	while (e > p && (header[e - 1] == ' ' || header[e - 1] == '\t' ||
			 header[e - 1] == '\r' || header[e - 1] == '\n'))
		e--;

	return header.substr(p, e - p);
}

bool looksLikeToken(const std::string &s)
{
	const size_t prefixLen = std::strlen(PREFIX);
	if (s.size() != prefixLen + RANDOM_BYTES * 2)
		return false;
	if (s.compare(0, prefixLen, PREFIX) != 0)
		return false;
	for (size_t i = prefixLen; i < s.size(); i++) {
		const char c = s[i];
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
			return false;
	}
	return true;
}

} /* namespace tgweb::mcp::token */
