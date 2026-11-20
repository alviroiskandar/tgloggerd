// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include "FileApi.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <vector>

namespace tgloggerd {

uint64_t parseFileId(const std::string &s)
{
	bool digits = !s.empty() && s.size() <= 20 &&
		std::all_of(s.begin(), s.end(),
			    [](char c) { return c >= '0' && c <= '9'; });
	if (digits) {
		errno = 0;
		unsigned long long v = strtoull(s.c_str(), nullptr, 10);
		if (errno == 0 && v > 0)
			return (uint64_t)v;
	}
	throw InternalApi::Error::badRequest("bad file id");
}

namespace {

/* A body field that must be a non-negative integer; @def when absent. */
uint64_t body_u64(const nlohmann::json &b, const char *key, uint64_t def)
{
	if (!b.contains(key))
		return def;
	const auto &v = b[key];
	if (!v.is_number_unsigned() &&
	    !(v.is_number_integer() && v.get<int64_t>() >= 0))
		throw InternalApi::Error::badRequest(
			std::string(key) + " must be a non-negative integer");
	return v.get<uint64_t>();
}

} /* namespace */

void registerFileApi(InternalApi &api, DB &db, FileFetcher *fetcher)
{
	DB *d = &db;

	api.route("GET", "/v1/files/{id}", [d, fetcher](const InternalApi::Request &req) {
		uint64_t id = parseFileId(req.param("id"));
		auto st = d->getFileStatus(id);
		if (!st)
			throw InternalApi::Error::notFound("unknown file");

		InternalApi::Response r;
		r.body = {
			{ "id", st->id },
			{ "on_disk", st->on_disk },
			{ "size", st->file_size },
			{ "type", st->file_type },
			{ "ext", st->ext.empty() ? nlohmann::json(nullptr)
						 : nlohmann::json(st->ext) },
			{ "alias_of", st->alias_of ? nlohmann::json(*st->alias_of)
						   : nlohmann::json(nullptr) },
			{ "has_unique_id", st->has_unique_id },
			/* Every row keeps its tg_file_id; only a file found
			 * gone from Telegram within the hour is not, nor a
			 * still the daemon generated. */
			{ "fetchable", !st->generated &&
				       !(fetcher && fetcher->isGone(id)) },
			{ "accessed_at", st->accessed_at },
		};
		return r;
	});

	api.route("POST", "/v1/files/access", [d](const InternalApi::Request &req) {
		const auto &b = req.body;
		if (!b.is_object() || !b.contains("ids") || !b["ids"].is_array())
			throw InternalApi::Error::badRequest(
				"want {\"ids\": [file ids]}");
		const auto &arr = b["ids"];
		if (arr.size() > kMaxAccessBatch)
			throw InternalApi::Error::badRequest(
				"at most " + std::to_string(kMaxAccessBatch) +
				" ids per request");

		std::vector<uint64_t> ids;
		ids.reserve(arr.size());
		for (const auto &v : arr) {
			if (!v.is_number_unsigned() || v.get<uint64_t>() == 0)
				throw InternalApi::Error::badRequest(
					"ids must be positive integers");
			ids.push_back(v.get<uint64_t>());
		}
		std::sort(ids.begin(), ids.end());
		ids.erase(std::unique(ids.begin(), ids.end()), ids.end());

		uint64_t n = d->touchFiles(ids);
		InternalApi::Response r;
		r.body = { { "ids", ids.size() }, { "updated", n } };
		return r;
	});

	if (!fetcher)
		return;

	api.route("POST", "/v1/files/{id}/fetch",
		  [fetcher](const InternalApi::Request &req) {
		uint64_t id = parseFileId(req.param("id"));
		const auto &b = req.body;
		if (!b.is_null() && !b.is_object())
			throw InternalApi::Error::badRequest(
				"want {\"offset\": n, \"length\": n}");
		uint64_t offset = 0, length = 0;
		if (b.is_object()) {
			offset = body_u64(b, "offset", 0);
			length = body_u64(b, "length", 0);
		}

		FileFetcher::Result res = fetcher->fetch(id, offset, length);
		InternalApi::Response r;
		if (res.stored)
			r.body = { { "state", "stored" }, { "id", res.id } };
		else
			r.body = {
				{ "state", "partial" },
				{ "path", res.path },
				{ "offset", res.offset },
				{ "available", res.available },
				{ "size", res.size },
			};
		return r;
	});
}

} /* namespace tgloggerd */
