// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
/*
 * photo_history_db_test -- the video of an animated profile or group photo
 * in the photo history (migration 000051): DB::setPhotoHistoryVideo and
 * finding the video again through its history row (listFileSources).
 * Needs a throwaway MySQL with every migration applied:
 *   TGL_TEST_MYSQL_HOST=127.0.0.1 TGL_TEST_MYSQL_PASSWORD=...
 *   TGL_TEST_MYSQL_DB=tgloggerd build/photo_history_db_test
 * Without TGL_TEST_MYSQL_HOST it does nothing and exits 77 (skipped).
 */
#include <tgloggerd/DB.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace tgloggerd;

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

/* Ids no real chat has. */
static const int64_t kUser = 4243;
static const int64_t kGroup = -4243003;

static std::string env(const char *k, const char *def = "")
{
	const char *v = std::getenv(k);
	return v ? v : def;
}

static std::string one(mysql::Database &db, const std::string &sql,
		       const std::vector<mysql::Param> &args = {})
{
	auto rows = db.query(sql, args);
	if (rows.empty() || rows[0].empty())
		return "<none>";
	return rows[0][0].value_or("NULL");
}

static uint64_t meta(DB &db, const std::string &tag, const char *type,
		     const char *ext)
{
	models::File f;
	f.tg_file_id = "photo-video-test-" + tag;
	f.tg_unique_id = "photo-video-test-u-" + tag;
	f.file_type = type;
	f.on_disk = false;
	f.file_ext = ext;
	return db.upsertFileMeta(f).id;
}

static void cleanup(mysql::Database &raw)
{
	raw.execute("DELETE FROM telegram_user_hist_profile_photo WHERE user_id = ?", { kUser });
	raw.execute("DELETE FROM telegram_group_hist_photo WHERE group_id = ?", { kGroup });
	raw.execute("DELETE FROM telegram_users WHERE id = ?", { kUser });
	raw.execute("DELETE FROM `telegram_groups` WHERE id = ?", { kGroup });
	raw.execute("DELETE FROM telegram_files WHERE tg_file_id LIKE 'photo-video-test-%'");
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

	models::User u;
	u.id = kUser;
	u.first_name = "photo video test";
	db.upsertUser(u);
	models::Group g;
	g.id = kGroup;
	g.title = "photo video test";
	db.upsertGroup(g);

	const uint64_t still = meta(db, "still", "photo", "jpg");
	const uint64_t other = meta(db, "other", "photo", "jpg");
	const uint64_t video = meta(db, "video", "video", "mp4");
	db.ensurePhotoHistory(kUser, false, still, 1700000000);
	db.ensurePhotoHistory(kUser, false, other, 1700000100);
	db.ensurePhotoHistory(kGroup, true, still, 1700000000);

	printf("linking an animated photo's video:\n");
	CHECK(db.setPhotoHistoryVideo(kUser, false, still, video),
	      "the user's history row of the still gets the video");
	CHECK(one(raw, "SELECT video_file_id FROM telegram_user_hist_profile_photo"
		  " WHERE user_id = ? AND file_id = ?", { kUser, (int64_t)still }) ==
	      std::to_string(video), "video_file_id set");
	CHECK(one(raw, "SELECT IFNULL(video_file_id, 'NULL') FROM"
		  " telegram_user_hist_profile_photo WHERE user_id = ? AND file_id = ?",
		  { kUser, (int64_t)other }) == "NULL", "another photo keeps none");
	CHECK(!db.setPhotoHistoryVideo(kUser, false, still, video),
	      "linking again changes nothing");
	CHECK(!db.setPhotoHistoryVideo(kUser, false, 0, video) &&
		      !db.setPhotoHistoryVideo(kUser, false, still, 0),
	      "no still or no video: nothing");
	CHECK(db.setPhotoHistoryVideo(kGroup, true, still, video) &&
		      one(raw, "SELECT video_file_id FROM telegram_group_hist_photo"
			  " WHERE group_id = ?", { kGroup }) == std::to_string(video),
	      "the group's history row too");

	printf("finding the video again:\n");
	auto src = db.listFileSources(video, 5);
	bool user_src = false, group_src = false;
	for (const auto &s : src) {
		user_src |= s.kind == models::FileSource::Kind::UserPhotoHistory &&
			    s.chat_id == kUser && s.unique_id == "photo-video-test-u-video";
		group_src |= s.kind == models::FileSource::Kind::GroupPhotoHistory &&
			     s.chat_id == kGroup && s.unique_id == "photo-video-test-u-video";
	}
	CHECK(user_src, "through the user's photo list, by the video's unique id");
	CHECK(group_src, "through the group's photos, by the video's unique id");

	printf("the video file going away:\n");
	raw.execute("DELETE FROM telegram_files WHERE id = ?", { (int64_t)video });
	CHECK(one(raw, "SELECT IFNULL(video_file_id, 'NULL') FROM"
		  " telegram_user_hist_profile_photo WHERE user_id = ? AND file_id = ?",
		  { kUser, (int64_t)still }) == "NULL",
	      "the history row stays, without a video");

	cleanup(raw);
	if (g_failures) {
		printf("%d check(s) failed\n", g_failures);
		return 1;
	}
	printf("all checks passed\n");
	return 0;
}
