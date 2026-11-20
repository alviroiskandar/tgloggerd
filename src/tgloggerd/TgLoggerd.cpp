// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 */
#include <tgloggerd/helpers/common.h>
#include <tgloggerd/TgLoggerd.hpp>
#include <tgloggerd/InternalApi.hpp>
#include <tgloggerd/FileApi.hpp>
#include <tgloggerd/StoragePath.hpp>
#include <tgloggerd/StorageJanitor.hpp>
#include <tgloggerd/StillsWorker.hpp>
#include <tgloggerd/StorageApi.hpp>
#include <tgloggerd/StatusApi.hpp>
#include <tgloggerd/FileFetcher.hpp>
#include <tgloggerd/ChatApi.hpp>
#include <tgloggerd/PhotoApi.hpp>
#include <tgloggerd/EmojiApi.hpp>
#include <tgloggerd/RecheckApi.hpp>
#include <tgloggerd/ContactApi.hpp>
#include <tgloggerd/BotWatch.hpp>
#include <tgloggerd/EventBus.hpp>
#include <tgloggerd/EventApi.hpp>
#include <stdexcept>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cctype>
#include <fstream>
#include <algorithm>
#include <filesystem>
#include <future>
#include <chrono>

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

/* The events of a message upsert, once it is committed. */
void publish_message(EventBus &ev, int64_t chat_id, int64_t message_id,
		     MsgChange c, std::optional<int64_t> sender_user_id)
{
	switch (c) {
	case MsgChange::New:
		ev.messageNew(chat_id, message_id);
		ev.countersMoved(chat_id, sender_user_id.value_or(0));
		break;
	case MsgChange::Edited:
		ev.messageEdited(chat_id, message_id);
		break;
	case MsgChange::Deleted:
		ev.messageDeleted(chat_id, message_id);
		break;
	case MsgChange::None:
		break;
	}
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
	/* l_ stays valid until the members are gone; see log_owner_. */
	pr_debug(l_, "Destroying TgLoggerd...");
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
	/* Internal API handler threads each may hold a connection too. */
	std::string api_listen = env("TG_API_LISTEN", "");
	size_t api_threads = (size_t)atoi(env("TG_API_THREADS", "4").c_str());
	if (api_threads < 1)
		api_threads = 1;
	size_t api_conns = api_listen.empty() ? 0 : api_threads;
	/* The stills worker, when on, is one more thread on the pool. */
	const std::string gen_stills = env("TG_GENERATE_STILLS", "0");
	const bool stills_on = gen_stills != "0" && gen_stills != "dry";
	const size_t stills_conns = stills_on ? 1 : 0;
	/* +1 over file_threads for serial_, +1 for the Discord refresh thread,
	 * +1 for the storage janitor. */
	std::string db_pool_def = std::to_string(file_threads + 4 + api_conns +
						 stills_conns);
	size_t db_pool = (size_t)atoi(env("TG_DB_POOL", db_pool_def.c_str()).c_str());
	if (db_pool < file_threads + 3 + api_conns + stills_conns)
		db_pool = file_threads + 3 + api_conns + stills_conns;
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
	 * TDLib downloads each file into its own store and tgloggerd keeps a
	 * second content-addressed copy; delete TDLib's copy once ours is
	 * secured so files are not stored twice. Both default on.
	 */
	prune_tdlib_files_ = env("TG_PRUNE_TDLIB_FILES", "1") != "0";
	pr_debug(l_, "prune_tdlib_files: %d", (int)prune_tdlib_files_);

	/*
	 * Storage is limited: files at or above this size are not copied into
	 * the store; only their metadata (tg_file_id, sha256, size, ...) is
	 * recorded so they remain identifiable and re-downloadable later. 0
	 * disables the cap.
	 */
	max_store_file_size_ = strtoull(
		env("TG_MAX_STORE_FILE_SIZE", "1073741824").c_str(), nullptr, 10);
	pr_debug(l_, "max_store_file_size: %llu bytes",
		 (unsigned long long)max_store_file_size_);

	/*
	 * Media from chats the account is not a member of (reached by the
	 * backfiller, reply resolution and edits) is recorded without its
	 * bytes; the messages themselves are still logged. On by default.
	 */
	skip_nonmember_media_ = env("TG_SKIP_NONMEMBER_MEDIA", "1") != "0";
	pr_debug(l_, "skip_nonmember_media: %d", (int)skip_nonmember_media_);

	/*
	 * Seed the file index so the loop thread can link a message to a file
	 * we already recorded instead of re-downloading it.
	 */
	try {
		known_files_.load(db_->loadFileIndex(), db_->loadUniqueIndex());
		auto n = known_files_.size();
		pr_debug(l_, "file index: %zu known files, %zu by unique id",
			 n.first, n.second);
	} catch (const std::exception &e) {
		pr_error(l_, "Failed to load file index: %s", e.what());
	}

	/*
	 * Construct the workers only past the early-return points above, so a
	 * failed startup never leaves threads to join. serial_ = 1 thread
	 * (strict FIFO); files_ = file_threads.
	 */
	events_ = std::make_unique<EventBus>();
	events_->start();
	bot_watch_ = std::make_unique<BotWatch>(db_.get(), events_.get(), l_);
	bot_watch_->start();
	serial_ = std::make_unique<ThreadPool>(1, queue_max, l_);
	files_ = std::make_unique<ThreadPool>(file_threads, queue_max, l_);
	pr_debug(l_, "workers: serial=1 files=%zu queue_max=%zu",
		 file_threads, queue_max);

	/*
	 * Discord forwarder: its own pool so slow/blocked webhook POSTs never
	 * back-pressure the DB writer serial_. Refresh interval controls how
	 * quickly web-UI changes to telegram_discord_webhooks take effect.
	 */
	size_t discord_threads = (size_t)atoi(env("TG_DISCORD_THREADS", "2").c_str());
	if (discord_threads < 1)
		discord_threads = 1;
	size_t discord_queue = (size_t)strtoull(
		env("TG_DISCORD_QUEUE_MAX", "20000").c_str(), nullptr, 10);
	int discord_refresh = atoi(env("TG_DISCORD_REFRESH_SECS", "20").c_str());
	/* Public base URL of the web app (e.g. https://tgd.gnuweeb.org) so
	 * forwarded avatars/media resolve to /files/<token>; WEB_APP_KEY (shared
	 * with the web) mints those tokens. */
	std::string discord_public_url = env("TG_DISCORD_PUBLIC_URL", "");
	std::string web_app_key = env("WEB_APP_KEY", "");
	discord_ = std::make_unique<DiscordForwarder>(db_.get(), l_,
			discord_threads, discord_queue, discord_refresh,
			discord_public_url, web_app_key);
	discord_->start();
	pr_debug(l_, "discord: threads=%zu queue=%zu refresh=%ds",
		 discord_threads, discord_queue, discord_refresh);

	char tdlib_path[sizeof(this->data_dir_) + 32];
	snprintf(tdlib_path, sizeof(tdlib_path), "%s/tdlib", this->data_dir_);

	tdlib_ = std::make_unique<TDLib>(this->api_id_, this->api_hash_,
					 tdlib_path);
	tdlib_->setLogger(l_);
	tdlib_->setPruneOnStart(env("TG_PRUNE_TDLIB_ON_START", "1") != "0");
	/*
	 * Opt-in, one-off repair: re-fetch every message that has media but no
	 * linked file, so the download and link step runs for it again. A file
	 * whose bytes already sit in the store -- left there when a database
	 * failure lost its row -- is linked back without a second copy. Off by
	 * default because a pass costs one lookup per such message, and the
	 * ones that can never be repaired would be retried on every boot.
	 */
	if (env("TG_RELINK_MEDIA_ON_START", "0") != "0") {
		try {
			auto msgs = db_->listMediaMessagesWithoutFile();
			pr_info(l_, "relink: %zu message(s) have media but no"
				" linked file; re-fetching them", msgs.size());
			tdlib_->relinkMessageMedia(std::move(msgs));
		} catch (const std::exception &e) {
			pr_error(l_, "relink: could not list messages: %s",
				 e.what());
		}
	}
	/*
	 * Opt-in, one-off repair of rows stored before content
	 * classification: messages kept as 'unknown', service messages
	 * without their detail, and rich messages stored before their
	 * mapping are re-fetched and stored again
	 * (docs/design/message-types.md section 4.9). "dry" only counts them.
	 * Repaired rows get their kind and detail and are not listed again,
	 * so a pass that a restart cuts short resumes where it was; the rows
	 * TDLib can no longer return are tried again on every start while
	 * this is on, which is why it is off by default.
	 */
	const std::string repair = env("TG_REPAIR_MESSAGES_ON_START", "0");
	if (repair == "dry") {
		try {
			uint64_t total = 0;
			for (const auto &c : db_->countMessagesToRepair()) {
				pr_info(l_, "repair (dry run): %llu %s message(s)"
					" stored as %s",
					(unsigned long long)std::get<2>(c),
					std::get<0>(c).c_str(),
					std::get<1>(c).c_str());
				total += std::get<2>(c);
			}
			pr_info(l_, "repair (dry run): %llu message(s) would be"
				" re-fetched", (unsigned long long)total);
		} catch (const std::exception &e) {
			pr_error(l_, "repair: could not count messages: %s",
				 e.what());
		}
	} else if (repair != "0") {
		try {
			auto msgs = db_->listMessagesToRepair();
			pr_info(l_, "repair: %zu old message(s) lack their kind"
				" or detail; re-fetching them", msgs.size());
			tdlib_->repairMessages(std::move(msgs));
		} catch (const std::exception &e) {
			pr_error(l_, "repair: could not list messages: %s",
				 e.what());
		}
	}
	/*
	 * Opt-in, one-off pass for the videos, animations, video notes and
	 * video stickers logged before the daemon stored their still
	 * thumbnails: each is re-fetched and stored again, which stores the
	 * still TDLib offers (docs/design/message-types.md section 4.9).
	 * "dry" only counts them. It shares the repair's budget and runs
	 * after it. A message that got its still is not listed again; one
	 * whose video has no Telegram thumbnail is re-read on every start
	 * while this is on, which is why it is off by default.
	 */
	const std::string thumbs = env("TG_THUMBNAILS_ON_START", "0");
	if (thumbs == "dry") {
		try {
			uint64_t total = 0;
			for (const auto &c : db_->countMessagesWithoutThumbnail()) {
				pr_info(l_, "thumbnails (dry run): %llu %s %s"
					" message(s) without a still",
					(unsigned long long)std::get<2>(c),
					std::get<0>(c).c_str(),
					std::get<1>(c).c_str());
				total += std::get<2>(c);
			}
			pr_info(l_, "thumbnails (dry run): %llu message(s) would"
				" be re-fetched", (unsigned long long)total);
		} catch (const std::exception &e) {
			pr_error(l_, "thumbnails: could not count messages: %s",
				 e.what());
		}
	} else if (thumbs != "0") {
		try {
			auto msgs = db_->listMessagesWithoutThumbnail();
			pr_info(l_, "thumbnails: %zu message(s) have no still;"
				" re-fetching them", msgs.size());
			tdlib_->refetchThumbnails(std::move(msgs));
		} catch (const std::exception &e) {
			pr_error(l_, "thumbnails: could not list messages: %s",
				 e.what());
		}
	}
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
				events_->userUpdated(u.id);
			} catch (const std::exception &e) {
				pr_error(l_, "Failed to store user %lld: %s",
					 (long long)u.id, e.what());
			}
		});
	});
	/*
	 * An animated current photo's video: recorded without its bytes,
	 * like the photo lists' files, and linked to the history row of its
	 * still (the row trackProfilePhotoChange made for it, if any).
	 */
	tdlib_->setPhotoVideoHandler([this](int64_t owner_id, bool is_group,
					    const models::File &still,
					    const models::File &video) {
		serial_->post([this, owner_id, is_group, still, video] {
			try {
				const uint64_t sid = db_->upsertFileMeta(still).id;
				const uint64_t vid = db_->upsertFileMeta(video).id;
				if (db_->setPhotoHistoryVideo(owner_id, is_group,
							      sid, vid))
					pr_info(l_, "photo video: %s %lld: video"
						" file %llu linked to photo file"
						" %llu", is_group ? "group" : "user",
						(long long)owner_id,
						(unsigned long long)vid,
						(unsigned long long)sid);
			} catch (const std::exception &e) {
				pr_error(l_, "photo video: %lld: %s",
					 (long long)owner_id, e.what());
			}
		});
	});
	tdlib_->setUserFullInfoHandler([this](const models::UserFullInfo &fi) {
		serial_->post([this, fi] {
			try {
				db_->upsertUserFullInfo(fi);
				events_->userUpdated(fi.user_id, { "bio",
					"birthday", "personal_chat" });
			} catch (const std::exception &e) {
				pr_error(l_, "Failed to store user full info"
					 " %lld: %s", (long long)fi.user_id,
					 e.what());
			}
		});
	});
	tdlib_->setProfilePhotoHandler([this](const ProfilePhoto &p) {
		store_holds_.hold(p.tg_local_file_id);
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
				events_->groupUpdated(g.id);
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
				events_->groupUpdated(al.group_id, { "admins" });
			} catch (const std::exception &e) {
				pr_error(l_, "Failed to sync group admins for"
					 " %lld: %s", (long long)al.group_id,
					 e.what());
			}
		});
	});
	tdlib_->setGroupPhotoHandler([this](const GroupPhoto &p) {
		store_holds_.hold(p.tg_local_file_id);
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
	/* Mirror every live new message to any Discord webhook for its chat. */
	tdlib_->setForwardHandler([this](const ForwardMessage &fm) {
		discord_->forward(fm);
	});
	/* Apply live edits to the already-forwarded Discord message. */
	tdlib_->setEditForwardHandler([this](const ForwardMessage &fm) {
		discord_->forward_edit(fm);
	});
	/* Tombstone the forwarded Discord message when the source is deleted. */
	tdlib_->setDeleteForwardHandler([this](int64_t chat_id, int64_t msg_id) {
		discord_->forward_delete(chat_id, msg_id);
	});
	tdlib_->setPrivateMessageHandler([this](const models::PrivateMessage &pm) {
		serial_->post([this, pm] {
			try {
				auto r = db_->upsertPrivateMessage(pm);
				if (r.restored)
					pr_warn(l_, "recheck: chat %lld: message"
						" %lld marked deleted on an earlier"
						" recheck came back; restored",
						(long long)pm.chat_id,
						(long long)pm.message_id);
				publish_message(*events_, pm.chat_id,
						pm.message_id, r.change,
						pm.sender_id);
				/* Every 10th message: refresh the sender's info. */
				if (r.user.has_value())
					tdlib_->refetchUser(*r.user);
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
				auto r = db_->upsertGroupMessage(gm);
				if (r.restored)
					pr_warn(l_, "recheck: chat %lld: message"
						" %lld marked deleted on an earlier"
						" recheck came back; restored",
						(long long)gm.chat_id,
						(long long)gm.message_id);
				publish_message(*events_, gm.chat_id,
						gm.message_id, r.change,
						gm.sender_user_id);
				/* Every 10th message: refresh the group and the
				 * sender's info. */
				if (r.group.has_value())
					tdlib_->refetchGroup(*r.group);
				if (r.user.has_value())
					tdlib_->refetchUser(*r.user);
			} catch (const std::exception &e) {
				pr_error(l_, "Failed to store group message"
					 " chat_id=%lld msg_id=%lld: %s",
					 (long long)gm.chat_id,
					 (long long)gm.message_id, e.what());
			}
		});
	});
	/*
	 * A chat recheck's answer, once its messages are stored (they were
	 * posted to serial_ first): the stored live messages strictly
	 * between the lowest and highest id returned that it did not return
	 * are marked deleted, flagged as found on recheck, unless the answer
	 * was too short to tell (recheck::selectDeleted). Each sends the
	 * usual deletion event. Its Discord copy is NOT tombstoned: a
	 * deletion concluded from a recheck is undone when a later recheck
	 * finds the message (setRecheckFoundHandler), and a tombstone on
	 * Discord could not be.
	 */
	/*
	 * The restart catch-up rechecks only the chats with a gap at the
	 * newest end: TDLib's last message newer than the newest stored.
	 */
	tdlib_->setRecheckCatchUpHandler(
		[this](std::vector<int64_t> chats,
		       std::unordered_map<int64_t, int64_t> tdlib_last) {
			serial_->post([this, chats, tdlib_last] {
				try {
					auto stored =
						db_->newestStoredMessageIds(chats);
					auto near = db_->storedNearNewest(stored);
					auto gaps = recheck::gapChats(
						chats, tdlib_last, stored, near);
					const size_t queued =
						tdlib_->queueRecheckCatchUp(gaps);
					pr_info(l_, "recheck: restart catch-up"
						" queued %zu of %zu chat(s) with a"
						" gap at or below the newest message"
						" (%zu active in the last 48 h)",
						queued, gaps.size(), chats.size());
				} catch (const std::exception &e) {
					pr_error(l_, "recheck: catch-up failed: %s",
						 e.what());
				}
			});
		});
	tdlib_->setRecheckFoundHandler([this](const RecheckAnswer &a) {
		serial_->post([this, a] {
			const int64_t chat_id = a.chat_id;
			const bool is_group = a.is_group;
			const std::vector<int64_t> &ids = a.page.ids;
			try {
				/* Before the page is stored: how much of it is
				 * new, for the decision to read another page. */
				recheck_new_[a.seq] = ids.size() -
					db_->countStoredMessages(chat_id,
								 is_group, ids);
				auto back = db_->restoreRecheckDeleted(
					chat_id, is_group, ids);
				for (int64_t id : back)
					events_->messageEdited(chat_id, id);
				if (!back.empty())
					pr_warn(l_, "recheck: chat %lld: %zu"
						" message(s) marked deleted on an"
						" earlier recheck came back;"
						" restored", (long long)chat_id,
						back.size());
			} catch (const std::exception &e) {
				pr_error(l_, "recheck: chat %lld: restore"
					 " failed: %s", (long long)chat_id,
					 e.what());
			}
		});
	});
	tdlib_->setRecheckHandler([this](const RecheckAnswer &a) {
		serial_->post([this, a] {
			try {
				std::vector<int64_t> marked;
				const char *why = "fewer than two messages returned";
				bool older_exists = false;
				if (auto span = recheck::spanOf(a.page)) {
					auto s = db_->listRecheckStored(a.chat_id,
						a.is_group, *span);
					older_exists = s.older_exists;
					auto v = recheck::selectDeleted(a.page, s);
					why = v.why;
					if (v.concluded)
						marked = db_->markDeletedOnRecheck(
							a.chat_id, a.is_group,
							v.deleted);
				} else if (!a.page.ids.empty()) {
					/* One message: is there an older one
					 * stored, i.e. is this not the start? */
					const int64_t id = a.page.ids.front();
					older_exists = db_->listRecheckStored(
						a.chat_id, a.is_group,
						recheck::Span{ id, id }).older_exists;
				}
				size_t fresh = 0;
				if (auto n = recheck_new_.find(a.seq);
				    n != recheck_new_.end()) {
					fresh = n->second;
					recheck_new_.erase(n);
				}
				const bool more = recheck::askAgain(
					a.item.page,
					recheck::isShort(a.page, older_exists),
					fresh);
				if (more)
					tdlib_->recheckNextPage(a);
				for (int64_t id : marked) {
					publish_message(*events_, a.chat_id, id,
							MsgChange::Deleted,
							std::nullopt);
				}
				if (marked.size() > recheck::kWarnMarked)
					pr_warn(l_, "recheck: chat %lld around %lld"
						" page %d: %zu returned, %zu new, %zu"
						" marked deleted (after %llds)%s",
						(long long)a.chat_id,
						(long long)a.page.around,
						a.item.page, a.page.total, fresh,
						marked.size(),
						(long long)a.waited_secs,
						more ? "; reading the next page" : "");
				else
					pr_info(l_, "recheck: chat %lld around %lld"
						" page %d: %zu returned, %zu new, %zu"
						" marked deleted (after %llds)%s%s%s",
						(long long)a.chat_id,
						(long long)a.page.around,
						a.item.page, a.page.total, fresh,
						marked.size(),
						(long long)a.waited_secs,
						*why ? "; none concluded: " : "",
						why,
						more ? "; reading the next page" : "");
			} catch (const std::exception &e) {
				pr_error(l_, "recheck: chat %lld around %lld: %s",
					 (long long)a.chat_id,
					 (long long)a.page.around, e.what());
			}
		});
	});
	tdlib_->setContentVersionHandler(
		[this](int64_t chat_id, int64_t message_id,
		       const models::MessageContent &mc, bool is_group) {
			serial_->post([this, chat_id, message_id, mc, is_group] {
				try {
					if (db_->recordMessageContentVersion(
						chat_id, message_id, mc,
						is_group)) {
						pr_info(l_, "Recorded content"
							" version chat_id=%lld"
							" msg_id=%lld",
							(long long)chat_id,
							(long long)message_id);
						events_->messageEdited(chat_id,
							message_id);
					}
				} catch (const std::exception &e) {
					pr_error(l_, "Failed to record message"
						 " content version chat_id=%lld"
						 " msg_id=%lld: %s",
						 (long long)chat_id,
						 (long long)message_id,
						 e.what());
				}
			});
		});
	tdlib_->setForumTopicHandlers(
		[this](const models::ForumTopic &ft, bool only_if_absent) {
			serial_->post([this, ft, only_if_absent] {
				try {
					db_->upsertForumTopic(ft, only_if_absent);
					pr_debug(l_, "Stored forum topic | chat_id=%lld"
						 " topic_id=%d name=%.64s%s",
						 (long long)ft.chat_id, ft.topic_id,
						 ft.name.c_str(),
						 only_if_absent ? " (if new)" : "");
				} catch (const std::exception &e) {
					pr_error(l_, "Failed to store forum topic"
						 " chat_id=%lld topic_id=%d: %s",
						 (long long)ft.chat_id, ft.topic_id,
						 e.what());
				}
			});
		},
		[this](int64_t chat_id, int32_t topic_id, bool pinned) {
			serial_->post([this, chat_id, topic_id, pinned] {
				try {
					db_->setForumTopicPinned(chat_id, topic_id,
								 pinned);
				} catch (const std::exception &e) {
					pr_error(l_, "Failed to record pinned forum"
						 " topic chat_id=%lld topic_id=%d: %s",
						 (long long)chat_id, topic_id,
						 e.what());
				}
			});
		});
	tdlib_->setMessageMetaHandlers(
		[this](int64_t chat_id, int64_t message_id, bool is_group,
		       bool pinned) {
			serial_->post([this, chat_id, message_id, is_group, pinned] {
				try {
					db_->setMessagePinned(chat_id, message_id,
							      is_group, pinned);
					events_->messageEdited(chat_id, message_id);
				} catch (const std::exception &e) {
					pr_error(l_, "Failed to record pinned message"
						 " chat_id=%lld msg_id=%lld: %s",
						 (long long)chat_id,
						 (long long)message_id, e.what());
				}
			});
		},
		[this](int64_t chat_id, int64_t message_id, bool is_group,
		       const std::optional<std::string> &fc) {
			serial_->post([this, chat_id, message_id, is_group, fc] {
				try {
					db_->setMessageFactCheck(chat_id, message_id,
								 is_group, fc);
					events_->messageEdited(chat_id, message_id);
				} catch (const std::exception &e) {
					pr_error(l_, "Failed to record fact check"
						 " chat_id=%lld msg_id=%lld: %s",
						 (long long)chat_id,
						 (long long)message_id, e.what());
				}
			});
		});
	/*
	 * Views, forwards, replies and reaction totals change constantly on
	 * busy channels; TDLib's updates are coalesced so each message is
	 * written at most once per TG_INTERACTION_FLUSH_SECS (0 = keep only
	 * the totals a message carries when it is stored).
	 */
	tdlib_->setInteractionHandler(
		[this](std::vector<InteractionUpdate> batch) {
			serial_->post([this, batch = std::move(batch)] {
				size_t stored = 0;
				for (const auto &u : batch) {
					try {
						if (db_->setMessageInteraction(
							    u.chat_id, u.message_id,
							    u.is_group, u.interaction))
							stored++;
					} catch (const std::exception &e) {
						pr_error(l_, "Failed to store"
							 " interaction chat_id=%lld"
							 " msg_id=%lld: %s",
							 (long long)u.chat_id,
							 (long long)u.message_id,
							 e.what());
					}
				}
				pr_debug(l_, "interaction: stored %zu of %zu",
					 stored, batch.size());
			});
		},
		std::atoi(env("TG_INTERACTION_FLUSH_SECS", "60").c_str()));
	tdlib_->setGroupMemberInfoHandler(
		[this](int64_t chat_id, std::optional<int64_t> count,
		       std::optional<bool> can_get_members) {
			serial_->post([this, chat_id, count, can_get_members] {
				try {
					db_->setGroupMemberInfo(chat_id, count,
								can_get_members);
				} catch (const std::exception &e) {
					pr_error(l_, "Failed to store member count"
						 " chat_id=%lld: %s",
						 (long long)chat_id, e.what());
				}
			});
		});
	/*
	 * Communities (message-types.md section 4.13): rows and chat lists
	 * on serial_, like the users and groups. A community a logged line
	 * names gets its full info asked for when its row has no chats yet.
	 */
	tdlib_->setCommunityHandlers(
		[this](const models::Community &c) {
			serial_->post([this, c] {
				try {
					onCommunity(c);
				} catch (const std::exception &e) {
					pr_error(l_, "Failed to store community"
						 " %lld: %s", (long long)c.id,
						 e.what());
				}
			});
		},
		[this](const models::CommunityFullInfo &fi) {
			serial_->post([this, fi] {
				try {
					onCommunityFullInfo(fi);
				} catch (const std::exception &e) {
					pr_error(l_, "Failed to store community"
						 " full info %lld: %s",
						 (long long)fi.community_id,
						 e.what());
				}
			});
		},
		[this](int64_t community_id) {
			serial_->post([this, community_id] {
				try {
					if (db_->communityNeedsFullInfo(
						    community_id))
						tdlib_->fetchCommunityFullInfo(
							community_id);
				} catch (const std::exception &e) {
					pr_error(l_, "Failed to look up community"
						 " %lld: %s",
						 (long long)community_id,
						 e.what());
				}
			});
		});
	tdlib_->setPollHandler([this](const models::Poll &p) {
		/* Serialised with the message writes, like everything that
		 * belongs to a message. */
		serial_->post([this, p] {
			try {
				db_->upsertPoll(p);
				pr_debug(l_, "Stored poll | poll_id=%lld"
					 " options=%zu voters=%d%s",
					 (long long)p.id, p.options.size(),
					 p.total_voter_count,
					 p.is_closed ? " closed" : "");
			} catch (const std::exception &e) {
				pr_error(l_, "Failed to store poll %lld: %s",
					 (long long)p.id, e.what());
			}
		});
	});
	tdlib_->setLinkPreviewHandler(
		[this](const models::LinkPreview &lp, bool is_group) {
			/* Serialised with the message writes so the preview
			 * never lands before the row it points at. */
			serial_->post([this, lp, is_group] {
				try {
					db_->upsertLinkPreview(lp, is_group);
					events_->messageEdited(lp.chat_id,
							       lp.message_id);
					pr_info(l_,
						"Stored link preview | %s "
						"chat_id=%lld msg_id=%lld "
						"type=%s site=%.32s",
						is_group ? "group" : "private",
						(long long)lp.chat_id,
						(long long)lp.message_id,
						lp.type.c_str(),
						lp.site_name.c_str());
				} catch (const std::exception &e) {
					pr_error(l_,
						 "Failed to store link preview "
						 "chat_id=%lld msg_id=%lld: %s",
						 (long long)lp.chat_id,
						 (long long)lp.message_id,
						 e.what());
				}
			});
		});

	tdlib_->setMessageFileHandler([this](const MessageFile &mf) {
		store_holds_.hold(mf.tg_local_file_id);
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
	/*
	 * One extra hold across all the emits of a completed file, so the
	 * first job cannot release the last hold and prune the copy before
	 * the next emit takes its own. Its release stores nothing: the copy
	 * is pruned here only if a job already stored it.
	 */
	tdlib_->setFileEmitBatchHandlers(
		[this](int32_t fid) { store_holds_.hold(fid); },
		[this](int32_t fid) { store_holds_.release(fid, false); });
	/* Loop-thread predicate: is this file already recorded? */
	tdlib_->setFileLookup([this](const std::string &tg_file_id,
				     const std::string &tg_unique_id)
			      -> std::optional<models::KnownFile> {
		return known_files_.find(tg_file_id, tg_unique_id);
	});
	/* Point a user or group at an already-stored photo. */
	tdlib_->setPhotoLinkHandler([this](const PhotoLink &pl) {
		serial_->post([this, pl] {
			try {
				onPhotoLink(pl);
			} catch (const std::exception &e) {
				pr_error(l_, "Failed to link %s photo for"
					 " %lld: %s",
					 pl.is_group ? "group" : "profile",
					 (long long)pl.owner_id, e.what());
			}
		});
	});
	/* Link a message to an already-recorded file (no re-download). */
	tdlib_->setMessageFileLinkHandler([this](const MessageFileLink &lk) {
		serial_->post([this, lk] {
			try {
				onMessageFileLink(lk);
			} catch (const std::exception &e) {
				pr_error(l_, "Failed to link message file"
					 " chat_id=%lld msg_id=%lld: %s",
					 (long long)lk.chat_id,
					 (long long)lk.message_id, e.what());
			}
		});
	});
	/*
	 * Files recorded without their bytes (non-member chats, over the size
	 * cap): upsert and link on serial_, after the message row was written
	 * and off the loop thread, exactly like an index hit.
	 */
	tdlib_->setMediaPolicy(skip_nonmember_media_, max_store_file_size_);
	tdlib_->setFileMetaHandler([this](const FileMeta &m) {
		serial_->post([this, m] {
			try {
				onFileMeta(m);
			} catch (const std::exception &e) {
				pr_error(l_, "Failed to record file metadata"
					 " chat_id=%lld msg_id=%lld: %s",
					 (long long)m.chat_id,
					 (long long)m.message_id, e.what());
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
				events_->messageEdited(mr.chat_id, mr.message_id);
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
	 * telegram_chat_backfill_state (fire-and-forget on serial_) and reloaded here so
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

	/*
	 * The storage janitor (docs/design/storage-cache.md section 9). It
	 * only reports what it would evict unless TG_STORAGE_DRY_RUN=0.
	 */
	StorageJanitor::Config sj_cfg;
	sj_cfg.storage_dir = storage_dir_;
	sj_cfg.max_bytes = strtoull(env("TG_STORAGE_MAX_BYTES", "0").c_str(),
				    nullptr, 10);
	sj_cfg.retention_days = (uint32_t)strtoul(
		env("TG_STORAGE_RETENTION_DAYS", "90").c_str(), nullptr, 10);
	sj_cfg.sweep_secs = (uint32_t)strtoul(
		env("TG_STORAGE_SWEEP_SECS", "600").c_str(), nullptr, 10);
	sj_cfg.dry_run = env("TG_STORAGE_DRY_RUN", "1") != "0";
	janitor_ = std::make_unique<StorageJanitor>(db_.get(), l_, sj_cfg);
	janitor_->start();

	/*
	 * Stills taken from the stored bytes of the videos Telegram sent no
	 * thumbnail for (docs/design/message-types.md section 4.3), by the
	 * stills service. Off by default; "dry" only counts the videos.
	 */
	if (gen_stills == "dry") {
		try {
			uint64_t msgs = 0;
			for (const auto &c : db_->countStillCandidates()) {
				pr_info(l_, "stills (dry run): %llu %s %s"
					" message(s), %llu video(s), without a"
					" still", (unsigned long long)c.messages,
					c.table.c_str(), c.kind.c_str(),
					(unsigned long long)c.videos);
				msgs += c.messages;
			}
			pr_info(l_, "stills (dry run): %llu message(s) in all"
				" would get a still", (unsigned long long)msgs);
		} catch (const std::exception &e) {
			pr_error(l_, "stills: could not count the videos: %s",
				 e.what());
		}
	} else if (stills_on) {
		StillsWorker::Config st_cfg;
		st_cfg.socket_path = env("TG_STILLS_SOCKET",
					 "/run/tgloggerd-stills/stills.sock");
		st_cfg.storage_dir = storage_dir_;
		st_cfg.tmp_dir = std::string(data_dir_) + "/stills-tmp";
		stills_ = std::make_unique<StillsWorker>(*db_, l_, st_cfg,
			[this](const std::string &path)
				-> std::optional<StillsWorker::Stored> {
				auto b = putInStore(path, -1, "generated still");
				if (!b)
					return std::nullopt;
				return StillsWorker::Stored{ b->sha256_hex,
							     b->ext,
							     b->on_disk };
			},
			[this](uint64_t still,
			       const std::vector<std::pair<int64_t, int64_t>> &m) {
				serial_->post([this, still, m] {
					for (const auto &x : m)
						events_->fileStored(still, x.first,
								    x.second);
				});
			});
		stills_->start();
		pr_info(l_, "stills: generating stills over %s",
			st_cfg.socket_path.c_str());
	}

	/*
	 * Before anything else touches the contacts: a contact that an
	 * interrupted "Look up real name" left removed is added back as soon
	 * as TDLib is ready (ContactLookup.hpp). The looked-up name is stored
	 * through serial_, after the user updates the lookup caused.
	 */
	ContactLookup::Config contact_cfg;
	contact_cfg.min_interval = std::chrono::seconds(std::max(60,
		atoi(env("TG_CONTACT_LOOKUP_INTERVAL", "300").c_str())));
	contacts_ = std::make_unique<Contacts>(*tdlib_, *db_,
		[this](int64_t user_id, const models::Name &own) {
			auto done = std::make_shared<std::promise<void>>();
			auto fut = done->get_future();
			serial_->post([this, user_id, own, done] {
				try {
					db_->recordOwnNameLookup(user_id, own);
					events_->userUpdated(user_id,
							     { "name" });
					done->set_value();
				} catch (...) {
					done->set_exception(
						std::current_exception());
				}
			});
			if (fut.wait_for(std::chrono::seconds(30)) !=
			    std::future_status::ready)
				throw std::runtime_error("the write queue is"
							 " busy");
			fut.get();
		}, contact_cfg, l_);
	contacts_->lookup().startRestore();

	/*
	 * Last, so every handler can rely on db_ and tdlib_. A bad setting
	 * leaves the API off but the logger running: the web treats an
	 * absent daemon API as "fetching off".
	 */
	if (!api_listen.empty()) {
		FileFetcher::Config fcfg;
		fcfg.loadEnv();
		fcfg.max_store_file_size = max_store_file_size_;
		fcfg.prune_stored = prune_tdlib_files_;
		fcfg.tdlib_dir = tdlib_path;
		fetcher_ = std::make_unique<FileFetcher>(*db_, *tdlib_, l_, fcfg,
			[this](const std::string &path, const std::string &tg_file_id,
			       const std::string &tg_unique_id, int64_t size,
			       const std::string &type, const std::string &orig) {
				return storeDownloadedFile(path, tg_file_id,
							   tg_unique_id, size,
							   type.c_str(), orig);
			},
			[this](const std::string &hex, const std::string &ext) {
				std::error_code ec;
				return hex.size() == 64 &&
				       fs::exists(blobPath(storage_dir_, hex, ext),
						  ec);
			},
			[this](int32_t fid) { return store_holds_.held(fid); });
		pr_debug(l_, "fetch: max_active=%zu chunk=%llu timeout=%llds"
			 " idle=%llds", fcfg.max_active,
			 (unsigned long long)fcfg.chunk,
			 (long long)fcfg.timeout.count(),
			 (long long)fcfg.idle.count());

		api_ = std::make_unique<InternalApi>(l_);
		registerFileApi(*api_, *db_, fetcher_.get());
		registerStorageApi(*api_, *db_, *janitor_);
		registerChatApi(*api_, *tdlib_, *db_);
		registerPhotoApi(*api_, *tdlib_, *db_);
		registerEmojiApi(*api_, emojiBackendOf(*tdlib_, *db_));
		registerRecheckApi(*api_, recheckBackendOf(*tdlib_, *db_));
		registerContactApi(*api_, *contacts_);
		registerEventApi(*api_, *events_);
		registerStatusApi(*api_, [t = tdlib_.get()] {
			return t->status();
		});
		InternalApi::Config api_cfg;
		api_cfg.listen = api_listen;
		api_cfg.threads = api_threads;
		api_cfg.web_app_key = web_app_key;
		if (api_->start(api_cfg) < 0) {
			api_.reset();
			fetcher_.reset();
		} else {
			fetcher_->start();
		}
	}

	pr_info(l_, "Listening for incoming messages...");
	while (!tdlib_->isStopped() && !g_tgld_stop)
		tdlib_->loop(10);
	if (g_tgld_stop)
		pr_info(l_, "Shutdown requested; draining pending writes...");

	/*
	 * Drain before returning, in this order rather than the members'
	 * destruction order. files_ first so its pending link jobs land in
	 * serial_, then serial_ drains fully.
	 * The API goes first: its handlers post to those pools and call
	 * TDLib. Then the fetcher's reaper, which calls TDLib, and the
	 * janitor, which may be mid-batch on db_.
	 */
	/*
	 * The contact lookup before the API, so stopping the API does not
	 * wait out a whole remove/re-add cycle: no new lookup starts, and a
	 * running one cuts its waits short but still re-adds its contact.
	 */
	if (contacts_)
		contacts_->lookup().stop();
	if (api_)
		api_->stop();
	if (fetcher_)
		fetcher_->stop();
	janitor_->stop();
	if (stills_)
		stills_->stop();
	files_->shutdown();
	serial_->shutdown();
	bot_watch_->stop();
	events_->stop();
	/* The event loop has exited, so no more forward() calls arrive; stop the
	 * refresh thread and drain in-flight webhook POSTs. */
	if (discord_)
		discord_->stop();

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
 * row was written and serialized against other DB writes. Each job holds
 * TDLib's local copy (store_holds_, taken when it was queued), so a copy
 * shared by several jobs is pruned only after the last of them.
 */
void TgLoggerd::onProfilePhoto(const ProfilePhoto &p)
{
	StoreHolds::Job job(store_holds_, p.tg_local_file_id);
	auto file_id = storeDownloadedFile(p.local_path, p.tg_file_id,
					   p.tg_unique_id,
					   p.file_size, "photo");
	if (!file_id.has_value())
		return;
	/* Our copy is durable; TDLib's goes after the last job on it. */
	job.stored = true;

	int64_t user_id = p.user_id;
	uint64_t fid = *file_id;
	serial_->post([this, user_id, fid] {
		try {
			db_->setUserProfilePhoto(user_id, fid);
			events_->fileStored(fid);
			events_->userUpdated(user_id, { "photo" });
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
	StoreHolds::Job job(store_holds_, p.tg_local_file_id);
	auto file_id = storeDownloadedFile(p.local_path, p.tg_file_id,
					   p.tg_unique_id,
					   p.file_size, "photo");
	if (!file_id.has_value())
		return;
	/* Our copy is durable; TDLib's goes after the last job on it. */
	job.stored = true;

	int64_t group_id = p.group_id;
	uint64_t fid = *file_id;
	serial_->post([this, group_id, fid] {
		try {
			db_->setGroupPhoto(group_id, fid);
			events_->fileStored(fid);
			events_->groupUpdated(group_id, { "photo" });
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
	StoreHolds::Job job(store_holds_, m.tg_local_file_id);
	auto file_id = storeDownloadedFile(m.local_path, m.tg_file_id,
					   m.tg_unique_id,
					   m.file_size, m.content_type.c_str(),
					   m.orig_file_name);
	if (!file_id.has_value())
		return;
	/* Our copy is durable; TDLib's goes after the last job on it. */
	job.stored = true;

	int64_t chat_id = m.chat_id;
	int64_t message_id = m.message_id;
	bool is_group = m.is_group;
	uint64_t fid = *file_id;
	const bool is_preview = m.is_link_preview;
	const std::string role = m.role;
	const int position = m.position;
	/*
	 * A video stored as a message's main file may need a generated still;
	 * the worker looks again once the message's own thumbnail has had
	 * time to arrive. A sticker only when it is a video (.webm).
	 */
	std::string ext = fs::path(m.local_path).extension().string();
	std::transform(ext.begin(), ext.end(), ext.begin(),
		       [](unsigned char c) { return (char)std::tolower(c); });
	const bool still = stills_ && !is_preview && role.empty() &&
		!StillsWorker::kindOf(m.content_type).empty() &&
		(m.content_type != "sticker" || ext == ".webm");
	serial_->post([this, chat_id, message_id, is_group, fid, is_preview,
		       role, position, still] {
		try {
			/*
			 * A preview image belongs to the card, not to the
			 * message: setting it as the message's own file would
			 * make a text message render as an attachment
			 * everywhere. A further file (a cover, a paid item)
			 * goes to telegram_message_files for the same reason.
			 */
			if (is_preview)
				db_->setLinkPreviewPhoto(chat_id, message_id,
							 is_group, fid);
			else if (!role.empty())
				db_->setMessageExtraFile(chat_id, message_id,
							 is_group, role,
							 position, fid);
			else if (is_group)
				db_->setGroupMessageFile(chat_id, message_id,
							 fid);
			else
				db_->setPrivateMessageFile(chat_id, message_id,
							   fid);
			events_->fileStored(fid, chat_id, message_id);
			if (still)
				stills_->messageFileStored(is_group, chat_id,
							   message_id);
			pr_info(l_, "Stored %s%s | %s chat_id=%lld"
				" msg_id=%lld file_id=%llu",
				is_preview ? "link preview image"
					   : "message file",
				role.empty() ? "" : (" " + role).c_str(),
				is_group ? "group" : "private",
				(long long)chat_id, (long long)message_id,
				(unsigned long long)fid);
		} catch (const std::exception &e) {
			pr_error(l_, "Failed to link message file chat_id=%lld"
				 " msg_id=%lld: %s", (long long)chat_id,
				 (long long)message_id, e.what());
		}
	});

	/* If this file belongs to a live forwarded message, mirror it to
	 * Discord -- but a preview image is not the message's media, and
	 * Discord renders the link's own card anyway. Only the main file is
	 * mirrored, as before. */
	if (discord_ && !is_preview && role.empty())
		discord_->on_media_stored(chat_id, message_id, fid);
}

/*
 * Link a message to a file we already recorded, looked up by remote id on the
 * loop thread (see setFileLookup). Runs on serial_, after the message row was
 * written, so the FK update lands in order -- exactly like onMessageFile's link
 * step, but without downloading, hashing or storing the file again.
 */
void TgLoggerd::onMessageFileLink(const MessageFileLink &lk)
{
	if (lk.is_link_preview) {
		db_->setLinkPreviewPhoto(lk.chat_id, lk.message_id, lk.is_group,
					 lk.file_id);
		events_->fileStored(lk.file_id, lk.chat_id, lk.message_id);
		return;
	}
	if (!lk.role.empty()) {
		db_->setMessageExtraFile(lk.chat_id, lk.message_id, lk.is_group,
					 lk.role, lk.position, lk.file_id);
		events_->fileStored(lk.file_id, lk.chat_id, lk.message_id);
		pr_info(l_, "Linked message %s file (already recorded) | %s"
			" chat_id=%lld msg_id=%lld file_id=%llu",
			lk.role.c_str(), lk.is_group ? "group" : "private",
			(long long)lk.chat_id, (long long)lk.message_id,
			(unsigned long long)lk.file_id);
		return;
	}
	if (lk.is_group)
		db_->setGroupMessageFile(lk.chat_id, lk.message_id, lk.file_id);
	else
		db_->setPrivateMessageFile(lk.chat_id, lk.message_id, lk.file_id);
	events_->fileStored(lk.file_id, lk.chat_id, lk.message_id);
	/*
	 * The file's kind is not known here; the worker's check of the
	 * message says whether it is a video that needs a still (one made
	 * for an earlier message of the video is linked, not made again).
	 */
	if (stills_)
		stills_->messageFileStored(lk.is_group, lk.chat_id,
					   lk.message_id);
	pr_info(l_, "Linked message file (already recorded) | %s chat_id=%lld"
		" msg_id=%lld file_id=%llu", lk.is_group ? "group" : "private",
		(long long)lk.chat_id, (long long)lk.message_id,
		(unsigned long long)lk.file_id);

	if (discord_)
		discord_->on_media_stored(lk.chat_id, lk.message_id, lk.file_id);
}

/*
 * Point a user or group at a photo already recorded, looked up on the loop
 * thread (see setFileLookup). Runs on serial_, after the user or group row
 * was written, through the same setters (and so the same photo history) as
 * onProfilePhoto and onGroupPhoto, without downloading or storing it again.
 */
void TgLoggerd::onPhotoLink(const PhotoLink &pl)
{
	if (pl.is_group) {
		db_->setGroupPhoto(pl.owner_id, pl.file_id);
		events_->groupUpdated(pl.owner_id, { "photo" });
	} else {
		db_->setUserProfilePhoto(pl.owner_id, pl.file_id);
		events_->userUpdated(pl.owner_id, { "photo" });
	}
	pr_debug(l_, "Linked %s photo (already recorded) | %s=%lld"
		 " file_id=%llu", pl.is_group ? "group" : "profile",
		 pl.is_group ? "group_id" : "user_id", (long long)pl.owner_id,
		 (unsigned long long)pl.file_id);
}

/*
 * Record a file without its bytes (see FileMeta) and link it. Runs on
 * serial_. The index learns the row, so later messages with the same file
 * link it without a download or a DB lookup.
 */
void TgLoggerd::onFileMeta(const FileMeta &m)
{
	models::File f;
	f.tg_file_id = m.tg_file_id;
	f.tg_unique_id = m.tg_unique_id;
	f.file_type = m.content_type;
	f.file_size = (uint64_t)(m.file_size < 0 ? 0 : m.file_size);
	f.orig_file_name = m.orig_file_name;
	f.on_disk = false;
	/* No local path to take an extension from: use the original name's,
	 * if it fits the column (VARCHAR(10)). */
	std::string ext = fs::path(m.orig_file_name).extension().string();
	if (!ext.empty() && ext[0] == '.')
		ext.erase(0, 1);
	std::transform(ext.begin(), ext.end(), ext.begin(),
		       [](unsigned char c) { return (char)std::tolower(c); });
	if (!ext.empty() && ext.size() <= 10)
		f.file_ext = ext;

	models::KnownFile k = db_->upsertFileMeta(f);
	known_files_.put(m.tg_file_id, m.tg_unique_id, k);

	pr_info(l_, "Recorded file without its bytes | tg_file_id=%s"
		" unique_id=%s size=%lld file_id=%llu%s", m.tg_file_id.c_str(),
		m.tg_unique_id.c_str(), (long long)m.file_size,
		(unsigned long long)k.id, k.meta_only ? "" : " (stored)");

	if (m.is_group_photo) {
		db_->setGroupPhoto(m.chat_id, k.id);
		events_->groupUpdated(m.chat_id, { "photo" });
		pr_info(l_, "Linked group photo (metadata only) | group_id=%lld"
			" file_id=%llu", (long long)m.chat_id,
			(unsigned long long)k.id);
		return;
	}
	MessageFileLink lk{ m.chat_id, m.message_id, m.is_group,
			    m.is_link_preview, k.id };
	lk.role = m.role;
	lk.position = m.position;
	onMessageFileLink(lk);
}

/*
 * A community photo's telegram_files row, recorded without its bytes
 * like a chat photo from a chat the account is not in; 0 for none.
 */
uint64_t TgLoggerd::recordCommunityPhoto(const std::optional<models::File> &f)
{
	if (!f)
		return 0;
	models::KnownFile k = db_->upsertFileMeta(*f);
	known_files_.put(f->tg_file_id, f->tg_unique_id, k);
	return k.id;
}

void TgLoggerd::onCommunity(const models::Community &c)
{
	uint64_t photo = recordCommunityPhoto(c.photo);
	db_->upsertCommunity(c, photo);
	pr_debug(l_, "Stored community | id=%lld name=\"%s\" photo=%llu%s",
		 (long long)c.id, c.name.c_str(), (unsigned long long)photo,
		 c.have_access ? "" : " (no access)");
}

void TgLoggerd::onCommunityFullInfo(const models::CommunityFullInfo &fi)
{
	uint64_t photo = recordCommunityPhoto(fi.photo);
	db_->putCommunityFullInfo(fi, photo);
	pr_info(l_, "Stored community full info | id=%lld chats=%zu"
		" photo=%llu", (long long)fi.community_id, fi.chats.size(),
		(unsigned long long)photo);
}

/*
 * The store half of storeDownloadedFile, without the row: hash the file
 * at @local_path, name it by its digest and the extension of its path,
 * and copy it into the store unless it is over the per-file cap
 * (TG_MAX_STORE_FILE_SIZE). @file_size is the size to judge by when the
 * file's own size cannot be read. nullopt when it could not be hashed or
 * copied.
 */
std::optional<TgLoggerd::Blob>
TgLoggerd::putInStore(const std::string &local_path, int64_t file_size,
		      const std::string &what)
{
	auto hex = sha256_file_hex(local_path);
	if (!hex.has_value()) {
		pr_error(l_, "Failed to hash file: %s", local_path.c_str());
		return std::nullopt;
	}

	/* Content-addressed: the blob is named <sha256>[.ext]. */
	std::string ext = fs::path(local_path).extension().string();
	if (!ext.empty() && ext[0] == '.')
		ext.erase(0, 1);
	std::transform(ext.begin(), ext.end(), ext.begin(),
		       [](unsigned char c) { return (char)std::tolower(c); });

	/*
	 * Storage is limited. Files at or above max_store_file_size_ are not
	 * copied into the store -- only their metadata is recorded, so they
	 * stay identifiable and re-downloadable by tg_file_id. Decide on the
	 * actual on-disk size, falling back to TDLib's reported size.
	 */
	std::error_code szec;
	uintmax_t on_disk_size = fs::file_size(local_path, szec);
	uint64_t size_for_policy = szec
		? (uint64_t)(file_size < 0 ? 0 : file_size)
		: (uint64_t)on_disk_size;
	bool store_bytes = (max_store_file_size_ == 0) ||
			   (size_for_policy < max_store_file_size_);

	if (store_bytes) {
		/* 0a/53/3d/97/ed/<sha256>[.ext]; see blobPath(). */
		fs::path dest = blobPath(storage_dir_, *hex, ext);
		fs::path dir = dest.parent_path();

		std::error_code ec;
		fs::create_directories(dir, ec);
		if (ec) {
			pr_error(l_, "Failed to create storage directory %s: %s",
				 dir.c_str(), ec.message().c_str());
			return std::nullopt;
		}
		/*
		 * skip_existing makes the copy a no-op (no error) when the
		 * content-addressed destination is already present, which also
		 * makes concurrent file-pool workers copying identical content
		 * race-free.
		 */
		fs::copy_file(local_path, dest, fs::copy_options::skip_existing,
			      ec);
		if (ec) {
			pr_error(l_, "Failed to copy file to %s: %s",
				 dest.c_str(), ec.message().c_str());
			return std::nullopt;
		}
	} else {
		pr_info(l_, "File too large to store (%llu bytes >= %llu limit);"
			" recording metadata only | %s sha256=%s",
			(unsigned long long)size_for_policy,
			(unsigned long long)max_store_file_size_,
			what.c_str(), hex->c_str());
	}
	return Blob{ std::move(*hex), std::move(ext), store_bytes };
}

std::optional<uint64_t>
TgLoggerd::storeDownloadedFile(const std::string &local_path,
			       const std::string &tg_file_id,
			       const std::string &tg_unique_id,
			       int64_t file_size, const char *file_type,
			       const std::string &orig_file_name)
{
	auto blob = putInStore(local_path, file_size,
			       "tg_file_id=" + tg_file_id);
	if (!blob.has_value())
		return std::nullopt;

	models::File f;
	f.tg_file_id = tg_file_id;
	f.tg_unique_id = tg_unique_id;
	f.file_type = file_type;
	f.file_size = (uint64_t)(file_size < 0 ? 0 : file_size);
	f.sha256_hex = blob->sha256_hex;
	if (!blob->ext.empty())
		f.file_ext = blob->ext;
	f.orig_file_name = orig_file_name;
	f.on_disk = blob->on_disk;

	std::string row_ext;
	uint64_t id = db_->upsertFile(f, &row_ext);
	/*
	 * The row may already have had another extension (recorded
	 * metadata-only first, or these bytes stored before under another
	 * one): its blob must be named by the row's, which is where the web
	 * and the janitor look for it.
	 */
	if (blob->on_disk && row_ext != blob->ext) {
		std::error_code ec;
		if (renameBlob(storage_dir_, blob->sha256_hex, blob->ext,
			       row_ext, ec))
			pr_info(l_, "Renamed stored file to its row's extension"
				" | id=%llu sha256=%s ext=%s->%s",
				(unsigned long long)id,
				blob->sha256_hex.c_str(),
				blob->ext.empty() ? "(none)" : blob->ext.c_str(),
				row_ext.empty() ? "(none)" : row_ext.c_str());
		else
			pr_error(l_, "Failed to rename stored file to its row's"
				 " extension | id=%llu sha256=%s ext=%s->%s: %s",
				 (unsigned long long)id,
				 blob->sha256_hex.c_str(),
				 blob->ext.empty() ? "(none)" : blob->ext.c_str(),
				 row_ext.empty() ? "(none)" : row_ext.c_str(),
				 ec.message().c_str());
	}
	/* Remember it so later references link instead of re-downloading. */
	known_files_.put(tg_file_id, tg_unique_id,
			 models::KnownFile{ id, false });
	return id;
}

void TgLoggerd::setLogger(log_hd_t *h) noexcept
{
	/* Takes ownership; a different handle replaces (and frees) the old. */
	if (h != l_)
		log_owner_.reset(h);
	l_ = h;
}

void TgLoggerd::setLoggerLevel(int8_t log_level) noexcept
{
	pr_set_log_level(l_, log_level);
}

} /* namespace tgloggerd */
