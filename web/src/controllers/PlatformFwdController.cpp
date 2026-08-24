// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include "controllers/PlatformFwdController.hpp"

#include "controllers/Common.hpp"
#include "views/Render.hpp"

#include <exception>

namespace tgweb::controllers {

drogon::Task<drogon::HttpResponsePtr>
PlatformFwdController::index(drogon::HttpRequestPtr req)
{
	auto db = drogon::app().getDbClient("ro");

	nlohmann::json data = pageBase(req);
	data["title"] = "Platform forwarding";

	/*
	 * Counts only, so the index stays a summary: how many directions are
	 * configured, and how many of those are actually live. The direction
	 * pages own everything else.
	 *
	 * Wrapped because a missing GRANT on the newer tables would otherwise
	 * take down the index as well as the page that needs it.
	 */
	uint64_t tdTotal = 0, tdEnabled = 0, dtTotal = 0, dtEnabled = 0;
	try {
		auto r = co_await db->execSqlCoro(
			"SELECT COUNT(1) AS n, "
			"COALESCE(SUM(enabled), 0) AS live "
			"FROM telegram_discord_webhooks");
		if (!r.empty()) {
			tdTotal = r[0]["n"].as<uint64_t>();
			tdEnabled = r[0]["live"].as<uint64_t>();
		}
	} catch (const std::exception &) {
		/* Leave the counts at zero; the direction page reports why. */
	}
	try {
		/*
		 * A route is live only when its bot is enabled too, which is
		 * why this is not a plain SUM(enabled) like the row above.
		 */
		auto r = co_await db->execSqlCoro(
			"SELECT COUNT(1) AS n, "
			"COALESCE(SUM(r.enabled AND b.enabled), 0) AS live "
			"FROM discord_telegram_routes r "
			"JOIN telegram_bots b ON b.id = r.telegram_bot_id");
		if (!r.empty()) {
			dtTotal = r[0]["n"].as<uint64_t>();
			dtEnabled = r[0]["live"].as<uint64_t>();
		}
	} catch (const std::exception &) {
	}

	nlohmann::json dirs = nlohmann::json::array();
	dirs.push_back({
		{"href", "/platform-fwd/telegram-discord"},
		{"title", "Telegram → Discord"},
		{"desc", "Mirror a Telegram group, channel or private chat into a "
			 "Discord channel through that channel's incoming webhook."},
		{"total", tdTotal},
		{"enabled", tdEnabled},
		{"noun", "integration"},
	});
	dirs.push_back({
		{"href", "/platform-fwd/discord-telegram"},
		{"title", "Discord → Telegram"},
		{"desc", "Forward a Discord channel into a Telegram group, sending "
			 "as a Telegram bot."},
		{"total", dtTotal},
		{"enabled", dtEnabled},
		{"noun", "route"},
	});
	data["directions"] = dirs;

	co_return htmlPage(views::Render::page("fwd_index.html", data));
}

} /* namespace tgweb::controllers */
