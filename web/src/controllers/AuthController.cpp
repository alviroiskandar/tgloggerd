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
#include "views/Render.hpp"

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

/* Render the login page through the base layout. The only dynamic values are
 * the per-session CSRF token (hex) and a fixed error string. */
std::string renderLogin(const std::string &csrf, const char *error)
{
	nlohmann::json data;
	data["title"] = "Sign in";
	data["csrf"] = csrf;
	if (error)
		data["error"] = error;
	return tgweb::views::Render::page("login.html", data);
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

/*
 * Render the login page and set a fresh signed pre-auth CSRF cookie whose token
 * is echoed in the form; the two are matched on POST.
 */
drogon::HttpResponsePtr loginPage(const char *error, drogon::HttpStatusCode code)
{
	std::string token = tgweb::auth::csrf::newLoginToken();
	auto resp = htmlResponse(renderLogin(token, error), code);
	tgweb::auth::csrf::setLoginCookie(resp, token);
	return resp;
}

} /* namespace */

drogon::Task<drogon::HttpResponsePtr>
AuthController::getLogin(drogon::HttpRequestPtr req)
{
	if (tgweb::auth::session::isLoggedIn(req))
		co_return redirect("/");

	co_return loginPage(nullptr, drogon::k200OK);
}

drogon::Task<drogon::HttpResponsePtr>
AuthController::postLogin(drogon::HttpRequestPtr req)
{
	auto db = drogon::app().getDbClient("app");

	std::string username = req->getParameter("username");
	std::string password = req->getParameter("password");
	std::string csrf = req->getParameter("csrf");
	std::string ip = req->getPeerAddr().toIp();

	/* CSRF first: a bad/missing token means the request is not trusted. */
	if (!tgweb::auth::csrf::checkLogin(req, csrf))
		co_return loginPage("Your session expired. Please try again.",
				    drogon::k403Forbidden);

	std::string rlKey = ip + "\n" + username;
	if (!limiter().allowed(rlKey)) {
		co_await tgweb::dao::audit::log(db, std::nullopt, "login_ratelimited",
						ip, username);
		co_return loginPage("Too many attempts. Please wait and try again.",
				    drogon::k429TooManyRequests);
	}

	auto user = co_await tgweb::dao::accounts::findByUsername(db, username);

	bool ok = user && user->isActive &&
		  tgweb::auth::verifyPassword(user->passwordHash, password);

	if (!ok) {
		limiter().recordFailure(rlKey);
		co_await tgweb::dao::audit::log(db, std::nullopt, "login_fail",
						ip, username);
		co_return loginPage("Invalid username or password.",
				    drogon::k401Unauthorized);
	}

	limiter().reset(rlKey);
	co_await tgweb::dao::audit::log(db, user->id, "login_ok", ip, user->username);

	/* Issue the signed session cookie on the redirect response. */
	auto resp = redirect("/");
	tgweb::auth::session::issue(resp, user->id, user->username, user->role,
				   user->epoch);
	co_return resp;
}

drogon::Task<drogon::HttpResponsePtr>
AuthController::postLogout(drogon::HttpRequestPtr req)
{
	auto db = drogon::app().getDbClient("app");

	if (!tgweb::auth::csrf::checkSession(req, req->getParameter("csrf")))
		co_return htmlResponse("403 Forbidden\n", drogon::k403Forbidden);

	if (auto s = tgweb::auth::session::current(req)) {
		std::string ip = req->getPeerAddr().toIp();
		co_await tgweb::dao::audit::log(db, s->uid, "logout", ip,
						s->username);
	}

	auto resp = redirect("/login");
	tgweb::auth::session::clear(resp);
	co_return resp;
}

} /* namespace tgweb::controllers */
