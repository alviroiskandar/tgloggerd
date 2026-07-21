// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include "controllers/SearchController.hpp"

#include "auth/FileToken.hpp"
#include "auth/Session.hpp"
#include "controllers/Common.hpp"
#include "dao/Search.hpp"

#include <cstdint>
#include <cstdlib>
#include <string>

namespace tgweb::controllers {

namespace {

/* Longest raw `search` JSON we will even attempt to parse. */
constexpr size_t kMaxSearchBytes = 8192;

drogon::HttpResponsePtr jsonResp(const nlohmann::json &body,
				 drogon::HttpStatusCode code)
{
	auto resp = drogon::HttpResponse::newHttpResponse();
	resp->setStatusCode(code);
	resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
	resp->setBody(body.dump());
	return resp;
}

drogon::HttpResponsePtr jsonError(const std::string &msg,
				  drogon::HttpStatusCode code)
{
	return jsonResp(nlohmann::json{ { "error", msg } }, code);
}

/*
 * Parse the `search` query param (URL-decoded by drogon) into conditions.
 * Returns false + sets err on malformed input. An empty/absent param is a
 * valid "browse all" (conds stays empty).
 */
bool parseSearch(const std::string &raw,
		 std::vector<dao::search::Condition> &conds, std::string &err)
{
	if (raw.empty())
		return true;
	if (raw.size() > kMaxSearchBytes) {
		err = "search parameter too large";
		return false;
	}

	nlohmann::json j;
	try {
		j = nlohmann::json::parse(raw);
	} catch (const std::exception &) {
		err = "search must be valid JSON";
		return false;
	}
	if (!j.is_array()) {
		err = "search must be a JSON array";
		return false;
	}

	for (const auto &item : j) {
		if (!item.is_object()) {
			err = "each condition must be a JSON object";
			return false;
		}
		dao::search::Condition c;
		auto strField = [&](const char *k, std::string &out,
				    bool required) -> bool {
			if (!item.contains(k) || item[k].is_null()) {
				if (required) {
					err = std::string("missing '") + k +
					      "' in a condition";
					return false;
				}
				return true;
			}
			if (!item[k].is_string()) {
				err = std::string("'") + k +
				      "' must be a string";
				return false;
			}
			out = item[k].get<std::string>();
			return true;
		};
		if (!strField("c", c.c, true) || !strField("o", c.o, true) ||
		    !strField("n", c.n, false))
			return false;
		if (item.contains("v") && !item["v"].is_null()) {
			if (!item["v"].is_string()) {
				err = "'v' must be a string";
				return false;
			}
			c.v = item["v"].get<std::string>();
			c.hasV = true;
		}
		conds.push_back(std::move(c));
	}
	return true;
}

std::string fileUrl(int64_t id)
{
	return "/files/" + auth::filetoken::encrypt((uint64_t)id);
}

/* Add the tokenized photo URLs the browser has no key to compute. */
void enrichRows(nlohmann::json &result)
{
	if (!result.contains("rows"))
		return;
	for (auto &row : result["rows"])
		if (row.contains("photo_file_id"))
			row["_photo_url"] =
				fileUrl(row["photo_file_id"].get<int64_t>());
}

} /* namespace */

drogon::Task<drogon::HttpResponsePtr>
SearchController::users(drogon::HttpRequestPtr req)
{
	dao::search::Request sreq;
	sreq.limit  = clampedIntParam(req, "limit", 50, 1,
				      dao::search::MAX_LIMIT);
	sreq.offset = clampedIntParam(req, "offset", 0, 0,
				      dao::search::MAX_OFFSET);
	sreq.sort   = req->getParameter("sort");
	sreq.order  = req->getParameter("order");
	sreq.debug  = (req->getParameter("debug") == "1") &&
		      auth::session::isAdmin(req);

	std::string err;
	if (!parseSearch(req->getParameter("search"), sreq.conds, err))
		co_return jsonError(err, drogon::k400BadRequest);

	auto db = drogon::app().getDbClient("ro");
	nlohmann::json result = co_await dao::search::run(
		db, dao::search::usersSchema(), std::move(sreq));

	if (result.contains("error"))
		co_return jsonError(result["error"].get<std::string>(),
				    drogon::k400BadRequest);

	enrichRows(result);
	co_return jsonResp(result, drogon::k200OK);
}

} /* namespace tgweb::controllers */
