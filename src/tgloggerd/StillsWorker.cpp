// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include "StillsWorker.hpp"
#include "StillsClient.hpp"
#include "StoragePath.hpp"

#include <filesystem>
#include <fstream>
#include <sys/socket.h>
#include <system_error>

namespace tgloggerd {

namespace fs = std::filesystem;

StillsWorker::StillsWorker(DB &db, log_hd_t *l, Config cfg, StoreFn store,
			   LinkedFn linked)
	: db_(db), l_(l), cfg_(std::move(cfg)), store_(std::move(store)),
	  linked_(std::move(linked))
{
}

StillsWorker::~StillsWorker(void)
{
	stop();
}

void StillsWorker::start(void)
{
	if (thr_.joinable())
		return;
	thr_ = std::thread([this] { loop(); });
}

void StillsWorker::stop(void)
{
	{
		std::lock_guard<std::mutex> lk(mtx_);
		stop_ = true;
	}
	cv_.notify_all();
	int fd = active_fd_.load();
	if (fd >= 0)
		::shutdown(fd, SHUT_RDWR);
	if (thr_.joinable())
		thr_.join();
}

std::string StillsWorker::kindOf(const std::string &content_type)
{
	if (content_type == "video" || content_type == "animation" ||
	    content_type == "video_note" || content_type == "sticker")
		return content_type;
	return std::string();
}

void StillsWorker::messageFileStored(bool is_group, int64_t chat_id,
				     int64_t message_id)
{
	{
		std::lock_guard<std::mutex> lk(mtx_);
		if (stop_ || fresh_.size() >= cfg_.max_new ||
		    !fresh_ids_.insert({ chat_id, message_id }).second)
			return;
		fresh_.push_back(Fresh{ is_group, chat_id, message_id,
					Clock::now() + cfg_.new_delay });
	}
	cv_.notify_all();
}

void StillsWorker::stillsEvicted(const std::vector<uint64_t> &still_ids)
{
	size_t queued = 0;
	{
		std::lock_guard<std::mutex> lk(mtx_);
		if (stop_)
			return;
		for (uint64_t id : still_ids) {
			if (evicted_.size() >= cfg_.max_new)
				break;
			if (evicted_ids_.insert(id).second) {
				evicted_.push_back(id);
				queued++;
			}
		}
	}
	if (queued)
		pr_info(l_, "stills: %zu evicted still(s) to take again",
			queued);
	cv_.notify_all();
}

void StillsWorker::queueStillVideos(uint64_t still_id)
{
	std::vector<uint64_t> videos;
	try {
		videos = db_.listStillVideos(still_id);
	} catch (const std::exception &e) {
		pr_error(l_, "stills: could not list the videos of evicted"
			 " still %llu: %s", (unsigned long long)still_id,
			 e.what());
		return;
	}
	std::lock_guard<std::mutex> lk(mtx_);
	for (uint64_t v : videos) {
		if (regen_.size() >= cfg_.max_new)
			break;
		if (regen_ids_.insert(v).second)
			regen_.push_back(v);
	}
}

bool StillsWorker::sleepUntil(Clock::time_point t)
{
	std::unique_lock<std::mutex> lk(mtx_);
	cv_.wait_until(lk, t, [this] { return stop_; });
	return !stop_;
}

void StillsWorker::loop(void)
{
	try {
		auto all = db_.listStillCandidates();
		std::lock_guard<std::mutex> lk(mtx_);
		for (const auto &c : all)
			backlog_.push_back(c.video_file_id);
		backlog_total_ = all.size();
	} catch (const std::exception &e) {
		pr_error(l_, "stills: could not list the videos without a"
			 " still: %s", e.what());
	}
	pr_info(l_, "stills: %zu stored video(s) have no still; generating"
		" them", backlog_total_);

	Clock::time_point next = Clock::now();
	for (;;) {
		std::optional<uint64_t> video;
		std::optional<Fresh> msg;
		std::optional<uint64_t> evicted;
		/* video is one of an evicted still, not of the pass. */
		bool regen = false;
		{
			std::unique_lock<std::mutex> lk(mtx_);
			for (;;) {
				if (stop_)
					return;
				const auto now = Clock::now();
				if (!fresh_.empty() && fresh_.front().due <= now) {
					msg = fresh_.front();
					fresh_ids_.erase({ msg->chat_id,
							   msg->message_id });
					fresh_.pop_front();
					break;
				}
				if (!regen_.empty()) {
					video = regen_.front();
					regen_ids_.erase(*video);
					regen_.pop_front();
					regen = true;
					break;
				}
				if (!evicted_.empty()) {
					evicted = evicted_.front();
					evicted_ids_.erase(*evicted);
					evicted_.pop_front();
					break;
				}
				if (!backlog_.empty()) {
					video = backlog_.front();
					backlog_.pop_front();
					break;
				}
				if (fresh_.empty())
					cv_.wait(lk);
				else
					cv_.wait_until(lk, fresh_.front().due);
			}
		}
		if (evicted) {
			/* Only reads: not paced. */
			queueStillVideos(*evicted);
			continue;
		}
		if (Clock::now() < next && !sleepUntil(next))
			return;

		Outcome o = Outcome::Worked;
		try {
			/* Looked at again: it may have got a still since. */
			auto c = video ? db_.stillCandidate(*video)
				       : db_.stillCandidateForMessage(
					       msg->is_group, msg->chat_id,
					       msg->message_id);
			o = c ? process(*c) : Outcome::Nothing;
		} catch (const std::exception &e) {
			pr_error(l_, "stills: %s %llu: %s",
				 video ? "video file" : "message",
				 (unsigned long long)(video ? *video
					: (uint64_t)msg->message_id),
				 e.what());
		}
		if (o == Outcome::Away) {
			/* Keep it for when the service is back. */
			std::lock_guard<std::mutex> lk(mtx_);
			if (regen) {
				if (regen_ids_.insert(*video).second)
					regen_.push_front(*video);
			} else if (video) {
				backlog_.push_front(*video);
			} else if (fresh_ids_.insert({ msg->chat_id,
						       msg->message_id }).second) {
				msg->due = Clock::now();
				fresh_.push_front(*msg);
			}
			next = Clock::now() + cfg_.retry_after;
			continue;
		}
		if (o == Outcome::Worked)
			next = Clock::now() + cfg_.pace;
		if (video && !regen && (++backlog_done_ % 1000 == 0 ||
			      backlog_done_ == backlog_total_))
			pr_info(l_, "stills: went through %zu of %zu video(s);"
				" %llu still(s) made so far", backlog_done_,
				backlog_total_, (unsigned long long)made_);
	}
}

/* One video whose messages need a still. */
StillsWorker::Outcome StillsWorker::process(const StillCandidate &c)
{
	const uint64_t video_file_id = c.video_file_id;
	if (refused_.count(video_file_id))
		return Outcome::Nothing;

	/* Another message of the same video got its still already. */
	if (auto s = db_.findGeneratedStill(video_file_id))
		return linkStill(video_file_id, *s);

	const std::string kind = kindOf(c.kind);
	if (kind.empty() || c.sha256_hex.size() != 64)
		return Outcome::Nothing;
	const fs::path rel = blobPath("", c.sha256_hex, c.ext);
	std::error_code ec;
	if (!fs::exists(fs::path(cfg_.storage_dir) / rel, ec)) {
		pr_debug(l_, "stills: video file %llu: its blob is not in the"
			 " store", (unsigned long long)video_file_id);
		return Outcome::Nothing;
	}

	stills::Request req;
	req.path = rel.string();
	req.kind = kind;
	req.duration = c.duration;
	stills::Answer a = stills::ask(cfg_.socket_path, req, cfg_.timeout,
				       &active_fd_);
	switch (a.kind) {
	case stills::Answer::Kind::Still:
		if (away_) {
			pr_info(l_, "stills: the stills service is back");
			away_ = false;
		}
		break;
	case stills::Answer::Kind::Refused:
		refused_.insert(video_file_id);
		pr_info(l_, "stills: no still for video file %llu (%s): %s: %s",
			(unsigned long long)video_file_id, kind.c_str(),
			a.error.c_str(), a.message.c_str());
		return Outcome::Worked;
	case stills::Answer::Kind::Unreachable:
		/* Said once, not every minute while it stays away. */
		if (!away_)
			pr_warn(l_, "stills: the stills service is not reachable"
				" (%s); trying again every %llds",
				a.message.c_str(),
				(long long)cfg_.retry_after.count());
		away_ = true;
		return Outcome::Away;
	case stills::Answer::Kind::Failed:
		{
			/* stop() cut it short: nothing wrong with the video. */
			std::lock_guard<std::mutex> lk(mtx_);
			if (stop_)
				return Outcome::Nothing;
		}
		refused_.insert(video_file_id);
		pr_warn(l_, "stills: video file %llu: the exchange with the"
			" stills service failed: %s",
			(unsigned long long)video_file_id, a.message.c_str());
		return Outcome::Worked;
	}

	/* Through the store's own path, from a file named for its type. */
	fs::create_directories(cfg_.tmp_dir, ec);
	const fs::path tmp = fs::path(cfg_.tmp_dir) /
		("still-" + std::to_string(video_file_id) + "." + a.format);
	{
		std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
		out.write(a.bytes.data(), (std::streamsize)a.bytes.size());
		if (!out) {
			pr_error(l_, "stills: could not write %s", tmp.c_str());
			fs::remove(tmp, ec);
			return Outcome::Worked;
		}
	}
	std::optional<Stored> blob = store_(tmp.string());
	fs::remove(tmp, ec);
	if (!blob)
		return Outcome::Worked;

	const uint64_t still = db_.upsertGeneratedStill(
		blob->sha256_hex, blob->ext, a.bytes.size(), blob->on_disk,
		video_file_id);
	made_++;
	pr_info(l_, "stills: made a %s still of video file %llu (%s, %zu"
		" bytes) | file_id=%llu", a.format.c_str(),
		(unsigned long long)video_file_id, kind.c_str(),
		a.bytes.size(), (unsigned long long)still);
	return linkStill(video_file_id, still);
}

StillsWorker::Outcome StillsWorker::linkStill(uint64_t video_file_id,
					     uint64_t still_id)
{
	db_.linkGeneratedStill(video_file_id, still_id);
	auto msgs = db_.listStillMessages(still_id);
	if (linked_ && !msgs.empty())
		linked_(still_id, msgs);
	pr_debug(l_, "stills: still %llu of video file %llu is the thumbnail"
		 " of %zu message(s)", (unsigned long long)still_id,
		 (unsigned long long)video_file_id, msgs.size());
	return Outcome::Worked;
}

} /* namespace tgloggerd */
