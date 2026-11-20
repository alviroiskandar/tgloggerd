// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include "FileFetcher.hpp"
#include "InternalApi.hpp"

#include <td/telegram/td_api.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <utility>
#include <vector>

namespace tgloggerd {

namespace td_api = td::td_api;
namespace fs = std::filesystem;
using fetch::Clock;
using fetch::ErrorKind;

namespace {

/* Sources tried when the remote id no longer downloads. */
constexpr size_t kMaxSources = 8;
/*
 * A fetched small file's TDLib copy is deleted this long after it was
 * stored, not at once: ingest may have picked up the same completed file
 * for a message at the same moment and still be about to hash it.
 */
constexpr auto kPruneGrace = std::chrono::seconds(60);
/* How often to look at a download ingest is running. */
constexpr auto kPollInterval = std::chrono::milliseconds(250);
/* How often the reaper looks for idle files. */
constexpr auto kReapInterval = std::chrono::seconds(10);
/* Fetches come from a person waiting: TDLib's highest priority. */
constexpr int32_t kPriority = 32;
/* Bound on the row -> TDLib file id cache; cleared when full. */
constexpr size_t kMaxFidCache = 100000;

/* A TDLib error met while downloading, before it becomes an answer. */
struct TdFailure {
	fetch::ErrorClass	cls;
	std::string		msg;
};

const td_api::file *asFile(const TDLib::TdObject &o)
{
	if (!o || o->get_id() != td_api::file::ID)
		return nullptr;
	return static_cast<const td_api::file *>(o.get());
}

TdFailure failureOf(const TDLib::TdObject &o)
{
	if (o && o->get_id() == td_api::error::ID) {
		auto &e = static_cast<const td_api::error &>(*o);
		return { fetch::classifyError(e.code_, e.message_),
			 std::to_string(e.code_) + " " + e.message_ };
	}
	return { { ErrorKind::Other, 5 }, "unexpected answer from TDLib" };
}

[[noreturn]] void throwFor(const TdFailure &f)
{
	switch (f.cls.kind) {
	case ErrorKind::NotReady:
		throw InternalApi::Error::unavailable(
			"Telegram client not ready: " + f.msg, f.cls.retry_after);
	case ErrorKind::Timeout:
		throw InternalApi::Error::timeout(
			"Telegram did not answer in time");
	case ErrorKind::FloodWait:
		throw InternalApi::Error::unavailable(
			"Telegram asks to wait: " + f.msg, f.cls.retry_after);
	case ErrorKind::Access:
		throw InternalApi::Error::gone(
			"the file is gone from Telegram or not accessible: " +
			f.msg);
	case ErrorKind::Other:
		break;
	}
	throw InternalApi::Error::unavailable("Telegram error: " + f.msg,
					      f.cls.retry_after);
}

/*
 * Whether TDLib's file is the row's file. Only decidable when both unique
 * ids are known; a source whose message was edited or whose photo changed
 * carries another file, and its bytes must not land in this row.
 */
bool uniqueMatches(const FetchRow &row, const td_api::file &f)
{
	if (row.tg_unique_id.empty() || !f.remote_ ||
	    f.remote_->unique_id_.empty())
		return true;
	return row.tg_unique_id == f.remote_->unique_id_;
}

std::chrono::milliseconds left(Clock::time_point deadline)
{
	auto d = std::chrono::duration_cast<std::chrono::milliseconds>(
		deadline - Clock::now());
	return d.count() > 0 ? d : std::chrono::milliseconds(0);
}

/* A positive integer from the environment, else @def. */
uint64_t env_u64(const char *key, uint64_t def)
{
	const char *v = getenv(key);
	if (!v || !*v)
		return def;
	char *end = nullptr;
	unsigned long long n = strtoull(v, &end, 10);
	if (*end != '\0' || n == 0)
		return def;
	return (uint64_t)n;
}

} /* namespace */

void FileFetcher::Config::loadEnv(void)
{
	max_active = (size_t)env_u64("TG_FETCH_MAX_ACTIVE", 4);
	chunk = env_u64("TG_FETCH_CHUNK", 16ull << 20);
	timeout = std::chrono::seconds(env_u64("TG_FETCH_TIMEOUT_SECS", 30));
	idle = std::chrono::seconds(env_u64("TG_FETCH_IDLE_SECS", 900));
}

FileFetcher::FileFetcher(DB &db, TDLib &tdlib, log_hd_t *l, Config cfg,
			 StoreFn store, BlobFn blob_present,
			 HeldFn ingest_held)
	: db_(db), tdlib_(tdlib), l_(l), cfg_(std::move(cfg)),
	  store_(std::move(store)), blob_present_(std::move(blob_present)),
	  ingest_held_(std::move(ingest_held)),
	  gone_(std::chrono::hours(1)), limit_(cfg_.max_active ? cfg_.max_active : 1)
{
	if (cfg_.chunk == 0)
		cfg_.chunk = 16ull << 20;
	if (!cfg_.tdlib_dir.empty())
		cfg_.tdlib_dir =
			fs::absolute(cfg_.tdlib_dir).lexically_normal().string();
	while (cfg_.tdlib_dir.size() > 1 && cfg_.tdlib_dir.back() == '/')
		cfg_.tdlib_dir.pop_back();
}

FileFetcher::~FileFetcher(void)
{
	stop();
}

void FileFetcher::start(void)
{
	if (reaper_.joinable())
		return;
	reaper_stop_ = false;
	reaper_ = std::thread([this] { reaperLoop(); });
}

void FileFetcher::stop(void)
{
	{
		std::lock_guard<std::mutex> lk(reaper_mtx_);
		reaper_stop_ = true;
	}
	reaper_cv_.notify_all();
	if (reaper_.joinable())
		reaper_.join();
}

bool FileFetcher::isGone(uint64_t id)
{
	return gone_.contains(id, Clock::now());
}

/*
 * Delete what nobody needs any more: partial files idle for
 * TG_FETCH_IDLE_SECS and stored small files past their grace time. A file
 * ingest is downloading or still storing is left alone; ingest prunes it
 * itself.
 */
void FileFetcher::reaperLoop(void)
{
	std::unique_lock<std::mutex> lk(reaper_mtx_);
	while (!reaper_stop_) {
		reaper_cv_.wait_for(lk, kReapInterval,
				    [this] { return reaper_stop_; });
		if (reaper_stop_)
			break;
		lk.unlock();
		auto now = Clock::now();
		leases_.reap(now, cfg_.idle, [this](int32_t fid) {
			if (ingestNeeds(fid))
				return;
			tdlib_.deleteLocalFile(fid);
			pr_debug(l_, "fetch: deleted TDLib's copy of file %d",
				 (int)fid);
		});
		gone_.purge(now);
		lk.lock();
	}
}

/*
 * Whether ingest still needs TDLib's copy of @fid: downloading it, or
 * done downloading with store jobs left that have to read it.
 */
bool FileFetcher::ingestNeeds(int32_t fid) const
{
	return tdlib_.isIngestDownloading(fid) ||
	       (ingest_held_ && ingest_held_(fid));
}

void FileFetcher::rememberFid(uint64_t id, int32_t fid)
{
	std::lock_guard<std::mutex> lk(fids_mtx_);
	if (fids_.size() >= kMaxFidCache)
		fids_.clear();
	fids_[id] = fid;
}

void FileFetcher::forgetFid(uint64_t id)
{
	std::lock_guard<std::mutex> lk(fids_mtx_);
	fids_.erase(id);
}

TDLib::TdObject FileFetcher::call(TDLib::TdFunction f,
				  Clock::time_point deadline)
{
	auto ms = left(deadline);
	if (ms.count() == 0)
		return td_api::make_object<td_api::error>(
			TDLib::kRequestTimeout, "fetch timed out");
	return tdlib_.request(std::move(f), ms);
}

uint64_t FileFetcher::prefixAt(int32_t fid, uint64_t offset,
			       Clock::time_point deadline)
{
	auto o = call(td_api::make_object<td_api::getFileDownloadedPrefixSize>(
			      fid, (int64_t)offset), deadline);
	if (!o || o->get_id() != td_api::fileDownloadedPrefixSize::ID)
		return 0;
	auto &p = static_cast<const td_api::fileDownloadedPrefixSize &>(*o);
	return p.size_ > 0 ? (uint64_t)p.size_ : 0;
}

/* The row's bytes, or its alias target's, are in the store already. */
std::optional<FileFetcher::Result> FileFetcher::storedCopy(const FetchRow &row)
{
	auto present = [this](const FetchRow &r) {
		return r.on_disk && !r.sha256_hex.empty() &&
		       blob_present_(r.sha256_hex, r.ext);
	};
	Result res;
	res.stored = true;
	if (present(row)) {
		res.id = row.id;
		return res;
	}
	if (row.alias_of) {
		auto t = db_.getFileForFetch(*row.alias_of);
		if (t && present(*t)) {
			res.id = t->id;
			return res;
		}
	}
	return std::nullopt;
}

/*
 * The TDLib file for the row from what is known already: the cached TDLib
 * file id, else the row's remote id. nullptr when neither leads to the
 * file (try a fresh source); other errors are thrown as answers.
 */
TDLib::TdObject FileFetcher::resolveKnown(const FetchRow &row,
					  Clock::time_point deadline)
{
	int32_t fid = 0;
	{
		std::lock_guard<std::mutex> lk(fids_mtx_);
		auto it = fids_.find(row.id);
		if (it != fids_.end())
			fid = it->second;
	}
	if (fid != 0) {
		auto o = call(td_api::make_object<td_api::getFile>(fid),
			      deadline);
		if (const auto *f = asFile(o); f && uniqueMatches(row, *f))
			return o;
		forgetFid(row.id);
	}

	if (row.tg_file_id.empty())
		return nullptr;
	auto o = call(td_api::make_object<td_api::getRemoteFile>(
			      row.tg_file_id, nullptr), deadline);
	if (const auto *f = asFile(o)) {
		if (!uniqueMatches(row, *f))
			return nullptr;
		rememberFid(row.id, f->id_);
		return o;
	}
	TdFailure fl = failureOf(o);
	if (fl.cls.kind == ErrorKind::Access)
		return nullptr;
	throwFor(fl);
}

/*
 * The TDLib file for the row from a fresh source (storage-cache.md section
 * 8): what references it is fetched again, which gives TDLib a valid file
 * reference. Throws when no source works; when every source answered that
 * it no longer has the file, the file is remembered as gone.
 */
TDLib::TdObject FileFetcher::resolveFresh(const FetchRow &row,
					  Clock::time_point deadline)
{
	auto sources = db_.listFileSources(row.id, kMaxSources);
	std::optional<TdFailure> transient;
	bool tried_all = true;
	for (const auto &s : sources) {
		if (left(deadline).count() == 0) {
			tried_all = false;
			break;
		}
		auto o = tdlib_.resolveFile(s, left(deadline));
		if (const auto *f = asFile(o)) {
			if (!uniqueMatches(row, *f))
				continue;
			rememberFid(row.id, f->id_);
			return o;
		}
		TdFailure fl = failureOf(o);
		if (fl.cls.kind == ErrorKind::NotReady)
			throwFor(fl);
		if (fl.cls.kind != ErrorKind::Access)
			transient = fl;
	}
	if (transient)
		throwFor(*transient);
	if (!tried_all)
		throw InternalApi::Error::timeout(
			"Telegram did not answer in time");

	gone_.add(row.id, Clock::now());
	pr_info(l_, "fetch: file %llu is gone from Telegram (%zu source(s)"
		" tried)", (unsigned long long)row.id, sources.size());
	throw InternalApi::Error::gone(sources.empty()
		? "nothing on Telegram references the file any more"
		: "no source on Telegram still carries the file");
}

FileFetcher::Result FileFetcher::fetch(uint64_t id, uint64_t offset,
				       uint64_t length)
{
	if (gone_.contains(id, Clock::now()))
		throw InternalApi::Error::gone(
			"the file was found gone from Telegram within the hour");

	fetch::ActiveLimit::Slot slot(limit_);
	if (!slot.held())
		throw InternalApi::Error::unavailable(
			"too many fetches running", 1);
	const auto deadline = Clock::now() + cfg_.timeout;

	auto row = db_.getFileForFetch(id);
	if (!row)
		throw InternalApi::Error::notFound("unknown file");
	if (auto r = storedCopy(*row))
		return *r;
	/*
	 * A still the daemon took from a stored video: Telegram never had
	 * it. Its message's sources would hand over the message's own
	 * Telegram thumbnail, if it has one now, and those bytes must not
	 * land in this row.
	 */
	if (isGeneratedFileId(row->tg_file_id))
		throw InternalApi::Error(410, "generated",
					 "the file is a still generated from a"
					 " stored video; it is not on Telegram"
					 " and is generated again later");

	bool fresh = false;
	TDLib::TdObject file = resolveKnown(*row, deadline);
	if (!asFile(file)) {
		file = resolveFresh(*row, deadline);
		fresh = true;
	}
	for (;;) {
		try {
			return fetchFile(*row, file, offset, length, deadline);
		} catch (const TdFailure &f) {
			if (f.cls.kind != ErrorKind::Access)
				throwFor(f);
			if (fresh) {
				gone_.add(id, Clock::now());
				pr_info(l_, "fetch: file %llu is gone from"
					" Telegram: %s",
					(unsigned long long)id, f.msg.c_str());
				throwFor(f);
			}
			/* The remote id's file reference has expired. */
			pr_debug(l_, "fetch: file %llu: %s; trying a fresh"
				 " source", (unsigned long long)id,
				 f.msg.c_str());
			forgetFid(id);
			file = resolveFresh(*row, deadline);
			fresh = true;
		}
	}
}

/*
 * Fetch with the TDLib file in hand, holding its lease and its lock so no
 * other fetch moves its download range meanwhile.
 */
FileFetcher::Result FileFetcher::fetchFile(const FetchRow &row,
					   const TDLib::TdObject &file,
					   uint64_t offset, uint64_t length,
					   Clock::time_point deadline)
{
	const td_api::file &f = *asFile(file);
	const int32_t fid = f.id_;
	uint64_t size = f.size_ > 0 ? (uint64_t)f.size_ : row.file_size;
	const uint64_t policy_size = size ? size
		: (uint64_t)std::max<int64_t>(0, f.expected_size_);
	const bool small = cfg_.max_store_file_size == 0 ||
			   policy_size < cfg_.max_store_file_size;
	const std::string unique_id =
		(f.remote_ && !f.remote_->unique_id_.empty())
			? f.remote_->unique_id_ : row.tg_unique_id;

	bool partial = false;
	std::optional<Clock::time_point> prune_at;
	auto lease = leases_.acquire(fid);
	struct Release {
		fetch::LeaseTable			&t;
		int32_t					fid;
		bool					&partial;
		std::optional<Clock::time_point>	&prune_at;
		~Release()
		{
			t.release(fid, Clock::now(), partial, prune_at);
		}
	} release{ leases_, fid, partial, prune_at };

	std::unique_lock<std::timed_mutex> lk(lease->mtx, std::defer_lock);
	if (!lk.try_lock_until(deadline))
		throw InternalApi::Error::timeout(
			"another fetch of this file is still running");

	if (small) {
		auto r = fetchWhole(row, fid, (int64_t)size, unique_id,
				    deadline, prune_at);
		if (r)
			return *r;
		/* Bigger than its size said: serve it as a big file. */
		auto st = call(td_api::make_object<td_api::getFile>(fid),
			       deadline);
		if (const auto *nf = asFile(st); nf && nf->size_ > 0)
			size = (uint64_t)nf->size_;
	}
	partial = true;
	return fetchRange(row, fid, size, offset, length, deadline);
}

/*
 * A small file: download it whole and store it as ingest would. nullopt
 * when the downloaded file turns out to be at or over the store cap.
 */
std::optional<FileFetcher::Result>
FileFetcher::fetchWhole(const FetchRow &row, int32_t fid, int64_t size,
			const std::string &unique_id, Clock::time_point deadline,
			std::optional<Clock::time_point> &prune_at)
{
	/*
	 * Offset 0 and no limit are what ingest asks for too, so a download
	 * ingest already runs is not disturbed: TDLib only raises its
	 * priority, and the synchronous call returns when it completes.
	 */
	for (int attempt = 0; attempt < 2; attempt++) {
		auto o = call(td_api::make_object<td_api::downloadFile>(
				      fid, kPriority, 0, 0, true), deadline);
		const auto *f = asFile(o);
		if (!f)
			throw failureOf(o);
		if (!f->local_ || !f->local_->is_downloading_completed_ ||
		    f->local_->path_.empty()) {
			if (left(deadline).count() == 0)
				throw InternalApi::Error::timeout(
					"Telegram did not finish the download"
					" in time");
			throw TdFailure{ { ErrorKind::Other, 1 },
					 "the download stopped before it"
					 " completed" };
		}

		const std::string &path = f->local_->path_;
		std::error_code ec;
		uintmax_t actual = fs::file_size(path, ec);
		if (!ec && cfg_.max_store_file_size != 0 &&
		    actual >= cfg_.max_store_file_size)
			return std::nullopt;

		int64_t fsize = f->size_ > 0 ? f->size_ : size;
		auto sid = store_(path, row.tg_file_id, unique_id, fsize,
				  row.file_type, row.orig_file_name);
		if (sid) {
			if (cfg_.prune_stored && !ingestNeeds(fid))
				prune_at = Clock::now() + kPruneGrace;
			try {
				db_.touchFiles({ row.id, *sid });
			} catch (const std::exception &e) {
				pr_error(l_, "fetch: could not touch file %llu:"
					 " %s", (unsigned long long)row.id,
					 e.what());
			}
			pr_info(l_, "fetch: stored file %llu (row %llu)",
				(unsigned long long)row.id,
				(unsigned long long)*sid);
			Result r;
			r.stored = true;
			r.id = *sid;
			return r;
		}
		/*
		 * Not stored: ingest may have stored the same file and pruned
		 * TDLib's copy under us. Then the row has its bytes already;
		 * otherwise download it again, once.
		 */
		if (auto again = db_.getFileForFetch(row.id))
			if (auto r = storedCopy(*again))
				return r;
	}
	throw InternalApi::Error(500, "internal",
				 "the downloaded file could not be stored");
}

/*
 * A big file: make [offset, offset + n) local in TDLib's cache, n being
 * at most one chunk, and say where it is. Never copied into the store.
 */
FileFetcher::Result FileFetcher::fetchRange(const FetchRow &row, int32_t fid,
					    uint64_t size, uint64_t offset,
					    uint64_t length,
					    Clock::time_point deadline)
{
	if (size != 0 && offset >= size)
		throw InternalApi::Error(416, "bad_range",
					 "offset is at or past the end of the"
					 " file");
	const uint64_t want = fetch::rangeLength(offset, length, size,
						 cfg_.chunk);

	uint64_t prefix = prefixAt(fid, offset, deadline);
	if (prefix < want) {
		auto st = call(td_api::make_object<td_api::getFile>(fid),
			       deadline);
		const auto *f = asFile(st);
		if (!f)
			throw failureOf(st);
		const bool ingest = tdlib_.isIngestDownloading(fid) &&
				    f->local_ &&
				    f->local_->is_downloading_active_;
		if (ingest) {
			/*
			 * Ingest downloads the whole file; a ranged
			 * downloadFile would cut it short. Raise its
			 * priority with the same (0, 0) range and wait for
			 * the bytes to arrive.
			 */
			call(td_api::make_object<td_api::downloadFile>(
				     fid, kPriority, 0, 0, false), deadline);
			while (prefix < want &&
			       left(deadline) > kPollInterval) {
				std::this_thread::sleep_for(kPollInterval);
				prefix = prefixAt(fid, offset, deadline);
			}
		} else {
			auto o = call(td_api::make_object<td_api::downloadFile>(
					      fid, kPriority, (int64_t)offset,
					      (int64_t)want, true), deadline);
			if (!asFile(o))
				throw failureOf(o);
			prefix = prefixAt(fid, offset, deadline);
		}
	}
	if (prefix == 0) {
		if (left(deadline).count() == 0)
			throw InternalApi::Error::timeout(
				"Telegram did not deliver the range in time");
		throw TdFailure{ { ErrorKind::Other, 1 },
				 "no bytes downloaded at the offset" };
	}

	auto st = call(td_api::make_object<td_api::getFile>(fid), deadline);
	const auto *f = asFile(st);
	if (!f)
		throw failureOf(st);
	if (!f->local_ || f->local_->path_.empty())
		throw TdFailure{ { ErrorKind::Other, 1 },
				 "TDLib reports no local path" };
	if (size == 0 && f->size_ > 0)
		size = (uint64_t)f->size_;

	/* The web serves this path; it must be TDLib's and absolute. */
	std::string path =
		fs::absolute(f->local_->path_).lexically_normal().string();
	if (cfg_.tdlib_dir.empty() ||
	    path.compare(0, cfg_.tdlib_dir.size() + 1,
			 cfg_.tdlib_dir + "/") != 0) {
		pr_error(l_, "fetch: file %llu: TDLib's path %s is outside %s",
			 (unsigned long long)row.id, path.c_str(),
			 cfg_.tdlib_dir.c_str());
		throw InternalApi::Error(500, "internal",
					 "TDLib's file is outside its"
					 " directory");
	}

	Result r;
	r.path = std::move(path);
	r.offset = offset;
	r.available = fetch::availableBytes(offset, prefix, size);
	r.size = size;
	return r;
}

} /* namespace tgloggerd */
