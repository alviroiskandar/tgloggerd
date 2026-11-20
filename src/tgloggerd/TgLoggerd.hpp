// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 */
#ifndef TGLOGGERD__TGLOGGERD_HPP
#define TGLOGGERD__TGLOGGERD_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <optional>
#include <unordered_map>
#include <mutex>
#include "helpers/log.h"
#include "TDLib.hpp"
#include "DB.hpp"
#include "ThreadPool.hpp"
#include "DiscordForwarder.hpp"
#include "StoreHolds.hpp"
#include "FileIndex.hpp"

namespace tgloggerd {

class InternalApi;
class BotWatch;
class EventBus;
class StorageJanitor;
class FileFetcher;
class Contacts;
class StillsWorker;

class TgLoggerd {
public:
	TgLoggerd(uint32_t api_id, const char *api_hash, const char *data_dir) noexcept;
	~TgLoggerd(void);
	int start(void);
	int stop(void);
	void setLogger(log_hd_t *h) noexcept;
	void setLoggerLevel(int8_t log_level) noexcept;

private:
	inline int initDataDir(void);
	inline int initDataDirC(const char *dir);
	/* A file put in the store: its digest, extension, and whether its
	 * bytes were copied (not over the per-file cap). */
	struct Blob {
		std::string	sha256_hex;
		std::string	ext;
		bool		on_disk;
	};
	std::optional<Blob> putInStore(const std::string &local_path,
				       int64_t file_size,
				       const std::string &what);
	std::optional<uint64_t> storeDownloadedFile(const std::string &local_path,
						    const std::string &tg_file_id,
						    const std::string &tg_unique_id,
						    int64_t file_size,
						    const char *file_type,
						    const std::string &orig_file_name = std::string());
	void onProfilePhoto(const ProfilePhoto &p);
	void onGroupPhoto(const GroupPhoto &p);
	void onMessageFile(const MessageFile &m);
	void onMessageFileLink(const MessageFileLink &lk);
	void onPhotoLink(const PhotoLink &pl);
	void onFileMeta(const FileMeta &m);
	uint64_t recordCommunityPhoto(const std::optional<models::File> &f);
	void onCommunity(const models::Community &c);
	void onCommunityFullInfo(const models::CommunityFullInfo &fi);

	/*
	 * The log handle, owned through log_owner_ and used through l_.
	 * Declared first so it is freed last, after every member below has
	 * been destroyed: the API, the fetcher, the janitor, the pools, the
	 * Discord forwarder and TDLib all hold l_ and may log from their
	 * destructors or from a thread they are still joining.
	 */
	struct LogFree {
		void operator()(log_hd_t *h) const noexcept { pr_free(h); }
	};
	std::unique_ptr<log_hd_t, LogFree> log_owner_;
	log_hd_t *l_ = nullptr;

	uint32_t api_id_;
	char api_hash_[64];
	char data_dir_[512];
	std::string storage_dir_;
	/* Delete TDLib's own cached copy after storing our own (avoid dup). */
	bool prune_tdlib_files_ = true;
	/*
	 * Files whose size is >= this many bytes are not copied into the store
	 * (only their metadata is recorded, so they stay identifiable/
	 * re-downloadable by tg_file_id); 0 disables the limit. Default 1 GiB.
	 */
	uint64_t max_store_file_size_ = 1073741824ULL;
	/* Record media from non-member chats without its bytes. */
	bool skip_nonmember_media_ = true;
	/*
	 * In-memory index of recorded files, by remote id and by unique id
	 * (FileIndex.hpp). Seeded at startup and updated on each store and
	 * each metadata-only record.
	 */
	FileIndex known_files_;
	/*
	 * The store jobs still reading each TDLib local file, so a file
	 * several messages wait on is pruned after the last job, not under
	 * the others (StoreHolds.hpp). Declared before files_ so it outlives
	 * every job; the prune uses tdlib_, which also outlives them.
	 */
	StoreHolds store_holds_{ [this](int32_t fid) {
		if (prune_tdlib_files_)
			tdlib_->deleteLocalFile(fid);
	} };
	std::unique_ptr<TDLib> tdlib_;
	std::unique_ptr<DB> db_;

	/*
	 * What the workers change in the archive, published after each write
	 * for GET /v1/events (EventBus.hpp). Declared before the pools and
	 * the API, which publish to and read from it, so it outlives them.
	 */
	std::unique_ptr<EventBus> events_;

	/* Publishes bot.updated when a bot row changes (BotWatch.hpp). */
	std::unique_ptr<BotWatch> bot_watch_;

	/*
	 * Mirrors live messages to Discord; owns its own HTTP pool and a refresh
	 * thread that reads telegram_discord_webhooks via db_. Declared after db_ so it is
	 * destroyed before db_; start() also stops it explicitly after the loop.
	 */
	std::unique_ptr<DiscordForwarder> discord_;

	/*
	 * Persistence runs off the TDLib event loop. serial_ is a single
	 * worker: it owns all DB writes and the file->row link updates, so
	 * their order (and thus every FK/edit/delete invariant) is preserved.
	 * files_ hashes/copies downloaded media in parallel, then hands the
	 * final link update back to serial_. Declared last so they are joined
	 * before db_ on destruction; start() also drains them explicitly.
	 */
	std::unique_ptr<ThreadPool> serial_;
	/* Per recheck page (RecheckAnswer::seq): how many of its messages
	 * were not stored before; serial_ only. */
	std::unordered_map<uint64_t, size_t> recheck_new_;
	std::unique_ptr<ThreadPool> files_;

	/*
	 * Evicts files from the store by retention and by the total cap. Uses
	 * db_ only; start() stops it before draining the pools. Declared after
	 * db_ and before api_, whose storage routes call it.
	 */
	std::unique_ptr<StorageJanitor> janitor_;

	/*
	 * Generates stills for stored videos without a thumbnail
	 * (TG_GENERATE_STILLS). Uses db_ and the store, and posts its events
	 * to serial_; start() stops it with the janitor, before the pools
	 * drain. Null while off.
	 */
	std::unique_ptr<StillsWorker> stills_;

	/*
	 * Fetches files from Telegram for the API's fetch route; stopped
	 * right after the API, before the pools drain.
	 */
	std::unique_ptr<FileFetcher> fetcher_;

	/*
	 * The real-name lookup of contacts and its boot-time restore
	 * (ContactLookup.hpp). Uses tdlib_, db_ and serial_; stopped before
	 * the API in start(). Declared before api_, whose handler uses
	 * it, so it is destroyed after it.
	 */
	std::unique_ptr<Contacts> contacts_;

	/*
	 * The internal HTTP API for the web (docs/internal-api.md). Its
	 * handlers use db_ and tdlib_, so start() stops it before draining
	 * the pools; declared last so it is also destroyed first.
	 */
	std::unique_ptr<InternalApi> api_;
};

} /* namespace tgloggerd */
#endif /* #ifndef TGLOGGERD__TGLOGGERD_HPP */
