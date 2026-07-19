// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 */
#ifndef TGLOGGERD__TGLOGGERD_HPP
#define TGLOGGERD__TGLOGGERD_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <optional>
#include "helpers/log.h"
#include "TDLib.hpp"
#include "DB.hpp"
#include "ThreadPool.hpp"

namespace tgloggerd {

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
	std::optional<uint64_t> storeDownloadedFile(const std::string &local_path,
						    const std::string &tg_file_id,
						    int64_t file_size,
						    const char *file_type,
						    const std::string &orig_file_name = std::string());
	void onProfilePhoto(const ProfilePhoto &p);
	void onGroupPhoto(const GroupPhoto &p);
	void onMessageFile(const MessageFile &m);

	uint32_t api_id_;
	char api_hash_[64];
	char data_dir_[512];
	std::string storage_dir_;
	/* Delete TDLib's own cached copy after storing our own (avoid dup). */
	bool prune_tdlib_files_ = true;
	log_hd_t *l_ = nullptr;
	std::unique_ptr<TDLib> tdlib_;
	std::unique_ptr<DB> db_;

	/*
	 * Persistence runs off the TDLib event loop. serial_ is a single
	 * worker: it owns all DB writes and the file->row link updates, so
	 * their order (and thus every FK/edit/delete invariant) is preserved.
	 * files_ hashes/copies downloaded media in parallel, then hands the
	 * final link update back to serial_. Declared last so they are joined
	 * before db_ on destruction; start() also drains them explicitly.
	 */
	std::unique_ptr<ThreadPool> serial_;
	std::unique_ptr<ThreadPool> files_;
};

} /* namespace tgloggerd */
#endif /* #ifndef TGLOGGERD__TGLOGGERD_HPP */
