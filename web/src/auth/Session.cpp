// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include "auth/Session.hpp"

#include "Config.hpp"
#include "auth/Token.hpp"

#include <ctime>

namespace tgweb::auth::session {

namespace {

/* Cookie lifetime from WEB_SESSION_TIMEOUT (seconds); <= 0 means "never", for
 * which we still set a long-lived cookie so the login persists. */
int maxAge(void)
{
	static const int v = atoi(tgweb::env("WEB_SESSION_TIMEOUT", "43200").c_str());
	return v;
}

bool secure(void)
{
	static const bool v = tgweb::env("WEB_SECURE_COOKIE", "1") != "0";
	return v;
}

std::string encode(uint64_t uid, const std::string &username,
		   const std::string &role, int64_t exp, uint32_t epoch)
{
	std::string payload = std::to_string(uid) + "\n" +
			      std::to_string(exp) + "\n" +
			      std::to_string(epoch) + "\n" + role + "\n" +
			      username;
	return token::make(payload);
}

/* Parse "<uid>\n<exp>\n<epoch>\n<role>\n<username>"; username is the remainder
 * so it may contain anything. Returns false on a malformed payload (which
 * includes the old 4-field format, so pre-epoch cookies force a re-login). */
bool parse(const std::string &p, Session &out)
{
	size_t a = p.find('\n');
	if (a == std::string::npos)
		return false;
	size_t b = p.find('\n', a + 1);
	if (b == std::string::npos)
		return false;
	size_t c = p.find('\n', b + 1);
	if (c == std::string::npos)
		return false;
	size_t d = p.find('\n', c + 1);
	if (d == std::string::npos)
		return false;

	try {
		out.uid = std::stoull(p.substr(0, a));
		out.exp = std::stoll(p.substr(a + 1, b - a - 1));
		out.epoch = (uint32_t)std::stoul(p.substr(b + 1, c - b - 1));
	} catch (...) {
		return false;
	}
	out.role = p.substr(c + 1, d - c - 1);
	out.username = p.substr(d + 1);
	return true;
}

} /* namespace */

std::string rawCookie(const drogon::HttpRequestPtr &req)
{
	return req->getCookie(kCookie);
}

std::optional<Session> current(const drogon::HttpRequestPtr &req)
{
	std::string raw = req->getCookie(kCookie);
	if (raw.empty())
		return std::nullopt;

	auto payload = token::open(raw);
	if (!payload)
		return std::nullopt;

	Session s;
	if (!parse(*payload, s))
		return std::nullopt;

	if (s.exp != 0 && (int64_t)std::time(nullptr) >= s.exp)
		return std::nullopt;

	return s;
}

bool isLoggedIn(const drogon::HttpRequestPtr &req)
{
	return current(req).has_value();
}

bool isAdmin(const drogon::HttpRequestPtr &req)
{
	auto s = current(req);
	return s && s->role == "admin";
}

void issue(const drogon::HttpResponsePtr &resp, uint64_t uid,
	   const std::string &username, const std::string &role,
	   uint32_t epoch)
{
	int age = maxAge();
	int64_t exp = age > 0 ? (int64_t)std::time(nullptr) + age : 0;

	drogon::Cookie c(kCookie, encode(uid, username, role, exp, epoch));
	c.setHttpOnly(true);
	c.setSecure(secure());
	c.setSameSite(drogon::Cookie::SameSite::kLax);
	c.setPath("/");
	/* Persist the cookie on disk (survives browser restart) even when the
	 * server sets no idle timeout; 10 years for the "never expires" case. */
	c.setMaxAge(age > 0 ? age : 315360000);
	resp->addCookie(std::move(c));
}

void clear(const drogon::HttpResponsePtr &resp)
{
	drogon::Cookie c(kCookie, "");
	c.setHttpOnly(true);
	c.setSecure(secure());
	c.setSameSite(drogon::Cookie::SameSite::kLax);
	c.setPath("/");
	c.setMaxAge(0);
	resp->addCookie(std::move(c));
}

} /* namespace tgweb::auth::session */
