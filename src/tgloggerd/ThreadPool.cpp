// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 */
#include <tgloggerd/ThreadPool.hpp>

#include <utility>

namespace tgloggerd {

ThreadPool::ThreadPool(size_t threads, size_t cap, log_hd_t *l)
	: cap_(cap)
	, l_(l)
{
	if (threads < 1)
		threads = 1;
	threads_.reserve(threads);
	for (size_t i = 0; i < threads; i++)
		threads_.emplace_back([this] { run(); });
}

ThreadPool::~ThreadPool(void)
{
	shutdown();
}

void ThreadPool::post(std::function<void()> job)
{
	std::unique_lock<std::mutex> lock(mtx_);

	/* Block while full (backpressure); wake early if shutting down. */
	if (cap_) {
		not_full_.wait(lock, [this] {
			return q_.size() < cap_ || draining_;
		});
	}

	/* Once draining, silently drop: no new work is accepted. */
	if (draining_)
		return;

	q_.push_back(std::move(job));
	not_empty_.notify_one();
}

void ThreadPool::shutdown(void)
{
	{
		std::lock_guard<std::mutex> lock(mtx_);
		draining_ = true;
	}
	/* Wake workers (to drain and exit) and any blocked post()s. */
	not_empty_.notify_all();
	not_full_.notify_all();

	for (auto &t : threads_) {
		if (t.joinable())
			t.join();
	}
}

void ThreadPool::run(void)
{
	for (;;) {
		std::function<void()> job;
		{
			std::unique_lock<std::mutex> lock(mtx_);
			not_empty_.wait(lock, [this] {
				return !q_.empty() || draining_;
			});

			/*
			 * Exit only once draining AND the queue is empty, so
			 * shutdown() drains all posted jobs before joining.
			 */
			if (q_.empty())
				return;

			job = std::move(q_.front());
			q_.pop_front();
		}
		not_full_.notify_one();

		try {
			job();
		} catch (...) {
			pr_error(l_, "ThreadPool: a job threw an exception");
		}
	}
}

} /* namespace tgloggerd */
