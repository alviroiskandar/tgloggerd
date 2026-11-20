// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef TGLOGGERD__STILLS_WORKER_HPP
#define TGLOGGERD__STILLS_WORKER_HPP

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "helpers/log.h"
#include "DB.hpp"

namespace tgloggerd {

/*
 * Generates stills for the stored videos Telegram sent no thumbnail for
 * (TG_GENERATE_STILLS; docs/design/message-types.md section 4.3): a thread
 * of its own that asks the stills service for a frame of each such video,
 * one at a time and about one a second, puts it in the store and links it
 * to the video's messages. It never runs on the TDLib loop or the write
 * queue, so storing messages does not wait for it.
 *
 * Work comes from three queues: the videos of the pass over the archive,
 * listed when the thread starts (newest first); new messages whose main
 * file was stored (messageFileStored), which go first once they are a
 * little old, so a Telegram thumbnail stored just after the video is
 * seen first; and the generated stills the storage janitor evicted
 * (stillsEvicted), whose videos go before the pass's. Each is checked against the database again before a frame
 * is asked for (a new message by itself, cheaply, whatever its file is).
 * A video that already has a generated still gets it linked, not made
 * again. A video the service refuses is not asked for again until the
 * daemon restarts.
 */
class StillsWorker {
public:
	struct Config {
		/* The stills service's socket (TG_STILLS_SOCKET). */
		std::string			socket_path;
		/* The file store (TG_STORAGE_DIR), to check the video's blob. */
		std::string			storage_dir;
		/* Where an answer's bytes are written before they are stored;
		 * created if missing. */
		std::string			tmp_dir;
		/* At least this long between two stills. */
		std::chrono::milliseconds	pace{1000};
		/* A new message's video waits this long first. */
		std::chrono::seconds		new_delay{30};
		/* After the service could not be reached. */
		std::chrono::seconds		retry_after{60};
		/* Bound on one exchange with the service. */
		std::chrono::seconds		timeout{75};
		/* New videos queued at most; more wait for the next pass. */
		size_t				max_new = 10000;
	};

	/* A file put in the store: digest, extension, bytes copied. */
	struct Stored {
		std::string	sha256_hex;
		std::string	ext;
		bool		on_disk = false;
	};
	/* Put the file at a path in the store (TgLoggerd::putInStore). */
	using StoreFn = std::function<std::optional<Stored>(const std::string &)>;
	/* A still was linked to these messages (chat_id, message_id). */
	using LinkedFn = std::function<void(
		uint64_t, const std::vector<std::pair<int64_t, int64_t>> &)>;

	StillsWorker(DB &db, log_hd_t *l, Config cfg, StoreFn store,
		     LinkedFn linked);
	~StillsWorker(void);

	StillsWorker(const StillsWorker &) = delete;
	StillsWorker &operator=(const StillsWorker &) = delete;

	/* Start the thread; it lists the archive's candidates first. */
	void start(void);
	/* Stop it, cutting a running exchange short. Idempotent. */
	void stop(void);

	/* A message's main file is stored or linked: maybe a still to do. */
	void messageFileStored(bool is_group, int64_t chat_id,
			       int64_t message_id);

	/*
	 * The storage janitor evicted these generated stills: take them
	 * again from the videos they stand for (DB::listStillVideos). Up to
	 * max_new stills and as many videos are queued; more wait for the
	 * pass at the next start.
	 */
	void stillsEvicted(const std::vector<uint64_t> &still_ids);

	/* The service's kind for a message's content_type; empty if none. */
	static std::string kindOf(const std::string &content_type);

private:
	using Clock = std::chrono::steady_clock;

	/* What one video came to. */
	enum class Outcome {
		Nothing,	/* nothing to do: not paced */
		Worked,		/* asked the service, or linked a still */
		Away,		/* the service is not there: try it later */
	};

	/* A new message to look at. */
	struct Fresh {
		bool			is_group;
		int64_t			chat_id;
		int64_t			message_id;
		Clock::time_point	due;
	};

	void loop(void);
	/* Queue the videos evicted still @still_id stands for. */
	void queueStillVideos(uint64_t still_id);
	Outcome process(const StillCandidate &c);
	Outcome linkStill(uint64_t video_file_id, uint64_t still_id);
	/* Wait until @t or stop; false when stopping. */
	bool sleepUntil(Clock::time_point t);

	DB			&db_;
	log_hd_t		*l_;
	Config			cfg_;
	StoreFn			store_;
	LinkedFn		linked_;

	std::mutex		mtx_;
	std::condition_variable	cv_;
	bool			stop_ = false;
	std::deque<uint64_t>	backlog_;
	std::deque<Fresh>	fresh_;
	/* (chat_id, message_id) of fresh_; chat ids tell the tables apart. */
	std::set<std::pair<int64_t, int64_t>> fresh_ids_;
	/* Evicted generated stills, and the videos they stand for. */
	std::deque<uint64_t>	evicted_;
	std::unordered_set<uint64_t> evicted_ids_;
	std::deque<uint64_t>	regen_;
	std::unordered_set<uint64_t> regen_ids_;
	/* Refused by the service; worker thread only. */
	std::unordered_set<uint64_t> refused_;
	size_t			backlog_total_ = 0;
	size_t			backlog_done_ = 0;
	uint64_t		made_ = 0;
	/* The service was found away and is not back yet. */
	bool			away_ = false;
	/* The socket of the exchange running, for stop(). */
	std::atomic<int>	active_fd_{ -1 };
	std::thread		thr_;
};

} /* namespace tgloggerd */

#endif /* #ifndef TGLOGGERD__STILLS_WORKER_HPP */
