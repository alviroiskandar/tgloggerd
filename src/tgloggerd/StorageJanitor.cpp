// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include "StorageJanitor.hpp"
#include "StoragePath.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <system_error>
#include <utility>

namespace tgloggerd {

namespace {

/*
 * Pauses between batches. A real batch is up to kBatch committed UPDATEs and
 * unlinks; the pause keeps a sweep that evicts hundreds of thousands of
 * files from saturating the disk the logger also writes to. A dry batch only
 * reads one index range, so it pauses less.
 */
constexpr auto kPauseReal = std::chrono::milliseconds(1000);
constexpr auto kPauseDry = std::chrono::milliseconds(100);
/* The first periodic sweep waits this long (at most) after start, so it
 * does not compete with the daemon's own start-up. */
constexpr uint32_t kFirstSweepDelay = 60;

bool is_hex64(const std::string &s)
{
	return s.size() == 64 &&
	       std::all_of(s.begin(), s.end(), [](char c) {
		       return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
	       });
}

/* An extension storeDownloadedFile could have written: no separator. */
bool is_safe_ext(const std::string &s)
{
	return s.find('/') == std::string::npos &&
	       s.find('\0') == std::string::npos;
}

uint64_t sat_sub(uint64_t a, uint64_t b)
{
	return a > b ? a - b : 0;
}

} /* namespace */

StorageJanitor::StorageJanitor(DB *db, log_hd_t *l, Config cfg)
	: db_(db), l_(l), cfg_(std::move(cfg))
{
}

StorageJanitor::~StorageJanitor(void)
{
	stop();
}

void StorageJanitor::start(void)
{
	if (started_)
		return;
	started_ = true;
	thr_ = std::thread([this] { loop(); });
}

void StorageJanitor::stop(void)
{
	if (!started_)
		return;
	{
		std::lock_guard<std::mutex> lk(mtx_);
		stop_ = true;
	}
	cv_.notify_all();
	if (thr_.joinable())
		thr_.join();
	started_ = false;
}

bool StorageJanitor::requestSweep(bool dry_run, bool *effective_dry)
{
	/* Nothing over the API may override a report-only deployment. */
	if (!dry_run && cfg_.dry_run)
		return false;
	{
		std::lock_guard<std::mutex> lk(mtx_);
		pending_dry_ = pending_ ? (pending_dry_ && dry_run) : dry_run;
		pending_ = true;
		if (effective_dry)
			*effective_dry = pending_dry_;
	}
	cv_.notify_all();
	return true;
}

StorageJanitor::Status StorageJanitor::status(void)
{
	Status st;
	st.cfg = cfg_;
	st.low_watermark = cfg_.max_bytes ? lowWatermark(cfg_.max_bytes) : 0;
	std::lock_guard<std::mutex> lk(mtx_);
	st.sweeping = sweeping_;
	st.pending = pending_;
	st.last = last_;
	return st;
}

void StorageJanitor::loop(void)
{
	pr_info(l_, "storage: janitor started: max_bytes=%llu retention=%u days"
		" sweep=%us%s", (unsigned long long)cfg_.max_bytes,
		cfg_.retention_days, cfg_.sweep_secs,
		cfg_.dry_run ? " (dry run: reporting only)" : "");

	std::unique_lock<std::mutex> lk(mtx_);
	auto next = std::chrono::steady_clock::now() +
		    std::chrono::seconds(std::min(cfg_.sweep_secs,
						  kFirstSweepDelay));
	auto woken = [this] { return stop_.load() || pending_; };
	while (!stop_) {
		if (cfg_.sweep_secs == 0)
			cv_.wait(lk, woken);
		else
			cv_.wait_until(lk, next, woken);
		if (stop_)
			break;

		bool dry = cfg_.dry_run;
		if (pending_) {
			dry = pending_dry_;
			pending_ = false;
		}
		sweeping_ = true;
		lk.unlock();
		Sweep s = sweep(dry);
		lk.lock();
		sweeping_ = false;
		last_ = s;
		next = std::chrono::steady_clock::now() +
		       std::chrono::seconds(cfg_.sweep_secs);
	}
}

bool StorageJanitor::pace(bool dry_run)
{
	std::unique_lock<std::mutex> lk(mtx_);
	return cv_.wait_for(lk, dry_run ? kPauseDry : kPauseReal,
			    [this] { return stop_.load(); });
}

bool StorageJanitor::evictOne(const EvictCandidate &c, bool dry_run, Sweep &s)
{
	if (dry_run)
		return true;
	if (!db_->evictFile(c.id, c.accessed_at)) {
		s.kept++;
		return false;
	}
	if (c.generated && stills_fn_) {
		if (stills_.size() < kMaxStills)
			stills_.push_back(c.id);
		else
			stills_dropped_++;
	}

	/* A row with no digest has no blob (it should not be on_disk). */
	if (c.sha256.empty())
		return true;
	if (!is_hex64(c.sha256) || !is_safe_ext(c.ext)) {
		s.unlink_errors++;
		pr_error(l_, "storage: evicted file_id=%llu but left its blob:"
			 " bad sha256 or extension in the row",
			 (unsigned long long)c.id);
		return true;
	}

	std::filesystem::path p = blobPath(cfg_.storage_dir, c.sha256, c.ext);
	std::error_code ec;
	if (std::filesystem::remove(p, ec)) {
		pr_debug(l_, "storage: evicted file_id=%llu size=%llu %s",
			 (unsigned long long)c.id,
			 (unsigned long long)c.file_size, p.c_str());
	} else if (!ec) {
		/* Already gone: nothing to free, and not an error. */
		s.missing_blobs++;
	} else {
		s.unlink_errors++;
		pr_error(l_, "storage: evicted file_id=%llu but could not"
			 " unlink %s: %s", (unsigned long long)c.id, p.c_str(),
			 ec.message().c_str());
	}
	return true;
}

StorageJanitor::Sweep StorageJanitor::sweep(bool dry_run)
{
	Sweep s;
	s.started_at = (int64_t)time(nullptr);
	s.dry_run = dry_run;
	const char *verb = dry_run ? "would evict" : "evicted";
	bool stopped = false;
	stills_.clear();
	stills_dropped_ = 0;

	try {
		StoreTotals tot = db_->storedTotals();
		s.bytes_before = tot.bytes;
		/* The stored bytes, as the sweep has left them so far. */
		uint64_t remaining = tot.bytes;

		/* Retention: everything stored and unused since the cutoff. */
		std::string cutoff;
		if (cfg_.retention_days > 0) {
			cutoff = db_->retentionCutoff(cfg_.retention_days);
			if (dry_run) {
				StoreTotals r = db_->storedTotals(cutoff);
				s.retention.files = r.files;
				s.retention.bytes = r.bytes;
				remaining = sat_sub(remaining, r.bytes);
			}
			std::string after_at;
			uint64_t after_id = 0;
			while (!dry_run && !stopped) {
				auto batch = db_->listEvictCandidates(
					cutoff, "", after_at, after_id, kBatch);
				for (const auto &c : batch) {
					if (stop_) {
						stopped = true;
						break;
					}
					if (!evictOne(c, dry_run, s))
						continue;
					s.retention.files++;
					s.retention.bytes += c.file_size;
					remaining = sat_sub(remaining,
							    c.file_size);
				}
				if (stopped || batch.size() < kBatch)
					break;
				after_at = batch.back().accessed_at;
				after_id = batch.back().id;
				stopped = pace(dry_run);
			}
		}

		/*
		 * Cap: least recently accessed first, past what retention
		 * took, until the total is down to the low watermark. A real
		 * sweep re-reads the total: ingest keeps storing meanwhile.
		 */
		if (cfg_.max_bytes > 0 && !stopped) {
			uint64_t target = lowWatermark(cfg_.max_bytes);
			if (!dry_run)
				remaining = db_->storedTotals().bytes;
			std::string after_at;
			uint64_t after_id = 0;
			bool over = remaining > cfg_.max_bytes;
			while (over && !stopped) {
				auto batch = db_->listEvictCandidates(
					"", cutoff, after_at, after_id, kBatch);
				for (const auto &c : batch) {
					if (stop_) {
						stopped = true;
						break;
					}
					if (remaining <= target) {
						over = false;
						break;
					}
					if (!evictOne(c, dry_run, s))
						continue;
					s.cap.files++;
					s.cap.bytes += c.file_size;
					remaining = sat_sub(remaining,
							    c.file_size);
				}
				if (remaining <= target)
					over = false;
				if (!over || stopped)
					break;
				if (batch.size() < kBatch) {
					pr_warn(l_, "storage: ran out of files"
						" to evict with %llu bytes still"
						" stored over the %llu low"
						" watermark",
						(unsigned long long)remaining,
						(unsigned long long)target);
					break;
				}
				after_at = batch.back().accessed_at;
				after_id = batch.back().id;
				stopped = pace(dry_run);
			}
		}

		s.bytes_after = dry_run || stopped ? remaining
						   : db_->storedTotals().bytes;
		if (stopped)
			s.error = "stopped at shutdown";
		else
			s.completed = true;
	} catch (const std::exception &e) {
		s.error = e.what();
	}
	s.finished_at = (int64_t)time(nullptr);

	if (!s.error.empty())
		__pr_log(l_, stopped ? PR_LOG_WARNING : PR_LOG_ERROR,
			 "storage: %ssweep stopped after %lld s: %s;"
			 " %s %llu file(s), %llu bytes so far",
			 dry_run ? "dry-run " : "",
			 (long long)(s.finished_at - s.started_at),
			 s.error.c_str(), verb,
			 (unsigned long long)(s.retention.files + s.cap.files),
			 (unsigned long long)(s.retention.bytes + s.cap.bytes));
	else
		pr_info(l_, "storage: %ssweep done in %lld s: retention %s"
			" %llu file(s) (%llu bytes), cap %s %llu file(s)"
			" (%llu bytes); stored %llu -> %llu bytes; kept %llu,"
			" blobs missing %llu, unlink errors %llu",
			dry_run ? "dry-run " : "",
			(long long)(s.finished_at - s.started_at), verb,
			(unsigned long long)s.retention.files,
			(unsigned long long)s.retention.bytes, verb,
			(unsigned long long)s.cap.files,
			(unsigned long long)s.cap.bytes,
			(unsigned long long)s.bytes_before,
			(unsigned long long)s.bytes_after,
			(unsigned long long)s.kept,
			(unsigned long long)s.missing_blobs,
			(unsigned long long)s.unlink_errors);

	/* Not at shutdown: the stills worker is stopping too. */
	if (!stopped && !stills_.empty()) {
		pr_info(l_, "storage: %zu evicted generated still(s) to take"
			" again%s", stills_.size(), stills_dropped_
			? "; more wait for the stills pass at the next start"
			: "");
		stills_fn_(stills_);
	}
	stills_.clear();
	return s;
}

} /* namespace tgloggerd */
