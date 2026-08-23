// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef TGLOGGERD__DISCORD_FORWARDER_HPP
#define TGLOGGERD__DISCORD_FORWARDER_HPP

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "helpers/log.h"
#include "DiscordClient.hpp"
#include "FileToken.hpp"
#include "ThreadPool.hpp"
#include "TDLib.hpp" /* ForwardMessage */

namespace tgloggerd {

class DB;

/*
 * Mirrors live Telegram messages to Discord channels via incoming webhooks.
 *
 * forward() is called on the TDLib event thread and stays cheap: it looks the
 * chat up in an in-memory cache and hands the work to its own thread pool. Pool
 * tasks do the DB lookups (reply quote, sender avatar) and the HTTP POST, so the
 * event loop never blocks. Media is deferred: a live media message is recorded,
 * and forwarded (as a Discord embed/link) once its file finishes downloading
 * (on_media_stored, driven from the file pipeline) -- backfilled files, absent
 * from the pending set, are ignored. Avatar/media URLs are the web's public
 * /files/<token> links, minted with the shared WEB_APP_KEY.
 */
class DiscordForwarder {
public:
	DiscordForwarder(DB *db, log_hd_t *l, size_t threads, size_t queue_cap,
			 int refresh_secs, std::string public_url,
			 const std::string &web_app_key);
	~DiscordForwarder(void);

	DiscordForwarder(const DiscordForwarder &) = delete;
	DiscordForwarder &operator=(const DiscordForwarder &) = delete;

	void start(void);
	void stop(void);

	/* Forward one live message. Called on the TDLib thread; returns fast. */
	void forward(const ForwardMessage &fm);

	/* A live message was edited: update its forwarded Discord message(s). */
	void forward_edit(const ForwardMessage &fm);

	/* A live message was deleted (for everyone): tombstone its forwarded
	 * Discord message(s) with a "(Deleted)" prefix instead of removing them. */
	void forward_delete(int64_t chat_id, int64_t message_id);

	/*
	 * A media file finished downloading/linking. If it belongs to a live
	 * message we recorded, forward it. Called from the file pipeline (files_
	 * worker), so it may run concurrently with forward().
	 */
	void on_media_stored(int64_t chat_id, int64_t message_id,
			     uint64_t files_id);

private:
	struct Sender {
		std::string name;
		std::string avatar_url;
	};
	struct PendingMedia {
		int64_t     deadline;
		int64_t     sender_id;
		int64_t     sender_chat_id;
		std::string sender_name;
		int64_t     reply_to_chat_id; /* the message this media replies to, */
		int64_t     reply_to_msg_id;  /* so its preview is posted with the media */
		bool        has_caption;      /* if so, do_text_forward showed the preview */
	};
	/* A webhook's guild/channel, to build a message jump link (from a
	 * one-time GET of the webhook, cached in webhook_info_). */
	struct WebhookInfo {
		std::string guild_id;
		std::string channel_id;
		bool ok(void) const {
			return !guild_id.empty() && !channel_id.empty();
		}
	};
	/* The replied-to message rendered as a reply embed (author + snippet). */
	struct ReplyInfo {
		Sender      sender;         /* replied message's author */
		std::string snippet;        /* first line of the replied text */
		int64_t     chat_id = 0;    /* replied message (chat_id, message_id), */
		int64_t     message_id = 0; /* to resolve its Discord jump link */
		bool        ok = false;
	};

	std::vector<std::string> webhooks_for(int64_t chat_id);
	/*
	 * True when `sender_id` is one of the bots discordd forwards Discord
	 * messages into Telegram with. Such a message originated on Discord,
	 * so mirroring it back would show the sender their own message twice.
	 */
	bool is_bridge_bot(int64_t sender_id);
	void reload(void);
	void refresh_loop(void);

	Sender resolve_sender(int64_t chat_id, int64_t sender_id,
			      int64_t sender_chat_id,
			      const std::string &known_name);
	std::string media_url(uint64_t files_id) const;
	/* Resolve the reply (if any) into an embed's author + snippet. */
	ReplyInfo resolve_reply(const ForwardMessage &fm);
	/* Build the reply embed JSON fragment; jump_url makes the author clickable. */
	std::string reply_embed(const ReplyInfo &ri,
				const std::string &jump_url) const;
	/* Discord jump link to the replied message in `webhook_url`'s channel, or
	 * "" if it wasn't forwarded there / the webhook's guild is unknown. */
	std::string reply_jump_url(const std::string &webhook_url,
				   int64_t reply_chat_id, int64_t reply_msg_id);
	/* Guild/channel of a webhook (cached; one GET per webhook). */
	WebhookInfo webhook_info(const std::string &webhook_url);
	/* Post the replied-message preview as its own (untracked) message to one
	 * webhook, so a following post renders below it. */
	void post_reply_preview(const std::string &webhook_url, const Sender &s,
				const ReplyInfo &ri);
	std::string build_payload(const Sender &s, const std::string &content,
				  const std::string &embed) const;
	/* POST one payload to a webhook and record the created message id (so a
	 * later edit/delete can find it; the content is re-derived, not stored). */
	void post_one_and_record(const std::string &url, const std::string &payload,
				 int64_t chat_id, int64_t message_id,
				 const char *kind);

	void do_text_forward(ForwardMessage fm, std::vector<std::string> urls);
	void do_media_forward(int64_t chat_id, int64_t message_id, PendingMedia pm,
			      uint64_t files_id, std::vector<std::string> urls);
	void do_edit_forward(ForwardMessage fm);
	void do_delete_forward(int64_t chat_id, int64_t message_id);
	void sweep_pending_locked(int64_t now);

	DB		*db_;
	log_hd_t	*l_;
	int		refresh_secs_;
	std::string	public_url_;   /* e.g. https://tgd.gnuweeb.org (no slash) */
	int64_t		media_ttl_ = 120; /* seconds to wait for a media file */

	DiscordClient	client_;
	FileToken	token_;
	ThreadPool	pool_;

	std::mutex	cache_mtx_;
	std::unordered_map<int64_t, std::vector<std::string>> cache_;
	/* Telegram user ids of discordd's forwarding bots; see is_bridge_bot.
	 * Reloaded with the webhook cache, under cache_mtx_. */
	std::vector<int64_t> bridge_bots_;

	std::mutex	webhook_info_mtx_;
	std::unordered_map<std::string, WebhookInfo> webhook_info_;

	std::mutex	media_mtx_;
	std::map<std::pair<int64_t, int64_t>, PendingMedia> pending_media_;

	std::thread			refresh_thr_;
	std::mutex			wake_mtx_;
	std::condition_variable		wake_cv_;
	std::atomic<bool>		stop_{false};
	bool				started_ = false;
};

} /* namespace tgloggerd */

#endif /* #ifndef TGLOGGERD__DISCORD_FORWARDER_HPP */
