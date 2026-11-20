// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef TGLOGGERD__STORAGE_JANITOR_HPP
#define TGLOGGERD__STORAGE_JANITOR_HPP

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <condition_variable>
#include <vector>

#include "helpers/log.h"
#include "DB.hpp"

namespace tgloggerd {

/*
 * Evicts files from the store (docs/design/storage-cache.md section 9): by
 * retention (stored files not accessed for retention_days) and by the total
 * cap (least recently accessed first, down to 95% of max_bytes). Eviction
 * clears on_disk and unlinks the blob; the row stays, and the file can be
 * fetched from Telegram again.
 *
 * A thread of its own, woken every sweep_secs and by requestSweep(). Only
 * the database (one pool connection) and the store directory are used.
 *
 * The generated stills a real sweep evicted (section 9.1) are handed to
 * the StillsEvictedFn once the sweep is over, so that they are taken
 * again from their videos: not during the sweep, which may still evict
 * the video too, and not before the sweep has moved past them.
 */
class StorageJanitor {
public:
	struct Config {
		std::string	storage_dir;
		/* Total cap in bytes (SUM(file_size) of stored files); 0 = none. */
		uint64_t	max_bytes = 0;
		/* Evict files not accessed for this many days; 0 = off. */
		uint32_t	retention_days = 90;
		/* Seconds between sweeps; 0 = only on request. */
		uint32_t	sweep_secs = 600;
		/* Report what would be evicted, change nothing. */
		bool		dry_run = true;
	};

	struct Phase {
		uint64_t	files = 0;	/* evicted, or would be */
		uint64_t	bytes = 0;
	};

	struct Sweep {
		int64_t		started_at = 0;	/* Unix seconds */
		int64_t		finished_at = 0;
		bool		dry_run = true;
		/* Finished; false: failed (error) or stopped at shutdown. */
		bool		completed = false;
		std::string	error;
		Phase		retention;
		Phase		cap;
		/* Stored bytes before the sweep and after it (or, dry, after
		 * the evictions it reports). */
		uint64_t	bytes_before = 0;
		uint64_t	bytes_after = 0;
		/* Rows the guard kept: accessed or re-stored mid-sweep. */
		uint64_t	kept = 0;
		/* Evicted rows whose blob was already gone. */
		uint64_t	missing_blobs = 0;
		/* Evicted rows whose blob could not be unlinked (now orphans). */
		uint64_t	unlink_errors = 0;
	};

	struct Status {
		Config			cfg;
		uint64_t		low_watermark = 0;	/* 0 if no cap */
		bool			sweeping = false;
		bool			pending = false;
		std::optional<Sweep>	last;
	};

	/* Rows listed per query, and evicted per batch. */
	static constexpr size_t kBatch = 1000;
	/* Evicted generated stills handed on per sweep at most; the rest
	 * wait for the stills pass at the next start. */
	static constexpr size_t kMaxStills = 10000;

	/* The ids of the generated stills one sweep evicted. */
	using StillsEvictedFn =
		std::function<void(const std::vector<uint64_t> &)>;

	StorageJanitor(DB *db, log_hd_t *l, Config cfg);
	~StorageJanitor(void);

	StorageJanitor(const StorageJanitor &) = delete;
	StorageJanitor &operator=(const StorageJanitor &) = delete;

	/* Where evicted generated stills go; before start(). */
	void setStillsEvicted(StillsEvictedFn fn) { stills_fn_ = std::move(fn); }

	void start(void);
	/* Stop promptly, mid-batch if need be, and join. Idempotent. */
	void stop(void);

	/*
	 * Ask for a sweep now. dry_run = false is refused (returns false)
	 * while the configuration says dry run. Requests made while one is
	 * pending merge into it; a real one wins over a dry one. The mode
	 * the pending sweep will run in is stored in *effective_dry.
	 */
	bool requestSweep(bool dry_run, bool *effective_dry = nullptr);

	Status status(void);

	/* The cap's low watermark: 95% of max_bytes. */
	static uint64_t lowWatermark(uint64_t max_bytes)
	{
		return max_bytes / 100 * 95 + max_bytes % 100 * 95 / 100;
	}

private:
	void loop(void);
	Sweep sweep(bool dry_run);
	/* Evict (or count) one listed row; false if the guard kept it. */
	bool evictOne(const EvictCandidate &c, bool dry_run, Sweep &s);
	/* Sleep between batches; true if stop() was called. */
	bool pace(bool dry_run);

	DB		*db_;
	log_hd_t	*l_;
	const Config	cfg_;
	StillsEvictedFn	stills_fn_;
	/* The generated stills the running sweep evicted; its thread only. */
	std::vector<uint64_t>	stills_;
	uint64_t		stills_dropped_ = 0;

	std::thread		thr_;
	std::mutex		mtx_;
	std::condition_variable	cv_;
	std::atomic<bool>	stop_{false};
	bool			started_ = false;
	/* Guarded by mtx_. */
	bool			pending_ = false;
	bool			pending_dry_ = true;
	bool			sweeping_ = false;
	std::optional<Sweep>	last_;
};

} /* namespace tgloggerd */

#endif /* #ifndef TGLOGGERD__STORAGE_JANITOR_HPP */
