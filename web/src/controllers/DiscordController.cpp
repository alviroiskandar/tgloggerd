// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include "controllers/DiscordController.hpp"

#include "auth/Csrf.hpp"
#include "controllers/Common.hpp"
#include "dao/Discord.hpp"
#include "views/Render.hpp"

#include <drogon/HttpClient.h>

#include <cstdlib>
#include <optional>
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
 * Validate a Discord webhook URL and split it into host and path+query. Accepts
 * only https on a known Discord host with an /api/webhooks/ path, so the value
 * cannot be pointed at an arbitrary server.
 */
bool parseWebhook(const std::string &url, std::string &host, std::string &pathq)
{
	const std::string pfx = "https://";
	if (url.compare(0, pfx.size(), pfx) != 0)
		return false;
	size_t slash = url.find('/', pfx.size());
	if (slash == std::string::npos)
		return false;
	host = url.substr(pfx.size(), slash - pfx.size());
	pathq = url.substr(slash);

	if (host != "discord.com" && host != "discordapp.com" &&
	    host != "ptb.discord.com" && host != "canary.discord.com")
		return false;
	if (pathq.compare(0, 14, "/api/webhooks/") != 0)
		return false;
	/* Require id and token segments after /api/webhooks/. */
	size_t idStart = 14;
	size_t idSlash = pathq.find('/', idStart);
	if (idSlash == std::string::npos || idSlash == idStart)
		return false;
	if (idSlash + 1 >= pathq.size())
		return false;
	return true;
}

} /* namespace */

drogon::Task<drogon::HttpResponsePtr>
DiscordController::page(drogon::HttpRequestPtr req)
{
	auto db = drogon::app().getDbClient("ro");

	nlohmann::json data = pageBase(req);
	data["title"] = "Integrations";
	data["webhooks"] = co_await dao::discord::list(db);

	co_return htmlPage(views::Render::page("integrations.html", data));
}

drogon::Task<drogon::HttpResponsePtr>
DiscordController::save(drogon::HttpRequestPtr req)
{
	if (!auth::csrf::checkSession(req, req->getParameter("csrf")))
		co_return jsonError("Your session expired. Please reload.",
				    drogon::k403Forbidden);

	auto db = drogon::app().getDbClient("ro");

	std::string idStr   = req->getParameter("id");
	std::string chatStr = req->getParameter("chat_id");
	std::string url     = req->getParameter("webhook_url");
	bool enabled        = req->getParameter("enabled") == "1";

	if (chatStr.empty())
		co_return jsonError("Please choose a Telegram chat to forward.");
	if (url.empty())
		co_return jsonError("Please enter a Discord webhook URL.");

	std::string host, pathq;
	if (!parseWebhook(url, host, pathq))
		co_return jsonError("That does not look like a Discord webhook URL "
				    "(https://discord.com/api/webhooks/...).");

	int64_t chatId = std::strtoll(chatStr.c_str(), nullptr, 10);
	/* resolveChat still gates on the chat being in the log (accessible); its
	 * title is no longer stored -- the list resolves it live. */
	auto chat = co_await dao::discord::resolveChat(db, chatId);
	if (!chat)
		co_return jsonError("That chat is not in the log yet, so the bot "
				    "cannot access it. Pick one it has seen.");

	if (idStr.empty()) {
		co_await dao::discord::create(db, chat->chatId, chat->type, url,
					      enabled);
	} else {
		uint64_t id = std::strtoull(idStr.c_str(), nullptr, 10);
		co_await dao::discord::update(db, id, chat->chatId, chat->type, url,
					      enabled);
	}
	co_return jsonResp({ {"ok", true} });
}

drogon::Task<drogon::HttpResponsePtr>
DiscordController::remove(drogon::HttpRequestPtr req)
{
	if (!auth::csrf::checkSession(req, req->getParameter("csrf")))
		co_return jsonError("Your session expired. Please reload.",
				    drogon::k403Forbidden);

	std::string idStr = req->getParameter("id");
	if (idStr.empty())
		co_return jsonError("Missing integration id.");

	auto db = drogon::app().getDbClient("ro");
	co_await dao::discord::remove(db, std::strtoull(idStr.c_str(), nullptr, 10));
	co_return jsonResp({ {"ok", true} });
}

drogon::Task<drogon::HttpResponsePtr>
DiscordController::test(drogon::HttpRequestPtr req)
{
	if (!auth::csrf::checkSession(req, req->getParameter("csrf")))
		co_return jsonError("Your session expired. Please reload.",
				    drogon::k403Forbidden);

	std::string url = req->getParameter("webhook_url");
	std::string host, pathq;
	if (!parseWebhook(url, host, pathq))
		co_return jsonError("That does not look like a Discord webhook URL.");

	auto client = drogon::HttpClient::newHttpClient("https://" + host);
	auto out = drogon::HttpRequest::newHttpRequest();
	out->setMethod(drogon::Post);
	out->setPath(pathq);
	out->setContentTypeCode(drogon::CT_APPLICATION_JSON);
	out->setBody(nlohmann::json({
		{"username", "tgloggerd"},
		{"content", "\xE2\x9C\x85 Test message from the tgloggerd Discord "
			    "integration \xE2\x80\x94 the webhook works."}
	}).dump());

	try {
		auto resp = co_await client->sendRequestCoro(out, 10.0);
		int code = resp->getStatusCode();
		if (code >= 200 && code < 300)
			co_return jsonResp({ {"ok", true},
				{"message", "Sent \xE2\x80\x94 check the Discord channel."} });
		std::string body(resp->getBody());
		if (body.size() > 300)
			body.resize(300);
		co_return jsonResp({ {"ok", false},
			{"error", "Discord returned HTTP " + std::to_string(code) +
				  (body.empty() ? "" : ": " + body)} });
	} catch (const std::exception &e) {
		co_return jsonResp({ {"ok", false},
			{"error", std::string("Could not reach Discord: ") + e.what()} });
	}
}

drogon::Task<drogon::HttpResponsePtr>
DiscordController::chats(drogon::HttpRequestPtr req)
{
	auto db = drogon::app().getDbClient("ro");
	std::string q = req->getParameter("q");
	int limit = clampedIntParam(req, "limit", 20, 1, 50);

	nlohmann::json results = co_await dao::discord::searchChats(db, q, limit);
	/* select2 expects { results: [{id, text}, ...] }; keep type/title too. */
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
