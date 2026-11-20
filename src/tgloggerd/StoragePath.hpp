// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef TGLOGGERD__STORAGE_PATH_HPP
#define TGLOGGERD__STORAGE_PATH_HPP

#include <string>
#include <filesystem>
#include <system_error>

namespace tgloggerd {

/*
 * Where a blob lives in the content-addressed store: 5 levels of two-hex-digit
 * directories from the first 5 octets of the digest, then <sha256>[.ext]. For
 * 0a533d97ed... that is <storage_dir>/0a/53/3d/97/ed/0a533d97ed...[.ext].
 *
 * The one definition of the layout in the daemon: storeDownloadedFile writes
 * here and the StorageJanitor unlinks here. The web builds the same path in
 * web/src/controllers/MediaController.cpp. sha256_hex must be 64 hex digits;
 * ext is lowercase without the dot, or empty.
 */
inline std::filesystem::path blobPath(const std::string &storage_dir,
				      const std::string &sha256_hex,
				      const std::string &ext)
{
	std::filesystem::path p = storage_dir;
	for (size_t i = 0; i < 5; i++)
		p /= sha256_hex.substr(i * 2, 2);
	std::string name = sha256_hex;
	if (!ext.empty())
		name += "." + ext;
	return p / name;
}

/*
 * Give the blob stored as <sha256>[.from_ext] the name <sha256>[.to_ext]
 * that its telegram_files row gives it: the bytes are stored under the
 * extension of the local path they came from, which can differ from the
 * one the row already had (a row recorded metadata-only first, or the
 * same bytes stored before under another extension). The row is the one
 * of those bytes (sha256 is unique), so nothing else uses the old name:
 * when the new name already exists, the old copy is removed instead.
 * Returns true when the blob is under the new name afterwards; @ec says
 * why not otherwise. A concurrent store of the same bytes may move or
 * remove the old copy first; that is not an error.
 */
inline bool renameBlob(const std::string &storage_dir,
		       const std::string &sha256_hex,
		       const std::string &from_ext, const std::string &to_ext,
		       std::error_code &ec)
{
	namespace fs = std::filesystem;
	const fs::path from = blobPath(storage_dir, sha256_hex, from_ext);
	const fs::path to = blobPath(storage_dir, sha256_hex, to_ext);
	std::error_code ignored;

	ec.clear();
	if (from == to)
		return fs::exists(to, ec);
	if (fs::exists(to, ec)) {
		fs::remove(from, ignored);
		return true;
	}
	if (ec)
		return false;
	fs::rename(from, to, ec);
	if (!ec)
		return true;
	if (fs::exists(to, ignored)) {
		ec.clear();
		return true;
	}
	return false;
}

} /* namespace tgloggerd */

#endif /* #ifndef TGLOGGERD__STORAGE_PATH_HPP */
