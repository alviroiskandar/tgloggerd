// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include "controllers/MediaController.hpp"

#include "Config.hpp"
#include "auth/FileToken.hpp"
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

/* A single parsed byte range. present: a "bytes=..." header was given;
 * satisfiable: it maps to a real [offset, offset+length) slice. */
struct ByteRange {
	bool	present = false;
	bool	satisfiable = false;
	size_t	offset = 0;
	size_t	length = 0;
};

/*
 * Parse the first range of an HTTP Range header ("bytes=start-end",
 * "bytes=start-", "bytes=-suffix"). Only a single range is honoured; a
 * multi-range request falls back to its first range. Returns present=false
 * for anything that is not a byte range, and satisfiable=false for a byte
 * range that cannot be met (so the caller answers 416).
 */
ByteRange parseRange(const std::string &header, uint64_t filesize)
{
	ByteRange r;
	if (header.rfind("bytes=", 0) != 0 || filesize == 0)
		return r;

	std::string spec = header.substr(6);
	auto comma = spec.find(',');
	if (comma != std::string::npos)
		spec = spec.substr(0, comma);
	auto dash = spec.find('-');
	if (dash == std::string::npos)
		return r;

	r.present = true;
	std::string s = spec.substr(0, dash);
	std::string e = spec.substr(dash + 1);
	auto num = [](const std::string &x) -> uint64_t {
		return strtoull(x.c_str(), nullptr, 10);
	};

	if (s.empty()) {
		/* "-N": the last N bytes. */
		if (e.empty())
			return r;
		uint64_t n = num(e);
		if (n == 0)
			return r;
		if (n > filesize)
			n = filesize;
		r.offset = (size_t)(filesize - n);
		r.length = (size_t)n;
		r.satisfiable = true;
	} else {
		uint64_t start = num(s);
		if (start >= filesize)
			return r;	/* start past EOF -> 416 */
		uint64_t end = e.empty() ? filesize - 1 : num(e);
		if (end >= filesize)
			end = filesize - 1;
		if (end < start)
			return r;
		r.offset = (size_t)start;
		r.length = (size_t)(end - start + 1);
		r.satisfiable = true;
	}
	return r;
}

} /* namespace */

drogon::Task<drogon::HttpResponsePtr>
MediaController::serve(drogon::HttpRequestPtr req, std::string token)
{
	/* The token is the encrypted file id; an invalid or forged one (which a
	 * public visitor probing the URL space would produce) fails to decrypt
	 * and is indistinguishable from a missing file. */
	auto decoded = auth::filetoken::decrypt(token);
	if (!decoded)
		co_return renderStatus(req, drogon::k404NotFound, "File not found",
				       "No file exists with that id.");
	int64_t fid = (int64_t)*decoded;

	auto ro = drogon::app().getDbClient("ro");
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

	/* Audit only authenticated access. The route is public, so anonymous
	 * hits (hotlinked images, crawlers) must not flood the audit log. */
	auto sess = auth::session::current(req);
	if (sess) {
		auto app = drogon::app().getDbClient("app");
		co_await dao::audit::log(app, sess->uid, "media",
					 req->getPeerAddr().toIp(),
					 "file " + std::to_string(fid));
	}

	std::string attachment;
	if (!viewInline(meta->fileType)) {
		attachment = safeFilename(meta->origName);
		if (attachment.empty())
			attachment = name;
	}

	/*
	 * Honour a Range request so media can be sought/skipped and downloads
	 * resumed. This Drogon's newFileResponse(..., req) does not itself read
	 * the Range header, so parse it and use the offset/length overload for
	 * a partial (206). The on-disk extension drives the Content-Type.
	 */
	std::error_code sizeEc;
	uint64_t filesize = std::filesystem::file_size(path, sizeEc);
	ByteRange rng = parseRange(req->getHeader("range"), filesize);

	drogon::HttpResponsePtr resp;
	if (rng.present && !rng.satisfiable) {
		resp = drogon::HttpResponse::newHttpResponse();
		resp->setStatusCode(drogon::k416RequestedRangeNotSatisfiable);
		resp->addHeader("Content-Range",
				"bytes */" + std::to_string(filesize));
	} else if (rng.satisfiable && rng.length < filesize) {
		resp = drogon::HttpResponse::newFileResponse(
			path.string(), rng.offset, rng.length,
			/*setContentRange=*/true, attachment, drogon::CT_NONE,
			"", req);
	} else {
		resp = drogon::HttpResponse::newFileResponse(
			path.string(), attachment, drogon::CT_NONE, "", req);
	}

	/* Advertise range support on every response; a token maps to one
	 * immutable, content-addressed file, so it stays cacheable. */
	resp->addHeader("Accept-Ranges", "bytes");
	resp->addHeader("Cache-Control", "public, max-age=31536000, immutable");
	co_return resp;
}

} /* namespace tgweb::controllers */
