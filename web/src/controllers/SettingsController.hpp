// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef TGLOGGERD_WEB_CONTROLLERS_SETTINGSCONTROLLER_HPP
#define TGLOGGERD_WEB_CONTROLLERS_SETTINGSCONTROLLER_HPP

#include <drogon/HttpController.h>

namespace tgweb::controllers {

/*
 * Account settings. GET /settings renders the settings page (currently just the
 * Account section with a change-password form); POST /settings/password applies
 * a password change (CSRF-protected, re-verifies the current password). The
 * page is laid out as sections so more can be added later without reshaping it.
 */
class SettingsController : public drogon::HttpController<SettingsController> {
public:
	METHOD_LIST_BEGIN
	ADD_METHOD_TO(SettingsController::account, "/settings", drogon::Get,
		      "tgweb::auth::AuthFilter");
	ADD_METHOD_TO(SettingsController::changePassword, "/settings/password",
		      drogon::Post, "tgweb::auth::AuthFilter");
	METHOD_LIST_END

	drogon::Task<drogon::HttpResponsePtr> account(drogon::HttpRequestPtr req);
	drogon::Task<drogon::HttpResponsePtr>
	changePassword(drogon::HttpRequestPtr req);
};

} /* namespace tgweb::controllers */

#endif /* TGLOGGERD_WEB_CONTROLLERS_SETTINGSCONTROLLER_HPP */
