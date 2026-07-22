// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include "controllers/SettingsController.hpp"

#include "auth/Csrf.hpp"
#include "auth/Password.hpp"
#include "auth/Session.hpp"
#include "controllers/Common.hpp"
#include "dao/Accounts.hpp"
#include "dao/Audit.hpp"
#include "views/Render.hpp"

#include <cstddef>
#include <optional>
#include <string>

namespace tgweb::controllers {

namespace {

constexpr size_t kMinPasswordLen = 8;

/*
 * Render the settings page (Account section). `error`, when non-empty, is shown
 * inline in the change-password card; `changed` shows the success banner.
 */
drogon::HttpResponsePtr renderSettings(const drogon::HttpRequestPtr &req,
				       const std::string &error, bool changed)
{
	nlohmann::json data = pageBase(req);
	data["title"]   = "Settings";
	auto s = auth::session::current(req);
	data["acct_name"] = views::Render::esc(s ? s->username : std::string());
	data["role"]      = s ? s->role : std::string();
	data["error"]     = views::Render::esc(error);
	data["changed"]   = changed;
	return htmlPage(views::Render::page("settings.html", data));
}

} /* namespace */

drogon::Task<drogon::HttpResponsePtr>
SettingsController::account(drogon::HttpRequestPtr req)
{
	bool changed = req->getParameter("changed") == "1";
	co_return renderSettings(req, "", changed);
}

drogon::Task<drogon::HttpResponsePtr>
SettingsController::changePassword(drogon::HttpRequestPtr req)
{
	auto db = drogon::app().getDbClient("app");

	/* CSRF first: an untrusted POST changes nothing. */
	if (!auth::csrf::checkSession(req, req->getParameter("csrf")))
		co_return renderStatus(req, drogon::k403Forbidden, "Forbidden",
			"Your session expired. Please reload and try again.");

	auto s = auth::session::current(req);
	if (!s) /* AuthFilter guarantees a session; stay defensive anyway. */
		co_return renderStatus(req, drogon::k401Unauthorized,
			"Not signed in", "Please sign in and try again.");

	std::string current = req->getParameter("current_password");
	std::string next    = req->getParameter("new_password");
	std::string confirm = req->getParameter("confirm_password");
	std::string ip      = req->getPeerAddr().toIp();

	/* Re-fetch by the session's (signed) uid so the check uses live data. */
	auto user = co_await dao::accounts::findById(db, s->uid);
	if (!user || !user->isActive)
		co_return renderStatus(req, drogon::k401Unauthorized,
			"Account unavailable", "Your account is no longer active.");

	std::string err;
	if (!auth::verifyPassword(user->passwordHash, current))
		err = "Your current password is incorrect.";
	else if (next.size() < kMinPasswordLen)
		err = "The new password must be at least 8 characters.";
	else if (next != confirm)
		err = "The new passwords do not match.";
	else if (next == current)
		err = "The new password must be different from the current one.";

	if (!err.empty()) {
		co_await dao::audit::log(db, s->uid, "password_change_fail", ip,
					 s->username);
		co_return renderSettings(req, err, false);
	}

	std::string hash = auth::hashPassword(next);
	if (hash.empty())
		co_return renderSettings(req,
			"Could not process the new password. Please try again.",
			false);

	/* Storing the new hash bumps session_epoch, invalidating every cookie
	 * issued before now (sign out everywhere); re-issue this session's cookie
	 * with the new epoch so the caller stays signed in. */
	uint32_t epoch = co_await dao::accounts::setPassword(db, s->uid, hash);
	co_await dao::audit::log(db, s->uid, "password_change", ip, s->username);

	/* Post/redirect/get so a refresh does not resubmit the form. */
	auto resp = drogon::HttpResponse::newRedirectionResponse("/settings?changed=1");
	auth::session::issue(resp, s->uid, s->username, s->role, epoch);
	co_return resp;
}

} /* namespace tgweb::controllers */
