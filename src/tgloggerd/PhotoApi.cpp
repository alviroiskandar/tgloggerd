// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include "PhotoApi.hpp"

#include "ChatRef.hpp"
#include "DB.hpp"
#include "MessageExtract.hpp"
#include "TDLib.hpp"

#include <td/telegram/td_api.h>

#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>

namespace tgloggerd {

namespace {

namespace td_api = td::td_api;
using namespace std::chrono_literals;

/* Per TDLib request, as the chat endpoints. */
constexpr std::chrono::milliseconds kRequestTimeout = 15s;

/* How long a photo list is reused for the same chat, offset and limit. */
constexpr std::chrono::seconds kPhotoListCacheTtl = 10min;

/* At most this many photos in one answer. */
constexpr int64_t kMaxLimit = 100;

/* A td_api::error answer, carried out of call(). */
struct TdFailure {
	int32_t		code;
	std::string	message;
};

template <class T>
td_api::object_ptr<T> call(TDLib &t, td_api::object_ptr<td_api::Function> f)
{
	TDLib::TdObject obj = t.request(std::move(f), kRequestTimeout);
	if (!obj)
		throw TdFailure{ 500, "empty answer from TDLib" };
	if (obj->get_id() == td_api::error::ID) {
		auto e = td::move_tl_object_as<td_api::error>(obj);
		throw TdFailure{ e->code_, e->message_ };
	}
	if (obj->get_id() != T::ID)
		throw TdFailure{ 500, "unexpected answer type from TDLib" };
	return td::move_tl_object_as<T>(obj);
}

/* An id from the path: a nonzero integer; a 400 otherwise. */
int64_t requireId(const InternalApi::Request &req, const char *name)
{
	const std::string &s = req.param(name);
	size_t used = 0;
	int64_t id = 0;
	try {
		id = std::stoll(s, &used);
	} catch (const std::exception &) {
		used = 0;
	}
	if (used == 0 || used != s.size() || id == 0)
		throw InternalApi::Error::badRequest(
			std::string(name) + " must be an integer");
	return id;
}

/* The value of @name in a raw query string ("" if absent); integers only,
 * so no percent-decoding. */
std::string queryValue(const std::string &query, const std::string &name)
{
	size_t i = 0;
	while (i <= query.size()) {
		size_t amp = query.find('&', i);
		if (amp == std::string::npos)
			amp = query.size();
		std::string kv = query.substr(i, amp - i);
		i = amp + 1;
		size_t eq = kv.find('=');
		if (kv.substr(0, eq) == name)
			return eq == std::string::npos ? std::string()
						       : kv.substr(eq + 1);
	}
	return std::string();
}

int64_t queryInt(const InternalApi::Request &req, const char *name,
		 int64_t dflt, int64_t lo, int64_t hi)
{
	const std::string v = queryValue(req.query, name);
	if (v.empty())
		return dflt;
	size_t used = 0;
	int64_t n = 0;
	try {
		n = std::stoll(v, &used);
	} catch (const std::exception &) {
		used = 0;
	}
	if (used == 0 || used != v.size() || n < lo || n > hi)
		throw InternalApi::Error::badRequest(
			std::string(name) + " must be an integer from " +
			std::to_string(lo) + " to " + std::to_string(hi));
	return n;
}

/*
 * The telegram_files row of a photo's file, recorded without its bytes
 * (the store downloads a profile or group photo through its own paths;
 * listing earlier photos fetches none), or 0 when the file carries no
 * remote id to record.
 */
uint64_t recordFile(DB &db, const td_api::file *f, const char *type,
		    const char *ext)
{
	if (!f || !f->remote_ || f->remote_->id_.empty())
		return 0;
	models::File mf;
	mf.tg_file_id = f->remote_->id_;
	mf.tg_unique_id = f->remote_->unique_id_;
	mf.file_type = type;
	mf.file_size = f->size_ > 0 ? (uint64_t)f->size_ : 0;
	mf.on_disk = false;
	mf.file_ext = ext;
	return db.upsertFileMeta(mf).id;
}

uint64_t recordPhotoFile(DB &db, const td_api::file *f)
{
	return recordFile(db, f, "photo", "jpg");
}

std::string uniqueOf(const td_api::file *f)
{
	return f && f->remote_ ? f->remote_->unique_id_ : std::string();
}

/*
 * Record one photo (its largest size) and its history row, and describe it
 * for the answer. @date is the photo's own date (0 when unknown). @video
 * is the MPEG-4 of an animated photo (the big animation, else the small
 * one; nullptr for a still), recorded without its bytes like the still and
 * linked to the history row as video_file_id.
 */
nlohmann::json photoEntry(DB &db, bool is_group, int64_t owner, int64_t photo_id,
			  const td_api::file *f, int64_t date, bool animated,
			  bool is_current, const td_api::file *video = nullptr)
{
	uint64_t fid = recordPhotoFile(db, f);
	uint64_t vid = 0;
	if (fid != 0) {
		db.ensurePhotoHistory(owner, is_group, fid, date);
		vid = recordFile(db, video, "video", "mp4");
		db.setPhotoHistoryVideo(owner, is_group, fid, vid);
	}
	return {
		{ "photo_id", photo_id ? nlohmann::json(std::to_string(photo_id))
				       : nlohmann::json(nullptr) },
		{ "date", date > 0 ? nlohmann::json(date) : nlohmann::json(nullptr) },
		{ "file_id", fid ? nlohmann::json(fid) : nlohmann::json(nullptr) },
		{ "video_file_id", vid ? nlohmann::json(vid) : nlohmann::json(nullptr) },
		{ "is_current", is_current },
		{ "animated", animated },
	};
}

/* Telegram's failure as an answer: 503/504 thrown, the rest an outcome. */
nlohmann::json failureBody(const TdFailure &f)
{
	ChatOutcome o = classifyChatError(f.code, f.message);
	if (o.outcome == "unavailable")
		throw InternalApi::Error::unavailable("TDLib is not ready: " + f.message);
	if (o.outcome == "timeout")
		throw InternalApi::Error::timeout("Telegram did not answer in time");
	nlohmann::json b = {
		{ "outcome", o.outcome == "flood_wait" ? "flood_wait" : "error" },
		{ "message", f.message },
		{ "photos", nlohmann::json::array() },
		{ "total", nullptr },
	};
	if (o.outcome == "flood_wait")
		b["retry_after"] = o.retry_after;
	return b;
}

/* GET /v1/users/{user_id}/photos */
nlohmann::json userPhotos(TDLib &t, DB &db, int64_t user_id, int64_t offset,
			  int64_t limit)
{
	if (!db.userExists(user_id))
		throw InternalApi::Error::notFound("no such user");
	try {
		/* The current photo's id, from TDLib's own copy of the user. */
		auto u = call<td_api::user>(t, td_api::make_object<td_api::getUser>(user_id));
		const int64_t current = u->profile_photo_ ? u->profile_photo_->id_ : 0;
		auto list = call<td_api::chatPhotos>(
			t, td_api::make_object<td_api::getUserProfilePhotos>(
				   user_id, (int32_t)offset, (int32_t)limit));
		nlohmann::json photos = nlohmann::json::array();
		for (const auto &ph : list->photos_) {
			if (!ph)
				continue;
			photos.push_back(photoEntry(
				db, false, user_id, ph->id_,
				chat_photo_best_size_of(ph->sizes_), ph->added_date_,
				ph->animation_ != nullptr,
				current != 0 && ph->id_ == current,
				chat_photo_animation(ph.get())));
		}
		return { { "outcome", "ok" }, { "message", "" },
			 { "photos", photos }, { "total", list->total_count_ } };
	} catch (const TdFailure &f) {
		return failureBody(f);
	}
}

/*
 * GET /v1/chats/{chat_id}/photos: the chat's photo changes, newest first
 * (searchChatMessages with the chat photo filter), and its current photo
 * from the chat itself when no change message shows it.
 */
nlohmann::json chatPhotos(TDLib &t, DB &db, int64_t chat_id, int64_t offset,
			  int64_t limit)
{
	if (!db.groupExists(chat_id))
		throw InternalApi::Error::notFound("no such group");
	try {
		auto c = call<td_api::chat>(t, td_api::make_object<td_api::getChat>(chat_id));
		const td_api::file *cur = c->photo_ ? c->photo_->big_.get() : nullptr;
		const std::string cur_unique = uniqueOf(cur);

		/* Change messages up to offset + limit, a page of 100 at a
		 * time. */
		std::vector<td_api::object_ptr<td_api::chatPhoto>> found;
		int64_t from = 0;
		int32_t total = 0;
		for (int page = 0; page < 5 &&
				   (int64_t)found.size() < offset + limit; page++) {
			auto r = call<td_api::foundChatMessages>(
				t, td_api::make_object<td_api::searchChatMessages>(
					   chat_id, nullptr, std::string(), nullptr,
					   from, 0, 100,
					   td_api::make_object<td_api::searchMessagesFilterChatPhoto>()));
			total = r->total_count_;
			for (auto &m : r->messages_) {
				if (!m || !m->content_ ||
				    m->content_->get_id() != td_api::messageChatChangePhoto::ID)
					continue;
				auto &cp = static_cast<td_api::messageChatChangePhoto &>(
					*m->content_);
				if (!cp.photo_)
					continue;
				if (cp.photo_->added_date_ == 0)
					cp.photo_->added_date_ = m->date_;
				found.push_back(std::move(cp.photo_));
			}
			if (r->next_from_message_id_ == 0 || r->messages_.empty())
				break;
			from = r->next_from_message_id_;
		}

		nlohmann::json photos = nlohmann::json::array();
		bool current_listed = false;
		for (size_t i = 0; i < found.size(); i++) {
			const td_api::file *f = chat_photo_best_size_of(found[i]->sizes_);
			const bool is_current = !cur_unique.empty() &&
				uniqueOf(f) == cur_unique;
			current_listed = current_listed || is_current;
			if ((int64_t)i < offset || (int64_t)photos.size() >= limit)
				continue;
			photos.push_back(photoEntry(
				db, true, chat_id, found[i]->id_, f,
				found[i]->added_date_, found[i]->animation_ != nullptr,
				is_current, chat_photo_animation(found[i].get())));
		}
		/*
		 * The current photo, set before the history Telegram keeps
		 * (or by a message since deleted), heads the first page.
		 */
		if (cur && !current_listed && offset == 0) {
			photos.insert(photos.begin(), photoEntry(
				db, true, chat_id, 0, cur, 0,
				c->photo_->has_animation_, true));
			if ((int64_t)photos.size() > limit)
				photos.erase(photos.end() - 1);
			total++;
		}
		return { { "outcome", "ok" }, { "message", "" },
			 { "photos", photos }, { "total", total } };
	} catch (const TdFailure &f) {
		return failureBody(f);
	}
}

struct Cached {
	std::chrono::steady_clock::time_point	at;
	nlohmann::json				body;
};

/* The cache of both lists: (is_group, id, offset, limit). */
std::mutex cache_mtx;
std::map<std::tuple<bool, int64_t, int64_t, int64_t>, Cached> cache;

InternalApi::Response answerList(bool is_group, int64_t id,
				 const InternalApi::Request &req, TDLib &t, DB &db)
{
	const int64_t offset = queryInt(req, "offset", 0, 0, 10000);
	const int64_t limit = queryInt(req, "limit", kMaxLimit, 1, kMaxLimit);
	const auto key = std::make_tuple(is_group, id, offset, limit);
	const auto now = std::chrono::steady_clock::now();
	{
		std::lock_guard<std::mutex> lk(cache_mtx);
		auto it = cache.find(key);
		if (it != cache.end() && now - it->second.at < kPhotoListCacheTtl) {
			InternalApi::Response r;
			r.body = it->second.body;
			return r;
		}
	}

	nlohmann::json body = is_group ? chatPhotos(t, db, id, offset, limit)
				       : userPhotos(t, db, id, offset, limit);

	if (body["outcome"] == "ok") {
		std::lock_guard<std::mutex> lk(cache_mtx);
		for (auto it = cache.begin(); it != cache.end();)
			it = now - it->second.at >= kPhotoListCacheTtl ? cache.erase(it)
								       : std::next(it);
		cache[key] = Cached{ now, body };
	}
	InternalApi::Response r;
	r.body = std::move(body);
	return r;
}

} /* namespace */

void registerPhotoApi(InternalApi &api, TDLib &tdlib, DB &db)
{
	TDLib *t = &tdlib;
	DB *d = &db;

	api.route("GET", "/v1/users/{user_id}/photos",
		  [t, d](const InternalApi::Request &req) {
		return answerList(false, requireId(req, "user_id"), req, *t, *d);
	});

	api.route("GET", "/v1/chats/{chat_id}/photos",
		  [t, d](const InternalApi::Request &req) {
		return answerList(true, requireId(req, "chat_id"), req, *t, *d);
	});
}

} /* namespace tgloggerd */
