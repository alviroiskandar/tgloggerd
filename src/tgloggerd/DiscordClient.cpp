// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include "DiscordClient.hpp"

#include <curl/curl.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
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

} /* namespace */

void DiscordClient::global_init(void)
{
	curl_global_init(CURL_GLOBAL_DEFAULT);
}

DiscordResponse DiscordClient::post_once(const std::string &url,
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
	curl_easy_setopt(curl, CURLOPT_POST, 1L);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body.c_str());
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)json_body.size());
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
	}

	curl_slist_free_all(hdrs);
	curl_easy_cleanup(curl);
	return r;
}

DiscordResponse DiscordClient::post_json(const std::string &url,
					 const std::string &json_body)
{
	DiscordResponse r;
	for (int attempt = 0; attempt < 4; attempt++) {
		r = post_once(url, json_body);
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

} /* namespace tgloggerd */
