// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#include "TelegramSender.hpp"

#include <td/telegram/Client.h>
#include <td/telegram/td_api.h>
#include <td/telegram/td_api.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <utility>

namespace discordd {

namespace td_api = td::td_api;

/*
 * TDLib exposes message ids shifted left by 20 bits; the server id everything
 * else stores is that value shifted back down.
 */
static inline int64_t to_server_id(int64_t tdlib_id)
{
	return tdlib_id >> 20;
}

static inline int64_t to_tdlib_id(int64_t server_id)
{
	return server_id << 20;
}

enum { LOG_ERROR = 0, LOG_WARN = 1, LOG_INFO = 2, LOG_DEBUG = 3 };

struct Bot {
	int32_t		client_id = 0;
	uint64_t	key = 0;
	std::string	token;
	int64_t		user_id = 0;
	std::string	username;
	std::string	error;

	enum State { Authorizing, Ready, Failed } state = Authorizing;

	std::mutex		mtx;
	std::condition_variable	cv;
};

struct TelegramSender::Impl {
	int32_t		api_id;
	std::string	api_hash;
	std::string	data_dir;
	SenderLog	log_sink;

	std::unique_ptr<td::ClientManager>	cm;
	std::thread				recv_thr;
	std::atomic<bool>			stop{false};

	std::mutex					bots_mtx;
	std::map<uint64_t, std::shared_ptr<Bot>>	bots;
	std::map<int32_t, std::shared_ptr<Bot>>		by_client;

	/* In-flight queries, keyed by the request id we allocated. */
	std::mutex	req_mtx;
	uint64_t	next_req = 1;
	std::map<uint64_t, std::function<void(td_api::object_ptr<td_api::Object>)>>
		handlers;

	/*
	 * Sends awaiting confirmation. TDLib answers sendMessage immediately
	 * with a provisional message id and only later reports the final one
	 * via updateMessageSendSucceeded, so the id worth storing is not the
	 * one the call returns. Keyed by (client, provisional id).
	 */
	std::mutex	send_mtx;
	std::map<std::pair<int32_t, int64_t>, std::shared_ptr<std::promise<int64_t>>>
		pending_sends;

	/* (client, chat) pairs already resolved by ensure_chat(). */
	std::mutex				chat_mtx;
	std::set<std::pair<int32_t, int64_t>>	known_chats;

	/*
	 * TDLib will not send to a chat it has not loaded, answering
	 * "Chat not found" -- and a bot, unlike a user account, gets no chat
	 * list at startup: it only learns chats from incoming updates. Since
	 * this bot never receives anything (it only sends), the chat has to be
	 * fetched explicitly the first time we address it.
	 */
	bool ensure_chat(int32_t client_id, int64_t chat_id, std::string *err);

	void log(int lvl, const std::string &msg) const
	{
		if (log_sink)
			log_sink(lvl, msg);
	}

	/* One TDLib session directory per bot, so tokens never share state. */
	std::string data_dir_for(uint64_t key) const
	{
		return data_dir + "/bot-" + std::to_string(key);
	}

	uint64_t
	send_query(int32_t client_id, td_api::object_ptr<td_api::Function> f,
		   std::function<void(td_api::object_ptr<td_api::Object>)> cb)
	{
		uint64_t id;
		{
			std::lock_guard<std::mutex> lk(req_mtx);
			id = next_req++;
			if (cb)
				handlers[id] = std::move(cb);
		}
		cm->send(client_id, id, std::move(f));
		return id;
	}

	std::shared_ptr<Bot> bot_for_client(int32_t client_id)
	{
		std::lock_guard<std::mutex> lk(bots_mtx);
		auto it = by_client.find(client_id);
		return it == by_client.end() ? nullptr : it->second;
	}

	std::shared_ptr<Bot> bot_for_key(uint64_t key)
	{
		std::lock_guard<std::mutex> lk(bots_mtx);
		auto it = bots.find(key);
		return it == bots.end() ? nullptr : it->second;
	}

	void settle(const std::shared_ptr<Bot> &b, Bot::State st,
		    const std::string &err)
	{
		{
			std::lock_guard<std::mutex> lk(b->mtx);
			b->state = st;
			if (!err.empty())
				b->error = err;
		}
		b->cv.notify_all();
	}

	void on_auth_state(const std::shared_ptr<Bot> &b,
			   td_api::AuthorizationState &st);
	void on_update(int32_t client_id,
		       td_api::object_ptr<td_api::Object> obj);
	void recv_loop(void);
};

void TelegramSender::Impl::on_auth_state(const std::shared_ptr<Bot> &b,
					 td_api::AuthorizationState &st)
{
	switch (st.get_id()) {
	case td_api::authorizationStateWaitTdlibParameters::ID: {
		auto p = td_api::make_object<td_api::setTdlibParameters>();
		/* One session directory per bot, keyed by its row id. */
		p->database_directory_ = data_dir_for(b->key);
		/*
		 * A sender needs no local message database: it never reads
		 * history, and keeping one would grow without bound.
		 */
		p->use_message_database_ = false;
		p->use_secret_chats_ = false;
		p->api_id_ = api_id;
		p->api_hash_ = api_hash;
		p->system_language_code_ = "en";
		p->device_model_ = "Server";
		p->application_version_ = "1.0";
		send_query(b->client_id, std::move(p), {});
		break;
	}
	case td_api::authorizationStateWaitPhoneNumber::ID: {
		/* A bot authenticates with its token instead of a number. */
		send_query(b->client_id,
			   td_api::make_object<
				   td_api::checkAuthenticationBotToken>(b->token),
			   [this, b](td_api::object_ptr<td_api::Object> o) {
				   if (o && o->get_id() == td_api::error::ID) {
					   auto e = td::move_tl_object_as<
						   td_api::error>(o);
					   settle(b, Bot::Failed,
						  "bot token rejected: " +
							  e->message_);
				   }
			   });
		break;
	}
	case td_api::authorizationStateReady::ID: {
		send_query(b->client_id, td_api::make_object<td_api::getMe>(),
			   [this, b](td_api::object_ptr<td_api::Object> o) {
				   if (o && o->get_id() == td_api::user::ID) {
					   auto u = td::move_tl_object_as<
						   td_api::user>(o);
					   std::lock_guard<std::mutex> lk(b->mtx);
					   b->user_id = u->id_;
					   b->username =
						   u->usernames_ &&
								   !u->usernames_
									    ->active_usernames_
									    .empty()
							   ? u->usernames_
								     ->active_usernames_[0]
							   : std::string();
				   }
				   settle(b, Bot::Ready, "");
			   });
		break;
	}
	case td_api::authorizationStateClosed::ID:
		settle(b, Bot::Failed, "authorization closed");
		break;
	default:
		break;
	}
}

void TelegramSender::Impl::on_update(int32_t client_id,
				     td_api::object_ptr<td_api::Object> obj)
{
	if (!obj)
		return;

	switch (obj->get_id()) {
	case td_api::updateAuthorizationState::ID: {
		auto u = td::move_tl_object_as<td_api::updateAuthorizationState>(
			obj);
		auto b = bot_for_client(client_id);
		if (b && u->authorization_state_)
			on_auth_state(b, *u->authorization_state_);
		break;
	}
	case td_api::updateMessageSendSucceeded::ID: {
		auto u = td::move_tl_object_as<td_api::updateMessageSendSucceeded>(
			obj);
		std::shared_ptr<std::promise<int64_t>> pr;
		{
			std::lock_guard<std::mutex> lk(send_mtx);
			auto it = pending_sends.find(
				{client_id, u->old_message_id_});
			if (it != pending_sends.end()) {
				pr = it->second;
				pending_sends.erase(it);
			}
		}
		if (pr && u->message_)
			pr->set_value(to_server_id(u->message_->id_));
		break;
	}
	case td_api::updateMessageSendFailed::ID: {
		auto u = td::move_tl_object_as<td_api::updateMessageSendFailed>(
			obj);
		std::shared_ptr<std::promise<int64_t>> pr;
		{
			std::lock_guard<std::mutex> lk(send_mtx);
			auto it = pending_sends.find(
				{client_id, u->old_message_id_});
			if (it != pending_sends.end()) {
				pr = it->second;
				pending_sends.erase(it);
			}
		}
		if (pr)
			pr->set_value(0);
		log(LOG_WARN, "telegram: send failed");
		break;
	}
	default:
		break;
	}
}

bool TelegramSender::Impl::ensure_chat(int32_t client_id, int64_t chat_id,
				       std::string *err)
{
	{
		std::lock_guard<std::mutex> lk(chat_mtx);
		if (known_chats.count({client_id, chat_id}))
			return true;
	}

	auto done = std::make_shared<std::promise<std::string>>();
	auto done_f = done->get_future();
	send_query(client_id, td_api::make_object<td_api::getChat>(chat_id),
		   [done](td_api::object_ptr<td_api::Object> o) {
			   if (o && o->get_id() == td_api::error::ID) {
				   auto e = td::move_tl_object_as<td_api::error>(o);
				   done->set_value(e->message_);
				   return;
			   }
			   done->set_value(std::string());
		   });

	if (done_f.wait_for(std::chrono::seconds(20)) !=
	    std::future_status::ready) {
		if (err)
			*err = "timed out resolving chat";
		return false;
	}
	const std::string e = done_f.get();
	if (!e.empty()) {
		if (err)
			*err = "cannot resolve chat " +
			       std::to_string(chat_id) + ": " + e +
			       " (is the bot a member of it?)";
		return false;
	}

	std::lock_guard<std::mutex> lk(chat_mtx);
	known_chats.insert({client_id, chat_id});
	return true;
}

void TelegramSender::Impl::recv_loop(void)
{
	while (!stop.load()) {
		auto resp = cm->receive(0.5);
		if (!resp.object)
			continue;

		if (resp.request_id == 0) {
			on_update(resp.client_id, std::move(resp.object));
			continue;
		}

		std::function<void(td_api::object_ptr<td_api::Object>)> h;
		{
			std::lock_guard<std::mutex> lk(req_mtx);
			auto it = handlers.find(resp.request_id);
			if (it != handlers.end()) {
				h = std::move(it->second);
				handlers.erase(it);
			}
		}
		if (h)
			h(std::move(resp.object));
	}
}

TelegramSender::TelegramSender(int32_t api_id, std::string api_hash,
			       std::string data_dir, SenderLog log)
	: impl_(new Impl())
{
	impl_->api_id = api_id;
	impl_->api_hash = std::move(api_hash);
	impl_->data_dir = std::move(data_dir);
	impl_->log_sink = std::move(log);
}

TelegramSender::~TelegramSender(void)
{
	stop();
}

void TelegramSender::start(void)
{
	if (impl_->cm)
		return;
	/* Quieten TDLib's own logging; ours is the interesting one. */
	td::ClientManager::execute(
		td_api::make_object<td_api::setLogVerbosityLevel>(1));
	impl_->cm = std::make_unique<td::ClientManager>();
	impl_->recv_thr = std::thread([this] { impl_->recv_loop(); });
}

void TelegramSender::stop(void)
{
	if (!impl_->cm)
		return;
	impl_->stop = true;
	if (impl_->recv_thr.joinable())
		impl_->recv_thr.join();
	impl_->cm.reset();
}

int64_t TelegramSender::addBot(uint64_t key, const std::string &token,
			       std::string *err)
{
	if (!impl_->cm) {
		if (err)
			*err = "TelegramSender not started";
		return 0;
	}

	if (auto existing = impl_->bot_for_key(key)) {
		std::lock_guard<std::mutex> lk(existing->mtx);
		if (existing->state == Bot::Ready)
			return existing->user_id;
	}

	auto b = std::make_shared<Bot>();
	b->key = key;
	b->token = token;
	b->client_id = impl_->cm->create_client_id();
	{
		std::lock_guard<std::mutex> lk(impl_->bots_mtx);
		impl_->bots[key] = b;
		impl_->by_client[b->client_id] = b;
	}

	/*
	 * Nudge TDLib into emitting its first authorization state; everything
	 * after that is driven from on_auth_state().
	 */
	impl_->send_query(b->client_id,
			  td_api::make_object<td_api::getAuthorizationState>(),
			  {});

	std::unique_lock<std::mutex> lk(b->mtx);
	if (!b->cv.wait_for(lk, std::chrono::seconds(60), [&b] {
		    return b->state != Bot::Authorizing;
	    })) {
		if (err)
			*err = "timed out waiting for bot authorization";
		return 0;
	}
	if (b->state != Bot::Ready) {
		if (err)
			*err = b->error.empty() ? "authorization failed"
						: b->error;
		return 0;
	}
	return b->user_id;
}

std::string TelegramSender::botUsername(uint64_t key)
{
	auto b = impl_->bot_for_key(key);
	if (!b)
		return std::string();
	std::lock_guard<std::mutex> lk(b->mtx);
	return b->username;
}

int64_t TelegramSender::sendText(uint64_t key, int64_t chat_id,
				 const std::string &text,
				 int64_t reply_to_server_id, std::string *err)
{
	auto b = impl_->bot_for_key(key);
	if (!b) {
		if (err)
			*err = "unknown bot";
		return 0;
	}

	if (!impl_->ensure_chat(b->client_id, chat_id, err))
		return 0;

	auto content = td_api::make_object<td_api::inputMessageText>();
	auto ft = td_api::make_object<td_api::formattedText>();
	ft->text_ = text;
	content->text_ = std::move(ft);

	auto req = td_api::make_object<td_api::sendMessage>();
	req->chat_id_ = chat_id;
	req->input_message_content_ = std::move(content);
	if (reply_to_server_id) {
		auto r = td_api::make_object<td_api::inputMessageReplyToMessage>();
		r->message_id_ = to_tdlib_id(reply_to_server_id);
		req->reply_to_ = std::move(r);
	}

	auto provisional = std::make_shared<std::promise<int64_t>>();
	auto provisional_f = provisional->get_future();
	auto confirmed = std::make_shared<std::promise<int64_t>>();
	auto confirmed_f = confirmed->get_future();
	auto fail = std::make_shared<std::promise<std::string>>();
	auto fail_f = fail->get_future();

	impl_->send_query(
		b->client_id, std::move(req),
		[this, b, provisional, confirmed,
		 fail](td_api::object_ptr<td_api::Object> o) {
			if (o && o->get_id() == td_api::error::ID) {
				auto e = td::move_tl_object_as<td_api::error>(o);
				fail->set_value(e->message_);
				provisional->set_value(0);
				return;
			}
			if (!o || o->get_id() != td_api::message::ID) {
				fail->set_value("unexpected sendMessage reply");
				provisional->set_value(0);
				return;
			}
			auto m = td::move_tl_object_as<td_api::message>(o);
			const int64_t temp_id = m->id_;
			{
				std::lock_guard<std::mutex> lk(impl_->send_mtx);
				impl_->pending_sends[{b->client_id, temp_id}] =
					confirmed;
			}
			provisional->set_value(temp_id);
		});

	if (provisional_f.wait_for(std::chrono::seconds(30)) !=
	    std::future_status::ready) {
		if (err)
			*err = "timed out sending message";
		return 0;
	}
	if (provisional_f.get() == 0) {
		if (err) {
			*err = fail_f.wait_for(std::chrono::seconds(0)) ==
					       std::future_status::ready
				       ? fail_f.get()
				       : "send failed";
		}
		return 0;
	}

	/* Wait for Telegram to assign the real id. */
	if (confirmed_f.wait_for(std::chrono::seconds(30)) !=
	    std::future_status::ready) {
		if (err)
			*err = "timed out waiting for send confirmation";
		return 0;
	}
	const int64_t server_id = confirmed_f.get();
	if (!server_id && err)
		*err = "send was not confirmed";
	return server_id;
}

bool TelegramSender::editText(uint64_t key, int64_t chat_id,
			      int64_t server_message_id, const std::string &text,
			      std::string *err)
{
	auto b = impl_->bot_for_key(key);
	if (!b) {
		if (err)
			*err = "unknown bot";
		return false;
	}

	if (!impl_->ensure_chat(b->client_id, chat_id, err))
		return false;

	auto content = td_api::make_object<td_api::inputMessageText>();
	auto ft = td_api::make_object<td_api::formattedText>();
	ft->text_ = text;
	content->text_ = std::move(ft);

	auto req = td_api::make_object<td_api::editMessageText>();
	req->chat_id_ = chat_id;
	req->message_id_ = to_tdlib_id(server_message_id);
	req->input_message_content_ = std::move(content);

	auto done = std::make_shared<std::promise<std::string>>();
	auto done_f = done->get_future();
	impl_->send_query(b->client_id, std::move(req),
			  [done](td_api::object_ptr<td_api::Object> o) {
				  if (o && o->get_id() == td_api::error::ID) {
					  auto e = td::move_tl_object_as<
						  td_api::error>(o);
					  done->set_value(e->message_);
					  return;
				  }
				  done->set_value(std::string());
			  });

	if (done_f.wait_for(std::chrono::seconds(30)) !=
	    std::future_status::ready) {
		if (err)
			*err = "timed out editing message";
		return false;
	}
	const std::string e = done_f.get();
	if (!e.empty()) {
		if (err)
			*err = e;
		return false;
	}
	return true;
}

} /* namespace discordd */
