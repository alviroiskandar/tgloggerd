// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include "DiscordForwarder.hpp"

#include "DB.hpp"

#include <chrono>
#include <ctime>
#include <exception>
#include <optional>
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

/* First line of `s`, trimmed to at most n bytes (UTF-8 safe). */
std::string first_line(const std::string &s, size_t n)
{
	std::string t = s.substr(0, s.find('\n'));
	return utf8_truncate(t, n);
}

/* Whether a stored file should render as an inline Discord image embed. */
bool is_image(const std::string &file_type, const std::string &ext)
{
	if (file_type == "photo")
		return true;
	return ext == "jpg" || ext == "jpeg" || ext == "png" ||
	       ext == "gif" || ext == "webp";
}

int64_t now_epoch(void)
{
	return (int64_t)std::time(nullptr);
}

} /* namespace */

DiscordForwarder::DiscordForwarder(DB *db, log_hd_t *l, size_t threads,
				   size_t queue_cap, int refresh_secs,
				   std::string public_url,
				   const std::string &web_app_key)
	: db_(db), l_(l), refresh_secs_(refresh_secs < 1 ? 1 : refresh_secs),
	  public_url_(std::move(public_url)), pool_(threads, queue_cap, l)
{
	DiscordClient::global_init();
	if (!public_url_.empty() && public_url_.back() == '/')
		public_url_.pop_back();
	if (public_url_.empty()) {
		pr_warn(l_, "discord: TG_DISCORD_PUBLIC_URL unset; avatars and "
			    "media links are disabled");
	} else if (!token_.init(web_app_key)) {
		pr_warn(l_, "discord: WEB_APP_KEY unusable; avatars and media "
			    "links are disabled");
	}
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

std::vector<std::string> DiscordForwarder::webhooks_for(int64_t chat_id)
{
	std::lock_guard<std::mutex> lk(cache_mtx_);
	auto it = cache_.find(chat_id);
	if (it == cache_.end())
		return {};
	return it->second;
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

std::string DiscordForwarder::media_url(uint64_t files_id) const
{
	if (public_url_.empty() || !token_.ready())
		return std::string();
	std::string tok = token_.encrypt(files_id);
	if (tok.empty())
		return std::string();
	return public_url_ + "/files/" + tok;
}

DiscordForwarder::Sender
DiscordForwarder::resolve_sender(int64_t chat_id, int64_t sender_id,
				 int64_t sender_chat_id,
				 const std::string &known_name)
{
	Sender s;
	try {
		if (sender_id != 0) {
			s.name = known_name;
			auto pid = db_->getUserPhotoFileId(sender_id);
			if (pid)
				s.avatar_url = media_url(*pid);
		} else {
			int64_t cid = sender_chat_id != 0 ? sender_chat_id : chat_id;
			ChatPhoto cp = db_->getGroupPhoto(cid);
			s.name = known_name.empty() ? cp.title : known_name;
			if (cp.photo_file_id)
				s.avatar_url = media_url(*cp.photo_file_id);
		}
	} catch (const std::exception &e) {
		pr_warn(l_, "discord: sender lookup failed: %s", e.what());
		s.name = known_name;
	}
	if (s.name.empty())
		s.name = "Telegram";
	s.name = utf8_truncate(s.name, 80);
	return s;
}

std::string DiscordForwarder::quote_prefix(const ForwardMessage &fm)
{
	if (fm.reply_to_msg_id == 0)
		return std::string();
	int64_t qchat = fm.reply_to_chat_id != 0 ? fm.reply_to_chat_id : fm.chat_id;

	std::optional<QuotedMessage> q;
	try {
		q = db_->getQuotedMessage(qchat, fm.reply_to_msg_id);
	} catch (const std::exception &e) {
		pr_warn(l_, "discord: reply lookup failed: %s", e.what());
		return std::string();
	}
	if (!q)
		return std::string();

	std::string qt = first_line(q->text, 120);
	std::string who = q->sender_name.empty()
		? std::string() : ("**" + q->sender_name + "**: ");
	if (who.empty() && qt.empty())
		return std::string();
	return "> " + who + qt + "\n";
}

std::string DiscordForwarder::build_payload(const Sender &s,
					    const std::string &content,
					    const std::string &embed) const
{
	std::string p = "{\"username\":\"" + json_escape(s.name) + "\"";
	if (!s.avatar_url.empty())
		p += ",\"avatar_url\":\"" + json_escape(s.avatar_url) + "\"";
	p += ",\"content\":\"" + json_escape(content) + "\"";
	p += ",\"allowed_mentions\":{\"parse\":[]}";
	if (!embed.empty())
		p += "," + embed;
	p += "}";
	return p;
}

void DiscordForwarder::post_and_record(const std::vector<std::string> &urls,
				       const std::string &payload,
				       int64_t chat_id, int64_t message_id,
				       const char *kind)
{
	for (const auto &url : urls) {
		DiscordResponse r = client_.post_json(url, payload);
		if (!r.ok()) {
			std::string detail = r.status ? r.body.substr(0, 200)
						      : r.error;
			pr_warn(l_, "discord: webhook POST failed (status=%ld): %s",
				r.status, detail.c_str());
			continue;
		}
		/* Remember the Discord message so a later Telegram edit or delete
		 * can find it (the content is re-derived, not stored). */
		if (!r.message_id.empty()) {
			try {
				db_->recordSentMessage(chat_id, message_id, url,
						       r.message_id, kind);
			} catch (const std::exception &e) {
				pr_warn(l_, "discord: record sent-message failed: %s",
					e.what());
			}
		}
	}
}

void DiscordForwarder::forward(const ForwardMessage &fm)
{
	std::vector<std::string> urls = webhooks_for(fm.chat_id);
	if (urls.empty())
		return;

	if (fm.has_file) {
		/* Remember this live media message; its file forwards once
		 * stored (on_media_stored). Sweep stale entries opportunistically. */
		std::lock_guard<std::mutex> lk(media_mtx_);
		sweep_pending_locked(now_epoch());
		pending_media_[{ fm.chat_id, fm.message_id }] = PendingMedia{
			now_epoch() + media_ttl_, fm.sender_id,
			fm.sender_chat_id, fm.sender_name };
	}

	/* A caption/quote (if any) is sent now; the media (if any) follows. */
	pool_.post([this, fm, urls] { do_text_forward(fm, urls); });
}

void DiscordForwarder::do_text_forward(ForwardMessage fm,
				       std::vector<std::string> urls)
{
	std::string content = quote_prefix(fm);
	content += fm.text;

	/* Media with no caption/quote: skip -- the image will follow. A plain
	 * text/service message with no text: nothing to send either. */
	if (content.empty())
		return;
	content = utf8_truncate(content, 2000);

	Sender s = resolve_sender(fm.chat_id, fm.sender_id, fm.sender_chat_id,
				  fm.sender_name);
	pr_info(l_, "discord: forwarding chat_id=%lld (%s) to %zu webhook(s): %.60s",
		(long long)fm.chat_id, fm.kind.empty() ? "text" : fm.kind.c_str(),
		urls.size(), content.c_str());
	post_and_record(urls, build_payload(s, content, std::string()),
			fm.chat_id, fm.message_id, "text");
}

void DiscordForwarder::forward_edit(const ForwardMessage &fm)
{
	if (webhooks_for(fm.chat_id).empty())
		return;
	pool_.post([this, fm] { do_edit_forward(fm); });
}

void DiscordForwarder::do_edit_forward(ForwardMessage fm)
{
	std::vector<SentMessage> sent;
	try {
		sent = db_->getSentMessages(fm.chat_id, fm.message_id, "text");
	} catch (const std::exception &e) {
		pr_warn(l_, "discord: edit lookup failed: %s", e.what());
		return;
	}
	if (sent.empty())
		return; /* nothing forwarded, or expired, or media-only */

	std::string content = quote_prefix(fm);
	content += fm.text;
	if (content.empty())
		return; /* edited to empty -> leave the Discord message as-is */
	content = utf8_truncate(content, 2000);

	/* A webhook message edit changes only content (not username/avatar). */
	std::string payload = "{\"content\":\"" + json_escape(content) +
			      "\",\"allowed_mentions\":{\"parse\":[]}}";

	pr_info(l_, "discord: editing chat_id=%lld msg_id=%lld (%zu message(s))",
		(long long)fm.chat_id, (long long)fm.message_id, sent.size());
	for (const auto &s : sent) {
		DiscordResponse r = client_.patch_json(s.webhook_url,
						       s.discord_message_id, payload);
		if (!r.ok()) {
			std::string detail = r.status ? r.body.substr(0, 200)
						      : r.error;
			pr_warn(l_, "discord: edit PATCH failed (status=%ld): %s",
				r.status, detail.c_str());
		}
	}
}

void DiscordForwarder::forward_delete(int64_t chat_id, int64_t message_id)
{
	if (webhooks_for(chat_id).empty())
		return;
	pool_.post([this, chat_id, message_id] {
		do_delete_forward(chat_id, message_id);
	});
}

void DiscordForwarder::do_delete_forward(int64_t chat_id, int64_t message_id)
{
	std::vector<SentMessage> sent;
	try {
		sent = db_->getSentMessages(chat_id, message_id, nullptr);
	} catch (const std::exception &e) {
		pr_warn(l_, "discord: delete lookup failed: %s", e.what());
		return;
	}
	if (sent.empty())
		return; /* nothing forwarded here */

	/*
	 * Re-derive what each part originally showed from the (still-present)
	 * source message row, so the tombstone keeps the message rather than
	 * blanking it. The content is not stored on the tracking row; it is
	 * rebuilt exactly as the forward path built it:
	 *   text  part = reply quote + message text
	 *   media part = the media link (empty for an image, whose embed stays)
	 */
	std::string text_content, media_content;
	try {
		auto mf = db_->getMessageForward(chat_id, message_id);
		if (mf) {
			ForwardMessage q{};
			q.chat_id = chat_id;
			q.reply_to_chat_id = mf->reply_to_chat_id;
			q.reply_to_msg_id = mf->reply_to_msg_id;
			text_content = quote_prefix(q) + mf->text;

			if (mf->file_id) {
				auto fi = db_->getFileInfo(*mf->file_id);
				if (fi && fi->on_disk &&
				    !is_image(fi->file_type, fi->ext))
					media_content = media_url(*mf->file_id);
			}
		}
	} catch (const std::exception &e) {
		pr_warn(l_, "discord: delete re-render failed: %s", e.what());
	}

	pr_info(l_, "discord: tombstoning deleted chat_id=%lld msg_id=%lld "
		"(%zu message(s))", (long long)chat_id, (long long)message_id,
		sent.size());
	for (const auto &s : sent) {
		const std::string &orig = s.kind == "media" ? media_content
							    : text_content;
		/* Prepend "(Deleted)" to the original content, then re-fit it into
		 * Discord's 2000-char limit (the prefix is kept, the tail trimmed). */
		std::string content = utf8_truncate("(Deleted)\n\n" + orig, 2000);
		std::string payload = "{\"content\":\"" + json_escape(content) +
				      "\",\"allowed_mentions\":{\"parse\":[]}}";
		DiscordResponse r = client_.patch_json(s.webhook_url,
						       s.discord_message_id, payload);
		if (!r.ok()) {
			std::string detail = r.status ? r.body.substr(0, 200)
						      : r.error;
			pr_warn(l_, "discord: delete PATCH failed (status=%ld): %s",
				r.status, detail.c_str());
		}
	}
	/* Deletion is terminal: drop the tracking rows so nothing edits or
	 * re-tombstones them later. */
	try {
		db_->deleteSentMessages(chat_id, message_id);
	} catch (const std::exception &e) {
		pr_warn(l_, "discord: delete tracking rows failed: %s", e.what());
	}
}

void DiscordForwarder::on_media_stored(int64_t chat_id, int64_t message_id,
				       uint64_t files_id)
{
	PendingMedia pm;
	{
		std::lock_guard<std::mutex> lk(media_mtx_);
		auto it = pending_media_.find({ chat_id, message_id });
		if (it == pending_media_.end())
			return; /* not a live message (e.g. backfill) -> ignore */
		pm = it->second;
		pending_media_.erase(it);
	}

	std::vector<std::string> urls = webhooks_for(chat_id);
	if (urls.empty())
		return;

	pool_.post([this, chat_id, message_id, pm, files_id, urls] {
		do_media_forward(chat_id, message_id, pm, files_id, urls);
	});
}

void DiscordForwarder::do_media_forward(int64_t chat_id, int64_t message_id,
					PendingMedia pm, uint64_t files_id,
					std::vector<std::string> urls)
{
	std::optional<FileInfo> fi;
	try {
		fi = db_->getFileInfo(files_id);
	} catch (const std::exception &e) {
		pr_warn(l_, "discord: file lookup failed: %s", e.what());
		return;
	}
	if (!fi || !fi->on_disk)
		return; /* no servable bytes (too large / metadata only) */

	std::string url = media_url(files_id);
	if (url.empty())
		return; /* no public URL configured */

	Sender s = resolve_sender(chat_id, pm.sender_id, pm.sender_chat_id,
				  pm.sender_name);

	std::string content, embed;
	if (is_image(fi->file_type, fi->ext))
		embed = "\"embeds\":[{\"image\":{\"url\":\"" +
			json_escape(url) + "\"}}]";
	else
		content = url; /* video auto-embeds; documents render as a link */

	pr_info(l_, "discord: forwarding media chat_id=%lld (%s) to %zu webhook(s)",
		(long long)chat_id, fi->file_type.c_str(), urls.size());
	post_and_record(urls, build_payload(s, content, embed), chat_id,
			message_id, "media");
}

void DiscordForwarder::sweep_pending_locked(int64_t now)
{
	for (auto it = pending_media_.begin(); it != pending_media_.end();) {
		if (it->second.deadline <= now)
			it = pending_media_.erase(it);
		else
			++it;
	}
}

} /* namespace tgloggerd */
