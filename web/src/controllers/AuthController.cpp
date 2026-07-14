// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include "controllers/AuthController.hpp"

#include "Config.hpp"
#include "auth/Csrf.hpp"
#include "auth/Password.hpp"
#include "auth/RateLimiter.hpp"
#include "auth/Session.hpp"
#include "dao/Accounts.hpp"
#include "dao/Audit.hpp"

#include <chrono>
#include <cstdlib>
#include <string>

namespace tgweb::controllers {

namespace {

/*
 * Process-wide login throttle, configured once from the environment:
 * WEB_LOGIN_MAX_ATTEMPTS failures per WEB_LOGIN_WINDOW seconds, keyed by
 * (ip, username).
 */
tgweb::auth::RateLimiter &limiter(void)
{
	static tgweb::auth::RateLimiter inst(
		(size_t)atoi(tgweb::env("WEB_LOGIN_MAX_ATTEMPTS", "5").c_str()),
		std::chrono::seconds(
			atoi(tgweb::env("WEB_LOGIN_WINDOW", "900").c_str())));
	return inst;
}

/* Minimal login page. Replaced by the template layout in a later change. The
 * only dynamic values are the CSRF token (hex) and a fixed error string, so no
 * untrusted data is interpolated here. */
std::string renderLogin(const std::string &csrf, const char *error)
{
	std::string html =
		"<!doctype html>\n"
		"<html lang=\"en\"><head><meta charset=\"utf-8\">"
		"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
		"<title>Sign in — tgloggerd</title></head><body>"
		"<h1>tgloggerd</h1><h2>Sign in</h2>";
	if (error)
		html += std::string("<p style=\"color:#b00\">") + error + "</p>";
	html +=
		"<form method=\"post\" action=\"/login\">"
		"<input type=\"hidden\" name=\"csrf\" value=\"" + csrf + "\">"
		"<p><label>Username <input name=\"username\" autofocus "
		"autocomplete=\"username\"></label></p>"
		"<p><label>Password <input name=\"password\" type=\"password\" "
		"autocomplete=\"current-password\"></label></p>"
		"<p><button type=\"submit\">Sign in</button></p>"
		"</form></body></html>\n";
	return html;
}

drogon::HttpResponsePtr htmlResponse(const std::string &body,
				     drogon::HttpStatusCode code)
{
	auto resp = drogon::HttpResponse::newHttpResponse();
	resp->setStatusCode(code);
	resp->setContentTypeCode(drogon::CT_TEXT_HTML);
	resp->setBody(body);
	return resp;
}

drogon::HttpResponsePtr redirect(const std::string &to)
{
	return drogon::HttpResponse::newRedirectionResponse(to);
}

} /* namespace */

drogon::Task<drogon::HttpResponsePtr>
AuthController::getLogin(drogon::HttpRequestPtr req)
{
	const auto &s = req->session();
	if (tgweb::auth::session::isLoggedIn(s))
		co_return redirect("/");

	std::string token = tgweb::auth::csrf::ensure(s);
	co_return htmlResponse(renderLogin(token, nullptr), drogon::k200OK);
}

drogon::Task<drogon::HttpResponsePtr>
AuthController::postLogin(drogon::HttpRequestPtr req)
{
	const auto &s = req->session();
	auto db = drogon::app().getDbClient("app");

	std::string username = req->getParameter("username");
	std::string password = req->getParameter("password");
	std::string csrf = req->getParameter("csrf");
	std::string ip = req->getPeerAddr().toIp();

	/* CSRF first: a bad/missing token means the request is not trusted. */
	if (!tgweb::auth::csrf::check(s, csrf)) {
		std::string token = tgweb::auth::csrf::ensure(s);
		co_return htmlResponse(
			renderLogin(token, "Your session expired. Please try again."),
			drogon::k403Forbidden);
	}

	std::string rlKey = ip + "\n" + username;
	if (!limiter().allowed(rlKey)) {
		co_await tgweb::dao::audit::log(db, std::nullopt, "login_ratelimited",
						ip, username);
		std::string token = tgweb::auth::csrf::ensure(s);
		co_return htmlResponse(
			renderLogin(token,
				    "Too many attempts. Please wait and try again."),
			drogon::k429TooManyRequests);
	}

	auto user = co_await tgweb::dao::accounts::findByUsername(db, username);

	bool ok = user && user->isActive &&
		  tgweb::auth::verifyPassword(user->passwordHash, password);

	if (!ok) {
		limiter().recordFailure(rlKey);
		co_await tgweb::dao::audit::log(db, std::nullopt, "login_fail",
						ip, username);
		std::string token = tgweb::auth::csrf::ensure(s);
		co_return htmlResponse(
			renderLogin(token, "Invalid username or password."),
			drogon::k401Unauthorized);
	}

	limiter().reset(rlKey);
	tgweb::auth::session::login(s, user->id, user->username, user->role);
	co_await tgweb::dao::audit::log(db, user->id, "login_ok", ip, user->username);

	co_return redirect("/");
}

drogon::Task<drogon::HttpResponsePtr>
AuthController::postLogout(drogon::HttpRequestPtr req)
{
	const auto &s = req->session();
	auto db = drogon::app().getDbClient("app");

	if (!tgweb::auth::csrf::check(s, req->getParameter("csrf")))
		co_return htmlResponse("403 Forbidden\n", drogon::k403Forbidden);

	if (tgweb::auth::session::isLoggedIn(s)) {
		auto uid = s->getOptional<uint64_t>(tgweb::auth::session::kUid);
		std::string ip = req->getPeerAddr().toIp();
		co_await tgweb::dao::audit::log(db, uid, "logout", ip,
			s->getOptional<std::string>(
				tgweb::auth::session::kUsername).value_or(""));
	}

	s->clear();
	co_return redirect("/login");
}

} /* namespace tgweb::controllers */
