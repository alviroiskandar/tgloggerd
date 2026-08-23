// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include "controllers/DiscordTelegramController.hpp"

#include "auth/Csrf.hpp"
#include "controllers/Common.hpp"
#include "dao/Discord.hpp" /* resolveChat + searchChats, shared with the
				 reverse direction's page */
#include "dao/Routes.hpp"
#include "views/Render.hpp"

#include <cstdlib>
#include <exception>
#include <string>

namespace tgweb::controllers {

namespace {

/* A JSON body response with the given status. */
drogon::HttpResponsePtr jsonResp(const nlohmann::json &j,
				 drogon::HttpStatusCode code = drogon::k200OK)
{
	auto resp = drogon::HttpResponse::newHttpResponse();
	resp->setStatusCode(code);
	resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
	resp->setBody(j.dump());
	return resp;
}

drogon::HttpResponsePtr jsonError(const std::string &msg,
				  drogon::HttpStatusCode code = drogon::k400BadRequest)
{
	return jsonResp({ {"ok", false}, {"error", msg} }, code);
}

/*
 * A Discord snowflake, or 0 when the input is not one. Rejects anything
 * non-numeric rather than letting strtoull silently return 0 for "abc": the
 * operator pastes this by hand, so a typo should say so.
 */
uint64_t parseSnowflake(const std::string &s)
{
	if (s.empty() || s.size() > 20)
		return 0;
	for (char c : s) {
		if (c < '0' || c > '9')
			return 0;
	}
	return std::strtoull(s.c_str(), nullptr, 10);
}

/*
 * A Telegram bot token, shaped "<bot_user_id>:<secret>". Validated only for
 * shape -- whether it actually works is discordd's business, and it reports
 * that by logging the bot in. Rejecting obvious paste errors here saves a
 * confusing "route configured but nothing happens".
 */
bool looksLikeBotToken(const std::string &t)
{
	const size_t colon = t.find(':');
	if (colon == std::string::npos || colon == 0)
		return false;
	if (colon + 1 >= t.size() || t.size() > 255)
		return false;
	for (size_t i = 0; i < colon; i++) {
		if (t[i] < '0' || t[i] > '9')
			return false;
	}
	return true;
}

} /* namespace */

drogon::Task<drogon::HttpResponsePtr>
DiscordTelegramController::page(drogon::HttpRequestPtr req)
{
	auto db = drogon::app().getDbClient("ro");

	nlohmann::json data = pageBase(req);
	data["title"] = "Routes";
	data["routes"] = co_await dao::routes::list(db);
	data["bots"] = co_await dao::routes::listBots(db);

	co_return htmlPage(views::Render::page("fwd_discord_telegram.html", data));
}

drogon::Task<drogon::HttpResponsePtr>
DiscordTelegramController::save(drogon::HttpRequestPtr req)
{
	if (!auth::csrf::checkSession(req, req->getParameter("csrf")))
		co_return jsonError("Your session expired. Please reload.",
				    drogon::k403Forbidden);

	auto db = drogon::app().getDbClient("ro");

	const std::string idStr = req->getParameter("id");
	const std::string chanStr = req->getParameter("discord_channel_id");
	const std::string chatStr = req->getParameter("chat_id");
	const std::string botIdStr = req->getParameter("telegram_bot_id");
	const std::string botToken = req->getParameter("bot_token");
	const bool enabled = req->getParameter("enabled") == "1";

	const uint64_t channelId = parseSnowflake(chanStr);
	if (!channelId)
		co_return jsonError("Enter the Discord channel ID (the numeric id, "
				    "from Developer Mode -> Copy Channel ID).");
	if (chatStr.empty())
		co_return jsonError("Please choose a Telegram chat to forward to.");

	const int64_t chatId = std::strtoll(chatStr.c_str(), nullptr, 10);
	const uint64_t id = idStr.empty()
				    ? 0
				    : std::strtoull(idStr.c_str(), nullptr, 10);

	/*
	 * Everything below touches the database. Wrap it: the web user needs
	 * DML grants on these two tables that an existing deployment must add
	 * by hand (see docker/mysql/init/10-web-users.sh), and without this an
	 * "access denied" would escape as an HTML 500 into a JS caller that is
	 * expecting the {ok:false,error} envelope.
	 */
	try {
		/* The chat must be one the logger has actually seen. */
		auto chat = co_await dao::discord::resolveChat(db, chatId);
		if (!chat)
			co_return jsonError(
				"That chat is not in the log yet, so the bot "
				"cannot access it. Pick one it has seen.");

		/* Reject the duplicate before the UNIQUE key does. */
		if (co_await dao::routes::duplicateExists(db, channelId, chatId,
							  id))
			co_return jsonError(
				"That Discord channel already forwards to that "
				"Telegram chat.");

		uint64_t botId = botIdStr.empty()
					 ? 0
					 : std::strtoull(botIdStr.c_str(),
							 nullptr, 10);

		/*
		 * A token, when supplied, always wins: it is how both "use a
		 * new bot" and "replace this route's bot" are expressed. It is
		 * interned rather than stored per route, so several routes
		 * share one credential.
		 */
		if (!botToken.empty()) {
			if (!looksLikeBotToken(botToken))
				co_return jsonError(
					"That does not look like a Telegram bot "
					"token (123456789:AA...).");
			botId = co_await dao::routes::internBot(db, botToken);
			if (!botId)
				co_return jsonError(
					"Could not store the bot token.",
					drogon::k500InternalServerError);
		}

		if (!botId)
			co_return jsonError(
				"Choose an existing bot, or paste a bot token "
				"to add one.");

		if (!id)
			co_await dao::routes::create(db, channelId, chatId,
						     botId, enabled);
		else
			co_await dao::routes::update(db, id, channelId, chatId,
						     botId, enabled);
	} catch (const std::exception &e) {
		co_return jsonError(std::string("Database error: ") + e.what(),
				    drogon::k500InternalServerError);
	}

	co_return jsonResp({ {"ok", true} });
}

drogon::Task<drogon::HttpResponsePtr>
DiscordTelegramController::remove(drogon::HttpRequestPtr req)
{
	if (!auth::csrf::checkSession(req, req->getParameter("csrf")))
		co_return jsonError("Your session expired. Please reload.",
				    drogon::k403Forbidden);

	const std::string idStr = req->getParameter("id");
	if (idStr.empty())
		co_return jsonError("Missing route id.");
	const uint64_t id = std::strtoull(idStr.c_str(), nullptr, 10);

	auto db = drogon::app().getDbClient("ro");
	try {
		/*
		 * discord_telegram_sent_messages references the route under
		 * the default RESTRICT, so deleting a route that has forwarded
		 * anything fails at the database. Say why, and point at the
		 * alternative, rather than surfacing a foreign key error.
		 */
		const uint64_t n = co_await dao::routes::sentCount(db, id);
		if (n)
			co_return jsonError(
				"This route has forwarded " + std::to_string(n) +
				" message(s), which are still linked to it. "
				"Disable it instead of deleting it.");

		co_await dao::routes::remove(db, id);
	} catch (const std::exception &e) {
		co_return jsonError(std::string("Database error: ") + e.what(),
				    drogon::k500InternalServerError);
	}
	co_return jsonResp({ {"ok", true} });
}

drogon::Task<drogon::HttpResponsePtr>
DiscordTelegramController::chats(drogon::HttpRequestPtr req)
{
	auto db = drogon::app().getDbClient("ro");
	const std::string q = req->getParameter("q");
	const int limit = clampedIntParam(req, "limit", 20, 1, 50);

	/* Same source as /platform-fwd/telegram-discord/chats, so both pickers agree. */
	nlohmann::json results = co_await dao::discord::searchChats(db, q, limit);
	nlohmann::json out;
	out["results"] = nlohmann::json::array();
	for (auto &c : results) {
		out["results"].push_back({
			{"id", c["chat_id"]},
			{"text", c["text"]},
			{"type", c["type"]},
			{"title", c["title"]},
		});
	}
	co_return jsonResp(out);
}

} /* namespace tgweb::controllers */
