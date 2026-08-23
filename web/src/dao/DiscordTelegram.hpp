// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef TGLOGGERD_WEB_DAO_DISCORDTELEGRAM_HPP
#define TGLOGGERD_WEB_DAO_DISCORDTELEGRAM_HPP

#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>

/*
 * Discord -> Telegram forwarding routes (`discord_telegram_routes`, joined to
 * the sending bot's credentials in `telegram_bots`), in the logger schema. The
 * mirror image of dao::telegram_discord, which manages the opposite direction.
 * Serves /platform-fwd/discord-telegram. Like its sibling, everything here runs
 * on the "ro" DbClient, which is granted DML on just these two tables.
 *
 * SECRETS. telegram_bots.token is a Telegram bot token: whoever holds it can
 * read and send as that bot. It is therefore NEVER selected for display and
 * never leaves the server. The UI identifies a bot by its Telegram user id and
 * username, and the only way the token is written is by supplying a new one.
 * (This deliberately departs from dao::telegram_discord, which returns
 * webhook_url in full -- a webhook URL is at least scoped to one channel and
 * constrained by a host allowlist; a bot token is a whole account.)
 *
 * Strings placed on returned JSON are Render::esc()-escaped except where noted.
 */
namespace tgweb::dao::discord_telegram {

struct Route {
	uint64_t    id;
	uint64_t    discordChannelId;
	int64_t     telegramChatId;
	uint64_t    telegramBotId;
	bool        enabled;
};

/*
 * Every route, newest first, as an escaped JSON array for the admin UI. Each
 * entry carries the live Telegram chat title (resolved by joining the logged
 * schema) and a bot_label identifying the bot WITHOUT revealing its token.
 *
 * There is deliberately no Discord channel NAME: discordd only writes
 * discord_channels for channels that already have a route, and writes an empty
 * name when it does, so the column would always be blank. The channel is shown
 * by id, which is what the operator pastes in anyway.
 */
drogon::Task<nlohmann::json> list(drogon::orm::DbClientPtr db);

/* One route by id, or nullopt. Never includes the bot token. */
drogon::Task<std::optional<Route>> get(drogon::orm::DbClientPtr db,
				       uint64_t id);

/*
 * How many forwarded messages reference this route. discord_telegram_sent_
 * messages has an FK to discord_telegram_routes with the default RESTRICT, so
 * deleting a route that has ever forwarded anything fails at the database.
 * Checking first turns that into an explanation instead of a 500.
 */
drogon::Task<uint64_t> sentCount(drogon::orm::DbClientPtr db, uint64_t id);

/*
 * The bots available to route through, as [{id, label, connected}]. `label` is
 * the bot's Telegram identity (username, or its user id), never its token.
 */
drogon::Task<nlohmann::json> listBots(drogon::orm::DbClientPtr db);

/*
 * Intern a bot token and return its telegram_bots.id, creating the row on
 * first sight. Existing rows are matched on the token itself (it is UNIQUE),
 * so re-adding the same bot reuses it rather than duplicating it.
 *
 * bot_user_id is deliberately left at 0 here: only discordd can learn it, by
 * logging the bot in. Until it does, the loop guard in the Telegram -> Discord
 * forwarder cannot recognise this bot, so a freshly added route may echo once
 * before discordd picks it up.
 */
drogon::Task<uint64_t> internBot(drogon::orm::DbClientPtr db,
				 std::string token);

drogon::Task<uint64_t> create(drogon::orm::DbClientPtr db,
			      uint64_t discordChannelId, int64_t telegramChatId,
			      uint64_t telegramBotId, bool enabled);

drogon::Task<void> update(drogon::orm::DbClientPtr db, uint64_t id,
			  uint64_t discordChannelId, int64_t telegramChatId,
			  uint64_t telegramBotId, bool enabled);

drogon::Task<void> remove(drogon::orm::DbClientPtr db, uint64_t id);

/*
 * True when (channel, chat) is already routed by a DIFFERENT row. The table
 * has a UNIQUE key on the pair, so this turns a duplicate into a readable
 * message instead of a 1062 escaping as a 500.
 */
drogon::Task<bool> duplicateExists(drogon::orm::DbClientPtr db,
				   uint64_t discordChannelId,
				   int64_t telegramChatId, uint64_t excludeId);

} /* namespace tgweb::dao::discord_telegram */

#endif /* TGLOGGERD_WEB_DAO_DISCORDTELEGRAM_HPP */
