// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
/*
 * stills_worker_test -- the generated-still pass end to end: StillsWorker
 * against a MySQL with every migration applied and a running stills
 * service (docker/stills) whose store is a directory of this test. It
 * puts sample files into that store, records messages for them, runs the
 * worker and checks the stills it stores and links, the refusal of a file
 * that is no video, and a new message of a video that has its still. It
 * writes rows under ids no real chat has and deletes them after; run it
 * only against a throwaway database and store:
 *
 *   TGL_TEST_MYSQL_HOST=... TGL_TEST_MYSQL_PASSWORD=... (as stills_db_test)
 *   TGL_TEST_STILLS_SOCKET=<the service's socket>
 *   TGL_TEST_STILLS_STORE=<the directory the service has as its store>
 *   TGL_TEST_STILLS_SAMPLES=<a directory with video.mp4 and text.mp4>
 *   build/stills_worker_test
 *
 * Without TGL_TEST_MYSQL_HOST or TGL_TEST_STILLS_SOCKET it exits 77.
 */
#include <tgloggerd/DB.hpp>
#include <tgloggerd/StillsWorker.hpp>
#include <tgloggerd/StoragePath.hpp>

#include <openssl/evp.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace tgloggerd;
using CT = models::MessageContentType;
namespace fs = std::filesystem;

static int g_failures;

#define CHECK(cond, what)                                                     \
	do {                                                                  \
		if (!(cond)) {                                                \
			printf("  FAIL: %s\n", (what));                       \
			g_failures++;                                         \
		} else {                                                      \
			printf("  ok:   %s\n", (what));                       \
		}                                                             \
	} while (0)

static const int64_t kGroup = -4242053;
static const int64_t kUser = 4253;
static const char *kFilePrefix = "stills_worker_test:";

static std::string env(const char *k, const char *def = "")
{
	const char *v = std::getenv(k);
	return v ? v : def;
}

static std::string slurp(const fs::path &p)
{
	std::ifstream f(p, std::ios::binary);
	std::stringstream ss;
	ss << f.rdbuf();
	return ss.str();
}

static std::string sha256(const std::string &data)
{
	unsigned char d[EVP_MAX_MD_SIZE];
	unsigned int n = 0;
	EVP_Digest(data.data(), data.size(), d, &n, EVP_sha256(), nullptr);
	static const char hex[] = "0123456789abcdef";
	std::string out;
	for (unsigned int i = 0; i < n; i++) {
		out += hex[d[i] >> 4];
		out += hex[d[i] & 15];
	}
	return out;
}

/* Put @data in the store as its blob; its digest. */
static std::string toStore(const std::string &store, const std::string &data,
			   const std::string &ext)
{
	const std::string h = sha256(data);
	fs::path p = blobPath(store, h, ext);
	fs::create_directories(p.parent_path());
	std::ofstream(p, std::ios::binary) << data;
	return h;
}

static uint64_t fileRow(mysql::Database &raw, const std::string &name,
			const char *type, const std::string &hex,
			const char *ext)
{
	raw.execute("INSERT INTO telegram_files (tg_file_id, file_type, file_ext,"
		    " sha256, on_disk) VALUES (?, ?, ?, UNHEX(?), 1)",
		    { std::string(kFilePrefix) + name, std::string(type),
		      std::string(ext), hex });
	return std::stoull(*raw.query("SELECT id FROM telegram_files WHERE"
				      " tg_file_id = ?",
				      { std::string(kFilePrefix) + name })[0][0]);
}

static void cleanup(mysql::Database &raw, const std::vector<uint64_t> &videos)
{
	raw.execute("DELETE FROM telegram_group_messages WHERE chat_id = ?", { kGroup });
	raw.execute("DELETE FROM telegram_private_messages WHERE chat_id = ?", { kUser });
	raw.execute("DELETE FROM `telegram_groups` WHERE id = ?", { kGroup });
	raw.execute("DELETE FROM telegram_users WHERE id = ?", { kUser });
	for (uint64_t v : videos)
		raw.execute("DELETE FROM telegram_files WHERE"
			    " generated_from_file_id = ?", { v });
	raw.execute("DELETE FROM telegram_files WHERE tg_file_id LIKE ?",
		    { std::string(kFilePrefix) + "%" });
}

static uint64_t thumbOf(mysql::Database &raw, int64_t chat, int64_t id)
{
	auto rows = raw.query(chat < 0
		? "SELECT x.file_id FROM telegram_message_files x"
		  " JOIN telegram_group_messages m ON m.id = x.group_message_id"
		  " WHERE m.chat_id = ? AND m.message_id = ? AND x.role = 'thumbnail'"
		: "SELECT x.file_id FROM telegram_message_files x"
		  " JOIN telegram_private_messages m ON m.id = x.private_message_id"
		  " WHERE m.chat_id = ? AND m.message_id = ? AND x.role = 'thumbnail'",
		{ chat, id });
	return rows.empty() || !rows[0][0] ? 0 : std::stoull(*rows[0][0]);
}

/* Wait up to @secs for @cond. */
template <typename F> static bool waitFor(int secs, F cond)
{
	for (int i = 0; i < secs * 10; i++) {
		if (cond())
			return true;
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	return cond();
}

int main()
{
	if (env("TGL_TEST_MYSQL_HOST").empty() ||
	    env("TGL_TEST_STILLS_SOCKET").empty()) {
		printf("TGL_TEST_MYSQL_HOST or TGL_TEST_STILLS_SOCKET unset:"
		       " skipped\n");
		return 77;
	}
	mysql::Config cfg;
	cfg.host = env("TGL_TEST_MYSQL_HOST");
	cfg.port = (uint16_t)std::atoi(env("TGL_TEST_MYSQL_PORT", "3306").c_str());
	cfg.user = env("TGL_TEST_MYSQL_USER", "root");
	cfg.password = env("TGL_TEST_MYSQL_PASSWORD");
	cfg.database = env("TGL_TEST_MYSQL_DB", "tgloggerd");
	cfg.pool_size = 3;
	const std::string store = env("TGL_TEST_STILLS_STORE");
	const std::string samples = env("TGL_TEST_STILLS_SAMPLES");

	DB db(cfg);
	mysql::Database raw(cfg);
	cleanup(raw, {});

	models::Group g;
	g.id = kGroup;
	g.title = "stills worker test";
	db.upsertGroup(g);
	models::User u;
	u.id = kUser;
	u.first_name = "stills worker test";
	db.upsertUser(u);

	const std::string video = slurp(fs::path(samples) / "video.mp4");
	const std::string text = slurp(fs::path(samples) / "text.mp4");
	const uint64_t v = fileRow(raw, "video", "video",
				   toStore(store, video, "mp4"), "mp4");
	const uint64_t t = fileRow(raw, "text", "video",
				   toStore(store, text, "mp4"), "mp4");
	/* Recorded as stored, but its blob is not in the store. */
	const uint64_t gone = fileRow(raw, "gone", "video",
				      sha256("not in the store"), "mp4");

	auto message = [&](bool group, int64_t id, uint64_t file) {
		if (group) {
			models::GroupMessage m;
			m.chat_id = kGroup;
			m.message_id = id;
			m.date = 1700000000 + id;
			m.content.content_type = CT::Video;
			m.content.content_extra = std::string(R"({"duration": 4})");
			db.upsertGroupMessage(m);
			db.setGroupMessageFile(kGroup, id, file);
		} else {
			models::PrivateMessage m;
			m.chat_id = kUser;
			m.message_id = id;
			m.date = 1700000000 + id;
			m.content.content_type = CT::Video;
			db.upsertPrivateMessage(m);
			db.setPrivateMessageFile(kUser, id, file);
		}
	};
	message(true, 1, v);
	message(false, 1, v);
	message(true, 2, t);
	message(true, 3, gone);

	std::mutex mtx;
	std::vector<std::pair<int64_t, int64_t>> linked;
	size_t stores = 0;
	StillsWorker::Config wc;
	wc.socket_path = env("TGL_TEST_STILLS_SOCKET");
	wc.storage_dir = store;
	wc.tmp_dir = (fs::path(store).parent_path() / "stills-tmp").string();
	wc.pace = std::chrono::milliseconds(10);
	wc.new_delay = std::chrono::seconds(0);
	StillsWorker w(db, nullptr, wc,
		[&](const std::string &path) -> std::optional<StillsWorker::Stored> {
			const std::string data = slurp(path);
			std::string ext = fs::path(path).extension().string().substr(1);
			std::lock_guard<std::mutex> lk(mtx);
			stores++;
			return StillsWorker::Stored{ toStore(store, data, ext), ext,
						     true };
		},
		[&](uint64_t, const std::vector<std::pair<int64_t, int64_t>> &m) {
			std::lock_guard<std::mutex> lk(mtx);
			linked.insert(linked.end(), m.begin(), m.end());
		});
	w.start();

	printf("the pass:\n");
	CHECK(waitFor(60, [&] { return thumbOf(raw, kGroup, 1) != 0 &&
					thumbOf(raw, kUser, 1) != 0; }),
	      "both messages of the video get a thumbnail");
	const uint64_t still = thumbOf(raw, kGroup, 1);
	CHECK(still == thumbOf(raw, kUser, 1), "the same still for both");
	{
		auto r = raw.query("SELECT tg_file_id, file_type, file_ext, on_disk,"
				   " generated_from_file_id, LOWER(HEX(sha256)),"
				   " file_size FROM telegram_files WHERE id = ?",
				   { still });
		const std::string hex = r.at(0)[5].value_or("");
		CHECK(r.at(0)[0] == "generated:" + hex && r[0][1] == "thumbnail" &&
		      r[0][2] == "jpg" && r[0][3] == "1" &&
		      r[0][4] == std::to_string(v),
		      "a generated JPEG thumbnail of the video, in the store");
		const std::string blob = slurp(blobPath(store, hex, "jpg"));
		CHECK(blob.size() > 100 && blob.compare(0, 2, "\xff\xd8") == 0 &&
		      std::to_string(blob.size()) == r[0][6].value_or("") &&
		      sha256(blob) == hex,
		      "its blob is the JPEG, named by its digest");
	}
	{
		std::lock_guard<std::mutex> lk(mtx);
		CHECK(linked.size() == 2 && stores == 1,
		      "one still stored, its two messages reported");
	}
	/* Newest first: those two were done before the video. */
	CHECK(thumbOf(raw, kGroup, 2) == 0 && thumbOf(raw, kGroup, 3) == 0 &&
	      db.stillCandidate(t) && db.stillCandidate(gone),
	      "no still for a file that is no video or whose blob is gone");

	printf("a new message of the same video:\n");
	message(true, 4, v);
	w.messageFileStored(true, kGroup, 4);
	CHECK(waitFor(30, [&] { return thumbOf(raw, kGroup, 4) == still; }),
	      "gets the video's still");
	{
		std::lock_guard<std::mutex> lk(mtx);
		CHECK(stores == 1, "without a second one being made");
	}

	auto t0 = std::chrono::steady_clock::now();
	w.stop();
	CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(3),
	      "stop() returns at once");

	cleanup(raw, { v, t, gone });
	if (g_failures) {
		printf("%d check(s) failed\n", g_failures);
		return 1;
	}
	printf("all checks passed\n");
	return 0;
}
