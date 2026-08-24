// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include "DiscordForwarder.hpp"

#include "DB.hpp"
#include "CompactId.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <exception>
#include <optional>
#include <utility>

namespace tgloggerd {

namespace {

/* Left-bar accent of the reply embed (a muted green, matching the reference). */
constexpr int kReplyColor = 0x2ecc71;

/* Value of a top-level string field in a small JSON body: "field":"<value>".
 * Returns "" if the field is absent or not a string (e.g. "field":null). */
std::string json_str_field(const std::string &body, const char *field)
{
	std::string key = std::string("\"") + field + "\":\"";
	size_t p = body.find(key);
	if (p == std::string::npos)
		return std::string();
	p += key.size();
	size_t e = body.find('"', p);
	if (e == std::string::npos)
		return std::string();
	return body.substr(p, e - p);
}

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

/* Discord caps a webhook username at 80 characters. */
constexpr size_t kMaxUsername = 80;

/*
 * The display name for a forwarded message's author:
 *
 *     First Last (cx:<b64 user id>:<b64 message id>)
 *
 * The suffix stamps the sender's user id and the message's id in reversible
 * base64 (see CompactId.hpp) so a reader -- or a tool -- can recover exactly
 * which Telegram user and message a forward came from. The ids are never
 * shortened; if the whole label would exceed Discord's 80-character cap, the
 * name is truncated instead (last name first, since it trims from the end),
 * leaving the identifier intact. `name` is the already-joined "First Last".
 */
std::string forwarded_author_name(const std::string &name, int64_t user_id,
				  int64_t message_id)
{
	std::string suffix = " (cx:" +
			     compactid::encode((uint64_t)user_id) + ":" +
			     compactid::encode((uint64_t)message_id) + ")";

	/*
	 * utf8_truncate counts bytes; Discord counts code points, and every code
	 * point is at least one byte, so an 80-byte budget never exceeds 80 code
	 * points -- a safe under-approximation, matching the rest of this file.
	 */
	if (name.size() + suffix.size() <= kMaxUsername)
		return name + suffix;
	size_t budget = suffix.size() >= kMaxUsername
				? 0
				: kMaxUsername - suffix.size();
	return utf8_truncate(name, budget) + suffix;
}

/*
 * Backslash-escape Discord markdown metacharacters so forwarded Telegram text
 * renders verbatim. Notably `<...>` is otherwise consumed as an autolink
 * (`<a@b.com>`, `<http://x>`) or a mention/emoji/timestamp token (`<@123>`),
 * which silently drops the angle brackets. Backticks/asterisks/underscores/
 * tildes/pipes are the inline-formatting markers.
 */
bool is_md_special(char c)
{
	switch (c) {
	case '\\': case '`': case '*': case '_':
	case '~': case '|': case '<': case '>':
		return true;
	default:
		return false;
	}
}

std::string discord_escape(const std::string &s)
{
	std::string o;
	o.reserve(s.size() + s.size() / 8 + 4);
	for (char c : s) {
		if (is_md_special(c))
			o += '\\';
		o += c;
	}
	return o;
}

/*
 * Render Telegram text + formatting entities as escaped Discord markdown.
 *
 * Entity offsets/lengths are UTF-16 code units, so we walk the UTF-8 text code
 * point by code point tracking the UTF-16 offset. At each offset we emit the
 * markers for entities closing/opening there; between them, ordinary text is
 * escaped, but the inside of a code/inline-code span is emitted verbatim (and
 * other entities nested in it are dropped, since Discord code is literal).
 * Blockquotes are rendered as a "> " prefix on each of their lines.
 */
std::string render_markdown(const std::string &text,
			    const std::vector<FmtEntity> &ents)
{
	if (ents.empty())
		return discord_escape(text);

	auto in_range = [](const std::vector<std::pair<int32_t, int32_t>> &rs,
			   int32_t u) {
		for (const auto &r : rs)
			if (u >= r.first && u < r.second)
				return true;
		return false;
	};

	std::vector<std::pair<int32_t, int32_t>> raw_ranges; /* code/pre */
	std::vector<std::pair<int32_t, int32_t>> bq_ranges;  /* block quote */
	for (const auto &e : ents) {
		if (e.type == FmtEntity::Type::Code ||
		    e.type == FmtEntity::Type::Pre)
			raw_ranges.push_back({ e.offset, e.offset + e.length });
		else if (e.type == FmtEntity::Type::BlockQuote)
			bq_ranges.push_back({ e.offset, e.offset + e.length });
	}

	/* Marker insertions. kind 0 = close, 1 = open; key orders same-position
	 * markers so spans nest (outer opens first / closes last). */
	struct Ev { int32_t at; int kind; int32_t key; std::string mark; };
	std::vector<Ev> evs;
	for (const auto &e : ents) {
		int32_t s = e.offset, en = e.offset + e.length;
		std::string open, close;
		switch (e.type) {
		case FmtEntity::Type::Bold:          open = close = "**"; break;
		case FmtEntity::Type::Italic:        open = close = "*";  break;
		case FmtEntity::Type::Underline:     open = close = "__"; break;
		case FmtEntity::Type::Strikethrough: open = close = "~~"; break;
		case FmtEntity::Type::Spoiler:       open = close = "||"; break;
		case FmtEntity::Type::Code:          open = close = "`";  break;
		case FmtEntity::Type::Pre:
			open = "```" + e.language + "\n";
			close = "\n```";
			break;
		default:
			continue; /* BlockQuote via bq_ranges; Other = plain */
		}
		/* A non-code span inside a code/pre range would print literally; drop it. */
		if (e.type != FmtEntity::Type::Code && e.type != FmtEntity::Type::Pre &&
		    (in_range(raw_ranges, s) || in_range(raw_ranges, en - 1)))
			continue;
		evs.push_back({ s,  1, -en, open });
		evs.push_back({ en, 0, -s,  close });
	}
	std::sort(evs.begin(), evs.end(), [](const Ev &a, const Ev &b) {
		if (a.at != b.at)     return a.at < b.at;
		if (a.kind != b.kind) return a.kind < b.kind; /* close before open */
		return a.key < b.key;
	});

	std::string out;
	out.reserve(text.size() + text.size() / 4 + 16);
	size_t ei = 0;
	int32_t u = 0;            /* current UTF-16 offset */
	bool line_start = true;
	for (size_t i = 0; i < text.size();) {
		if (line_start && in_range(bq_ranges, u))
			out += "> ";
		while (ei < evs.size() && evs[ei].at == u)
			out += evs[ei++].mark;

		unsigned char c0 = static_cast<unsigned char>(text[i]);
		int len = 1;
		uint32_t cp = c0;
		if (c0 >= 0xF0)      { len = 4; cp = c0 & 0x07; }
		else if (c0 >= 0xE0) { len = 3; cp = c0 & 0x0F; }
		else if (c0 >= 0xC0) { len = 2; cp = c0 & 0x1F; }
		if (i + (size_t)len > text.size())
			len = 1;
		for (int k = 1; k < len; k++)
			cp = (cp << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);

		if (len == 1 && !in_range(raw_ranges, u) && is_md_special((char)c0))
			out += '\\';
		out.append(text, i, len);

		line_start = (len == 1 && c0 == '\n');
		u += (cp > 0xFFFF) ? 2 : 1;
		i += (size_t)len;
	}
	/* Emit any remaining markers (closes at end; also any past-end from
	 * malformed offsets, so nothing is left unbalanced). */
	while (ei < evs.size())
		out += evs[ei++].mark;
	return out;
}

/* utf8_truncate that also never ends on a dangling escape backslash (which the
 * cut could leave behind after discord_escape). */
std::string truncate_escaped(const std::string &s, size_t max_bytes)
{
	std::string t = utf8_truncate(s, max_bytes);
	size_t bs = 0;
	while (bs < t.size() &&
	       static_cast<unsigned char>(t[t.size() - 1 - bs]) == '\\')
		bs++;
	if (bs & 1)
		t.pop_back();
	return t;
}

/*
 * A reply embed's description is capped by Discord at 4096 characters; stay
 * comfortably under that, and show at most this many lines of the replied
 * message.
 */
constexpr size_t kReplyMaxChars = 4000;
constexpr size_t kReplyMaxLines = 5;

/*
 * The replied message rendered for a reply embed: at most kReplyMaxLines lines,
 * discord-escaped, and no longer than kReplyMaxChars, with a literal "[...]"
 * appended whenever anything was dropped -- lines beyond the fifth, or text
 * beyond the character budget. The budget always reserves room for the "[...]",
 * so the marker itself is never cut. Returns the finished (escaped) description;
 * the caller only JSON-encodes it.
 */
std::string reply_preview(const std::string &text)
{
	static constexpr char kEllipsis[] = "[...]";
	constexpr size_t kEllipsisLen = sizeof(kEllipsis) - 1;

	/* Keep the first kReplyMaxLines lines; remember whether more follow. */
	size_t end = text.size();
	bool more_lines = false;
	size_t nl = 0;
	for (size_t i = 0; i < text.size(); i++) {
		if (text[i] != '\n')
			continue;
		if (++nl == kReplyMaxLines) {
			end = i; /* cut just before the kReplyMaxLines-th '\n' */
			more_lines = i + 1 < text.size();
			break;
		}
	}

	std::string esc = discord_escape(text.substr(0, end));

	bool len_cut = false;
	if (esc.size() > kReplyMaxChars - kEllipsisLen) {
		esc = truncate_escaped(esc, kReplyMaxChars - kEllipsisLen);
		len_cut = true;
	}
	if (more_lines || len_cut)
		esc += kEllipsis;
	return esc;
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

	/*
	 * Loaded on the same cycle as the webhooks so a newly configured
	 * discordd route stops echoing within one refresh interval rather
	 * than needing a restart. A failure here is not fatal: keep the
	 * previous list rather than dropping the loop guard entirely.
	 */
	std::vector<int64_t> bots;
	bool bots_ok = true;
	try {
		bots = db_->loadForwardingBotUserIds();
	} catch (const std::exception &e) {
		bots_ok = false;
		pr_error(l_, "discord: failed to load bridge bot ids: %s",
			 e.what());
	}

	size_t n_hooks = rows.size();
	size_t n_chats = next.size();
	size_t n_bots;
	{
		std::lock_guard<std::mutex> lk(cache_mtx_);
		cache_.swap(next);
		if (bots_ok)
			bridge_bots_.swap(bots);
		n_bots = bridge_bots_.size();
	}
	pr_info(l_,
		"discord: loaded %zu webhook(s) across %zu chat(s), %zu bridge bot(s)",
		n_hooks, n_chats, n_bots);
}

bool DiscordForwarder::is_bridge_bot(int64_t sender_id)
{
	if (!sender_id)
		return false;
	std::lock_guard<std::mutex> lk(cache_mtx_);
	for (int64_t id : bridge_bots_) {
		if (id == sender_id)
			return true;
	}
	return false;
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

DiscordForwarder::ReplyInfo
DiscordForwarder::resolve_reply(const ForwardMessage &fm)
{
	ReplyInfo ri;
	if (fm.reply_to_msg_id == 0)
		return ri;
	int64_t qchat = fm.reply_to_chat_id != 0 ? fm.reply_to_chat_id : fm.chat_id;

	std::optional<QuotedMessage> q;
	try {
		q = db_->getQuotedMessage(qchat, fm.reply_to_msg_id);
	} catch (const std::exception &e) {
		pr_warn(l_, "discord: reply lookup failed: %s", e.what());
		return ri;
	}
	if (!q)
		return ri;

	ri.sender = resolve_sender(qchat, q->sender_id, q->sender_chat_id,
				   q->sender_name);
	ri.snippet = reply_preview(q->text);
	ri.chat_id = qchat;
	ri.message_id = fm.reply_to_msg_id;
	ri.ok = true;
	return ri;
}

std::string DiscordForwarder::reply_embed(const ReplyInfo &ri,
					  const std::string &jump_url) const
{
	/* An embed styled like a Discord reply: coloured bar, the replied
	 * author's name+avatar (name links to the message), and a text snippet. */
	std::string e = "\"embeds\":[{\"color\":" + std::to_string(kReplyColor) +
			",\"author\":{\"name\":\"" +
			json_escape(ri.sender.name) + "\"";
	if (!jump_url.empty())
		e += ",\"url\":\"" + json_escape(jump_url) + "\"";
	if (!ri.sender.avatar_url.empty())
		e += ",\"icon_url\":\"" + json_escape(ri.sender.avatar_url) + "\"";
	e += "}";
	/* ri.snippet is already discord-escaped and length-bounded by
	 * reply_preview(); here it only needs JSON encoding. */
	if (!ri.snippet.empty())
		e += ",\"description\":\"" + json_escape(ri.snippet) + "\"";
	e += "}]";
	return e;
}

DiscordForwarder::WebhookInfo
DiscordForwarder::webhook_info(const std::string &webhook_url)
{
	{
		std::lock_guard<std::mutex> lk(webhook_info_mtx_);
		auto it = webhook_info_.find(webhook_url);
		if (it != webhook_info_.end())
			return it->second;
	}

	WebhookInfo wi;
	DiscordResponse r = client_.get(webhook_url);
	if (!r.ok()) {
		/* Transient failure: don't cache, so it is retried next time. */
		pr_warn(l_, "discord: webhook info GET failed (status=%ld)", r.status);
		return wi;
	}
	wi.guild_id = json_str_field(r.body, "guild_id");
	wi.channel_id = json_str_field(r.body, "channel_id");
	{
		std::lock_guard<std::mutex> lk(webhook_info_mtx_);
		webhook_info_[webhook_url] = wi;
	}
	return wi;
}

std::string DiscordForwarder::reply_jump_url(const std::string &webhook_url,
					     int64_t reply_chat_id,
					     int64_t reply_msg_id)
{
	/* The replied message's Discord id in this same webhook's channel. */
	std::string msg_id;
	try {
		auto sent = db_->getSentMessages(reply_chat_id, reply_msg_id, nullptr);
		for (const auto &s : sent) {
			if (s.webhook_url != webhook_url)
				continue;
			if (s.kind == "text") {
				msg_id = s.discord_message_id; /* prefer the text part */
				break;
			}
			if (msg_id.empty())
				msg_id = s.discord_message_id;
		}
	} catch (const std::exception &e) {
		pr_warn(l_, "discord: reply target lookup failed: %s", e.what());
		return std::string();
	}
	if (msg_id.empty())
		return std::string(); /* replied message wasn't forwarded here */

	WebhookInfo wi = webhook_info(webhook_url);
	if (!wi.ok())
		return std::string();
	return "https://discord.com/channels/" + wi.guild_id + "/" +
	       wi.channel_id + "/" + msg_id;
}

void DiscordForwarder::post_reply_preview(const std::string &webhook_url,
					  const Sender &s, const ReplyInfo &ri)
{
	/* Own message, empty content + the reply embed, so the caller's next
	 * post to the same webhook lands below it. Not tracked: it mirrors
	 * another, still-present message. Jump link is per-channel. */
	std::string embed = reply_embed(ri,
		reply_jump_url(webhook_url, ri.chat_id, ri.message_id));
	DiscordResponse r = client_.post_json(webhook_url,
		build_payload(s, std::string(), embed));
	if (!r.ok()) {
		std::string detail = r.status ? r.body.substr(0, 200) : r.error;
		pr_warn(l_, "discord: reply preview POST failed (status=%ld): %s",
			r.status, detail.c_str());
	}
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

void DiscordForwarder::post_one_and_record(const std::string &url,
					   const std::string &payload,
					   int64_t chat_id, int64_t message_id,
					   const char *kind)
{
	DiscordResponse r = client_.post_json(url, payload);
	if (!r.ok()) {
		std::string detail = r.status ? r.body.substr(0, 200) : r.error;
		pr_warn(l_, "discord: webhook POST failed (status=%ld): %s",
			r.status, detail.c_str());
		return;
	}
	/* Remember the Discord message so a later Telegram edit or delete can
	 * find it (the content is re-derived, not stored). */
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

void DiscordForwarder::forward(const ForwardMessage &fm)
{
	std::vector<std::string> urls = webhooks_for(fm.chat_id);
	if (urls.empty())
		return;

	/*
	 * Loop guard. This message may itself be a Discord message that
	 * discordd just delivered into the chat; mirroring it back would echo
	 * it into the very channel it came from. discordd's own guard (it
	 * ignores webhook-authored messages) stops the cycle from running
	 * away, but only this check stops the duplicate being posted at all.
	 */
	if (is_bridge_bot(fm.sender_id)) {
		pr_debug(l_,
			 "discord: not mirroring message %lld from bridge bot %lld",
			 (long long)fm.message_id, (long long)fm.sender_id);
		return;
	}

	if (fm.has_file) {
		/* Remember this live media message; its file forwards once
		 * stored (on_media_stored). Sweep stale entries opportunistically. */
		std::lock_guard<std::mutex> lk(media_mtx_);
		sweep_pending_locked(now_epoch());
		pending_media_[{ fm.chat_id, fm.message_id }] = PendingMedia{
			now_epoch() + media_ttl_, fm.sender_id,
			fm.sender_chat_id, fm.sender_name,
			fm.reply_to_chat_id, fm.reply_to_msg_id,
			!fm.text.empty() };
	}

	/* A caption/quote (if any) is sent now; the media (if any) follows. */
	pool_.post([this, fm, urls] { do_text_forward(fm, urls); });
}

void DiscordForwarder::do_text_forward(ForwardMessage fm,
				       std::vector<std::string> urls)
{
	std::string content =
		truncate_escaped(render_markdown(fm.text, fm.entities), 2000);

	/* No text to post: a media message with no caption (sticker/photo) or an
	 * empty service message. Such a reply's preview is posted by
	 * do_media_forward, right before the media, so it stays above it. */
	if (content.empty())
		return;

	ReplyInfo ri = resolve_reply(fm);
	Sender s = resolve_sender(fm.chat_id, fm.sender_id, fm.sender_chat_id,
				  fm.sender_name);
	/* Stamp the author with the reversible (cx:user:message) identifier. */
	s.name = forwarded_author_name(s.name, fm.sender_id, fm.message_id);
	pr_info(l_, "discord: forwarding chat_id=%lld (%s%s) to %zu webhook(s): %.60s",
		(long long)fm.chat_id, fm.kind.empty() ? "text" : fm.kind.c_str(),
		ri.ok ? "+reply" : "", urls.size(), content.c_str());

	for (const auto &url : urls) {
		/* The replied-message preview goes first (own message), so the reply
		 * text lands below it -- Discord draws content above embeds, so they
		 * cannot share one message. Jump link is per-channel. */
		if (ri.ok)
			post_reply_preview(url, s, ri);
		post_one_and_record(url, build_payload(s, content, std::string()),
				    fm.chat_id, fm.message_id, "text");
	}
}

void DiscordForwarder::forward_edit(const ForwardMessage &fm)
{
	if (webhooks_for(fm.chat_id).empty())
		return;
	/* Its original was never mirrored (see forward), so neither is this. */
	if (is_bridge_bot(fm.sender_id))
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

	std::string content =
		truncate_escaped(render_markdown(fm.text, fm.entities), 2000);
	if (content.empty())
		return; /* edited to empty -> leave the Discord message as-is */

	/* A webhook message edit changes only content, leaving the reply embed
	 * (and username/avatar) intact. */
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
	 *   text  part = message text (the reply embed, if any, stays untouched)
	 *   media part = the media link (empty for an image, whose embed stays)
	 */
	std::string text_content, media_content;
	try {
		auto mf = db_->getMessageForward(chat_id, message_id);
		if (mf) {
			text_content = discord_escape(mf->text);

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
		/* Prepend a bold "(Deleted)" to the original content, then re-fit
		 * it into Discord's 2000-char limit (the self-contained bold prefix
		 * is kept, the tail trimmed). orig is already discord-escaped (text)
		 * or a bare URL (media); the "**(Deleted)**" markdown is intentional. */
		std::string content = truncate_escaped("**(Deleted)**\n\n" + orig, 2000);
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
	/* Same author stamp as the text path, keyed to this same message id. */
	s.name = forwarded_author_name(s.name, pm.sender_id, message_id);

	/* If this media is a reply with no caption, its preview was not posted by
	 * do_text_forward; post it here, right before the media, so it stays
	 * above it. (A captioned reply already showed the preview in the text
	 * message.) Posting both from this one task keeps them ordered. */
	ReplyInfo ri;
	if (pm.reply_to_msg_id != 0 && !pm.has_caption) {
		ForwardMessage q{};
		q.chat_id = chat_id;
		q.reply_to_chat_id = pm.reply_to_chat_id;
		q.reply_to_msg_id = pm.reply_to_msg_id;
		ri = resolve_reply(q);
	}

	std::string content, embed;
	if (is_image(fi->file_type, fi->ext))
		embed = "\"embeds\":[{\"image\":{\"url\":\"" +
			json_escape(url) + "\"}}]";
	else
		content = url; /* video auto-embeds; documents render as a link */

	pr_info(l_, "discord: forwarding media chat_id=%lld (%s%s) to %zu webhook(s)",
		(long long)chat_id, fi->file_type.c_str(),
		ri.ok ? "+reply" : "", urls.size());
	for (const auto &hook : urls) {
		if (ri.ok)
			post_reply_preview(hook, s, ri);
		post_one_and_record(hook, build_payload(s, content, embed),
				    chat_id, message_id, "media");
	}
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
