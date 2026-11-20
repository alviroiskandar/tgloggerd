// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
/*
 * stills_db_test -- which videos the generated-still pass takes a still
 * of (DB::listStillCandidates, its dry-run count and the check of one
 * video), how a still is recorded and linked to the video's messages,
 * that a Telegram thumbnail wins over it either way, and the plan of the
 * pass's read, against a MySQL with every migration applied. It writes
 * rows under ids no real chat has and deletes them after; run it only
 * against a throwaway database:
 *
 *   TGL_TEST_MYSQL_HOST=127.0.0.1 TGL_TEST_MYSQL_PORT=3306
 *   TGL_TEST_MYSQL_USER=root TGL_TEST_MYSQL_PASSWORD=...
 *   TGL_TEST_MYSQL_DB=tgloggerd build/stills_db_test
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
static const int64_t kGroup = -4242052;
static const int64_t kUser = 4252;
static const char *kFilePrefix = "stills_db_test:";
/* The digests of this test's files start so, and no real file's does
 * with any likelihood worth a thought. */
static const std::string kHexPrefix = "5717150000000000";

static std::string env(const char *k, const char *def = "")
{
	const char *v = std::getenv(k);
	return v ? v : def;
}

/* A 64-digit hex digest of our own, numbered @n. */
static std::string hex(int n)
{
	char tail[64];
	snprintf(tail, sizeof(tail), "%048d", n);
	return kHexPrefix + tail;
}

static void cleanup(mysql::Database &raw)
{
	/* The message files go with their messages (ON DELETE CASCADE). */
	raw.execute("DELETE FROM telegram_group_messages WHERE chat_id = ?", { kGroup });
	raw.execute("DELETE FROM telegram_private_messages WHERE chat_id = ?", { kUser });
	raw.execute("DELETE FROM `telegram_groups` WHERE id = ?", { kGroup });
	raw.execute("DELETE FROM telegram_users WHERE id = ?", { kUser });
	/* The stills first: they point at the videos. */
	raw.execute("DELETE FROM telegram_files WHERE tg_file_id LIKE ?",
		    { std::string("generated:") + kHexPrefix + "%" });
	raw.execute("DELETE FROM telegram_files WHERE tg_file_id LIKE ?",
		    { std::string(kFilePrefix) + "%" });
}

/*
 * A telegram_files row: digest number @n (0: never seen), extension
 * @ext, bytes in the store or not.
 */
static uint64_t file(mysql::Database &raw, const std::string &name,
		     const char *type, const char *ext, int n, bool on_disk)
{
	mysql::Param sha = std::monostate{};
	if (n)
		sha = hex(n);
	raw.execute("INSERT INTO telegram_files (tg_file_id, file_type, file_ext,"
		    " sha256, on_disk) VALUES (?, ?, ?, UNHEX(?), ?)",
		    { std::string(kFilePrefix) + name, std::string(type),
		      std::string(ext), sha, (int64_t)(on_disk ? 1 : 0) });
	auto rows = raw.query("SELECT id FROM telegram_files WHERE tg_file_id = ?",
			      { std::string(kFilePrefix) + name });
	return std::stoull(*rows.at(0).at(0));
}

static void groupMessage(DB &db, mysql::Database &raw, int64_t id, CT type,
			 uint64_t file_id, const char *extra = nullptr,
			 bool deleted = false)
{
	models::GroupMessage m;
	m.chat_id = kGroup;
	m.message_id = id;
	m.date = 1700000000 + id;
	m.content.content_type = type;
	if (extra)
		m.content.content_extra = std::string(extra);
	db.upsertGroupMessage(m);
	if (file_id)
		db.setGroupMessageFile(kGroup, id, file_id);
	if (deleted)
		raw.execute("UPDATE telegram_group_messages SET deleted_at = NOW()"
			    " WHERE chat_id = ? AND message_id = ?",
			    { kGroup, id });
}

static void privateMessage(DB &db, int64_t id, CT type, uint64_t file_id,
			   const char *extra = nullptr)
{
	models::PrivateMessage m;
	m.chat_id = kUser;
	m.message_id = id;
	m.date = 1700000000 + id;
	m.content.content_type = type;
	if (extra)
		m.content.content_extra = std::string(extra);
	db.upsertPrivateMessage(m);
	if (file_id)
		db.setPrivateMessageFile(kUser, id, file_id);
}

/* The thumbnail file of a message, 0 if none. */
static uint64_t thumbOf(mysql::Database &raw, bool group, int64_t id)
{
	auto rows = raw.query(group
		? "SELECT x.file_id FROM telegram_message_files x"
		  " JOIN telegram_group_messages m ON m.id = x.group_message_id"
		  " WHERE m.chat_id = ? AND m.message_id = ? AND x.role = 'thumbnail'"
		: "SELECT x.file_id FROM telegram_message_files x"
		  " JOIN telegram_private_messages m ON m.id = x.private_message_id"
		  " WHERE m.chat_id = ? AND m.message_id = ? AND x.role = 'thumbnail'",
		{ group ? kGroup : kUser, id });
	if (rows.empty() || !rows[0][0].has_value())
		return 0;
	return std::stoull(*rows[0][0]);
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
	std::map<std::pair<std::string, std::string>,
		 std::pair<uint64_t, uint64_t>> before;
	for (const auto &c : db.countStillCandidates())
		before[{ c.table, c.kind }] = { c.messages, c.videos };

	models::Group g;
	g.id = kGroup;
	g.title = "stills test";
	db.upsertGroup(g);
	models::User u;
	u.first_name = "stills test";
	u.id = kUser;
	db.upsertUser(u);

	const uint64_t vA = file(raw, "vA", "video", "mp4", 1, true);
	const uint64_t vB = file(raw, "vB", "video", "mp4", 2, true);
	const uint64_t vC = file(raw, "vC", "video", "mp4", 3, true);
	const uint64_t vD = file(raw, "vD", "animation", "mp4", 4, false);
	const uint64_t vE = file(raw, "vE", "video_note", "mp4", 5, true);
	const uint64_t webm = file(raw, "webm", "sticker", "webm", 6, true);
	const uint64_t webp = file(raw, "webp", "sticker", "webp", 7, true);
	const uint64_t vF = file(raw, "vF", "video", "mp4", 8, true);
	const uint64_t doc = file(raw, "doc", "document", "mp4", 9, true);
	const uint64_t vH = file(raw, "vH", "video", "mp4", 10, true);
	const uint64_t vI = file(raw, "vI", "video", "mp4", 11, true);
	const uint64_t vJ = file(raw, "vJ", "video", "mp4", 12, true);
	const uint64_t vK = file(raw, "vK", "video", "mp4", 0, true);
	const uint64_t vL = file(raw, "vL", "animation", "mp4", 13, true);
	const uint64_t vM = file(raw, "vM", "video", "mp4", 14, true);
	const uint64_t tgThumb = file(raw, "tgthumb", "thumbnail", "jpg", 20, true);
	const uint64_t tgThumbOff = file(raw, "tgthumboff", "thumbnail", "jpg",
					 21, false);
	const uint64_t cover = file(raw, "cover", "photo", "jpg", 22, true);
	/* Generated stills of vH (evicted) and vI (in the store). */
	const uint64_t genOff = db.upsertGeneratedStill(hex(30), "jpg", 900,
							 false, vH);
	const uint64_t genOn = db.upsertGeneratedStill(hex(31), "jpg", 900,
							true, vI);

	/* Group: candidates are the videos vA, vE, webm, vH, vM. */
	groupMessage(db, raw, 1, CT::Video, vA, R"({"duration": 3})");
	groupMessage(db, raw, 2, CT::Video, vA);	/* the same video */
	groupMessage(db, raw, 3, CT::Video, vB);	/* Telegram thumbnail */
	db.setMessageExtraFile(kGroup, 3, true, "thumbnail", 0, tgThumb);
	groupMessage(db, raw, 4, CT::Video, vC);	/* a cover */
	db.setMessageExtraFile(kGroup, 4, true, "cover", 0, cover);
	groupMessage(db, raw, 5, CT::Animation, vD);	/* video not stored */
	groupMessage(db, raw, 6, CT::VideoNote, vE);
	groupMessage(db, raw, 7, CT::Sticker, webm);
	groupMessage(db, raw, 8, CT::Sticker, webp);	/* not a video */
	groupMessage(db, raw, 9, CT::Video, vF, nullptr, true); /* deleted */
	groupMessage(db, raw, 10, CT::Document, doc);	/* not a video kind */
	groupMessage(db, raw, 11, CT::Video, vH);	/* generated, evicted */
	db.setMessageExtraFile(kGroup, 11, true, "thumbnail", 0, genOff);
	groupMessage(db, raw, 12, CT::Video, vI);	/* generated, stored */
	db.setMessageExtraFile(kGroup, 12, true, "thumbnail", 0, genOn);
	groupMessage(db, raw, 13, CT::Video, vJ);	/* Telegram's, evicted */
	db.setMessageExtraFile(kGroup, 13, true, "thumbnail", 0, tgThumbOff);
	groupMessage(db, raw, 14, CT::Video, 0);	/* no file at all */
	groupMessage(db, raw, 15, CT::Video, vK);	/* bytes never seen */
	groupMessage(db, raw, 16, CT::Video, vA);	/* vA, Telegram's thumb */
	db.setMessageExtraFile(kGroup, 16, true, "thumbnail", 0, tgThumb);
	groupMessage(db, raw, 17, CT::Video, vA);	/* vA with a cover */
	db.setMessageExtraFile(kGroup, 17, true, "cover", 0, cover);
	groupMessage(db, raw, 18, CT::Video, vM);	/* thumbnail row lost */
	db.setMessageExtraFile(kGroup, 18, true, "thumbnail", 0, tgThumb);
	raw.execute("UPDATE telegram_message_files x JOIN telegram_group_messages m"
		    " ON m.id = x.group_message_id SET x.file_id = NULL"
		    " WHERE m.chat_id = ? AND m.message_id = 18", { kGroup });

	/* Private: vA again, and vL with its duration. */
	privateMessage(db, 1, CT::Video, vA);
	privateMessage(db, 2, CT::Animation, vL, R"({"duration": 7})");

	auto ours = [&]() {
		std::map<uint64_t, StillCandidate> m;
		for (const auto &c : db.listStillCandidates())
			if (c.sha256_hex.rfind(kHexPrefix, 0) == 0)
				m[c.video_file_id] = c;
		return m;
	};

	printf("candidates:\n");
	auto c = ours();
	CHECK(c.size() == 6 && c.count(vA) && c.count(vE) && c.count(webm) &&
	      c.count(vH) && c.count(vM) && c.count(vL),
	      "the stored videos without a still: vA, vE, the .webm sticker,"
	      " vH (its still evicted), vM (its thumbnail row lost), vL");
	CHECK(!c.count(vB) && !c.count(vC) && !c.count(vI) && !c.count(vJ),
	      "not with a Telegram thumbnail (even evicted), a cover or a"
	      " generated still in the store");
	CHECK(!c.count(vD) && !c.count(vK) && !c.count(vF) && !c.count(webp) &&
	      !c.count(doc),
	      "not unstored videos, deleted messages, other stickers or kinds");
	CHECK(c[vA].kind == "video" && c[vA].duration == 3 &&
	      c[vA].sha256_hex == hex(1) && c[vA].ext == "mp4",
	      "one entry for vA, with its kind, duration, digest and ext");
	CHECK(c[vL].kind == "animation" && c[vL].duration == 7 &&
	      c[webm].kind == "sticker" && c[webm].ext == "webm" &&
	      c[vE].duration == 0,
	      "a private animation's duration; a sticker; no duration known");
	{
		auto all = db.listStillCandidates();
		bool sorted = std::is_sorted(all.begin(), all.end(),
			[](const StillCandidate &a, const StillCandidate &b) {
				return a.video_file_id > b.video_file_id;
			});
		CHECK(sorted, "newest video first");
	}

	printf("one video:\n");
	CHECK(db.stillCandidate(vA).has_value() &&
	      db.stillCandidate(vA)->sha256_hex == hex(1),
	      "vA needs a still");
	CHECK(db.stillCandidate(vL).has_value(), "vL (private) needs a still");
	CHECK(!db.stillCandidate(vB) && !db.stillCandidate(vI) &&
	      !db.stillCandidate(vD) && !db.stillCandidate(doc),
	      "vB, vI, vD and the document do not");

	printf("one message:\n");
	{
		auto a = db.stillCandidateForMessage(true, kGroup, 1);
		auto l = db.stillCandidateForMessage(false, kUser, 2);
		CHECK(a && a->video_file_id == vA && a->duration == 3 &&
		      l && l->video_file_id == vL && l->kind == "animation",
		      "a message that needs a still gives its video");
		CHECK(!db.stillCandidateForMessage(true, kGroup, 3) &&
		      !db.stillCandidateForMessage(true, kGroup, 8) &&
		      !db.stillCandidateForMessage(true, kGroup, 10) &&
		      !db.stillCandidateForMessage(true, kGroup, 16) &&
		      !db.stillCandidateForMessage(false, kUser, 99),
		      "one with a thumbnail, a still sticker, a document or"
		      " none at all gives nothing");
	}

	printf("dry-run counts:\n");
	{
		std::map<std::pair<std::string, std::string>,
			 std::pair<uint64_t, uint64_t>> after;
		for (const auto &x : db.countStillCandidates())
			after[{ x.table, x.kind }] = { x.messages, x.videos };
		auto added = [&](const char *t, const char *k) {
			auto a = after[{ t, k }], b = before[{ t, k }];
			return std::to_string(a.first - b.first) + "/" +
			       std::to_string(a.second - b.second);
		};
		/* Group videos: 1 and 2 (vA; 17 has a cover), 11 (vH), 18 (vM). */
		CHECK(added("group", "video") == "4/3",
		      "group: 4 video messages, 3 videos");
		CHECK(added("group", "video_note") == "1/1" &&
		      added("group", "sticker") == "1/1",
		      "group: 1 video note, 1 video sticker");
		CHECK(added("private", "video") == "1/1" &&
		      added("private", "animation") == "1/1",
		      "private: 1 video, 1 animation");
	}

	printf("recording a still:\n");
	const uint64_t s1 = db.upsertGeneratedStill(hex(40), "jpg", 5000, true, vA);
	{
		auto r = raw.query("SELECT tg_file_id, tg_unique_id IS NULL, file_type,"
				   " file_ext, on_disk, generated_from_file_id,"
				   " file_size FROM telegram_files WHERE id = ?",
				   { s1 });
		CHECK(r.size() == 1 && r[0][0] == "generated:" + hex(40) &&
		      r[0][1] == "1" && r[0][2] == "thumbnail" && r[0][3] == "jpg" &&
		      r[0][4] == "1" && r[0][5] == std::to_string(vA) &&
		      r[0][6] == "5000",
		      "generated:<sha256>, no unique id, thumbnail, jpg, on disk,"
		      " from vA");
	}
	CHECK(db.upsertGeneratedStill(hex(40), "jpg", 5000, true, vB) == s1,
	      "the same bytes again are the same row");
	CHECK(raw.query("SELECT generated_from_file_id FROM telegram_files"
			" WHERE id = ?", { s1 })[0][0] == std::to_string(vA),
	      "which keeps the video it was first taken from");
	CHECK(db.upsertGeneratedStill(hex(20), "jpg", 5000, true, vA) == tgThumb &&
	      raw.query("SELECT generated_from_file_id IS NULL FROM"
			" telegram_files WHERE id = ?", { tgThumb })[0][0] == "1",
	      "bytes equal to a Telegram file are that row, not marked"
	      " generated");
	CHECK(db.findGeneratedStill(vA) == s1 && db.findGeneratedStill(vI) ==
	      genOn && !db.findGeneratedStill(vH),
	      "found again by its video while it is in the store");
	{
		auto st = db.getFileStatus(s1);
		auto tg = db.getFileStatus(tgThumb);
		CHECK(st && st->generated && tg && !tg->generated,
		      "the file status says a generated still is one");
	}

	printf("linking it:\n");
	db.linkGeneratedStill(vA, s1);
	CHECK(thumbOf(raw, true, 1) == s1 && thumbOf(raw, true, 2) == s1 &&
	      thumbOf(raw, false, 1) == s1 && thumbOf(raw, true, 17) == s1,
	      "every live message of vA has it as its thumbnail");
	CHECK(thumbOf(raw, true, 16) == tgThumb,
	      "a Telegram thumbnail stays");
	CHECK(thumbOf(raw, true, 3) == tgThumb && thumbOf(raw, true, 6) == 0,
	      "messages of other videos are left alone");
	CHECK(!ours().count(vA) && !db.stillCandidate(vA),
	      "vA is no longer a candidate");
	{
		auto msgs = db.listStillMessages(s1);
		std::set<std::pair<int64_t, int64_t>> got(msgs.begin(), msgs.end());
		const std::set<std::pair<int64_t, int64_t>> want = {
			{ kGroup, 1 }, { kGroup, 2 }, { kGroup, 17 }, { kUser, 1 }
		};
		CHECK(got == want,
		      "the still's messages are listed for the events");
	}
	db.linkGeneratedStill(vM, db.upsertGeneratedStill(hex(41), "jpg", 10,
							   true, vM));
	CHECK(thumbOf(raw, true, 18) != 0 && !ours().count(vM),
	      "a thumbnail row whose file is gone is replaced");

	printf("a Telegram thumbnail later:\n");
	db.setMessageExtraFile(kGroup, 1, true, "thumbnail", 0, tgThumb);
	CHECK(thumbOf(raw, true, 1) == tgThumb,
	      "replaces the generated still");
	db.linkGeneratedStill(vA, s1);
	CHECK(thumbOf(raw, true, 1) == tgThumb && thumbOf(raw, true, 2) == s1,
	      "and linking the still again does not undo that");

	printf("eviction:\n");
	raw.execute("UPDATE telegram_files SET on_disk = 0 WHERE id = ?", { s1 });
	CHECK(ours().count(vA) && !db.findGeneratedStill(vA),
	      "an evicted still makes its video a candidate again");
	CHECK(db.upsertGeneratedStill(hex(40), "jpg", 5000, true, vA) == s1 &&
	      raw.query("SELECT on_disk FROM telegram_files WHERE id = ?",
			{ s1 })[0][0] == "1",
	      "taken again with the same bytes it is the same row, stored");
	CHECK(!ours().count(vA), "and vA is done again");
	raw.execute("UPDATE telegram_files SET on_disk = 0 WHERE id = ?", { s1 });
	const uint64_t s2 = db.upsertGeneratedStill(hex(42), "jpg", 4000, true, vA);
	db.linkGeneratedStill(vA, s2);
	CHECK(s2 != s1 && thumbOf(raw, true, 2) == s2 &&
	      thumbOf(raw, false, 1) == s2 && thumbOf(raw, true, 1) == tgThumb,
	      "a still with other bytes replaces the evicted one, not"
	      " Telegram's");

	printf("an evicted still's videos:\n");
	db.linkGeneratedStill(vE, s2);	/* the same frame on another video */
	{
		auto v = db.listStillVideos(s2);
		CHECK(std::set<uint64_t>(v.begin(), v.end()) ==
			      std::set<uint64_t>({ vA, vE }) && v.size() == 2,
		      "the video it was taken from and every video it is the"
		      " thumbnail of, each once");
		auto h = db.listStillVideos(genOff);
		CHECK(h == std::vector<uint64_t>({ vH }), "vH for its still");
		CHECK(db.listStillVideos(s1) == std::vector<uint64_t>({ vA }),
		      "a still no message shows any more: its own video");
	}
	{
		/* The janitor tells a generated still from the rest. */
		std::map<uint64_t, bool> gen;
		for (const auto &e : db.listEvictCandidates("", "", "", 0, 100000))
			gen[e.id] = e.generated;
		CHECK(gen.count(s2) && gen[s2] && gen.count(genOn) && gen[genOn] &&
		      gen.count(tgThumb) && !gen[tgThumb] && gen.count(vA) &&
		      !gen[vA] && !gen.count(s1),
		      "an eviction candidate says whether it is a generated"
		      " still");
	}

	printf("plan:\n");
	{
		/* Each table the list's statement reads, by alias, with the
		 * key and access type of every read of it. */
		auto rows = raw.query("EXPLAIN FORMAT=TRADITIONAL " +
				      DB::stillCandidatesSql());
		std::map<std::string, std::set<std::string>> by;
		for (const auto &r : rows) {
			if (r.size() < 7)
				continue;
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
		CHECK(by["v"] == std::set<std::string>({ "PRIMARY/eq_ref" }) &&
		      by["t"] == std::set<std::string>({ "PRIMARY/eq_ref" }),
		      "files read by their primary key");
		bool uq = by["x"].size() == 2;
		for (const auto &p : by["x"])
			uq = uq && p.rfind("uq_message_files_", 0) == 0;
		CHECK(uq, "message files read by their unique keys");

		/* The check of one video: by the messages' file_id index. */
		std::string one = DB::stillCandidateSql();
		for (size_t at; (at = one.find('?')) != std::string::npos;)
			one.replace(at, 1, std::to_string(vA));
		std::set<std::string> keys;
		for (const auto &r : raw.query("EXPLAIN FORMAT=TRADITIONAL " + one))
			if (r.size() >= 7 && r[2].value_or("") == "m")
				keys.insert(r[6].value_or("NULL") + "/" +
					    r[4].value_or("NULL"));
		for (const auto &k : keys)
			printf("  one video, m: %s\n", k.c_str());
		CHECK(keys == std::set<std::string>(
			      { "fk_group_messages_file/ref",
				"fk_private_messages_file/ref" }),
		      "one video's messages read by their file_id");

		/* The check of one message: by its unique key. */
		for (bool group : { true, false }) {
			std::string q = DB::stillMessageSql(group);
			q.replace(q.find('?'), 1, std::to_string(group ? kGroup : kUser));
			q.replace(q.find('?'), 1, "1");
			std::set<std::string> k;
			for (const auto &r : raw.query("EXPLAIN FORMAT=TRADITIONAL " + q))
				if (r.size() >= 7)
					k.insert(r[2].value_or("NULL") + ":" +
						 r[6].value_or("NULL") + "/" +
						 r[4].value_or("NULL"));
			for (const auto &x : k)
				printf("  one message: %s\n", x.c_str());
			const std::string want = std::string("m:uq_") +
				(group ? "group" : "private") +
				"_messages_chat_msg/const";
			CHECK(k.count(want) == 1, group
			      ? "a group message read by its unique key"
			      : "a private message read by its unique key");
		}
	}

	{
		/* An evicted still's videos: the still by its primary key, its
		 * links by their file_id, their messages by primary key. */
		std::string q = DB::stillVideosSql();
		for (size_t at; (at = q.find('?')) != std::string::npos;)
			q.replace(at, 1, std::to_string(s2));
		std::set<std::string> k;
		for (const auto &r : raw.query("EXPLAIN FORMAT=TRADITIONAL " + q))
			if (r.size() >= 7 && r[2].has_value())
				k.insert(*r[2] + ":" + r[6].value_or("NULL") +
					 "/" + r[4].value_or("NULL"));
		for (const auto &x : k)
			printf("  a still's videos: %s\n", x.c_str());
		bool ok = k.count("telegram_files:PRIMARY/const") == 1 &&
			  k.count("x:idx_message_files_file/ref") == 1 &&
			  k.count("m:PRIMARY/eq_ref") == 1;
		for (const auto &x : k)
			ok = ok && (x.rfind("<union", 0) == 0 ||
				    x == "telegram_files:PRIMARY/const" ||
				    x == "x:idx_message_files_file/ref" ||
				    x == "m:PRIMARY/eq_ref");
		CHECK(ok, "an evicted still's videos read by keys only");
	}

	printf("the video's row deleted:\n");
	raw.execute("DELETE FROM telegram_group_messages WHERE chat_id = ? AND"
		    " message_id = 12", { kGroup });
	raw.execute("DELETE FROM telegram_files WHERE id = ?", { vI });
	CHECK(raw.query("SELECT generated_from_file_id IS NULL FROM"
			" telegram_files WHERE id = ?", { genOn })[0][0] == "1",
	      "clears generated_from_file_id; the still stays generated:");

	cleanup(raw);
	if (g_failures) {
		printf("%d check(s) failed\n", g_failures);
		return 1;
	}
	printf("all checks passed\n");
	return 0;
}
