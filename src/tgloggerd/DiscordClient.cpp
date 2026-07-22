// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include "DiscordClient.hpp"

#include <curl/curl.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace tgloggerd {

std::string json_escape(const std::string &s)
{
	std::string o;
	o.reserve(s.size() + 8);
	for (unsigned char c : s) {
		switch (c) {
		case '"':  o += "\\\""; break;
		case '\\': o += "\\\\"; break;
		case '\n': o += "\\n";  break;
		case '\r': o += "\\r";  break;
		case '\t': o += "\\t";  break;
		case '\b': o += "\\b";  break;
		case '\f': o += "\\f";  break;
		default:
			if (c < 0x20) {
				char buf[8];
				std::snprintf(buf, sizeof(buf), "\\u%04x", c);
				o += buf;
			} else {
				o += static_cast<char>(c);
			}
		}
	}
	return o;
}

namespace {

size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	auto *body = static_cast<std::string *>(userdata);
	body->append(ptr, size * nmemb);
	return size * nmemb;
}

/* Best-effort parse of the retry_after (seconds) from a 429 JSON body. */
double parse_retry_after(const std::string &body)
{
	size_t p = body.find("\"retry_after\"");
	if (p == std::string::npos)
		return 0.0;
	p = body.find(':', p);
	if (p == std::string::npos)
		return 0.0;
	return std::atof(body.c_str() + p + 1);
}

/* The message id from a Discord message JSON: the first top-level "id":"..."
 * (it precedes channel_id/webhook_id/author.id in the response). */
std::string extract_message_id(const std::string &body)
{
	size_t p = body.find("\"id\":\"");
	if (p == std::string::npos)
		return std::string();
	p += 6;
	size_t e = p;
	while (e < body.size() && body[e] >= '0' && body[e] <= '9')
		e++;
	return body.substr(p, e - p);
}

} /* namespace */

void DiscordClient::global_init(void)
{
	curl_global_init(CURL_GLOBAL_DEFAULT);
}

DiscordResponse DiscordClient::request(const char *method,
				       const std::string &url,
				       const std::string &json_body)
{
	DiscordResponse r;
	CURL *curl = curl_easy_init();
	if (!curl) {
		r.error = "curl_easy_init failed";
		return r;
	}

	struct curl_slist *hdrs = nullptr;
	hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
	hdrs = curl_slist_append(hdrs, "User-Agent: tgloggerd-discord/1.0");

	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	if (std::strcmp(method, "GET") == 0) {
		curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
	} else {
		curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method); /* POST/PATCH */
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body.c_str());
		curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)json_body.size());
	}
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &r.body);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

	CURLcode rc = curl_easy_perform(curl);
	if (rc != CURLE_OK) {
		r.status = 0;
		r.error = curl_easy_strerror(rc);
	} else {
		long code = 0;
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
		r.status = code;
		if (r.ok())
			r.message_id = extract_message_id(r.body);
	}

	curl_slist_free_all(hdrs);
	curl_easy_cleanup(curl);
	return r;
}

DiscordResponse DiscordClient::with_retry(const char *method,
					  const std::string &url,
					  const std::string &json_body)
{
	DiscordResponse r;
	for (int attempt = 0; attempt < 4; attempt++) {
		r = request(method, url, json_body);
		if (r.status != 429)
			return r;

		double wait = parse_retry_after(r.body);
		if (wait <= 0.0)
			wait = 1.0;
		if (wait > 10.0)
			wait = 10.0;
		std::this_thread::sleep_for(
			std::chrono::milliseconds((int)(wait * 1000) + 50));
	}
	return r;
}

DiscordResponse DiscordClient::post_json(const std::string &url,
					 const std::string &json_body)
{
	/* ?wait=true so the created message id is returned (for later edits). */
	std::string u = url +
		(url.find('?') == std::string::npos ? "?wait=true" : "&wait=true");
	return with_retry("POST", u, json_body);
}

DiscordResponse DiscordClient::patch_json(const std::string &webhook_url,
					  const std::string &message_id,
					  const std::string &json_body)
{
	return with_retry("PATCH", webhook_url + "/messages/" + message_id,
			  json_body);
}

DiscordResponse DiscordClient::get(const std::string &url)
{
	return with_retry("GET", url, std::string());
}

} /* namespace tgloggerd */
