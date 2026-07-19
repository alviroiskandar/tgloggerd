// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include "controllers/ApiController.hpp"

#include "auth/FileToken.hpp"
#include "controllers/Common.hpp"
#include "dao/Browse.hpp"

#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>

namespace tgweb::controllers {

namespace {

bool validScope(const std::string &s)
{
	return s == "group" || s == "private";
}

std::string fileUrl(int64_t id)
{
	return "/files/" + auth::filetoken::encrypt((uint64_t)id);
}

/*
 * Add the tokenized file URLs the client cannot compute itself (it has no
 * key). The SSR template mints these through the media() helper; here we put
 * the same values on the JSON so the client renders identical markup.
 */
void enrichUrls(nlohmann::json &m)
{
	if (m.contains("media") && m["media"].contains("file_id"))
		m["media"]["url"] = fileUrl(m["media"]["file_id"].get<int64_t>());
	if (m.contains("sender") && m["sender"].contains("photo_file_id"))
		m["sender"]["photo_url"] =
			fileUrl(m["sender"]["photo_file_id"].get<int64_t>());
	if (m.contains("items"))
		for (auto &it : m["items"])
			if (it.contains("media") && it["media"].contains("file_id"))
				it["media"]["url"] = fileUrl(
					it["media"]["file_id"].get<int64_t>());
	if (m.contains("edits"))
		for (auto &e : m["edits"])
			if (e.contains("media") && e["media"].contains("file_id"))
				e["media"]["url"] =
					fileUrl(e["media"]["file_id"].get<int64_t>());
}

} /* namespace */

drogon::Task<drogon::HttpResponsePtr>
ApiController::messages(drogon::HttpRequestPtr req, std::string scope,
			std::string id)
{
	if (!validScope(scope)) {
		auto resp = drogon::HttpResponse::newHttpResponse();
		resp->setStatusCode(drogon::k404NotFound);
		co_return resp;
	}

	auto db = drogon::app().getDbClient("ro");
	int64_t chatId = strtoll(id.c_str(), nullptr, 10);

	int limit = clampedIntParam(req, "limit", 30, 1, 100);
	std::optional<int64_t> after;
	std::string afterParam = req->getParameter("after");
	if (!afterParam.empty())
		after = strtoll(afterParam.c_str(), nullptr, 10);
	std::optional<int64_t> afterTs;
	std::string afterTsParam = req->getParameter("after_ts");
	if (!afterTsParam.empty())
		afterTs = strtoll(afterTsParam.c_str(), nullptr, 10);

	nlohmann::json hist = co_await dao::browse::chatHistory(
		db, scope, chatId, limit, after, afterTs);
	for (auto &m : hist["messages"])
		enrichUrls(m);

	auto resp = drogon::HttpResponse::newHttpResponse();
	resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
	resp->setBody(hist.dump());
	co_return resp;
}

} /* namespace tgweb::controllers */
