// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
/*
 * file_ext_db_test -- a stored file's row and its blob agree on the
 * extension: the blob is <sha256>[.file_ext], so a row whose file_ext
 * changes after its bytes are stored names a blob that is not there.
 * Against a MySQL with every migration applied. It writes rows under
 * remote ids no real file has and deletes them after; run it only
 * against a throwaway database:
 *
 *   TGL_TEST_MYSQL_HOST=127.0.0.1 TGL_TEST_MYSQL_PORT=3306
 *   TGL_TEST_MYSQL_USER=root TGL_TEST_MYSQL_PASSWORD=...
 *   TGL_TEST_MYSQL_DB=tgloggerd build/file_ext_db_test
 *
 * Without TGL_TEST_MYSQL_HOST it does nothing and exits 77 (skipped).
 */
#include <tgloggerd/DB.hpp>

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>

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

static const char *kPrefix = "file_ext_db_test:";

static std::string env(const char *k, const char *def = "")
{
	const char *v = std::getenv(k);
	return v ? v : def;
}

static void cleanup(mysql::Database &raw)
{
	raw.execute("DELETE FROM telegram_files WHERE tg_file_id LIKE ?",
		    { std::string(kPrefix) + "%" });
}

static models::File file(const std::string &name, const std::string &hex,
			 std::optional<std::string> ext)
{
	models::File f;
	f.tg_file_id = std::string(kPrefix) + name;
	f.tg_unique_id = std::string(kPrefix) + name + ":u";
	f.file_type = "sticker";
	f.file_size = 10;
	f.sha256_hex = hex;
	f.file_ext = std::move(ext);
	return f;
}

/* file_ext of row @id, "NULL" when it has none. */
static std::string extOf(mysql::Database &raw, uint64_t id)
{
	auto rows = raw.query("SELECT file_ext FROM telegram_files WHERE id = ?",
			      { (int64_t)id });
	if (rows.empty())
		return "(no row)";
	return rows[0][0].value_or("NULL");
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

	const std::string h1(64, 'a');

	printf("metadata seen again after the bytes are stored:\n");
	{
		/* Stored from a local path without an extension. */
		uint64_t id = db.upsertFile(file("stored", h1, std::nullopt));
		CHECK(extOf(raw, id) == "NULL", "stored without an extension");
		/* The same file's metadata again, now with one. */
		models::File meta = file("stored", "", std::string("tgs"));
		meta.on_disk = false;
		models::KnownFile k = db.upsertFileMeta(meta);
		CHECK(k.id == id, "the metadata finds the stored row");
		CHECK(extOf(raw, id) == "NULL",
		      "a stored row keeps the extension its blob is named by");
	}

	printf("metadata before the bytes:\n");
	{
		models::File meta = file("meta", "", std::string("tgs"));
		meta.on_disk = false;
		models::KnownFile k = db.upsertFileMeta(meta);
		CHECK(extOf(raw, k.id) == "tgs",
		      "a row without bytes still takes the extension");
	}

	cleanup(raw);
	if (g_failures) {
		printf("%d check(s) failed\n", g_failures);
		return 1;
	}
	printf("all checks passed\n");
	return 0;
}
