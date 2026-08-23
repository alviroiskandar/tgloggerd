// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef DISCORDD__TELEGRAM_SENDER_HPP
#define DISCORDD__TELEGRAM_SENDER_HPP

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace discordd {

using SenderLog = std::function<void(int /*level*/, const std::string &)>;

/*
 * Sends messages to Telegram as one or more BOTS, over TDLib.
 *
 * Each bot token gets its own TDLib client (td::ClientManager supports many)
 * and its own session directory, so several routes can deliver through
 * different bots to different chats. A single background thread pumps
 * ClientManager::receive() and dispatches replies to the waiting caller, so
 * the public methods are blocking and safe to call from any thread.
 *
 * Telegram message ids: TDLib exposes them shifted left by 20 bits. Everything
 * crossing this interface is a SERVER id (the shifted-down value that the rest
 * of the project stores); the shifting is handled internally.
 */
class TelegramSender {
public:
	TelegramSender(int32_t api_id, std::string api_hash,
		       std::string data_dir, SenderLog log);
	~TelegramSender(void);

	TelegramSender(const TelegramSender &) = delete;
	TelegramSender &operator=(const TelegramSender &) = delete;

	void start(void);
	void stop(void);

	/*
	 * Bring up a bot session and block until it is authorized. `key` is
	 * the caller's own handle for the bot (telegram_bots.id), used to
	 * address it in later calls. Returns the bot's Telegram user id, or 0
	 * on failure with `err` set. Calling it twice for the same key is a
	 * no-op that returns the known user id.
	 */
	int64_t addBot(uint64_t key, const std::string &token,
		       std::string *err);

	/*
	 * Send `text` to `chat_id`. When `reply_to_server_id` is non-zero the
	 * message is threaded as a reply to it. Returns the SERVER message id
	 * of the sent message, or 0 on failure.
	 *
	 * Note this waits for Telegram to confirm the send: TDLib first hands
	 * back a message with a provisional id and only later reports the
	 * final one, and the final id is what must be stored for replies and
	 * edits to resolve.
	 */
	int64_t sendText(uint64_t key, int64_t chat_id, const std::string &text,
			 int64_t reply_to_server_id, std::string *err);

	/* Replace the text of a message this sender previously sent. */
	bool editText(uint64_t key, int64_t chat_id, int64_t server_message_id,
		      const std::string &text, std::string *err);

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} /* namespace discordd */

#endif /* #ifndef DISCORDD__TELEGRAM_SENDER_HPP */
