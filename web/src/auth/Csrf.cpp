// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include "auth/Csrf.hpp"

#include <sodium.h>

namespace tgweb::auth::csrf {

namespace {

constexpr const char *kKey = "csrf";
constexpr size_t kTokenBytes = 32; /* 256-bit token → 64 hex chars. */

std::string randomHex(void)
{
	unsigned char buf[kTokenBytes];
	randombytes_buf(buf, sizeof(buf));

	static const char hex[] = "0123456789abcdef";
	std::string out;
	out.reserve(sizeof(buf) * 2);
	for (unsigned char b : buf) {
		out.push_back(hex[b >> 4]);
		out.push_back(hex[b & 0x0f]);
	}
	return out;
}

} /* namespace */

std::string ensure(const drogon::SessionPtr &session)
{
	if (session->find(kKey))
		return session->getOptional<std::string>(kKey).value_or("");

	std::string token = randomHex();
	session->insert(kKey, token);
	return token;
}

bool check(const drogon::SessionPtr &session, const std::string &submitted)
{
	std::string expected =
		session->getOptional<std::string>(kKey).value_or("");

	/* No token in session, or length mismatch: reject before comparing. */
	if (expected.empty() || expected.size() != submitted.size())
		return false;

	return sodium_memcmp(expected.data(), submitted.data(),
			     expected.size()) == 0;
}

} /* namespace tgweb::auth::csrf */
