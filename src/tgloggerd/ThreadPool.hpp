// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 */
#ifndef TGLOGGERD__THREAD_POOL_HPP
#define TGLOGGERD__THREAD_POOL_HPP

#include <cstddef>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
#include <functional>
#include <condition_variable>

#include "helpers/log.h"

namespace tgloggerd {

/*
 * A bounded FIFO thread pool of std::function<void()> jobs.
 *
 * With a single worker thread it is a strict serial executor: jobs run
 * one at a time in the exact order they were posted. With N threads jobs
 * run concurrently and no cross-job ordering is guaranteed.
 *
 * post() applies backpressure: it blocks while the queue is at capacity.
 * shutdown() stops accepting new jobs, drains everything already queued,
 * then joins the workers. Each job is run under a catch-all so a throwing
 * job logs and is skipped instead of terminating the process.
 */
class ThreadPool {
public:
	/*
	 * threads: number of worker threads (clamped up to 1).
	 * cap:     maximum queued jobs; 0 means unbounded.
	 * l:       logger used to report jobs that throw.
	 */
	ThreadPool(size_t threads, size_t cap, log_hd_t *l);
	~ThreadPool(void);

	ThreadPool(const ThreadPool &) = delete;
	ThreadPool &operator=(const ThreadPool &) = delete;

	/* Enqueue a job. Blocks if the queue is full. No-op after shutdown. */
	void post(std::function<void()> job);

	/* Stop accepting, drain queued jobs, then join. Idempotent. */
	void shutdown(void);

private:
	void run(void);

	size_t					cap_;
	log_hd_t				*l_;
	std::mutex				mtx_;
	std::condition_variable			not_empty_;
	std::condition_variable			not_full_;
	std::deque<std::function<void()>>	q_;
	bool					draining_ = false;
	std::vector<std::thread>		threads_;
};

} /* namespace tgloggerd */

#endif /* #ifndef TGLOGGERD__THREAD_POOL_HPP */
