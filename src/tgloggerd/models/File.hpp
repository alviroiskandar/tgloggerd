// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 */
#ifndef TGLOGGERD__MODELS__FILE_HPP
#define TGLOGGERD__MODELS__FILE_HPP

#include <string>
#include <cstdint>
#include <optional>

namespace tgloggerd {
namespace models {

/*
 * A downloaded file recorded in the files table. Files are de-duplicated
 * by their SHA-256 digest.
 */
struct File {
	/* TDLib persistent remote file identifier (remoteFile.id_). */
	std::string	tg_file_id;
	/* telegram_files.file_type enum value, e.g. "photo". */
	std::string	file_type = "unknown";
	uint64_t	file_size = 0;
	/* 64-character lowercase hex of the SHA-256 digest. */
	std::string	sha256_hex;
	/* Lowercase file extension without the leading dot; may be empty. */
	std::optional<std::string>	file_ext;
	/* Original Telegram file name; empty if the file carries none. */
	std::string	orig_file_name;
	/*
	 * Whether the file's bytes are kept in the store. false = metadata-only
	 * (too large per TG_MAX_STORE_FILE_SIZE), re-downloadable by tg_file_id.
	 */
	bool		on_disk = true;
};

} /* namespace models */
} /* namespace tgloggerd */

#endif /* #ifndef TGLOGGERD__MODELS__FILE_HPP */
