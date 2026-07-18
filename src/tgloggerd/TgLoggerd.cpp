// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 */
#include <tgloggerd/helpers/common.h>
#include <tgloggerd/TgLoggerd.hpp>
#include <stdexcept>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cctype>
#include <fstream>
#include <algorithm>
#include <filesystem>

#include <openssl/evp.h>
#include <csignal>

namespace fs = std::filesystem;

/* Defined in main.c; set by the SIGINT/SIGTERM handler to stop the loop. */
extern "C" volatile sig_atomic_t g_tgld_stop;

namespace tgloggerd {

namespace {

/* Compute the lowercase hex SHA-256 digest of a file's contents. */
std::optional<std::string> sha256_file_hex(const std::string &path)
{
	std::ifstream f(path, std::ios::binary);
	if (!f)
		return std::nullopt;

	EVP_MD_CTX *ctx = EVP_MD_CTX_new();
	if (!ctx)
		return std::nullopt;

	if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) {
		EVP_MD_CTX_free(ctx);
		return std::nullopt;
	}

	char buf[65536];
	while (f) {
		f.read(buf, sizeof(buf));
		std::streamsize n = f.gcount();
		if (n > 0)
			EVP_DigestUpdate(ctx, buf, (size_t)n);
	}

	unsigned char digest[EVP_MAX_MD_SIZE];
	unsigned int len = 0;
	if (EVP_DigestFinal_ex(ctx, digest, &len) != 1 || len != 32) {
		EVP_MD_CTX_free(ctx);
		return std::nullopt;
	}
	EVP_MD_CTX_free(ctx);

	static const char hex[] = "0123456789abcdef";
	std::string out;
	out.reserve(64);
	for (unsigned int i = 0; i < len; i++) {
		out.push_back(hex[digest[i] >> 4]);
		out.push_back(hex[digest[i] & 0x0f]);
	}
	return out;
}

} /* namespace */

TgLoggerd::TgLoggerd(uint32_t api_id, const char *api_hash, const char *data_dir) noexcept
{
	this->api_id_ = api_id;
	snprintf(this->api_hash_, sizeof(this->api_hash_), "%s", api_hash);
	snprintf(this->data_dir_, sizeof(this->data_dir_), "%s", data_dir);
}

TgLoggerd::~TgLoggerd(void)
{
	pr_debug(l_, "Destroying TgLoggerd...");
	pr_free(l_);
}

inline int TgLoggerd::initDataDirC(const char *dir)
{
	char path[sizeof(this->data_dir_) + 256];
	int r;

	snprintf(path, sizeof(path), "%s/%s", this->data_dir_, dir);
	pr_debug(l_, "Initializing data directory: %s", path);
	r = mkdir_recursive(path, 0700);
	if (r < 0) {
		if (r == -EEXIST) {
			pr_debug(l_, "Data directory already exists: %s", path);
			return 0;
		}

		pr_error(l_, "Failed to create data directory: %s, error: %s",
			 path, strerror(-r));
		return r;
	} else {
		pr_debug(l_, "Data directory created: %s", path);
		return 0;
	}
}

inline int TgLoggerd::initDataDir(void)
{
	int r;

	r = initDataDirC("logs");
	if (r < 0)
		return r;

	r = initDataDirC("tdlib");
	if (r < 0)
		return r;

	return 0;
}

int TgLoggerd::start(void)
{
	if (initDataDir())
		return -1;

	pr_info(l_, "Starting tgloggerd...");
	pr_debug(l_, "api_id: %u", this->api_id_);
	pr_debug(l_, "api_hash: %s", this->api_hash_);
	pr_debug(l_, "data_dir: %s", this->data_dir_);

	auto env = [](const char *key, const char *def) -> std::string {
		const char *v = getenv(key);
		return (v && *v) ? std::string(v) : std::string(def);
	};

	mysql::Config db_cfg;
	db_cfg.host = env("TG_DB_HOST", "127.0.0.1");
	db_cfg.port = (uint16_t)atoi(env("TG_DB_PORT", "3306").c_str());
	db_cfg.user = env("TG_DB_USER", "tgloggerd");
	db_cfg.password = env("TG_DB_PASSWORD", "tgloggerd");
	db_cfg.database = env("TG_DB_NAME", "tgloggerd");

	/*
	 * Worker sizing. The file pool and the serial worker each write to
	 * the DB, so the connection pool must have at least file_threads + 1
	 * connections or those writers would serialize on acquire().
	 */
	size_t file_threads = (size_t)atoi(env("TG_FILE_THREADS", "4").c_str());
	if (file_threads < 1)
		file_threads = 1;
	std::string db_pool_def = std::to_string(file_threads + 2);
	size_t db_pool = (size_t)atoi(env("TG_DB_POOL", db_pool_def.c_str()).c_str());
	if (db_pool < file_threads + 1)
		db_pool = file_threads + 1;
	db_cfg.pool_size = db_pool;

	size_t queue_max = (size_t)strtoull(
		env("TG_QUEUE_MAX", "10000").c_str(), nullptr, 10);

	pr_debug(l_, "db: %s@%s:%u/%s pool=%zu", db_cfg.user.c_str(),
		 db_cfg.host.c_str(), db_cfg.port, db_cfg.database.c_str(),
		 db_cfg.pool_size);

	db_ = std::make_unique<DB>(db_cfg);
	try {
		db_->ping();
	} catch (const std::exception &e) {
		pr_error(l_, "Failed to connect to the database: %s", e.what());
		return -1;
	}

	storage_dir_ = env("TG_STORAGE_DIR", "./data/storage/files");
	try {
		fs::create_directories(storage_dir_);
	} catch (const std::exception &e) {
		pr_error(l_, "Failed to create storage directory %s: %s",
			 storage_dir_.c_str(), e.what());
		return -1;
	}
	pr_debug(l_, "storage_dir: %s", storage_dir_.c_str());

	/*
	 * Construct the workers only past the early-return points above, so a
	 * failed startup never leaves threads to join. serial_ = 1 thread
	 * (strict FIFO); files_ = file_threads.
	 */
	serial_ = std::make_unique<ThreadPool>(1, queue_max, l_);
	files_ = std::make_unique<ThreadPool>(file_threads, queue_max, l_);
	pr_debug(l_, "workers: serial=1 files=%zu queue_max=%zu",
		 file_threads, queue_max);

	char tdlib_path[sizeof(this->data_dir_) + 32];
	snprintf(tdlib_path, sizeof(tdlib_path), "%s/tdlib", this->data_dir_);

	tdlib_ = std::make_unique<TDLib>(this->api_id_, this->api_hash_,
					 tdlib_path);
	/*
	 * Handlers run on the event loop, where the models are already built
	 * from td_api objects. They only enqueue the persistence work: DB
	 * upserts go to serial_ (order preserved); media hashing goes to
	 * files_, which posts the row->file link back to serial_. Each model
	 * is captured by value so the job is self-contained.
	 */
	tdlib_->setUserHandler([this](const models::User &u) {
		serial_->post([this, u] {
			try {
				db_->upsertUser(u);
			} catch (const std::exception &e) {
				pr_error(l_, "Failed to store user %lld: %s",
					 (long long)u.id, e.what());
			}
		});
	});
	tdlib_->setUserFullInfoHandler([this](const models::UserFullInfo &fi) {
		serial_->post([this, fi] {
			try {
				db_->upsertUserFullInfo(fi);
			} catch (const std::exception &e) {
				pr_error(l_, "Failed to store user full info"
					 " %lld: %s", (long long)fi.user_id,
					 e.what());
			}
		});
	});
	tdlib_->setProfilePhotoHandler([this](const ProfilePhoto &p) {
		files_->post([this, p] {
			try {
				onProfilePhoto(p);
			} catch (const std::exception &e) {
				pr_error(l_, "Failed to store profile photo for"
					 " user %lld: %s", (long long)p.user_id,
					 e.what());
			}
		});
	});
	tdlib_->setGroupHandler([this](const models::Group &g) {
		serial_->post([this, g] {
			try {
				db_->upsertGroup(g);
			} catch (const std::exception &e) {
				pr_error(l_, "Failed to store group %lld: %s",
					 (long long)g.id, e.what());
			}
		});
	});
	tdlib_->setGroupAdminsHandler([this](const models::GroupAdminList &al) {
		serial_->post([this, al] {
			try {
				db_->syncGroupAdmins(al);
			} catch (const std::exception &e) {
				pr_error(l_, "Failed to sync group admins for"
					 " %lld: %s", (long long)al.group_id,
					 e.what());
			}
		});
	});
	tdlib_->setGroupPhotoHandler([this](const GroupPhoto &p) {
		files_->post([this, p] {
			try {
				onGroupPhoto(p);
			} catch (const std::exception &e) {
				pr_error(l_, "Failed to store group photo for"
					 " group %lld: %s", (long long)p.group_id,
					 e.what());
			}
		});
	});
	tdlib_->setMessageHandler([this](const TextMessage &msg) {
		pr_info(l_, "New message | sender_id=%lld name=\"%s\" "
			    "username=\"%s\" msg_id=%lld text=\"%s\"",
			(long long)msg.sender_id, msg.sender_name.c_str(),
			msg.sender_username.c_str(), (long long)msg.message_id,
			msg.text.c_str());
	});
	tdlib_->setPrivateMessageHandler([this](const models::PrivateMessage &pm) {
		serial_->post([this, pm] {
			try {
				db_->upsertPrivateMessage(pm);
			} catch (const std::exception &e) {
				pr_error(l_, "Failed to store private message"
					 " chat_id=%lld msg_id=%lld: %s",
					 (long long)pm.chat_id,
					 (long long)pm.message_id, e.what());
			}
		});
	});
	tdlib_->setGroupMessageHandler([this](const models::GroupMessage &gm) {
		serial_->post([this, gm] {
			try {
				db_->upsertGroupMessage(gm);
			} catch (const std::exception &e) {
				pr_error(l_, "Failed to store group message"
					 " chat_id=%lld msg_id=%lld: %s",
					 (long long)gm.chat_id,
					 (long long)gm.message_id, e.what());
			}
		});
	});
	tdlib_->setMessageFileHandler([this](const MessageFile &mf) {
		files_->post([this, mf] {
			try {
				onMessageFile(mf);
			} catch (const std::exception &e) {
				pr_error(l_, "Failed to store message file"
					 " chat_id=%lld msg_id=%lld: %s",
					 (long long)mf.chat_id,
					 (long long)mf.message_id, e.what());
			}
		});
	});
	tdlib_->setMessageReplyHandler([this](const MessageReply &mr) {
		serial_->post([this, mr] {
			try {
				if (mr.is_group)
					db_->setGroupMessageReply(mr.chat_id,
						mr.message_id,
						mr.reply_to_chat_id,
						mr.reply_to_msg_id);
				else
					db_->setPrivateMessageReply(mr.chat_id,
						mr.message_id,
						mr.reply_to_chat_id,
						mr.reply_to_msg_id);
			} catch (const std::exception &e) {
				pr_error(l_, "Failed to link reply chat_id=%lld"
					 " msg_id=%lld: %s", (long long)mr.chat_id,
					 (long long)mr.message_id, e.what());
			}
		});
	});

	/*
	 * Periodic group-admin polling. Interval <= 0 disables it. Set before
	 * the loop so it is in place when the client authorizes.
	 */
	double admin_interval = atof(env("TG_ADMIN_POLL_INTERVAL", "300").c_str());
	int admin_batch = atoi(env("TG_ADMIN_POLL_BATCH", "4").c_str());
	tdlib_->setAdminPollConfig(admin_interval, admin_batch);
	pr_debug(l_, "admin poll: interval=%.0fs batch=%d", admin_interval,
		 admin_batch);

	/*
	 * Background message backfiller. It walks every accessible chat's
	 * history newest->oldest, round-robin and gently paced, feeding messages
	 * through the same handlers as real time. Progress is persisted to
	 * chat_backfill_state (fire-and-forget on serial_) and reloaded here so
	 * each chat's walk resumes across restarts. Interval <= 0 disables it.
	 */
	double bf_interval = atof(env("TG_BACKFILL_INTERVAL", "3").c_str());
	double bf_discovery = atof(env("TG_BACKFILL_DISCOVERY_INTERVAL", "300").c_str());
	int bf_page = atoi(env("TG_BACKFILL_PAGE", "100").c_str());
	int bf_inflight = atoi(env("TG_BACKFILL_INFLIGHT", "1").c_str());
	tdlib_->setBackfillConfig(bf_interval, bf_discovery, bf_page, bf_inflight);
	pr_debug(l_, "backfill: interval=%.1fs discovery=%.0fs page=%d inflight=%d",
		 bf_interval, bf_discovery, bf_page, bf_inflight);

	tdlib_->setBackfillStateHandler([this](const models::BackfillState &st) {
		serial_->post([this, st] {
			try {
				db_->upsertBackfillState(st);
			} catch (const std::exception &e) {
				pr_error(l_, "Failed to persist backfill state"
					 " chat_id=%lld: %s",
					 (long long)st.chat_id, e.what());
			}
		});
	});

	if (bf_interval > 0.0) {
		try {
			auto states = db_->loadBackfillState();
			tdlib_->loadBackfillState(states);
			pr_debug(l_, "backfill: resumed %zu chats from state table",
				 states.size());
		} catch (const std::exception &e) {
			pr_error(l_, "Failed to load backfill state: %s",
				 e.what());
		}
	}

	pr_info(l_, "Listening for incoming messages...");
	while (!tdlib_->isStopped() && !g_tgld_stop)
		tdlib_->loop(10);
	if (g_tgld_stop)
		pr_info(l_, "Shutdown requested; draining pending writes...");

	/*
	 * Drain before returning, while db_ and l_ are still alive (the
	 * destructor frees l_ before members are destroyed). files_ first so
	 * its pending link jobs land in serial_, then serial_ drains fully.
	 */
	files_->shutdown();
	serial_->shutdown();

	return 0;
}

int TgLoggerd::stop(void)
{
	pr_info(l_, "Stopping tgloggerd...");
	if (tdlib_)
		tdlib_->close();
	return 0;
}

/*
 * onProfilePhoto/onGroupPhoto/onMessageFile run on a files_ worker: the
 * hash+copy+upsertFile happens there (in parallel), then only the row->file
 * link update is posted to serial_ so it is ordered after the entity/message
 * row was written and serialized against other DB writes.
 */
void TgLoggerd::onProfilePhoto(const ProfilePhoto &p)
{
	auto file_id = storeDownloadedFile(p.local_path, p.tg_file_id,
					   p.file_size, "photo");
	if (!file_id.has_value())
		return;

	int64_t user_id = p.user_id;
	uint64_t fid = *file_id;
	serial_->post([this, user_id, fid] {
		try {
			db_->setUserProfilePhoto(user_id, fid);
			pr_info(l_, "Stored profile photo | user_id=%lld"
				" file_id=%llu", (long long)user_id,
				(unsigned long long)fid);
		} catch (const std::exception &e) {
			pr_error(l_, "Failed to link profile photo for user"
				 " %lld: %s", (long long)user_id, e.what());
		}
	});
}

void TgLoggerd::onGroupPhoto(const GroupPhoto &p)
{
	auto file_id = storeDownloadedFile(p.local_path, p.tg_file_id,
					   p.file_size, "photo");
	if (!file_id.has_value())
		return;

	int64_t group_id = p.group_id;
	uint64_t fid = *file_id;
	serial_->post([this, group_id, fid] {
		try {
			db_->setGroupPhoto(group_id, fid);
			pr_info(l_, "Stored group photo | group_id=%lld"
				" file_id=%llu", (long long)group_id,
				(unsigned long long)fid);
		} catch (const std::exception &e) {
			pr_error(l_, "Failed to link group photo for group"
				 " %lld: %s", (long long)group_id, e.what());
		}
	});
}

void TgLoggerd::onMessageFile(const MessageFile &m)
{
	auto file_id = storeDownloadedFile(m.local_path, m.tg_file_id,
					   m.file_size, m.content_type.c_str(),
					   m.orig_file_name);
	if (!file_id.has_value())
		return;

	int64_t chat_id = m.chat_id;
	int64_t message_id = m.message_id;
	bool is_group = m.is_group;
	uint64_t fid = *file_id;
	serial_->post([this, chat_id, message_id, is_group, fid] {
		try {
			if (is_group)
				db_->setGroupMessageFile(chat_id, message_id,
							 fid);
			else
				db_->setPrivateMessageFile(chat_id, message_id,
							   fid);
			pr_info(l_, "Stored message file | %s chat_id=%lld"
				" msg_id=%lld file_id=%llu",
				is_group ? "group" : "private",
				(long long)chat_id, (long long)message_id,
				(unsigned long long)fid);
		} catch (const std::exception &e) {
			pr_error(l_, "Failed to link message file chat_id=%lld"
				 " msg_id=%lld: %s", (long long)chat_id,
				 (long long)message_id, e.what());
		}
	});
}

std::optional<uint64_t>
TgLoggerd::storeDownloadedFile(const std::string &local_path,
			       const std::string &tg_file_id,
			       int64_t file_size, const char *file_type,
			       const std::string &orig_file_name)
{
	auto hex = sha256_file_hex(local_path);
	if (!hex.has_value()) {
		pr_error(l_, "Failed to hash file: %s", local_path.c_str());
		return std::nullopt;
	}

	/* Content-addressed destination name: <sha256>[.ext]. */
	std::string ext = fs::path(local_path).extension().string();
	if (!ext.empty() && ext[0] == '.')
		ext.erase(0, 1);
	std::transform(ext.begin(), ext.end(), ext.begin(),
		       [](unsigned char c) { return (char)std::tolower(c); });

	std::string name = *hex;
	if (!ext.empty())
		name += "." + ext;

	/*
	 * Fan out into 5 levels of two-hex-digit directories based on the
	 * first 5 octets of the digest, e.g. for 0a533d97ed... the file is
	 * stored as 0a/53/3d/97/ed/<sha256>[.ext].
	 */
	fs::path dir = storage_dir_;
	for (int i = 0; i < 5; i++)
		dir /= hex->substr((size_t)i * 2, 2);
	fs::path dest = dir / name;

	std::error_code ec;
	fs::create_directories(dir, ec);
	if (ec) {
		pr_error(l_, "Failed to create storage directory %s: %s",
			 dir.c_str(), ec.message().c_str());
		return std::nullopt;
	}
	/*
	 * skip_existing makes the copy a no-op (no error) when the
	 * content-addressed destination is already present, which also makes
	 * concurrent file-pool workers copying identical content race-free.
	 */
	fs::copy_file(local_path, dest, fs::copy_options::skip_existing, ec);
	if (ec) {
		pr_error(l_, "Failed to copy file to %s: %s",
			 dest.c_str(), ec.message().c_str());
		return std::nullopt;
	}

	models::File f;
	f.tg_file_id = tg_file_id;
	f.file_type = file_type;
	f.file_size = (uint64_t)(file_size < 0 ? 0 : file_size);
	f.sha256_hex = *hex;
	if (!ext.empty())
		f.file_ext = ext;
	f.orig_file_name = orig_file_name;

	return db_->upsertFile(f);
}

void TgLoggerd::setLogger(log_hd_t *h) noexcept
{
	l_ = h;
}

void TgLoggerd::setLoggerLevel(int8_t log_level) noexcept
{
	pr_set_log_level(l_, log_level);
}

} /* namespace tgloggerd */
