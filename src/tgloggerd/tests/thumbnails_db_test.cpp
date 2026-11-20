// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
/*
 * thumbnails_db_test -- which messages the old-row thumbnail pass
 * re-reads (DB::listMessagesWithoutThumbnail and its dry-run count) and
 * the plan of that read, against a MySQL with every migration applied.
 * It writes rows under ids no real chat has and deletes them after; run
 * it only against a throwaway database:
 *
 *   TGL_TEST_MYSQL_HOST=127.0.0.1 TGL_TEST_MYSQL_PORT=3306
 *   TGL_TEST_MYSQL_USER=root TGL_TEST_MYSQL_PASSWORD=...
 *   TGL_TEST_MYSQL_DB=tgloggerd build/thumbnails_db_test
 *
 * Without TGL_TEST_MYSQL_HOST it does nothing and exits 77 (skipped).
 */
#include <tgloggerd/DB.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace tgloggerd;
using CT = models::MessageContentType;

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

/* Ids no real chat has: a basic group's chat id is minus its id, and no
 * real user id is this small. */
static const int64_t kGroup = -4242005;
static const int64_t kUser = 4245;
static const char *kFilePrefix = "thumbnails_db_test:";

static std::string env(const char *k, const char *def = "")
{
	const char *v = std::getenv(k);
	return v ? v : def;
}

static void cleanup(mysql::Database &raw)
{
	/* The message files go with their messages (ON DELETE CASCADE). */
	raw.execute("DELETE FROM telegram_group_messages WHERE chat_id = ?", { kGroup });
	raw.execute("DELETE FROM telegram_private_messages WHERE chat_id = ?", { kUser });
	raw.execute("DELETE FROM `telegram_groups` WHERE id = ?", { kGroup });
	raw.execute("DELETE FROM telegram_users WHERE id = ?", { kUser });
	raw.execute("DELETE FROM telegram_files WHERE tg_file_id LIKE ?",
		    { std::string(kFilePrefix) + "%" });
}

/* A telegram_files row with extension @ext, recorded without bytes. */
static uint64_t file(mysql::Database &raw, const std::string &name,
		     const char *type, const char *ext)
{
	raw.execute("INSERT INTO telegram_files (tg_file_id, file_type, file_ext)"
		    " VALUES (?, ?, ?)",
		    { std::string(kFilePrefix) + name, std::string(type),
		      std::string(ext) });
	auto rows = raw.query("SELECT id FROM telegram_files WHERE tg_file_id = ?",
			      { std::string(kFilePrefix) + name });
	return std::stoull(*rows.at(0).at(0));
}

static void groupMessage(DB &db, int64_t id, CT type,
			 const char *extra = nullptr, bool deleted = false)
{
	models::GroupMessage m;
	m.chat_id = kGroup;
	m.message_id = id;
	m.date = 1700000000 + id;
	m.content.content_type = type;
	if (extra)
		m.content.content_extra = std::string(extra);
	db.upsertGroupMessage(m);
	if (deleted) {
		models::GroupMessage d;
		d.chat_id = kGroup;
		d.message_id = id;
		d.is_deleted = true;
		db.upsertGroupMessage(d);
	}
}

static void privateMessage(DB &db, int64_t id, CT type)
{
	models::PrivateMessage m;
	m.chat_id = kUser;
	m.message_id = id;
	m.date = 1700000000 + id;
	m.content.content_type = type;
	db.upsertPrivateMessage(m);
}

static void setMainFile(mysql::Database &raw, const char *table, int64_t chat,
			int64_t id, uint64_t file_id)
{
	raw.execute(std::string("UPDATE ") + table + " SET file_id = ?"
		    " WHERE chat_id = ? AND message_id = ?",
		    { file_id, chat, id });
}

/* Our chats' candidates, as "g<id>" and "p<id>", sorted. */
static std::string ours(const std::vector<std::pair<int64_t, int64_t>> &v)
{
	std::vector<std::string> s;
	for (const auto &m : v) {
		if (m.first == kGroup)
			s.push_back("g" + std::to_string(m.second));
		else if (m.first == kUser)
			s.push_back("p" + std::to_string(m.second));
	}
	std::sort(s.begin(), s.end());
	std::string out;
	for (const auto &x : s)
		out += (out.empty() ? "" : ",") + x;
	return out;
}

int main()
{
	if (env("TGL_TEST_MYSQL_HOST").empty()) {
		printf("TGL_TEST_MYSQL_HOST unset: skipped\n");
		return 77;
	}
	mysql::Config cfg;
	cfg.host = env("TGL_TEST_MYSQL_HOST");
	cfg.port = (uint16_t)std::atoi(env("TGL_TEST_MYSQL_PORT", "3306").c_str());
	cfg.user = env("TGL_TEST_MYSQL_USER", "root");
	cfg.password = env("TGL_TEST_MYSQL_PASSWORD");
	cfg.database = env("TGL_TEST_MYSQL_DB", "tgloggerd");
	cfg.pool_size = 2;

	DB db(cfg);
	mysql::Database raw(cfg);
	cleanup(raw);

	/* What the database held before ours, so other rows do not count. */
	std::map<std::pair<std::string, std::string>, uint64_t> before;
	for (const auto &c : db.countMessagesWithoutThumbnail())
		before[{ std::get<0>(c), std::get<1>(c) }] = std::get<2>(c);

	models::Group g;
	g.id = kGroup;
	g.title = "thumbnails test";
	db.upsertGroup(g);
	models::User u;
	u.id = kUser;
	u.first_name = "thumbnails test";
	db.upsertUser(u);

	const uint64_t thumb = file(raw, "thumb", "unknown", "jpg");
	const uint64_t cover = file(raw, "cover", "photo", "jpg");
	const uint64_t webm = file(raw, "webm", "sticker", "webm");
	const uint64_t webp = file(raw, "webp", "sticker", "webp");

	/* Group: candidates are 1, 4, 5, 7, 9. */
	groupMessage(db, 1, CT::Video);			/* no still */
	groupMessage(db, 2, CT::Video);			/* has a thumbnail */
	db.setMessageExtraFile(kGroup, 2, true, "thumbnail", 0, thumb);
	groupMessage(db, 3, CT::Video);			/* has a cover */
	db.setMessageExtraFile(kGroup, 3, true, "cover", 0, cover);
	groupMessage(db, 4, CT::Animation);
	groupMessage(db, 5, CT::VideoNote);
	groupMessage(db, 6, CT::Video, nullptr, true);	/* deleted */
	groupMessage(db, 7, CT::Sticker);		/* .webm file */
	setMainFile(raw, "telegram_group_messages", kGroup, 7, webm);
	groupMessage(db, 8, CT::Sticker);		/* .webp file */
	setMainFile(raw, "telegram_group_messages", kGroup, 8, webp);
	groupMessage(db, 9, CT::Sticker, R"({"format": "webm"})"); /* no file */
	groupMessage(db, 10, CT::Sticker, R"({"format": "tgs"})");
	groupMessage(db, 11, CT::Sticker);		/* nothing known */
	groupMessage(db, 12, CT::Text);
	groupMessage(db, 13, CT::Photo);
	groupMessage(db, 14, CT::Sticker, nullptr, true); /* deleted .webm */
	setMainFile(raw, "telegram_group_messages", kGroup, 14, webm);
	groupMessage(db, 15, CT::Animation);		/* has a thumbnail */
	db.setMessageExtraFile(kGroup, 15, true, "thumbnail", 0, thumb);
	groupMessage(db, 16, CT::Video);		/* another role only */
	db.setMessageExtraFile(kGroup, 16, true, "live_video", 0, thumb);

	/* Private: candidates are 1 and 3. */
	privateMessage(db, 1, CT::Video);
	privateMessage(db, 2, CT::Video);
	db.setMessageExtraFile(kUser, 2, false, "thumbnail", 0, thumb);
	privateMessage(db, 3, CT::VideoNote);
	privateMessage(db, 4, CT::Text);

	printf("candidates:\n");
	const std::string got = ours(db.listMessagesWithoutThumbnail());
	printf("  got %s\n", got.c_str());
	CHECK(got == "g1,g16,g4,g5,g7,g9,p1,p3",
	      "videos, animations, video notes and .webm stickers without a"
	      " still; not with a thumbnail or a cover, not deleted, not"
	      " other kinds or stickers");

	printf("dry-run counts:\n");
	std::map<std::pair<std::string, std::string>, uint64_t> after;
	for (const auto &c : db.countMessagesWithoutThumbnail())
		after[{ std::get<0>(c), std::get<1>(c) }] = std::get<2>(c);
	auto added = [&](const char *t, const char *ct) {
		return after[{ t, ct }] - before[{ t, ct }];
	};
	CHECK(added("group", "video") == 2, "group: 2 videos");
	CHECK(added("group", "animation") == 1, "group: 1 animation");
	CHECK(added("group", "video_note") == 1, "group: 1 video note");
	CHECK(added("group", "sticker") == 2, "group: 2 video stickers");
	CHECK(added("private", "video") == 1, "private: 1 video");
	CHECK(added("private", "video_note") == 1, "private: 1 video note");
	CHECK(added("group", "text") == 0 && added("group", "photo") == 0,
	      "no other kinds");

	printf("a still stored takes it off the list:\n");
	db.setMessageExtraFile(kGroup, 1, true, "thumbnail", 0, thumb);
	db.setMessageExtraFile(kUser, 3, false, "thumbnail", 0, thumb);
	CHECK(ours(db.listMessagesWithoutThumbnail()) == "g16,g4,g5,g7,g9,p1",
	      "g1 and p3 no longer listed");

	printf("plan:\n");
	{
		/* Each table the list's statement reads, by alias, with the
		 * key and access type of every read of it. */
		auto rows = raw.query("EXPLAIN FORMAT=TRADITIONAL " +
				      DB::thumbnailCandidatesSql());
		std::map<std::string, std::set<std::string>> by;
		for (const auto &r : rows) {
			if (r.size() < 7)
				continue;
			/* id, select_type, table, partitions, type,
			 * possible_keys, key */
			by[r[2].value_or("NULL")].insert(r[6].value_or("NULL") +
				"/" + r[4].value_or("NULL"));
		}
		for (const auto &[tab, ps] : by)
			for (const auto &p : ps)
				printf("  %s: %s\n", tab.c_str(), p.c_str());
		CHECK(by["m"] == std::set<std::string>(
			      { "idx_group_messages_ctype_date/range",
				"idx_private_messages_ctype_date/range" }),
		      "both message tables read by content_type");
		CHECK(by["f"] == std::set<std::string>({ "PRIMARY/eq_ref" }),
		      "files read by their primary key");
		bool uq = by["x"].size() == 2;
		for (const auto &p : by["x"])
			uq = uq && p.rfind("uq_message_files_", 0) == 0;
		CHECK(uq, "message files read by their unique keys");
	}

	cleanup(raw);
	if (g_failures) {
		printf("%d check(s) failed\n", g_failures);
		return 1;
	}
	printf("all checks passed\n");
	return 0;
}
