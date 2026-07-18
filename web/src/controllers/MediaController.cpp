// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include "controllers/MediaController.hpp"

#include "Config.hpp"
#include "auth/Session.hpp"
#include "controllers/Common.hpp"
#include "dao/Browse.hpp"
#include "dao/Audit.hpp"

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>

namespace tgweb::controllers {

namespace {

/* Storage root; must equal the daemon's TG_STORAGE_DIR. Read once. */
const std::string &storageDir(void)
{
	static const std::string dir =
		tgweb::env("WEB_STORAGE_DIR", "data/storage/files");
	return dir;
}

bool isHex64(const std::string &s)
{
	if (s.size() != 64)
		return false;
	for (char c : s)
		if (!std::isxdigit((unsigned char)c))
			return false;
	return true;
}

/* Keep only [a-z0-9] from a file extension, lower-cased, at most 10 chars. */
std::string safeExt(const std::string &e)
{
	std::string o;
	for (char c : e) {
		if (std::isalnum((unsigned char)c))
			o += (char)std::tolower((unsigned char)c);
		if (o.size() >= 10)
			break;
	}
	return o;
}

/* Strip characters unsafe in a Content-Disposition filename. */
std::string safeFilename(const std::string &n)
{
	std::string o;
	for (unsigned char c : n) {
		if (c < 0x20 || c == '"' || c == '\\' || c == '/')
			continue;
		o += (char)c;
	}
	return o;
}

/* Content is viewed inline; documents are offered as a download. */
bool viewInline(const std::string &fileType)
{
	return fileType == "photo" || fileType == "video" ||
	       fileType == "audio" || fileType == "voice" ||
	       fileType == "animation" || fileType == "sticker";
}

} /* namespace */

drogon::Task<drogon::HttpResponsePtr>
MediaController::serve(drogon::HttpRequestPtr req, std::string id)
{
	auto ro = drogon::app().getDbClient("ro");
	int64_t fid = strtoll(id.c_str(), nullptr, 10);

	auto meta = co_await dao::browse::getFile(ro, fid);
	if (!meta || !isHex64(meta->hex))
		co_return renderStatus(req, drogon::k404NotFound, "File not found",
				       "No file exists with that id.");

	/* DB-derived path only: <dir>/aa/bb/cc/dd/ee/<hex>[.ext]. */
	std::string ext = safeExt(meta->ext);
	std::filesystem::path path = storageDir();
	for (int i = 0; i < 5; i++)
		path /= meta->hex.substr((size_t)i * 2, 2);
	std::string name = meta->hex;
	if (!ext.empty())
		name += "." + ext;
	path /= name;

	std::error_code ec;
	if (!std::filesystem::is_regular_file(path, ec))
		co_return renderStatus(req, drogon::k404NotFound, "File not found",
				       "The file is recorded but not in storage.");

	/* Audit the access before serving. */
	auto app = drogon::app().getDbClient("app");
	auto sess = auth::session::current(req);
	std::optional<uint64_t> uid =
		sess ? std::optional<uint64_t>(sess->uid) : std::nullopt;
	co_await dao::audit::log(app, uid, "media",
				 req->getPeerAddr().toIp(), "file " + id);

	std::string attachment;
	if (!viewInline(meta->fileType)) {
		attachment = safeFilename(meta->origName);
		if (attachment.empty())
			attachment = name;
	}

	/* Passing req lets Drogon honour Range and conditional requests; the
	 * on-disk extension drives the Content-Type. */
	co_return drogon::HttpResponse::newFileResponse(
		path.string(), attachment, drogon::CT_NONE, "", req);
}

} /* namespace tgweb::controllers */
