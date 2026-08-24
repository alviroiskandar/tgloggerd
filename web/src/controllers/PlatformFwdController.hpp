// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef TGLOGGERD_WEB_CONTROLLERS_PLATFORMFWDCONTROLLER_HPP
#define TGLOGGERD_WEB_CONTROLLERS_PLATFORMFWDCONTROLLER_HPP

#include <drogon/HttpController.h>

namespace tgweb::controllers {

/*
 * The index of cross-platform message forwarding: /platform-fwd.
 *
 * Each forwarding DIRECTION is its own page below this one, named
 * <source>-<destination> to match the table naming convention (see
 * web/docs/db-naming.md):
 *
 *   /platform-fwd/telegram-discord   TelegramDiscordController
 *   /platform-fwd/discord-telegram   DiscordTelegramController
 *
 * This page exists so the nav has one entry rather than one per direction, and
 * so adding a platform means adding a card here instead of another top-level
 * menu item. It only summarises; all configuration happens on the direction
 * pages.
 */
class PlatformFwdController
	: public drogon::HttpController<PlatformFwdController> {
public:
	METHOD_LIST_BEGIN
	ADD_METHOD_TO(PlatformFwdController::index, "/platform-fwd", drogon::Get,
		      "tgweb::auth::AuthFilter", "tgweb::auth::AdminFilter");
	METHOD_LIST_END

	drogon::Task<drogon::HttpResponsePtr> index(drogon::HttpRequestPtr req);
};

} /* namespace tgweb::controllers */

#endif /* TGLOGGERD_WEB_CONTROLLERS_PLATFORMFWDCONTROLLER_HPP */
