// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include "DiscordForwarder.hpp"

#include "DB.hpp"

#include <chrono>
#include <exception>
#include <utility>

namespace tgloggerd {

namespace {

/* Truncate `s` to at most max_bytes without splitting a UTF-8 sequence. */
std::string utf8_truncate(const std::string &s, size_t max_bytes)
{
	if (s.size() <= max_bytes)
		return s;
	size_t cut = max_bytes;
	while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80)
		cut--;
	return s.substr(0, cut);
}

} /* namespace */

DiscordForwarder::DiscordForwarder(DB *db, log_hd_t *l, size_t threads,
				   size_t queue_cap, int refresh_secs)
	: db_(db), l_(l), refresh_secs_(refresh_secs < 1 ? 1 : refresh_secs),
	  pool_(threads, queue_cap, l)
{
	DiscordClient::global_init();
}

DiscordForwarder::~DiscordForwarder(void)
{
	stop();
}

void DiscordForwarder::start(void)
{
	if (started_)
		return;
	started_ = true;
	reload();
	refresh_thr_ = std::thread([this] { refresh_loop(); });
}

void DiscordForwarder::stop(void)
{
	if (!started_)
		return;
	{
		std::lock_guard<std::mutex> lk(wake_mtx_);
		stop_ = true;
	}
	wake_cv_.notify_all();
	if (refresh_thr_.joinable())
		refresh_thr_.join();
	pool_.shutdown();
	started_ = false;
}

void DiscordForwarder::reload(void)
{
	std::vector<DiscordWebhook> rows;
	try {
		rows = db_->loadDiscordWebhooks();
	} catch (const std::exception &e) {
		pr_error(l_, "discord: failed to load webhooks: %s", e.what());
		return;
	}

	std::unordered_map<int64_t, std::vector<std::string>> next;
	for (const auto &w : rows)
		next[w.chat_id].push_back(w.webhook_url);

	size_t n_hooks = rows.size();
	size_t n_chats = next.size();
	{
		std::lock_guard<std::mutex> lk(cache_mtx_);
		cache_.swap(next);
	}
	pr_info(l_, "discord: loaded %zu webhook(s) across %zu chat(s)",
		n_hooks, n_chats);
}

void DiscordForwarder::refresh_loop(void)
{
	std::unique_lock<std::mutex> lk(wake_mtx_);
	while (!stop_) {
		wake_cv_.wait_for(lk, std::chrono::seconds(refresh_secs_),
				  [this] { return stop_.load(); });
		if (stop_)
			break;
		lk.unlock();
		reload();
		lk.lock();
	}
}

void DiscordForwarder::forward(const ForwardMessage &fm)
{
	std::vector<std::string> urls;
	{
		std::lock_guard<std::mutex> lk(cache_mtx_);
		auto it = cache_.find(fm.chat_id);
		if (it == cache_.end())
			return;
		urls = it->second;
	}
	if (urls.empty())
		return;

	/* Compose the Discord content: the text/caption, else a media
	 * placeholder like "[photo]". Nothing to show -> skip. */
	std::string content = fm.text;
	if (content.empty()) {
		if (!fm.kind.empty() && fm.kind != "text")
			content = "[" + fm.kind + "]";
		else
			return;
	}
	content = utf8_truncate(content, 2000);

	std::string username = fm.sender_name.empty() ? "Telegram" : fm.sender_name;
	username = utf8_truncate(username, 80);

	pr_info(l_, "discord: forwarding chat_id=%lld (%s) to %zu webhook(s): %.60s",
		(long long)fm.chat_id, fm.kind.empty() ? "text" : fm.kind.c_str(),
		urls.size(), content.c_str());

	/* allowed_mentions parse:[] so forwarded Telegram text can never ping
	 * @everyone/roles/users on the Discord side. */
	std::string payload =
		"{\"username\":\"" + json_escape(username) +
		"\",\"content\":\"" + json_escape(content) +
		"\",\"allowed_mentions\":{\"parse\":[]}}";

	for (const auto &url : urls) {
		pool_.post([this, url, payload] {
			DiscordResponse r = client_.post_json(url, payload);
			if (!r.ok()) {
				std::string detail = r.status
					? r.body.substr(0, 200)
					: r.error;
				pr_warn(l_,
					"discord: webhook POST failed (status=%ld): %s",
					r.status, detail.c_str());
			}
		});
	}
}

} /* namespace tgloggerd */
