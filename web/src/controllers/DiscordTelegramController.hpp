// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef TGLOGGERD_WEB_CONTROLLERS_DISCORDTELEGRAMCONTROLLER_HPP
#define TGLOGGERD_WEB_CONTROLLERS_DISCORDTELEGRAMCONTROLLER_HPP

#include <drogon/HttpController.h>

namespace tgweb::controllers {

/*
 * Admin-only management of the Discord -> Telegram forwarding routes
 * (discord_telegram_routes, joined to telegram_bots), which discordd reads.
 * The mirror image of DiscordController, which manages the opposite direction.
 *
 * GET /platform-fwd/discord-telegram renders the page; the save/delete POSTs are CSRF-protected;
 * /platform-fwd/discord-telegram/chats is the select2 Telegram chat picker, reusing dao::discord's
 * search so both pages resolve chats identically. Every route requires an
 * authenticated admin (AuthFilter for the epoch-checked session, then
 * AdminFilter for the role) -- these rows hold bot credentials.
 */
class DiscordTelegramController : public drogon::HttpController<DiscordTelegramController> {
public:
	METHOD_LIST_BEGIN
	ADD_METHOD_TO(DiscordTelegramController::page, "/platform-fwd/discord-telegram", drogon::Get,
		      "tgweb::auth::AuthFilter", "tgweb::auth::AdminFilter");
	ADD_METHOD_TO(DiscordTelegramController::save, "/platform-fwd/discord-telegram/save", drogon::Post,
		      "tgweb::auth::AuthFilter", "tgweb::auth::AdminFilter");
	ADD_METHOD_TO(DiscordTelegramController::remove, "/platform-fwd/discord-telegram/delete", drogon::Post,
		      "tgweb::auth::AuthFilter", "tgweb::auth::AdminFilter");
	ADD_METHOD_TO(DiscordTelegramController::chats, "/platform-fwd/discord-telegram/chats", drogon::Get,
		      "tgweb::auth::AuthFilter", "tgweb::auth::AdminFilter");
	METHOD_LIST_END

	drogon::Task<drogon::HttpResponsePtr> page(drogon::HttpRequestPtr req);
	drogon::Task<drogon::HttpResponsePtr> save(drogon::HttpRequestPtr req);
	drogon::Task<drogon::HttpResponsePtr> remove(drogon::HttpRequestPtr req);
	drogon::Task<drogon::HttpResponsePtr> chats(drogon::HttpRequestPtr req);
};

} /* namespace tgweb::controllers */

#endif /* TGLOGGERD_WEB_CONTROLLERS_DISCORDTELEGRAMCONTROLLER_HPP */
