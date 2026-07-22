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
	};

	std::vector<std::string> webhooks_for(int64_t chat_id);
	void reload(void);
	void refresh_loop(void);

	Sender resolve_sender(int64_t chat_id, int64_t sender_id,
			      int64_t sender_chat_id,
			      const std::string &known_name);
	std::string media_url(uint64_t files_id) const;
	std::string quote_prefix(const ForwardMessage &fm);
	std::string build_payload(const Sender &s, const std::string &content,
				  const std::string &embed) const;
	/* POST to each webhook and record the created message ids (for edits). */
	void post_and_record(const std::vector<std::string> &urls,
			     const std::string &payload, int64_t chat_id,
			     int64_t message_id, const char *kind);

	void do_text_forward(ForwardMessage fm, std::vector<std::string> urls);
	void do_media_forward(int64_t chat_id, int64_t message_id, PendingMedia pm,
			      uint64_t files_id, std::vector<std::string> urls);
	void do_edit_forward(ForwardMessage fm);
	void sweep_pending_locked(int64_t now);

	DB		*db_;
	log_hd_t	*l_;
	int		refresh_secs_;
	std::string	public_url_;   /* e.g. https://tgd.gnuweeb.org (no slash) */
	int64_t		media_ttl_ = 120; /* seconds to wait for a media file */
	int		sent_retention_days_ = 2; /* prune edit-tracking rows after */

	DiscordClient	client_;
	FileToken	token_;
	ThreadPool	pool_;

	std::mutex	cache_mtx_;
	std::unordered_map<int64_t, std::vector<std::string>> cache_;

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
