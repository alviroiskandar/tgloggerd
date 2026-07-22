// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef TGLOGGERD_WEB_CONTROLLERS_DISCORDCONTROLLER_HPP
#define TGLOGGERD_WEB_CONTROLLERS_DISCORDCONTROLLER_HPP

#include <drogon/HttpController.h>

namespace tgweb::controllers {

/*
 * Admin-only management of the Discord webhook integrations (rows in the
 * logger's discord_webhooks table). GET /integrations renders the page; the
 * save/delete/test POSTs are CSRF-protected; /integrations/chats is the select2
 * chat-picker search. All routes require an authenticated admin (AuthFilter for
 * the epoch-checked session, then AdminFilter for the role).
 */
class DiscordController : public drogon::HttpController<DiscordController> {
public:
	METHOD_LIST_BEGIN
	ADD_METHOD_TO(DiscordController::page, "/integrations", drogon::Get,
		      "tgweb::auth::AuthFilter", "tgweb::auth::AdminFilter");
	ADD_METHOD_TO(DiscordController::save, "/integrations/save", drogon::Post,
		      "tgweb::auth::AuthFilter", "tgweb::auth::AdminFilter");
	ADD_METHOD_TO(DiscordController::remove, "/integrations/delete",
		      drogon::Post, "tgweb::auth::AuthFilter",
		      "tgweb::auth::AdminFilter");
	ADD_METHOD_TO(DiscordController::test, "/integrations/test", drogon::Post,
		      "tgweb::auth::AuthFilter", "tgweb::auth::AdminFilter");
	ADD_METHOD_TO(DiscordController::chats, "/integrations/chats", drogon::Get,
		      "tgweb::auth::AuthFilter", "tgweb::auth::AdminFilter");
	METHOD_LIST_END

	drogon::Task<drogon::HttpResponsePtr> page(drogon::HttpRequestPtr req);
	drogon::Task<drogon::HttpResponsePtr> save(drogon::HttpRequestPtr req);
	drogon::Task<drogon::HttpResponsePtr> remove(drogon::HttpRequestPtr req);
	drogon::Task<drogon::HttpResponsePtr> test(drogon::HttpRequestPtr req);
	drogon::Task<drogon::HttpResponsePtr> chats(drogon::HttpRequestPtr req);
};

} /* namespace tgweb::controllers */

#endif /* TGLOGGERD_WEB_CONTROLLERS_DISCORDCONTROLLER_HPP */
