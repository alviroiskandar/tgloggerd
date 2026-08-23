// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef TGLOGGERD_WEB_CONTROLLERS_ROUTESCONTROLLER_HPP
#define TGLOGGERD_WEB_CONTROLLERS_ROUTESCONTROLLER_HPP

#include <drogon/HttpController.h>

namespace tgweb::controllers {

/*
 * Admin-only management of the Discord -> Telegram forwarding routes
 * (discord_telegram_routes, joined to telegram_bots), which discordd reads.
 * The mirror image of DiscordController, which manages the opposite direction.
 *
 * GET /routes renders the page; the save/delete POSTs are CSRF-protected;
 * /routes/chats is the select2 Telegram chat picker, reusing dao::discord's
 * search so both pages resolve chats identically. Every route requires an
 * authenticated admin (AuthFilter for the epoch-checked session, then
 * AdminFilter for the role) -- these rows hold bot credentials.
 */
class RoutesController : public drogon::HttpController<RoutesController> {
public:
	METHOD_LIST_BEGIN
	ADD_METHOD_TO(RoutesController::page, "/routes", drogon::Get,
		      "tgweb::auth::AuthFilter", "tgweb::auth::AdminFilter");
	ADD_METHOD_TO(RoutesController::save, "/routes/save", drogon::Post,
		      "tgweb::auth::AuthFilter", "tgweb::auth::AdminFilter");
	ADD_METHOD_TO(RoutesController::remove, "/routes/delete", drogon::Post,
		      "tgweb::auth::AuthFilter", "tgweb::auth::AdminFilter");
	ADD_METHOD_TO(RoutesController::chats, "/routes/chats", drogon::Get,
		      "tgweb::auth::AuthFilter", "tgweb::auth::AdminFilter");
	METHOD_LIST_END

	drogon::Task<drogon::HttpResponsePtr> page(drogon::HttpRequestPtr req);
	drogon::Task<drogon::HttpResponsePtr> save(drogon::HttpRequestPtr req);
	drogon::Task<drogon::HttpResponsePtr> remove(drogon::HttpRequestPtr req);
	drogon::Task<drogon::HttpResponsePtr> chats(drogon::HttpRequestPtr req);
};

} /* namespace tgweb::controllers */

#endif /* TGLOGGERD_WEB_CONTROLLERS_ROUTESCONTROLLER_HPP */
