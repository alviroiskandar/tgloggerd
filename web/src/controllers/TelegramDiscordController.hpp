// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef TGLOGGERD_WEB_CONTROLLERS_TELEGRAMDISCORDCONTROLLER_HPP
#define TGLOGGERD_WEB_CONTROLLERS_TELEGRAMDISCORDCONTROLLER_HPP

#include <drogon/HttpController.h>

namespace tgweb::controllers {

/*
 * Admin-only management of the Discord webhook integrations (rows in the
 * logger's telegram_discord_webhooks table). GET /platform-fwd/telegram-discord renders the page; the
 * save/delete/test POSTs are CSRF-protected; /platform-fwd/telegram-discord/chats is the select2
 * chat-picker search. All routes require an authenticated admin (AuthFilter for
 * the epoch-checked session, then AdminFilter for the role).
 */
class TelegramDiscordController : public drogon::HttpController<TelegramDiscordController> {
public:
	METHOD_LIST_BEGIN
	ADD_METHOD_TO(TelegramDiscordController::page, "/platform-fwd/telegram-discord", drogon::Get,
		      "tgweb::auth::AuthFilter", "tgweb::auth::AdminFilter");
	ADD_METHOD_TO(TelegramDiscordController::save, "/platform-fwd/telegram-discord/save", drogon::Post,
		      "tgweb::auth::AuthFilter", "tgweb::auth::AdminFilter");
	ADD_METHOD_TO(TelegramDiscordController::remove, "/platform-fwd/telegram-discord/delete",
		      drogon::Post, "tgweb::auth::AuthFilter",
		      "tgweb::auth::AdminFilter");
	ADD_METHOD_TO(TelegramDiscordController::test, "/platform-fwd/telegram-discord/test", drogon::Post,
		      "tgweb::auth::AuthFilter", "tgweb::auth::AdminFilter");
	ADD_METHOD_TO(TelegramDiscordController::chats, "/platform-fwd/telegram-discord/chats", drogon::Get,
		      "tgweb::auth::AuthFilter", "tgweb::auth::AdminFilter");
	METHOD_LIST_END

	drogon::Task<drogon::HttpResponsePtr> page(drogon::HttpRequestPtr req);
	drogon::Task<drogon::HttpResponsePtr> save(drogon::HttpRequestPtr req);
	drogon::Task<drogon::HttpResponsePtr> remove(drogon::HttpRequestPtr req);
	drogon::Task<drogon::HttpResponsePtr> test(drogon::HttpRequestPtr req);
	drogon::Task<drogon::HttpResponsePtr> chats(drogon::HttpRequestPtr req);
};

} /* namespace tgweb::controllers */

#endif /* TGLOGGERD_WEB_CONTROLLERS_TELEGRAMDISCORDCONTROLLER_HPP */
