// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
/*
 * JSON-RPC 2.0 framing: the shapes every response shares, and the validation
 * that decides whether an incoming object is a request, a notification, or
 * junk. Kept apart from Server.cpp so the MCP semantics there are not buried
 * in envelope bookkeeping.
 */
#include "Protocol.hpp"

namespace gwmcp {

Json makeResult(const Json &id, Json result)
{
	Json out;
	out["jsonrpc"] = "2.0";
	out["id"] = id;
	out["result"] = std::move(result);
	return out;
}

Json makeError(const Json &id, int code, const std::string &message)
{
	Json out;
	out["jsonrpc"] = "2.0";
	/*
	 * The spec allows a null id when the request was unparseable enough
	 * that no id could be recovered.
	 */
	out["id"] = id.is_null() ? Json(nullptr) : id;
	out["error"] = Json{ { "code", code }, { "message", message } };
	return out;
}

Json makeError(const Json &id, int code, const std::string &message,
	       const Json &data)
{
	Json out = makeError(id, code, message);
	out["error"]["data"] = data;
	return out;
}

bool isNotification(const Json &message)
{
	/*
	 * JSON-RPC says a notification is a request with no "id". Treat an
	 * explicit null id the same way: some clients send it, and there is
	 * nothing useful to correlate a reply to.
	 */
	if (!message.is_object())
		return false;
	if (!message.contains("id"))
		return true;
	return message["id"].is_null();
}

Json messageId(const Json &message)
{
	if (!message.is_object() || !message.contains("id"))
		return Json(nullptr);
	const Json &id = message["id"];
	/* Per spec an id is a string, a number, or null. */
	if (id.is_string() || id.is_number() || id.is_null())
		return id;
	return Json(nullptr);
}

bool validateEnvelope(const Json &message, std::string &err)
{
	if (!message.is_object()) {
		err = "message must be a JSON object";
		return false;
	}
	if (!message.contains("jsonrpc") || !message["jsonrpc"].is_string() ||
	    message["jsonrpc"].get<std::string>() != "2.0") {
		err = "missing or invalid \"jsonrpc\": expected \"2.0\"";
		return false;
	}
	if (!message.contains("method") || !message["method"].is_string()) {
		err = "missing or invalid \"method\"";
		return false;
	}
	if (message.contains("params")) {
		const Json &p = message["params"];
		/* params, when present, is by-name or by-position only. */
		if (!p.is_object() && !p.is_array() && !p.is_null()) {
			err = "\"params\" must be an object or an array";
			return false;
		}
	}
	return true;
}

Json paramsObject(const Json &message)
{
	if (!message.is_object() || !message.contains("params"))
		return Json::object();
	const Json &p = message["params"];
	return p.is_object() ? p : Json::object();
}

} /* namespace gwmcp */
