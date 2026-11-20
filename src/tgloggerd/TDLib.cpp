// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 */
#include <tgloggerd/TDLib.hpp>
#include <tgloggerd/MessageExtract.hpp>
#include <tgloggerd/EmojiExtract.hpp>
#include <tgloggerd/CommunityExtract.hpp>
#include <tgloggerd/CommunityPacer.hpp>

#include <td/telegram/Client.h>
#include <td/telegram/td_api.h>
#include <td/telegram/td_api.hpp>

#include <algorithm>
#include <map>
#include <string>
#include <memory>
#include <vector>
#include <atomic>
#include <optional>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <functional>
#include <ctime>
#include <set>
#include <deque>
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <future>
#include <thread>
#include <chrono>
#include <cassert>

namespace tgloggerd {

namespace td_api = td::td_api;

namespace {

using Object = td_api::object_ptr<td_api::Object>;

/*
 * Helper to combine a set of lambdas into a single overload set that can
 * be handed to td_api::downcast_call.
 */
template <class... Fs>
struct overload;

template <class F>
struct overload<F> : public F {
	explicit overload(F f) : F(f) {}
};

template <class F, class... Fs>
struct overload<F, Fs...> : public overload<F>, overload<Fs...> {
	overload(F f, Fs... fs) : overload<F>(f), overload<Fs...>(fs...) {}
	using overload<F>::operator();
	using overload<Fs...>::operator();
};

template <class... F>
static auto overloaded(F... f)
{
	return overload<F...>(f...);
}

/*
 * Convert a TDLib message id to the server (Bot API / t.me) message id.
 *
 * TDLib cannot use the server identifier as its message_id (it also orders
 * local-only messages by it), so it packs the server id shifted left by 20
 * bits: a real server message id is divisible by 2^20 and the server id is
 * that value >> 20 (e.g. 1205367472128 = 1149528 << 20). Messages that exist
 * only locally are not divisible by 2^20 and have no server id, so they are
 * left unchanged. Apply this ONLY to ids stored in the database, never to an
 * id handed back to a TDLib API call (e.g. getMessage), which needs the raw
 * TDLib id.
 */
constexpr int64_t kTdMsgIdShift = 1048576; /* 2^20 */

inline int64_t to_server_msg_id(int64_t td_id)
{
	if (td_id != 0 && (td_id % kTdMsgIdShift) == 0)
		return td_id / kTdMsgIdShift;
	return td_id;
}

/*
 * Whether a TDLib message id names a message that exists on the server.
 *
 * The others exist only inside this TDLib instance: a message still being
 * sent, which gets its real id once the server accepts it, and a "local"
 * message TDLib makes up itself and shows in a chat (seen in practice as a
 * sticker authored by the group, appearing in groups the account has just
 * been let into). Neither has a server id, and neither is anything a member
 * of the chat sent, so they are not logged.
 *
 * Storing one is worse than noise. Its raw id is kept as it is, and such ids
 * are either huge, so the message sorts as the newest in the chat for ever,
 * or tiny (2, for the first local message of an empty chat), so it collides
 * with the real server message of that number and is merged into its row.
 */
inline bool is_server_msg_id(int64_t td_id)
{
	return td_id != 0 && (td_id % kTdMsgIdShift) == 0;
}

models::User map_user(const td_api::user &u)
{
	models::User m;

	m.id = u.id_;
	m.first_name = u.first_name_;
	m.last_name = u.last_name_;
	m.phone_number = u.phone_number_;

	m.accent_color_id = u.accent_color_id_;
	m.background_custom_emoji_id = u.background_custom_emoji_id_;
	m.profile_accent_color_id = u.profile_accent_color_id_;
	m.profile_background_custom_emoji_id = u.profile_background_custom_emoji_id_;

	m.is_premium = u.is_premium_;
	m.is_support = u.is_support_;
	m.is_contact = u.is_contact_;
	m.is_mutual_contact = u.is_mutual_contact_;
	m.restricts_new_chats = u.restricts_new_chats_;
	m.paid_message_star_count = u.paid_message_star_count_;
	m.language_code = u.language_code_;

	if (u.verification_status_) {
		m.is_verified = u.verification_status_->is_verified_;
		m.is_scam = u.verification_status_->is_scam_;
		m.is_fake = u.verification_status_->is_fake_;
	}

	if (u.restriction_info_) {
		m.restriction_reason = u.restriction_info_->restriction_reason_;
		m.has_sensitive_content = u.restriction_info_->has_sensitive_content_;
	}

	if (u.emoji_status_ && u.emoji_status_->type_ &&
	    u.emoji_status_->type_->get_id() ==
		    td_api::emojiStatusTypeCustomEmoji::ID) {
		auto &t = static_cast<const td_api::emojiStatusTypeCustomEmoji &>(
			*u.emoji_status_->type_);
		m.emoji_status_custom_emoji_id = t.custom_emoji_id_;
		m.emoji_status_expiration_date = u.emoji_status_->expiration_date_;
	}

	if (u.usernames_) {
		m.active_usernames = u.usernames_->active_usernames_;
		m.disabled_usernames = u.usernames_->disabled_usernames_;
		m.collectible_usernames = u.usernames_->collectible_usernames_;
	}

	m.type = models::UserType::Unknown;
	if (u.type_) {
		switch (u.type_->get_id()) {
		case td_api::userTypeRegular::ID:
			m.type = models::UserType::Regular;
			break;
		case td_api::userTypeDeleted::ID:
			m.type = models::UserType::Deleted;
			break;
		case td_api::userTypeBot::ID:
			m.type = models::UserType::Bot;
			break;
		default:
			m.type = models::UserType::Unknown;
			break;
		}
	}

	return m;
}

models::UserFullInfo map_user_full_info(const td_api::userFullInfo &fi,
					int64_t user_id)
{
	models::UserFullInfo m;
	m.user_id = user_id;

	if (fi.bio_)
		m.bio = fi.bio_->text_;

	if (fi.birthdate_) {
		m.birthday_day = fi.birthdate_->day_;
		m.birthday_month = fi.birthdate_->month_;
		/* year_ is 0 when the user hides or omits the year. */
		if (fi.birthdate_->year_ != 0)
			m.birthday_year = fi.birthdate_->year_;
	}

	m.personal_chat_id = fi.personal_chat_id_;
	return m;
}

/*
 * Map a td_api::chatMember to a GroupAdmin. Returns false (skip) unless the
 * member is a user (member_id is messageSenderUser) whose status is creator
 * or administrator. A creator's rights are synthesized to all-true, since
 * the owner implicitly holds every privilege.
 */
bool map_admin(const td_api::chatMember &m, models::GroupAdmin &out)
{
	if (!m.member_id_ ||
	    m.member_id_->get_id() != td_api::messageSenderUser::ID)
		return false;
	out.user_id = static_cast<const td_api::messageSenderUser &>(
		*m.member_id_).user_id_;
	out.custom_title = m.tag_;
	out.inviter_user_id = m.inviter_user_id_;
	out.joined_date = m.joined_chat_date_;

	if (!m.status_)
		return false;

	switch (m.status_->get_id()) {
	case td_api::chatMemberStatusCreator::ID: {
		auto &s = static_cast<const td_api::chatMemberStatusCreator &>(
			*m.status_);
		out.is_owner = true;
		out.can_manage_chat = out.can_change_info = out.can_post_messages =
		out.can_edit_messages = out.can_delete_messages =
		out.can_invite_users = out.can_restrict_members =
		out.can_pin_messages = out.can_manage_topics =
		out.can_promote_members = out.can_manage_video_chats =
		out.can_post_stories = out.can_edit_stories =
		out.can_delete_stories = out.can_manage_direct_messages =
		out.can_manage_tags = true;
		out.is_anonymous = s.is_anonymous_;
		return true;
	}
	case td_api::chatMemberStatusAdministrator::ID: {
		auto &s = static_cast<const td_api::chatMemberStatusAdministrator &>(
			*m.status_);
		if (!s.rights_)
			return true;
		const auto &r = *s.rights_;
		out.can_manage_chat = r.can_manage_chat_;
		out.can_change_info = r.can_change_info_;
		out.can_post_messages = r.can_post_messages_;
		out.can_edit_messages = r.can_edit_messages_;
		out.can_delete_messages = r.can_delete_messages_;
		out.can_invite_users = r.can_invite_users_;
		out.can_restrict_members = r.can_restrict_members_;
		out.can_pin_messages = r.can_pin_messages_;
		out.can_manage_topics = r.can_manage_topics_;
		out.can_promote_members = r.can_promote_members_;
		out.can_manage_video_chats = r.can_manage_video_chats_;
		out.can_post_stories = r.can_post_stories_;
		out.can_edit_stories = r.can_edit_stories_;
		out.can_delete_stories = r.can_delete_stories_;
		out.can_manage_direct_messages = r.can_manage_direct_messages_;
		out.can_manage_tags = r.can_manage_tags_;
		out.is_anonymous = r.is_anonymous_;
		return true;
	}
	default:
		/* restricted / member / left / banned: not an admin. */
		return false;
	}
}

/*
 * Whether a chat id refers to a private (one-to-one) chat.
 *
 * In TDLib a private chat's id equals the peer user id and is always
 * positive, whereas basic groups, supergroups and channels use negative
 * ids. Secret chats are disabled (use_secret_chats_ = false), so a
 * positive id is unambiguously a private chat. Only private chats are
 * logged to telegram_private_messages, whose chat_id foreign key references
 * telegram_users.id; routing group/channel messages there (negative chat ids) is
 * what violates that constraint.
 */
bool is_private_chat(int64_t chat_id)
{
	return chat_id > 0;
}

/*
 * Whether the account is a member of a group chat, as far as TDLib has
 * told us. Unknown until a supergroup or basic group update arrives;
 * is_member_chat() treats it as a member, so a chat whose status has not
 * arrived yet keeps today's behaviour.
 */
enum class Membership : uint8_t {
	Unknown,
	Member,
	NotMember,
};

const char *membership_name(Membership m)
{
	switch (m) {
	case Membership::Member:
		return "member";
	case Membership::NotMember:
		return "not a member";
	default:
		return "unknown";
	}
}

/*
 * Map the account's status in a supergroup, channel or basic group to a
 * member flag (storage-cache.md section 5). A creator counts as a member
 * whatever its is_member says, like an administrator.
 */
Membership membership_from_status(const td_api::ChatMemberStatus *s)
{
	if (!s)
		return Membership::Unknown;

	switch (s->get_id()) {
	case td_api::chatMemberStatusCreator::ID:
	case td_api::chatMemberStatusAdministrator::ID:
	case td_api::chatMemberStatusMember::ID:
		return Membership::Member;
	case td_api::chatMemberStatusRestricted::ID:
		return static_cast<const td_api::chatMemberStatusRestricted &>(
			       *s).is_member_ ?
			       Membership::Member : Membership::NotMember;
	case td_api::chatMemberStatusLeft::ID:
	case td_api::chatMemberStatusBanned::ID:
		return Membership::NotMember;
	default:
		return Membership::Unknown;
	}
}

/*
 * A basic group that is no longer active (deactivated, or upgraded to a
 * supergroup) has no members to speak of: nothing new arrives in it.
 */
Membership basic_group_membership(const td_api::basicGroup &bg)
{
	if (!bg.is_active_)
		return Membership::NotMember;
	return membership_from_status(bg.status_.get());
}

/*
 * Whether the user actually keeps this chat in their Main or Archive chat
 * list: a private chat they have a history with, or a group/channel they
 * joined. Chats seen only incidentally -- the origin of a forward, the
 * target of a cross-chat reply -- are known to TDLib but are in no list, so
 * this returns false for them. The backfiller uses it to fetch the user's
 * own chats before the incidental ones.
 */
bool chat_in_user_list(const td_api::chat &c)
{
	auto is_user_list = [](const td_api::ChatList *l) {
		return l && (l->get_id() == td_api::chatListMain::ID ||
			     l->get_id() == td_api::chatListArchive::ID);
	};

	/* A non-zero order in a user list means it is actually placed there. */
	for (const auto &p : c.positions_) {
		if (p && p->order_ != 0 && is_user_list(p->list_.get()))
			return true;
	}
	/* Fallback: membership without an ordered position. */
	for (const auto &l : c.chat_lists_) {
		if (is_user_list(l.get()))
			return true;
	}
	return false;
}


/* Map a formattedText's entities to the Discord-renderable FmtEntity spans.
 * Types Discord cannot render (mentions, links, hashtags, ...) are dropped;
 * the covered text still forwards as plain text. */
void extract_fmt_entities(const td_api::formattedText &ft,
			  std::vector<FmtEntity> &out)
{
	for (const auto &e : ft.entities_) {
		if (!e || !e->type_ || e->length_ <= 0)
			continue;
		FmtEntity fe;
		fe.offset = e->offset_;
		fe.length = e->length_;
		switch (e->type_->get_id()) {
		case td_api::textEntityTypeBold::ID:
			fe.type = FmtEntity::Type::Bold; break;
		case td_api::textEntityTypeItalic::ID:
			fe.type = FmtEntity::Type::Italic; break;
		case td_api::textEntityTypeUnderline::ID:
			fe.type = FmtEntity::Type::Underline; break;
		case td_api::textEntityTypeStrikethrough::ID:
			fe.type = FmtEntity::Type::Strikethrough; break;
		case td_api::textEntityTypeSpoiler::ID:
			fe.type = FmtEntity::Type::Spoiler; break;
		case td_api::textEntityTypeCode::ID:
			fe.type = FmtEntity::Type::Code; break;
		case td_api::textEntityTypePre::ID:
			fe.type = FmtEntity::Type::Pre; break;
		case td_api::textEntityTypePreCode::ID:
			fe.type = FmtEntity::Type::Pre;
			fe.language = static_cast<const td_api::textEntityTypePreCode &>(
				*e->type_).language_;
			break;
		case td_api::textEntityTypeBlockQuote::ID:
		case td_api::textEntityTypeExpandableBlockQuote::ID:
			fe.type = FmtEntity::Type::BlockQuote; break;
		default:
			continue; /* not renderable -> leave as plain text */
		}
		out.push_back(std::move(fe));
	}
}

/* Short content-kind label used by the Discord forwarder's placeholder text. */
const char *forward_kind_str(models::MessageContentType t)
{
	switch (t) {
	case models::MessageContentType::Text:      return "text";
	case models::MessageContentType::Photo:     return "photo";
	case models::MessageContentType::Video:     return "video";
	case models::MessageContentType::Document:  return "document";
	case models::MessageContentType::Audio:     return "audio";
	case models::MessageContentType::Voice:     return "voice";
	case models::MessageContentType::Sticker:   return "sticker";
	case models::MessageContentType::Animation: return "animation";
	case models::MessageContentType::Service:   return "service";
	case models::MessageContentType::VideoNote: return "video_note";
	case models::MessageContentType::Location:  return "location";
	case models::MessageContentType::Venue:     return "venue";
	case models::MessageContentType::Contact:   return "contact";
	case models::MessageContentType::Dice:      return "dice";
	case models::MessageContentType::Game:      return "game";
	case models::MessageContentType::Poll:      return "poll";
	case models::MessageContentType::Checklist: return "checklist";
	case models::MessageContentType::Story:     return "story";
	case models::MessageContentType::PaidMedia: return "paid_media";
	case models::MessageContentType::Invoice:   return "invoice";
	case models::MessageContentType::Giveaway:  return "giveaway";
	case models::MessageContentType::Call:      return "call";
	case models::MessageContentType::Rich:      return "rich";
	default:                                    return "";
	}
}

/* Whether a content kind has a main file the forwarder mirrors once it is
 * stored (message_content_file returns it). */
bool forward_kind_has_file(models::MessageContentType t)
{
	switch (t) {
	case models::MessageContentType::Photo:
	case models::MessageContentType::Video:
	case models::MessageContentType::Document:
	case models::MessageContentType::Audio:
	case models::MessageContentType::Voice:
	case models::MessageContentType::Sticker:
	case models::MessageContentType::Animation:
	case models::MessageContentType::VideoNote:
		return true;
	default:
		return false;
	}
}

/*
 * Extract forwarded-message origin info from a td_api::message. Returns
 * nullopt for non-forwarded messages. Shared by private and group
 * messages, whose *_fwd_info tables are identical.
 */
std::optional<models::ForwardInfo>
extract_forward_info(const td_api::message &message)
{
	if (!message.forward_info_)
		return std::nullopt;

	models::ForwardInfo fi;
	fi.origin_date = message.forward_info_->date_;

	if (message.forward_info_->origin_) {
		switch (message.forward_info_->origin_->get_id()) {
		case td_api::messageOriginUser::ID: {
			auto &o = static_cast<const td_api::messageOriginUser &>(
				*message.forward_info_->origin_);
			fi.origin_type = models::ForwardOriginType::User;
			fi.origin_sender_user_id = o.sender_user_id_;
			break;
		}
		case td_api::messageOriginHiddenUser::ID: {
			auto &o = static_cast<
				const td_api::messageOriginHiddenUser &>(
				*message.forward_info_->origin_);
			fi.origin_type = models::ForwardOriginType::HiddenUser;
			fi.origin_sender_name = o.sender_name_;
			break;
		}
		case td_api::messageOriginChat::ID: {
			auto &o = static_cast<const td_api::messageOriginChat &>(
				*message.forward_info_->origin_);
			fi.origin_type = models::ForwardOriginType::Chat;
			fi.origin_chat_id = o.sender_chat_id_;
			if (!o.author_signature_.empty())
				fi.origin_sender_name = o.author_signature_;
			break;
		}
		case td_api::messageOriginChannel::ID: {
			auto &o = static_cast<
				const td_api::messageOriginChannel &>(
				*message.forward_info_->origin_);
			fi.origin_type = models::ForwardOriginType::Channel;
			fi.origin_chat_id = o.chat_id_;
			fi.origin_message_id = to_server_msg_id(o.message_id_);
			if (!o.author_signature_.empty())
				fi.origin_sender_name = o.author_signature_;
			break;
		}
		default:
			break;
		}
	}

	return fi;
}

/* The size among @sizes whose file has Telegram unique id @unique_id. */
const td_api::file *chat_photo_size_with_unique_id(
	const std::vector<td_api::object_ptr<td_api::photoSize>> &sizes,
	const std::string &unique_id)
{
	if (unique_id.empty())
		return nullptr;
	for (const auto &sz : sizes)
		if (sz && sz->photo_ && sz->photo_->remote_ &&
		    sz->photo_->remote_->unique_id_ == unique_id)
			return sz->photo_.get();
	return nullptr;
}

/* The file of @p (a size or its video) with Telegram unique id @unique_id. */
const td_api::file *chat_photo_file_with_unique_id(const td_api::chatPhoto &p,
						   const std::string &unique_id)
{
	if (auto *x = chat_photo_size_with_unique_id(p.sizes_, unique_id))
		return x;
	if (unique_id.empty())
		return nullptr;
	for (const auto *a : { p.animation_.get(), p.small_animation_.get() })
		if (a && a->file_ && a->file_->remote_ &&
		    a->file_->remote_->unique_id_ == unique_id)
			return a->file_.get();
	return nullptr;
}

const td_api::linkPreview *message_link_preview(const td_api::MessageContent &c)
{
	if (c.get_id() != td_api::messageText::ID)
		return nullptr;
	return static_cast<const td_api::messageText &>(c).link_preview_.get();
}

/*
 * Return the primary downloadable file of a message's content, or nullptr
 * if the content carries no file. On success, *category is set to the
 * matching telegram_files.file_type value. For photos, the largest size is chosen.
 */
const td_api::file *message_content_file(const td_api::MessageContent &content,
					 const char **category,
					 std::string *file_name)
{
	switch (content.get_id()) {
	case td_api::messagePhoto::ID: {
		auto &c = static_cast<const td_api::messagePhoto &>(content);
		*category = "photo";
		if (!c.photo_)
			return nullptr;
		const td_api::file *best = nullptr;
		int64_t best_px = -1;
		for (const auto &sz : c.photo_->sizes_) {
			if (!sz || !sz->photo_)
				continue;
			int64_t px = (int64_t)sz->width_ * sz->height_;
			if (px > best_px) {
				best_px = px;
				best = sz->photo_.get();
			}
		}
		return best;
	}
	case td_api::messageVideo::ID: {
		auto &c = static_cast<const td_api::messageVideo &>(content);
		*category = "video";
		if (!c.video_)
			return nullptr;
		*file_name = c.video_->file_name_;
		return c.video_->video_.get();
	}
	case td_api::messageDocument::ID: {
		auto &c = static_cast<const td_api::messageDocument &>(content);
		*category = "document";
		if (!c.document_)
			return nullptr;
		*file_name = c.document_->file_name_;
		return c.document_->document_.get();
	}
	case td_api::messageAudio::ID: {
		auto &c = static_cast<const td_api::messageAudio &>(content);
		*category = "audio";
		if (!c.audio_)
			return nullptr;
		*file_name = c.audio_->file_name_;
		return c.audio_->audio_.get();
	}
	case td_api::messageVoiceNote::ID: {
		auto &c = static_cast<const td_api::messageVoiceNote &>(content);
		*category = "voice";
		return c.voice_note_ ? c.voice_note_->voice_.get() : nullptr;
	}
	case td_api::messageSticker::ID: {
		auto &c = static_cast<const td_api::messageSticker &>(content);
		*category = "sticker";
		return c.sticker_ ? c.sticker_->sticker_.get() : nullptr;
	}
	case td_api::messageAnimation::ID: {
		auto &c = static_cast<const td_api::messageAnimation &>(content);
		*category = "animation";
		if (!c.animation_)
			return nullptr;
		*file_name = c.animation_->file_name_;
		return c.animation_->animation_.get();
	}
	case td_api::messageVideoNote::ID: {
		auto &c = static_cast<const td_api::messageVideoNote &>(content);
		*category = "video_note";
		return c.video_note_ ? c.video_note_->video_.get() : nullptr;
	}
	default:
		return nullptr;
	}
}


/* Bound the reply-chain walk and the dedup set of fetched reply targets. */
constexpr int kMaxReplyDepth = 128;
constexpr size_t kReplyDedupCap = 1000000;

/* Drop a group from admin polling after this many consecutive permission
 * errors (e.g. we left it or lost visibility). */
constexpr int kAdminPollMaxMiss = 3;

/* getChatHistory can return an empty page transiently while it loads from the
 * server; tolerate this many empties before declaring a chat's history done. */
constexpr int kBackfillEmptyRetries = 3;

/* Seconds from authorization to the restart catch-up of chat rechecks. */
constexpr double kRecheckCatchUpDelay = 120.0;

/*
 * The (chat_id, message_id) of the message @m replies to, written to
 * @chat_id / @msg_id. Returns false for a non-reply or a story reply. The
 * replied message may be in another chat (cross-chat reply); a zero
 * chat_id in the reply info means the same chat as @m.
 */
bool reply_target(const td_api::message &m, int64_t &chat_id, int64_t &msg_id)
{
	if (!m.reply_to_ ||
	    m.reply_to_->get_id() != td_api::messageReplyToMessage::ID)
		return false;

	auto &r = static_cast<const td_api::messageReplyToMessage &>(*m.reply_to_);
	if (r.message_id_ == 0)
		return false;
	chat_id = (r.chat_id_ != 0) ? r.chat_id_ : m.chat_id_;
	msg_id = r.message_id_;
	return true;
}

} /* namespace */


struct TDLib::Impl {
	uint32_t	api_id_;
	std::string	api_hash_;
	std::string	data_dir_;

	bool		stopped_ = false;
	bool		is_authorized_ = false;
	int64_t		client_id_ = 0;
	int64_t		user_id_ = 0;
	std::uint64_t	current_query_id_ = 0;

	/* The daemon's log; nullptr logs nothing (see setLogger()). */
	log_hd_t	*l_ = nullptr;

	/*
	 * Query-id counter for fire-and-forget requests issued off the loop
	 * thread (deleteLocalFile). Seeded high so it never collides with the
	 * loop thread's monotonic current_query_id_; no handler is registered
	 * for these, so the response is simply dropped by process_response.
	 */
	static constexpr std::uint64_t	kAuxQueryIdBase = 1ull << 62;
	std::atomic<std::uint64_t>	aux_query_id_{kAuxQueryIdBase};

	/*
	 * Cross-thread requests (TDLib::request): a promise per aux query id,
	 * fulfilled by process_response. requests_open_ is true only while
	 * the client is ready and not closing. Both guarded by requests_mtx_.
	 */
	std::mutex					requests_mtx_;
	std::unordered_map<std::uint64_t, std::promise<Object>>	requests_;
	bool						requests_open_ = false;

	/*
	 * TDLib::status(): the state and when it last changed. Guarded by
	 * requests_mtx_, so it moves together with requests_open_.
	 */
	TDLib::State					state_ = TDLib::State::Waiting;
	int64_t						state_since_ =
		static_cast<int64_t>(std::time(nullptr));
	/* user_id_ for status(), which other threads call. */
	int64_t						status_user_id_ = 0;

	/* The thread that runs TDLib::loop(), to refuse request() on it. */
	std::atomic<std::thread::id>			loop_thread_{};

	/* One-time startup optimizeStorage (reclaim TDLib's cached backlog). */
	bool		purge_on_start_ = false;
	bool		purge_started_ = false;

	std::function<void(const TextMessage &)>	msg_handler_;
	std::function<void(const ForwardMessage &)>	forward_handler_;
	std::function<void(const ForwardMessage &)>	edit_forward_handler_;
	std::function<void(int64_t, int64_t)>		delete_forward_handler_;
	std::function<void(const models::PrivateMessage &)> private_msg_handler_;
	std::function<void(const models::GroupMessage &)> group_msg_handler_;
	std::function<void(const models::LinkPreview &, bool)> link_preview_handler_;
	std::function<void(const models::Poll &)>	poll_handler_;
	std::function<void(const models::Community &)>	community_handler_;
	std::function<void(const models::CommunityFullInfo &)>
							community_full_info_handler_;
	std::function<void(int64_t)>			community_named_handler_;
	std::function<void(int64_t, std::optional<int64_t>, std::optional<bool>)>
		group_member_info_handler_;
	/* What was last passed on per chat, so repeats are not. Loop only. */
	std::unordered_map<int64_t, std::pair<int64_t, int>> group_member_info_last_;
	void note_group_member_info(int64_t chat_id, int64_t count,
				    std::optional<bool> can_get_members);
	std::function<void(int64_t, int64_t, bool, bool)> message_pinned_handler_;
	/*
	 * Interaction totals waiting to be written, latest per message
	 * (chat_id, server message id). Loop-thread only. Flushed every
	 * interaction_flush_secs_, so a message changes in the database at
	 * most once per interval however busy it is. 0 = not recorded.
	 */
	struct PendingInteraction {
		models::Interaction	ia;
		bool			is_group = false;
	};
	std::map<std::pair<int64_t, int64_t>, PendingInteraction> interaction_pending_;
	std::function<void(std::vector<InteractionUpdate>)> interaction_handler_;
	int		interaction_flush_secs_ = 60;
	time_t		interaction_last_flush_ = 0;
	/* For the rate log: updates and distinct messages since the last
	 * report, and writes handed over. */
	time_t		interaction_report_at_ = 0;
	size_t		interaction_updates_ = 0;
	size_t		interaction_writes_ = 0;
	std::set<std::pair<int64_t, int64_t>>	interaction_seen_;
	std::function<void(int64_t, int64_t, bool, const std::optional<std::string> &)> fact_check_handler_;
	std::function<void(const models::ForumTopic &, bool)> forum_topic_handler_;
	std::function<void(int64_t, int32_t, bool)>	forum_topic_pinned_handler_;
	/* Forums whose topic list was read in this run (getForumTopics). */
	std::unordered_set<int64_t>			forum_topics_swept_;
	/* A message's content as delivered inline by updateMessageContent. */
	std::function<void(int64_t, int64_t, const models::MessageContent &,
			   bool)> content_version_handler_;
	std::function<void(const MessageFile &)>	message_file_handler_;
	/* setFileEmitBatchHandlers(): around the emits of one file. */
	std::function<void(int32_t)>	file_batch_begin_;
	std::function<void(int32_t)>	file_batch_end_;
	std::function<std::optional<models::KnownFile>(const std::string &,
						       const std::string &)>
						file_lookup_;
	std::function<void(const MessageFileLink &)>	message_file_link_handler_;
	std::function<void(const FileMeta &)>		file_meta_handler_;
	std::function<void(const PhotoLink &)>		photo_link_handler_;
	/* Photos linked from the index vs downloaded; loop thread only, for
	 * the debug log of how many downloads the index saves. */
	uint64_t	photo_links_ = 0;
	uint64_t	photo_downloads_ = 0;
	/* setMediaPolicy(); loop thread only after loop() starts. */
	bool		skip_nonmember_media_ = true;
	uint64_t	max_store_file_size_ = 0;
	std::function<void(const MessageReply &)>	message_reply_handler_;
	std::function<void(const models::User &)>	user_handler_;
	std::function<void(const models::UserFullInfo &)> user_full_info_handler_;
	std::function<void(int64_t, bool, const models::File &,
			   const models::File &)>	photo_video_handler_;
	void note_photo_video(int64_t owner_id, bool is_group,
			      const td_api::chatPhoto *photo);
	std::function<void(const ProfilePhoto &)>	photo_handler_;
	std::function<void(const models::Group &)>	group_handler_;
	std::function<void(const GroupPhoto &)>		group_photo_handler_;
	std::function<void(const models::GroupAdminList &)> group_admins_handler_;

	std::unique_ptr<td::ClientManager>		client_manager_;
	td_api::object_ptr<td_api::AuthorizationState>	authorization_state_;

	std::unordered_map<std::uint64_t, std::function<void(Object)>>	handlers_;
	std::unordered_map<int64_t, td_api::object_ptr<td_api::user>>	users_;
	std::unordered_map<int32_t, int64_t>				pending_photo_;

	/* Cached supergroup fields, awaiting the chat to assemble a group. */
	struct SgInfo {
		std::vector<std::string>	active;
		std::vector<std::string>	disabled;
		std::vector<std::string>	collectible;
		bool				is_channel = false;
		Membership			member = Membership::Unknown;
	};

	/* Assembled group state, merged from the chat/supergroup/full info. */
	struct GroupState {
		models::GroupType		type = models::GroupType::BasicGroup;
		int64_t				chat_id = 0;
		std::string			title;
		std::string			description;
		std::vector<std::string>	active_usernames;
		std::vector<std::string>	disabled_usernames;
		std::vector<std::string>	collectible_usernames;
		bool				chat_seen = false;
		Membership			member = Membership::Unknown;
	};

	std::unordered_map<int64_t, SgInfo>		supergroups_;
	/* Basic group membership, awaiting the chat like SgInfo. */
	std::unordered_map<int64_t, Membership>		basic_groups_;
	std::unordered_map<int64_t, GroupState>		group_state_;
	std::unordered_map<int64_t, int64_t>		chat_to_group_;
	std::unordered_map<int32_t, int64_t>		pending_group_photo_;

	/* In-flight message media downloads, keyed by TDLib file id. */
	struct PendingMsgFile {
		int64_t		chat_id;
		int64_t		message_id;
		bool		is_group;
		std::string	content_type;
		std::string	orig_file_name;
		/* Appended, never inserted: this struct is built with an
		 * aggregate initializer, so a field added anywhere but the end
		 * silently shifts every value into the wrong member. */
		bool		is_link_preview = false;
		/* The downloadFile query this entry waits on. */
		std::uint64_t	dl_query = 0;
		/* For a file beyond the message's main one: its role and
		 * position in telegram_message_files; empty for the main. */
		std::string	role{};
		int		position = 0;
	};
	/*
	 * Keyed by TDLib file id -- which TDLib DEDUPLICATES by remote id, so
	 * two messages carrying the same photo (a forwarded meme, one link
	 * previewed twice) share one id. A plain map's operator[] silently
	 * replaced the first message's pending entry with the second, and the
	 * first message never got its file linked. Hence a multimap, with
	 * every waiting message emitted when the download lands.
	 */
	std::unordered_multimap<int32_t, PendingMsgFile> pending_message_file_;

	/*
	 * The TDLib file ids in pending_photo_, pending_group_photo_ and
	 * pending_message_file_, readable from other threads
	 * (TDLib::isIngestDownloading). Written on the loop thread next to
	 * those maps: note_ingest_download() on every insert,
	 * ingest_download_done() once a completed or failed file has been
	 * handled.
	 */
	mutable std::mutex		ingest_dl_mtx_;
	std::unordered_set<int32_t>	ingest_dl_;

	/*
	 * Loop thread. ingest_query_: the latest downloadFile query sent for
	 * each file in ingest_dl_; a failure of an older one leaves the photo
	 * entries to the newer one. ingest_displaced_: files whose ingest
	 * download a downloadFile with another range replaced (the fetch
	 * service streaming a big file), resumed once TDLib stops downloading
	 * them. ingest_failures_ counts the entries dropped on a failure.
	 */
	std::unordered_map<int32_t, std::uint64_t>	ingest_query_;
	std::unordered_set<int32_t>			ingest_displaced_;
	std::uint64_t					ingest_failures_ = 0;

	/* Reply targets already fetched, so a chain is not re-walked. */
	std::set<std::pair<int64_t, int64_t>>		resolved_reply_targets_;

	/*
	 * Messages with a getMessage in flight from refetch_edited_message(),
	 * so a burst of updates about one message does not put several
	 * identical queries on the wire at once. `rearm` records that another
	 * request came in while this one was outstanding and the fetch must be
	 * repeated once it lands. `rearm_mirror` belongs to that queued repeat
	 * alone -- the in-flight request carries its own mirror flag in its
	 * callback -- so an edit coalesced behind a preview resolution still
	 * reaches Discord, without the repeat re-mirroring what the first one
	 * already did. Only ever touched on the TDLib loop thread.
	 */
	struct InflightRefetch {
		bool rearm = false;
		bool rearm_mirror = false;
	};
	std::map<std::pair<int64_t, int64_t>, InflightRefetch> refetch_inflight_;

	/* Groups whose admins we track (chat_ids), and a round-robin queue
	 * for periodic refresh. Populated on first sight of a member group. */
	std::unordered_set<int64_t>			admin_poll_set_;
	std::deque<int64_t>				admin_poll_queue_;
	std::unordered_map<int64_t, int>		admin_poll_miss_;
	bool						admin_poll_started_ = false;
	bool						closing_ = false;
	double						admin_poll_interval_ = 300.0;
	int						admin_poll_batch_ = 4;

	/* Background backfiller: a newest->oldest history walk per chat, driven
	 * round-robin and gently paced. cursor_msg_id is the oldest TdLib LOCAL
	 * message id fetched so far (0 = start at newest). */
	struct BackfillEntry {
		int64_t	cursor_msg_id = 0;
		bool	done = false;
		bool	in_flight = false;
		int	empty_retries = 0;
		/* In the user's Main/Archive chat list: fetched ahead of chats
		 * seen only incidentally (forward origins, reply targets). */
		bool	priority = false;
	};
	std::function<void(const models::BackfillState &)> backfill_state_handler_;
	std::unordered_map<int64_t, BackfillEntry>	backfill_;
	std::vector<int64_t>				backfill_order_;
	size_t						backfill_rr_ = 0;
	size_t						backfill_rr_low_ = 0;
	int						backfill_inflight_ = 0;
	bool						backfill_started_ = false;
	bool						backfill_enabled_ = false;
	double						backfill_interval_ = 0.0;
	double						backfill_discovery_interval_ = 300.0;
	double						backfill_next_delay_ = 0.0;
	int						backfill_page_ = 100;
	int						backfill_inflight_max_ = 1;

	/*
	 * Periodic full-info refetch requested off the loop thread (every 10th
	 * message; see DB::bumpMsgCount). refetch_*_pending_ are filled by the
	 * serial DB worker under refetch_mtx_ and drained on the loop thread;
	 * refetch_*_last_ is a loop-thread-only per-entity cooldown so a burst
	 * (e.g. backfill) cannot hammer the API.
	 */
	std::mutex					refetch_mtx_;
	std::unordered_set<int64_t>			refetch_users_pending_;
	std::unordered_set<int64_t>			refetch_groups_pending_;
	std::unordered_map<int64_t, time_t>		refetch_user_last_;
	std::unordered_map<int64_t, time_t>		refetch_group_last_;
	double						refetch_cooldown_ = 300.0;

	/*
	 * Communities (message-types.md section 4.13). communities_ is the
	 * last updateCommunity of each, community_checked_ the ones already
	 * handed to community_named_handler_ this run, and
	 * community_full_done_ / community_full_failed_ (time of failure)
	 * the outcome of their full info: loop thread only. The fetch queue
	 * is filled from the serial worker (fetchCommunityFullInfo) under
	 * refetch_mtx_ and drained at community_pacer_'s pace.
	 */
	std::unordered_map<int64_t, models::Community>	communities_;
	std::unordered_set<int64_t>			community_checked_;
	std::unordered_set<int64_t>			community_full_done_;
	std::unordered_map<int64_t, time_t>		community_full_failed_;
	std::deque<int64_t>				community_fetch_pending_;
	std::unordered_set<int64_t>			community_fetch_queued_;
	CommunityFullInfoPacer			community_pacer_;

	/*
	 * Rich messages TDLib delivered without all of their blocks
	 * (richMessage.is_full false), to be asked for in full: loop thread
	 * only. (chat_id, TDLib message id); queued at most once per run,
	 * not again within kRichFullRetrySecs after a failure.
	 */
	std::deque<std::pair<int64_t, int64_t>>		rich_full_pending_;
	std::set<std::pair<int64_t, int64_t>>		rich_full_seen_;
	std::map<std::pair<int64_t, int64_t>, time_t>	rich_full_failed_;
	time_t						rich_full_tick_ = 0;
	int						rich_full_in_tick_ = 0;

	/*
	 * Media relink queue (see relinkMessageMedia), under refetch_mtx_.
	 * relink_tick_/relink_sent_ are loop-thread-only pacing state.
	 */
	std::deque<std::pair<int64_t, int64_t>>	relink_pending_;
	size_t						relink_total_ = 0;
	size_t						relink_sent_ = 0;
	/* Old-row repair queue (see repairMessages), same lock and budget. */
	std::deque<std::pair<int64_t, int64_t>>	repair_pending_;
	size_t						repair_total_ = 0;
	size_t						repair_sent_ = 0;
	time_t						relink_tick_ = 0;
	int						relink_in_tick_ = 0;
	/* What the repair sent this second (recheck::kRepairPerSecond). */
	int						repair_in_tick_ = 0;

	/*
	 * Chat rechecks (TDLib::recheckChat): the queue under refetch_mtx_,
	 * the rest loop thread only. chat_last_date_ is each chat's last
	 * message date from updateNewChat and updateChatLastMessage, for the
	 * restart catch-up; recheck_paused_until_ a flood wait's end.
	 */
	recheck::Queue					recheck_q_;
	std::function<void(const RecheckAnswer &)>	recheck_handler_;
	std::function<void(const RecheckAnswer &)>	recheck_found_handler_;
	/* Later pages of rechecks already let in (recheckNextPage), under
	 * refetch_mtx_; they run before the queue. */
	std::deque<recheck::Item>			recheck_more_;
	uint64_t					recheck_seq_ = 0;
	std::unordered_map<int64_t, int64_t>		chat_last_date_;
	/* Each chat's last message's server id, for the catch-up's gaps. */
	std::unordered_map<int64_t, int64_t>		chat_last_id_;
	std::function<void(std::vector<int64_t>,
			   std::unordered_map<int64_t, int64_t>)>
							recheck_catchup_handler_;
	int						recheck_inflight_ = 0;
	int						recheck_in_tick_ = 0;
	time_t						recheck_paused_until_ = 0;
	/* The repair's yielding to rechecks (recheck::repairMayRun): when it
	 * last sent, and whether it is paused, for the pause/resume log. */
	recheck::Clock::time_point			repair_last_sent_{};
	bool						repair_paused_ = false;
	bool						recheck_catchup_armed_ = false;

	Impl(uint32_t api_id, const char *api_hash, const char *data_dir);
	~Impl(void);

	std::uint64_t next_query_id(void) { return ++current_query_id_; }

	void send_query(td_api::object_ptr<td_api::Function> f,
			std::function<void(Object)> handler);
	void delete_local_file(int32_t file_id);
	std::future<Object> request(td_api::object_ptr<td_api::Function> f,
				    std::uint64_t *id_out);
	Object request_wait(td_api::object_ptr<td_api::Function> f,
			    std::chrono::milliseconds timeout);
	void set_requests_open(bool open);
	void set_state(TDLib::State st);
	void process_response(td::ClientManager::Response response);
	void process_update(td_api::object_ptr<td_api::Object> update);
	void on_authorization_state_update(void);
	void check_authentication_error(Object object);
	std::function<void(Object)> create_authentication_query_handler(void);
	void handle_new_message(td_api::message &message);
	void build_forward_message(const td_api::message &message,
				   ForwardMessage &fm);
	void handle_message_for_private_chat(td_api::message &message, int depth);
	void handle_message_for_group_chat(td_api::message &message, int depth);
	void resolve_reply_message(const td_api::message &message, bool is_group,
				   int depth);
	void ensure_message_entities(const td_api::message &message);
	void mark_reply_resolved(int64_t chat_id, int64_t msg_id);
	void handle_update_message_content(int64_t chat_id, int64_t message_id,
					   const td_api::MessageContent *content);
	void handle_delete_messages(int64_t chat_id,
				    const td_api::array<td_api::int53> &message_ids,
				    bool is_permanent);
	void build_private_message(const td_api::message &message,
				   models::PrivateMessage &out);
	void build_group_message(const td_api::message &message,
				 models::GroupMessage &out);
	void resolve_forward_origin(const models::ForwardInfo &info);
	void ensure_user_saved(int64_t user_id);
	void ensure_chat_saved(int64_t chat_id);
	void refetch_edited_message(int64_t chat_id, int64_t message_id,
				    bool mirror);
	void maybe_store_link_preview(int64_t chat_id, int64_t tdlib_msg_id,
				      const td_api::MessageContent &content,
				      bool is_group);
	void maybe_download_message_file(const td_api::message &message,
					 bool is_group);
	void maybe_store_poll(const td_api::message &message);
	void note_community_named(int64_t community_id);
	void enqueue_community_full_info(int64_t community_id);
	void drain_community_full_info(void);
	void maybe_fetch_full_rich(const td_api::message &message);
	void drain_rich_full(void);
	void emit_community_full_info(int64_t community_id,
				      const td_api::communityFullInfo &fi);
	void note_interaction(int64_t chat_id, int64_t td_message_id,
			      const models::Interaction &ia, bool reactions_only);
	void drain_interaction(void);
	void sweep_forum_topics(int64_t chat_id, int32_t offset_date,
				int64_t offset_message_id,
				int32_t offset_topic_id, int pages);
	void ingest_message_file(PendingMsgFile ref, const td_api::file &file);
	void emit_message_file(const PendingMsgFile &ref, const td_api::file &f);
	bool record_without_download(int64_t chat_id, const td_api::file &f);
	void emit_file_meta(const PendingMsgFile &ref, const td_api::file &f,
			    bool is_group_photo);
	void maybe_download_profile_photo(const td_api::user &u);
	bool link_known_photo(int64_t owner_id, bool is_group,
			      const td_api::file &f, bool meta_ok);
	void request_user_full_info(int64_t user_id);
	void enqueue_refetch_user(int64_t user_id);
	void enqueue_refetch_group(int64_t group_id);
	void drain_refetch(void);
	void drain_relink(void);
	void drain_recheck(void);
	void on_recheck_page(const recheck::Item &it, Object obj);
	void note_chat_last_message(int64_t chat_id, const td_api::message *m);
	void queue_recheck_catchup(void);
	void refetch_user_info(int64_t user_id);
	void refetch_group_info(int64_t chat_id);
	void handle_file_update(const td_api::file &f);
	void note_ingest_download(int32_t file_id);
	void ingest_download_done(int32_t file_id);
	std::uint64_t start_ingest_download(int32_t file_id);
	void on_ingest_download_result(int32_t file_id, std::uint64_t query,
				       Object obj);
	void drop_failed_ingest(int32_t file_id, std::uint64_t query,
				const std::string &why);
	bool has_ingest_entries(int32_t file_id) const;
	void resume_ingest_download(int32_t file_id);
	Object resolve_file(const models::FileSource &src,
			    std::chrono::milliseconds timeout);
	void emit_photo(int64_t user_id, const td_api::file &f);
	void handle_new_chat(const td_api::chat &chat, bool from_chat_list);
	void fetch_group_admins(int64_t supergroup_id, int64_t chat_id);
	void emit_basic_group_admins(int64_t chat_id,
				     const td_api::basicGroupFullInfo &fi);
	void arm_admin_alarm(void);
	void on_admin_alarm(void);
	void poll_admin_batch(void);
	void maybe_download_group_photo(int64_t group_id,
					const td_api::chatPhotoInfo *photo);
	void set_membership(GroupState &st, Membership m);
	void update_group_membership(int64_t group_id, bool is_basic,
				     Membership m);
	bool is_member_chat(int64_t chat_id) const;
	void emit_group(int64_t group_id);
	void emit_group_photo(int64_t group_id, const td_api::file &f);

	void arm_backfill_alarm(void);
	void on_backfill_alarm(void);
	void backfill_tick(void);
	void on_backfill_page(int64_t chat_id, Object obj);
	void arm_backfill_discovery(void);
	void on_backfill_discovery(void);
	void backfill_register_chat(int64_t chat_id, bool priority);
	void emit_backfill_state(int64_t chat_id);
};


TDLib::Impl::Impl(uint32_t api_id, const char *api_hash, const char *data_dir)
	: api_id_(api_id)
	, api_hash_(api_hash)
	, data_dir_(data_dir)
{
	td::ClientManager::execute(
		td_api::make_object<td_api::setLogVerbosityLevel>(1));

	client_manager_ = std::make_unique<td::ClientManager>();
	client_id_ = client_manager_->create_client_id();

	/* Kick off the client so that the first authorization state arrives. */
	send_query(td_api::make_object<td_api::getOption>("version"), {});
}


void TDLib::Impl::send_query(td_api::object_ptr<td_api::Function> f,
			     std::function<void(Object)> handler)
{
	auto query_id = next_query_id();

	if (handler)
		handlers_.emplace(query_id, std::move(handler));

	client_manager_->send(client_id_, query_id, std::move(f));
}


/*
 * Thread-safe fire-and-forget deleteFile. Unlike send_query this may be called
 * from a file worker thread, so it must not touch the loop-thread-owned state
 * (handlers_, current_query_id_). td::ClientManager::send is thread-safe; the
 * aux query id is drawn from a separate atomic counter and no handler is
 * registered, so process_response drops the Ok response.
 */
void TDLib::Impl::delete_local_file(int32_t file_id)
{
	std::uint64_t id = aux_query_id_.fetch_add(1);
	client_manager_->send(client_id_, id,
		td_api::make_object<td_api::deleteFile>(file_id));
}


TDLib::Impl::~Impl(void)
{
	/* Nobody is left to deliver the answers. */
	set_requests_open(false);
}


/*
 * Send @f from a thread other than the loop thread. Like delete_local_file it
 * leaves the loop-thread state alone: the id comes from aux_query_id_ and the
 * promise goes into requests_, which process_response checks for ids it does
 * not find in handlers_. The promise is registered before the send, so the
 * answer cannot arrive first. *id_out is the id, or 0 when nothing was sent.
 */
std::future<Object> TDLib::Impl::request(
	td_api::object_ptr<td_api::Function> f, std::uint64_t *id_out)
{
	std::promise<Object> p;
	auto fut = p.get_future();

	*id_out = 0;
	if (std::this_thread::get_id() == loop_thread_.load()) {
		assert(!"TDLib::request() called on the TDLib loop thread");
		p.set_value(td_api::make_object<td_api::error>(
			TDLib::kRequestWrongThread,
			"TDLib::request() called on the TDLib loop thread"));
		return fut;
	}

	std::uint64_t id = aux_query_id_.fetch_add(1);
	{
		std::lock_guard<std::mutex> lk(requests_mtx_);
		if (!requests_open_) {
			p.set_value(td_api::make_object<td_api::error>(
				TDLib::kRequestNotReady,
				"TDLib client is not ready"));
			return fut;
		}
		requests_.emplace(id, std::move(p));
	}

	*id_out = id;
	client_manager_->send(client_id_, id, std::move(f));
	return fut;
}


Object TDLib::Impl::request_wait(td_api::object_ptr<td_api::Function> f,
				 std::chrono::milliseconds timeout)
{
	std::uint64_t id;
	auto fut = request(std::move(f), &id);

	if (fut.wait_for(timeout) == std::future_status::ready)
		return fut.get();

	bool forgotten;
	{
		std::lock_guard<std::mutex> lk(requests_mtx_);
		forgotten = requests_.erase(id) != 0;
	}
	if (forgotten)
		return td_api::make_object<td_api::error>(
			TDLib::kRequestTimeout, "TDLib request timed out");

	/*
	 * Not in the map: process_response or set_requests_open took the
	 * promise out just now and sets it right after unlocking.
	 */
	return fut.get();
}


/*
 * Open or close TDLib::request(). Closing fails every pending request: their
 * answers would come back through loop() only if the client lives on, and a
 * closing client may never send them. Late answers to failed ids are dropped
 * by process_response.
 */
void TDLib::Impl::set_state(TDLib::State st)
{
	std::lock_guard<std::mutex> lk(requests_mtx_);
	if (state_ == st || state_ == TDLib::State::Closing)
		return;
	state_ = st;
	state_since_ = static_cast<int64_t>(std::time(nullptr));
}


void TDLib::Impl::set_requests_open(bool open)
{
	std::unordered_map<std::uint64_t, std::promise<Object>> failed;

	{
		std::lock_guard<std::mutex> lk(requests_mtx_);
		requests_open_ = open;
		if (!open)
			failed.swap(requests_);
	}

	for (auto &kv : failed)
		kv.second.set_value(td_api::make_object<td_api::error>(
			TDLib::kRequestNotReady, "TDLib client is no longer ready"));
}


void TDLib::Impl::process_response(td::ClientManager::Response response)
{
	if (!response.object)
		return;

	if (response.request_id == 0) {
		process_update(std::move(response.object));
		return;
	}

	auto it = handlers_.find(response.request_id);
	if (it != handlers_.end()) {
		auto handler = std::move(it->second);
		handlers_.erase(it);
		handler(std::move(response.object));
		return;
	}

	if (response.request_id < kAuxQueryIdBase)
		return;

	/* A cross-thread request (TDLib::request), unless the caller has
	 * given up on it; deleteLocalFile's ids are never in the map. */
	std::promise<Object> p;
	{
		std::lock_guard<std::mutex> lk(requests_mtx_);
		auto rit = requests_.find(response.request_id);
		if (rit == requests_.end())
			return;
		p = std::move(rit->second);
		requests_.erase(rit);
	}
	p.set_value(std::move(response.object));
}


void TDLib::Impl::process_update(td_api::object_ptr<td_api::Object> update)
{
	td_api::downcast_call(
		*update,
		overloaded(
			[this](td_api::updateAuthorizationState &u) {
				authorization_state_ =
					std::move(u.authorization_state_);
				on_authorization_state_update();
			},
			[this](td_api::updateUser &u) {
				if (!u.user_)
					return;
				int64_t uid = u.user_->id_;
				bool first_seen =
					users_.find(uid) == users_.end();
				if (user_handler_)
					user_handler_(map_user(*u.user_));
				maybe_download_profile_photo(*u.user_);
				users_[uid] = std::move(u.user_);
				/*
				 * Bio and other full-info fields are not in the
				 * user object; fetch them once, when the user is
				 * first seen.
				 */
				if (first_seen)
					request_user_full_info(uid);
			},
			[this](td_api::updateUserFullInfo &u) {
				if (!user_full_info_handler_ || !u.user_full_info_)
					return;
				auto fi = map_user_full_info(*u.user_full_info_,
							     u.user_id_);
				/*
				 * The personal chat is a channel referenced by
				 * telegram_user_extra_info.personal_chat_id (FK to groups).
				 * Make sure it is saved first so the link resolves;
				 * for a known chat this persists it synchronously
				 * (ahead of the full-info upsert on the serial
				 * queue), for an unknown one it fetches it for a
				 * later refresh.
				 */
				if (fi.personal_chat_id != 0)
					ensure_chat_saved(fi.personal_chat_id);
				user_full_info_handler_(fi);
				note_photo_video(u.user_id_, false,
						 u.user_full_info_->photo_.get());
				/* A bot's community, if it was added to one. */
				note_community_named(
					u.user_full_info_->community_id_);
			},
			[this](td_api::updateFile &u) {
				if (u.file_)
					handle_file_update(*u.file_);
			},
			[this](td_api::updateNewChat &u) {
				if (u.chat_) {
					note_chat_last_message(u.chat_->id_,
						u.chat_->last_message_.get());
					handle_new_chat(*u.chat_, true);
					/* Register every accessible chat (group and
					 * private) for background history backfill,
					 * prioritizing the ones the user keeps in
					 * their own chat list. */
					backfill_register_chat(u.chat_->id_,
						chat_in_user_list(*u.chat_));
				}
			},
			[this](td_api::updateChatAddedToList &u) {
				/* The user joined a group/channel or started a
				 * chat after we first saw it: promote it to the
				 * high-priority backfill tier. */
				if (u.chat_list_ &&
				    (u.chat_list_->get_id() == td_api::chatListMain::ID ||
				     u.chat_list_->get_id() == td_api::chatListArchive::ID))
					backfill_register_chat(u.chat_id_, true);
			},
			[this](td_api::updateChatTitle &u) {
				auto it = chat_to_group_.find(u.chat_id_);
				if (it == chat_to_group_.end())
					return;
				group_state_[it->second].title = u.title_;
				emit_group(it->second);
			},
			[this](td_api::updateChatPhoto &u) {
				auto it = chat_to_group_.find(u.chat_id_);
				if (it == chat_to_group_.end())
					return;
				maybe_download_group_photo(u.chat_id_,
							   u.photo_.get());
			},
			[this](td_api::updateSupergroup &u) {
				if (!u.supergroup_)
					return;
				const auto &sg = *u.supergroup_;
				SgInfo info;
				if (sg.usernames_) {
					info.active = sg.usernames_->active_usernames_;
					info.disabled = sg.usernames_->disabled_usernames_;
					info.collectible = sg.usernames_->collectible_usernames_;
				}
				info.is_channel = sg.is_channel_;
				info.member = membership_from_status(
					sg.status_.get());
				supergroups_[sg.id_] = info;
				update_group_membership(sg.id_, false,
							info.member);
				note_group_member_info(-1000000000000LL - sg.id_,
						       sg.member_count_,
						       std::nullopt);
				/*
				 * A forum: read its topic list once per run, so
				 * topics created or renamed while the daemon was
				 * down are known too. The chat id of supergroup N
				 * is -(10^12 + N).
				 */
				if (sg.is_forum_ && forum_topic_handler_ &&
				    forum_topics_swept_
					    .insert(-1000000000000LL - sg.id_)
					    .second)
					sweep_forum_topics(
						-1000000000000LL - sg.id_, 0, 0,
						0, 0);

				auto it = group_state_.find(sg.id_);
				if (it == group_state_.end())
					return;
				it->second.active_usernames = info.active;
				it->second.disabled_usernames = info.disabled;
				it->second.collectible_usernames = info.collectible;
				it->second.type = info.is_channel ?
					models::GroupType::Channel :
					models::GroupType::Supergroup;
				emit_group(sg.id_);
			},
			[this](td_api::updateSupergroupFullInfo &u) {
				if (!u.supergroup_full_info_)
					return;
				auto &st = group_state_[u.supergroup_id_];
				st.description =
					u.supergroup_full_info_->description_;
				if (st.chat_seen)
					emit_group(u.supergroup_id_);
				note_group_member_info(
					-1000000000000LL - u.supergroup_id_,
					u.supergroup_full_info_->member_count_,
					u.supergroup_full_info_->can_get_members_);
				note_community_named(
					u.supergroup_full_info_->community_id_);
				note_photo_video(-1000000000000LL - u.supergroup_id_,
						 true,
						 u.supergroup_full_info_->photo_.get());
			},
			[this](td_api::updateCommunity &u) {
				if (!u.community_)
					return;
				models::Community c = community_of(*u.community_);
				if (community_handler_)
					community_handler_(c);
				communities_[c.id] = std::move(c);
			},
			[this](td_api::updateCommunityFullInfo &u) {
				if (u.community_full_info_)
					emit_community_full_info(u.community_id_,
						*u.community_full_info_);
			},
			[this](td_api::updateBasicGroup &u) {
				if (!u.basic_group_)
					return;
				const auto &bg = *u.basic_group_;
				Membership m = basic_group_membership(bg);
				basic_groups_[bg.id_] = m;
				update_group_membership(bg.id_, true, m);
				/* A basic group's chat id is minus its id. */
				note_group_member_info(-bg.id_, bg.member_count_,
						       std::nullopt);
			},
			[this](td_api::updateBasicGroupFullInfo &u) {
				if (!u.basic_group_full_info_)
					return;
				auto &st = group_state_[u.basic_group_id_];
				st.description =
					u.basic_group_full_info_->description_;
				/* A basic group's full info lists its members, to
				 * every member; an empty list means the account
				 * cannot see them. */
				note_group_member_info(-u.basic_group_id_,
					(int64_t)u.basic_group_full_info_->members_.size(),
					!u.basic_group_full_info_->members_.empty());
				/* A basic group's chat id is minus its id. */
				note_photo_video(-u.basic_group_id_, true,
						 u.basic_group_full_info_->photo_.get());
				if (!st.chat_seen)
					return;
				emit_group(u.basic_group_id_);
				/*
				 * Basic-group admins ride along in the full
				 * info; sync them for groups we track.
				 */
				if (admin_poll_set_.count(st.chat_id))
					emit_basic_group_admins(st.chat_id,
						*u.basic_group_full_info_);
			},
			[this](td_api::updateNewMessage &u) {
				/* Not a message of the chat: see
				 * is_server_msg_id. One of ours that is still
				 * being sent arrives below once it is real. */
				if (!is_server_msg_id(u.message_->id_))
					return;
				handle_new_message(*u.message_);
			},
			[this](td_api::updateMessageSendSucceeded &u) {
				/*
				 * A message this account sent has reached the
				 * server and now has its server id; until now
				 * it was skipped above under a temporary one.
				 */
				if (!is_server_msg_id(u.message_->id_))
					return;
				handle_new_message(*u.message_);
			},
			[this](td_api::updateMessageContent &u) {
				if (!is_server_msg_id(u.message_id_))
					return;
				handle_update_message_content(u.chat_id_,
					u.message_id_, u.new_content_.get());
			},
			[this](td_api::updateMessageEdited &u) {
				/*
				 * updateMessageEdited fires with edit_date but
				 * not the content, so the message has to be
				 * fetched. This is the path that captures
				 * essentially every real edit.
				 */
				if (!is_server_msg_id(u.message_id_))
					return;
				refetch_edited_message(u.chat_id_, u.message_id_,
						       true);
			},
			[this](td_api::updateMessageInteractionInfo &u) {
				auto ia = extract_interaction(u.interaction_info_.get());
				if (ia)
					note_interaction(u.chat_id_, u.message_id_,
							 *ia, false);
			},
			[this](td_api::updateMessageReactions &u) {
				models::Interaction ia;
				ia.reactions = reactions_json(u.reactions_);
				if (!ia.reactions)
					ia.reactions = "[]";
				note_interaction(u.chat_id_, u.message_id_, ia, true);
			},
			[this](td_api::updateMessageIsPinned &u) {
				if (!message_pinned_handler_ ||
				    !is_server_msg_id(u.message_id_))
					return;
				message_pinned_handler_(u.chat_id_,
					to_server_msg_id(u.message_id_),
					!is_private_chat(u.chat_id_), u.is_pinned_);
			},
			[this](td_api::updateMessageFactCheck &u) {
				if (!fact_check_handler_ ||
				    !is_server_msg_id(u.message_id_))
					return;
				fact_check_handler_(u.chat_id_,
					to_server_msg_id(u.message_id_),
					!is_private_chat(u.chat_id_),
					fact_check_json(u.fact_check_.get()));
			},
			[this](td_api::updateForumTopicInfo &u) {
				if (forum_topic_handler_ && u.info_)
					forum_topic_handler_(
						extract_forum_topic(*u.info_),
						false);
			},
			[this](td_api::updateForumTopic &u) {
				if (forum_topic_pinned_handler_)
					forum_topic_pinned_handler_(
						u.chat_id_, u.forum_topic_id_,
						u.is_pinned_);
			},
			[this](td_api::updatePoll &u) {
				/*
				 * Votes, a closed poll, an added option. Keyed by
				 * the poll, not by a message, which is why polls
				 * are rows of their own.
				 */
				if (poll_handler_ && u.poll_)
					poll_handler_(extract_poll(*u.poll_));
			},
			[this](td_api::updateDeleteMessages &u) {
				handle_delete_messages(
					u.chat_id_,
					u.message_ids_,
					u.is_permanent_);
			},
			[this](td_api::updateChatLastMessage &u) {
				note_chat_last_message(u.chat_id_,
						       u.last_message_.get());
			},
			[](auto &) {}
		)
	);
}


void TDLib::Impl::on_authorization_state_update(void)
{
	td_api::downcast_call(
		*authorization_state_,
		overloaded(
			[this](td_api::authorizationStateWaitTdlibParameters &) {
				auto p = td_api::make_object<
					td_api::setTdlibParameters>();
				p->database_directory_ = data_dir_;
				p->use_message_database_ = true;
				p->use_secret_chats_ = false;
				p->api_id_ = static_cast<int32_t>(api_id_);
				p->api_hash_ = api_hash_;
				p->system_language_code_ = "en";
				p->device_model_ = "Desktop";
				p->application_version_ = "1.0";
				send_query(std::move(p),
					   create_authentication_query_handler());
			},
			[this](td_api::authorizationStateWaitPhoneNumber &) {
				std::string pn;
				std::cout << "Enter phone number: " << std::flush;
				std::getline(std::cin, pn);
				send_query(td_api::make_object<
					   td_api::setAuthenticationPhoneNumber>(
						   pn, nullptr),
					   create_authentication_query_handler());
			},
			[this](td_api::authorizationStateWaitCode &) {
				std::string code;
				std::cout << "Enter authentication code: "
					  << std::flush;
				std::getline(std::cin, code);
				send_query(td_api::make_object<
					   td_api::checkAuthenticationCode>(code),
					   create_authentication_query_handler());
			},
			[this](td_api::authorizationStateWaitRegistration &) {
				std::string first, last;
				std::cout << "Enter first name: " << std::flush;
				std::getline(std::cin, first);
				std::cout << "Enter last name: " << std::flush;
				std::getline(std::cin, last);
				send_query(td_api::make_object<
					   td_api::registerUser>(first, last,
								 false),
					   create_authentication_query_handler());
			},
			[this](td_api::authorizationStateWaitPassword &) {
				std::string pass;
				std::cout << "Enter password: " << std::flush;
				std::getline(std::cin, pass);
				send_query(td_api::make_object<
					   td_api::checkAuthenticationPassword>(
						   pass),
					   create_authentication_query_handler());
			},
			[this](td_api::authorizationStateReady &) {
				is_authorized_ = true;
				send_query(td_api::make_object<td_api::getMe>(),
					[this](Object obj) {
						if (obj->get_id() !=
						    td_api::user::ID)
							return;
						auto u = td::move_tl_object_as<
							td_api::user>(obj);
						user_id_ = u->id_;
						std::lock_guard<std::mutex> lk(
							requests_mtx_);
						status_user_id_ = u->id_;
					});
				/*
				 * Have TDLib report every contact (an updateUser
				 * comes before an id is returned), so the logger
				 * learns who is a contact and keeps their own
				 * name apart from the contact name at once, not
				 * only when each next shows up. One call per
				 * start; the answer itself is not needed.
				 */
				send_query(td_api::make_object<td_api::getContacts>(),
					   nullptr);
				/*
				 * Reclaim TDLib's cached file backlog once per
				 * boot. tgloggerd keeps its own copy of every
				 * file it needs, so purging TDLib's cache only
				 * removes duplicates; immunity_delay spares files
				 * a file worker may be copying right now.
				 * optimizeStorage never touches db.sqlite /
				 * td.binlog, unlike a raw rm of the tree.
				 */
				if (purge_on_start_ && !purge_started_) {
					purge_started_ = true;
					auto o = td_api::make_object<
						td_api::optimizeStorage>();
					o->size_ = 1;
					o->ttl_ = 0;
					o->count_ = 0;
					o->immunity_delay_ = 60;
					o->return_deleted_file_statistics_ = true;
					send_query(std::move(o),
						[this](Object obj) {
							if (obj->get_id() !=
							    td_api::storageStatistics::ID)
								return;
							auto s = td::move_tl_object_as<
								td_api::storageStatistics>(obj);
							std::cerr << "optimizeStorage:"
								" reclaimed TDLib cache;"
								" size now " << s->size_
								<< " bytes in " << s->count_
								<< " files\n";
						});
				}
				/*
				 * Start the periodic admin poll once. Guarded
				 * so a re-login does not spawn a second alarm
				 * chain; disabled when the interval is <= 0.
				 */
				if (!admin_poll_started_ &&
				    admin_poll_interval_ > 0.0 &&
				    group_admins_handler_) {
					admin_poll_started_ = true;
					arm_admin_alarm();
				}
				/*
				 * Start the background backfiller once. Kick an
				 * initial chat-list load so every accessible chat
				 * is discovered (each arrives via updateNewChat).
				 */
				if (backfill_enabled_ && !backfill_started_) {
					backfill_started_ = true;
					arm_backfill_alarm();
					on_backfill_discovery();
				}
				/*
				 * The restart catch-up, once per run, once the
				 * chat lists had time to load (the backfill's
				 * discovery above asks for them).
				 */
				if (!recheck_catchup_armed_) {
					recheck_catchup_armed_ = true;
					send_query(td_api::make_object<td_api::setAlarm>(
							   kRecheckCatchUpDelay),
						   [this](Object) {
							   queue_recheck_catchup();
						   });
				}
			},
			[this](td_api::authorizationStateLoggingOut &) {
				is_authorized_ = false;
			},
			[](td_api::authorizationStateClosing &) {},
			[this](td_api::authorizationStateClosed &) {
				is_authorized_ = false;
				stopped_ = true;
			},
			[](auto &) {}
		)
	);

	/*
	 * TDLib::request() works only while ready. Any other state (logging
	 * out, closing, closed, or back to logging in) fails what is pending.
	 */
	int32_t id = authorization_state_->get_id();
	bool ready = id == td_api::authorizationStateReady::ID && !closing_;
	set_requests_open(ready);

	if (ready)
		set_state(TDLib::State::Ready);
	else if (closing_ || id == td_api::authorizationStateLoggingOut::ID ||
		 id == td_api::authorizationStateClosing::ID ||
		 id == td_api::authorizationStateClosed::ID)
		set_state(TDLib::State::Closing);
	else
		set_state(TDLib::State::Waiting);
}


void TDLib::Impl::check_authentication_error(Object object)
{
	if (object->get_id() == td_api::error::ID) {
		auto error = td::move_tl_object_as<td_api::error>(object);
		std::cerr << "Authentication error: " << error->message_
			  << std::endl;
	}
}


std::function<void(Object)> TDLib::Impl::create_authentication_query_handler(void)
{
	return [this](Object object) {
		check_authentication_error(std::move(object));
	};
}


void TDLib::Impl::handle_new_message(td_api::message &message)
{
	/*
	 * Route by chat kind: private (positive) chat ids go to
	 * telegram_private_messages (chat_id is a telegram_users.id); group, supergroup and
	 * channel chats (negative ids) go to telegram_group_messages (chat_id is a
	 * telegram_groups.id).
	 */
	if (is_private_chat(message.chat_id_))
		handle_message_for_private_chat(message, 0);
	else
		handle_message_for_group_chat(message, 0);

	/*
	 * Forwarder path: emit every live new message (any content type) so the
	 * Discord forwarder can mirror it. This runs only here (updateNewMessage),
	 * never for edits/deletes/backfill, so nothing is mirrored twice.
	 */
	if (forward_handler_) {
		ForwardMessage fm;
		build_forward_message(message, fm);
		forward_handler_(fm);
	}

	/* Legacy text-only handler path. */
	if (!msg_handler_)
		return;

	/* Only text messages are handled for now. */
	if (!message.content_ ||
	    message.content_->get_id() != td_api::messageText::ID)
		return;

	auto &content = static_cast<td_api::messageText &>(*message.content_);
	if (!content.text_)
		return;

	TextMessage msg;
	msg.sender_id = 0;
	msg.message_id = to_server_msg_id(message.id_);
	msg.text = content.text_->text_;

	if (message.sender_id_) {
		if (message.sender_id_->get_id() ==
		    td_api::messageSenderUser::ID) {
			auto &s = static_cast<td_api::messageSenderUser &>(
				*message.sender_id_);
			msg.sender_id = s.user_id_;
		} else if (message.sender_id_->get_id() ==
			   td_api::messageSenderChat::ID) {
			auto &s = static_cast<td_api::messageSenderChat &>(
				*message.sender_id_);
			msg.sender_id = s.chat_id_;
		}
	}

	/* Resolve the sender name and username from the cached telegram_users. */
	auto it = users_.find(msg.sender_id);
	if (it != users_.end() && it->second) {
		const auto &user = *it->second;
		msg.sender_name = user.first_name_;
		if (!user.last_name_.empty()) {
			if (!msg.sender_name.empty())
				msg.sender_name += " ";
			msg.sender_name += user.last_name_;
		}
		if (user.usernames_ &&
		    !user.usernames_->active_usernames_.empty())
			msg.sender_username =
				user.usernames_->active_usernames_[0];
	}

	msg_handler_(msg);
}

/* Flatten a message into a ForwardMessage for the Discord forwarder. Shared by
 * the new-message and edit paths (same content, sender, reply extraction). */
void TDLib::Impl::build_forward_message(const td_api::message &message,
				       ForwardMessage &fm)
{
	fm.chat_id     = message.chat_id_;
	fm.message_id  = to_server_msg_id(message.id_);
	fm.is_outgoing = message.is_outgoing_;
	fm.sender_id   = sender_user_id(message);
	if (message.sender_id_ &&
	    message.sender_id_->get_id() == td_api::messageSenderChat::ID)
		fm.sender_chat_id = static_cast<const td_api::messageSenderChat &>(
			*message.sender_id_).chat_id_;

	int64_t rc = 0, rm = 0;
	if (reply_target(message, rc, rm)) {
		fm.reply_to_chat_id = rc;
		fm.reply_to_msg_id  = to_server_msg_id(rm);
	}

	auto it = users_.find(fm.sender_id);
	if (it != users_.end() && it->second) {
		const auto &user = *it->second;
		fm.sender_name = user.first_name_;
		if (!user.last_name_.empty()) {
			if (!fm.sender_name.empty())
				fm.sender_name += " ";
			fm.sender_name += user.last_name_;
		}
		if (user.usernames_ &&
		    !user.usernames_->active_usernames_.empty())
			fm.sender_username =
				user.usernames_->active_usernames_[0];
	}

	models::MessageContent mc;
	extract_message_content(message, mc);
	if (mc.text.has_value())
		fm.text = *mc.text;
	/* Carry the formatting entities so the forwarder can render markdown.
	 * They index into ft->text_, which is exactly fm.text here. */
	const td_api::formattedText *ft = message_formatted_text(message);
	if (ft && !ft->text_.empty())
		extract_fmt_entities(*ft, fm.entities);
	fm.kind = forward_kind_str(mc.content_type);
	/* Media kinds carry a downloadable file (forwarded once stored). */
	fm.has_file = forward_kind_has_file(mc.content_type);

	/*
	 * Content with nothing to read but its structure (a location, a poll,
	 * a contact, ...) is forwarded as a short rendering of it, which the
	 * forwarder sends after any text or caption, so that the entities,
	 * which index into that text, stay where they are.
	 */
	if (message.content_)
		fm.summary = forward_summary(*message.content_);
}

void TDLib::Impl::handle_message_for_private_chat(td_api::message &message,
						  int depth)
{
	if (!private_msg_handler_)
		return;

	models::PrivateMessage pm;
	build_private_message(message, pm);
	if (pm.forward_info.has_value())
		resolve_forward_origin(*pm.forward_info);
	private_msg_handler_(pm);

	/* Row now exists; download and link any media attachment. */
	maybe_download_message_file(message, false);
	maybe_store_poll(message);
	note_community_named(message_community_id(message.content_.get()));
	/* ...and record its link preview, whose row keys off that same row. */
	if (message.content_)
		maybe_store_link_preview(message.chat_id_, message.id_,
					 *message.content_, false);

	maybe_fetch_full_rich(message);
	resolve_reply_message(message, false, depth);
}

void TDLib::Impl::handle_message_for_group_chat(td_api::message &message,
						int depth)
{
	if (!group_msg_handler_)
		return;

	models::GroupMessage gm;
	build_group_message(message, gm);
	if (gm.forward_info.has_value())
		resolve_forward_origin(*gm.forward_info);
	group_msg_handler_(gm);

	/* Row now exists; download and link any media attachment. */
	maybe_download_message_file(message, true);
	maybe_store_poll(message);
	note_community_named(message_community_id(message.content_.get()));
	if (forum_topic_handler_)
		if (auto t = forum_topic_from_created(message))
			forum_topic_handler_(*t, true);
	/* ...and record its link preview, whose row keys off that same row. */
	if (message.content_)
		maybe_store_link_preview(message.chat_id_, message.id_,
					 *message.content_, true);

	maybe_fetch_full_rich(message);
	resolve_reply_message(message, true, depth);
}

void TDLib::Impl::mark_reply_resolved(int64_t chat_id, int64_t msg_id)
{
	/* Bound memory: drop the dedup set if it grows too large. */
	if (resolved_reply_targets_.size() >= kReplyDedupCap)
		resolved_reply_targets_.clear();
	resolved_reply_targets_.insert({ chat_id, msg_id });
}

void TDLib::Impl::ensure_message_entities(const td_api::message &message)
{
	/*
	 * A replied message may live in a chat we have never seen (cross-chat
	 * reply). Make sure the chat's own entity and the sender exist, so
	 * the message row's foreign keys resolve. Uses the same
	 * fetch-if-unknown helpers as forward-origin resolution.
	 */
	if (is_private_chat(message.chat_id_))
		ensure_user_saved(message.chat_id_);
	else
		ensure_chat_saved(message.chat_id_);

	if (message.sender_id_) {
		if (message.sender_id_->get_id() ==
		    td_api::messageSenderUser::ID) {
			auto &s = static_cast<const td_api::messageSenderUser &>(
				*message.sender_id_);
			ensure_user_saved(s.user_id_);
		} else if (message.sender_id_->get_id() ==
			   td_api::messageSenderChat::ID) {
			auto &s = static_cast<const td_api::messageSenderChat &>(
				*message.sender_id_);
			ensure_chat_saved(s.chat_id_);
		}
	}
}

void TDLib::Impl::resolve_reply_message(const td_api::message &message,
					bool is_group, int depth)
{
	if (!message_reply_handler_)
		return;

	int64_t reply_chat_id, reply_msg_id;
	if (!reply_target(message, reply_chat_id, reply_msg_id))
		return;

	int64_t chat_id = message.chat_id_;
	int64_t message_id = message.id_;

	auto emit = [this, chat_id, message_id, reply_chat_id, reply_msg_id,
		     is_group]() {
		MessageReply mr;
		mr.chat_id = chat_id;
		mr.message_id = to_server_msg_id(message_id);
		mr.reply_to_chat_id = reply_chat_id;
		mr.reply_to_msg_id = to_server_msg_id(reply_msg_id);
		mr.is_group = is_group;
		message_reply_handler_(mr);
	};

	/*
	 * If the replied message was already fetched (dedup) or we hit the
	 * chain-depth cap, just record the link. Otherwise fetch and save the
	 * replied message first (saving any new chat/sender it introduces),
	 * record the link, then continue up the chain from it. Cross-chat
	 * replies save the replied message to its own table by chat kind.
	 */
	std::pair<int64_t, int64_t> key(reply_chat_id, reply_msg_id);
	if (resolved_reply_targets_.count(key) || depth >= kMaxReplyDepth) {
		emit();
		return;
	}

	send_query(td_api::make_object<td_api::getMessage>(reply_chat_id,
							   reply_msg_id),
		[this, emit, reply_chat_id, reply_msg_id, depth](Object obj) {
			if (obj->get_id() != td_api::message::ID) {
				/* Replied message unavailable; record anyway. */
				emit();
				return;
			}
			auto r = td::move_tl_object_as<td_api::message>(obj);
			mark_reply_resolved(reply_chat_id, reply_msg_id);
			ensure_message_entities(*r);
			if (is_private_chat(reply_chat_id))
				handle_message_for_private_chat(*r, depth + 1);
			else
				handle_message_for_group_chat(*r, depth + 1);
			emit();
		});
}

/*
 * Re-fetch a message whose content may have changed, and re-run it through the
 * normal message path so the upsert snapshots the previous version.
 *
 * `mirror` says whether to also push the edit to the Discord bridge. The
 * updateMessageEdited path does; the content-update fallback does not, because
 * it fires for changes that are not edits (a link preview resolving) and
 * re-mirroring those would edit the Discord message for no reason.
 */
void TDLib::Impl::refetch_edited_message(int64_t chat_id, int64_t message_id,
					 bool mirror)
{
	auto key = std::make_pair(chat_id, message_id);

	/*
	 * One query per message at a time. A single edit routinely produces
	 * both an updateMessageEdited and an updateMessageContent, and a user
	 * correcting themselves produces several in a row; without this each
	 * of them raced the others to answer the same question.
	 */
	auto it = refetch_inflight_.find(key);
	if (it != refetch_inflight_.end()) {
		it->second.rearm = true;
		it->second.rearm_mirror = it->second.rearm_mirror || mirror;
		return;
	}
	refetch_inflight_[key] = InflightRefetch{};

	send_query(td_api::make_object<td_api::getMessage>(chat_id, message_id),
		   [this, key, mirror](Object obj) {
			   /*
			    * Clear the slot before doing anything else, and on
			    * every path including the error one -- a query that
			    * fails must not wedge the message permanently.
			    */
			   bool rearm = false;
			   bool rearm_mirror = false;
			   auto slot = refetch_inflight_.find(key);
			   if (slot != refetch_inflight_.end()) {
				   rearm = slot->second.rearm;
				   rearm_mirror = slot->second.rearm_mirror;
				   refetch_inflight_.erase(slot);
			   }

			   if (obj->get_id() == td_api::message::ID) {
				   auto msg = td::move_tl_object_as<td_api::message>(obj);
				   if (is_private_chat(msg->chat_id_))
					   handle_message_for_private_chat(*msg, 0);
				   else
					   handle_message_for_group_chat(*msg, 0);
				   if (mirror && edit_forward_handler_) {
					   ForwardMessage fm;
					   build_forward_message(*msg, fm);
					   edit_forward_handler_(fm);
				   }
			   }

			   /* Something changed while we were asking; ask again. */
			   if (rearm)
				   refetch_edited_message(key.first, key.second,
							  rearm_mirror);
		   });
}

/*
 * Content kinds whose changes are the SERVER mutating a message rather than
 * anyone editing it: a poll's vote counts change with every vote and carry
 * no edit_date, so recording them as versions would append a row per vote.
 *
 * Locations are not in the list: a live location's positions are the
 * history of the message, and the database keeps one version of a location
 * per minute at most (DB::locationVersionTooSoon), so a moving position
 * neither floods the edits table nor is lost.
 *
 * The messageExpired* kinds are the server destroying TTL/view-once media
 * after the fact. That is not an edit either, and treating it as one would
 * overwrite the archived photo/video with 'unknown' content -- the exact
 * opposite of how deletions are handled, which preserve what was seen.
 */
bool is_server_churn_content(const td_api::MessageContent &c)
{
	switch (c.get_id()) {
	case td_api::messagePoll::ID:
	case td_api::messageExpiredPhoto::ID:
	case td_api::messageExpiredVideo::ID:
	case td_api::messageExpiredVideoNote::ID:
	case td_api::messageExpiredVoiceNote::ID:
		return true;
	default:
		return false;
	}
}

void TDLib::Impl::handle_update_message_content(int64_t chat_id,
					       int64_t message_id,
					       const td_api::MessageContent *content)
{
	if (!content)
		return;

	/*
	 * This is how a LATE-RESOLVED LINK PREVIEW arrives.
	 *
	 * Telegram does not always have the preview ready when it delivers the
	 * message. It sends the text first and resolves the URL on its own
	 * servers afterwards, then pushes the completed card as a content
	 * update. So a preview captured only at updateNewMessage time misses
	 * exactly the links that were slow to resolve -- which correlates with
	 * the pages worth archiving, since a big page is a slow one.
	 *
	 * Storing it is an upsert keyed on the message, so the resolved card
	 * simply replaces the empty or partial one, and the image goes down the
	 * same download path it would have taken had it arrived on time. If the
	 * message row does not exist yet the upsert's subquery matches nothing
	 * and writes nothing, which is the right answer rather than an error.
	 *
	 * message_link_preview() returns nullptr for anything that is not a
	 * text message with a preview, so this is a no-op for the rest.
	 */
	maybe_store_link_preview(chat_id, message_id, *content,
				 !is_private_chat(chat_id));

	/*
	 * The preview's own media come with it. A new message has them
	 * from maybe_download_message_file (they are among the message's
	 * further files); a preview that resolves late, or changes with an
	 * edit, has them from here, each overwriting its (role, position).
	 */
	if (const td_api::linkPreview *lp = message_link_preview(*content);
	    lp && message_file_handler_)
		for (const auto &x : link_preview_media_files(*lp)) {
			PendingMsgFile ref{ chat_id, to_server_msg_id(message_id),
					    !is_private_chat(chat_id),
					    x.category, x.file_name };
			ref.role = x.role;
			ref.position = x.position;
			ingest_message_file(std::move(ref), *x.file);
		}

	/*
	 * ...and this is where a version of the message is actually recorded,
	 * from the content in hand.
	 *
	 * This update is the only place the daemon is TOLD what a message now
	 * says. The other one, updateMessageEdited, carries the date and not
	 * the content, so it has to ask -- and the answer describes the message
	 * whenever the server gets to the question, not when the edit happened.
	 * Edit twice quickly and the first question is answered with the second
	 * version; edit back to the original and the answer matches what is
	 * already stored, so it is read as "nothing changed" and the version in
	 * between is lost with nothing to show it existed. Content that arrives
	 * inline cannot go stale that way, because it IS the version.
	 *
	 * Skipped for the kinds that churn without anyone editing them, where
	 * this would append a version per vote.
	 */
	if (is_server_churn_content(*content))
		return;

	if (content_version_handler_) {
		models::MessageContent mc;
		extract_message_content(*content, mc);
		/*
		 * The stored row is keyed by the SERVER message id, not the
		 * TDLib one this update carries; forgetting the conversion
		 * makes the lookup miss every message and the whole path a
		 * silent no-op.
		 */
		content_version_handler_(chat_id, to_server_msg_id(message_id),
					 mc, !is_private_chat(chat_id));
	}
}

void TDLib::Impl::handle_delete_messages(int64_t chat_id,
					 const td_api::array<td_api::int53> &message_ids,
					 bool is_permanent)
{
	/*
	 * Only permanent, user-initiated deletions are logged. TDLib also fires
	 * updateDeleteMessages when messages merely leave its local cache
	 * (is_permanent = false / from_cache = true, e.g. on chat close, cache
	 * trimming or restart); those messages still exist on the server, so
	 * treating them as deletions wrongly tombstones live messages.
	 */
	if (!is_permanent)
		return;

	/* Route deletions to the same table the messages were stored in. */
	bool priv = is_private_chat(chat_id);
	if (priv && !private_msg_handler_)
		return;
	if (!priv && !group_msg_handler_)
		return;

	for (auto msg_id : message_ids) {
		/*
		 * A local message going away (its temporary id is retired
		 * when a sent message becomes real) deletes nothing on the
		 * server. Its raw id must not be taken for a server id: a
		 * small one would tombstone the real message of that number.
		 */
		if (!is_server_msg_id(msg_id))
			continue;
		int64_t sid = to_server_msg_id(msg_id);
		if (priv) {
			models::PrivateMessage pm;
			pm.chat_id = chat_id;
			pm.message_id = sid;
			pm.is_deleted = true;
			private_msg_handler_(pm);
		} else {
			models::GroupMessage gm;
			gm.chat_id = chat_id;
			gm.message_id = sid;
			gm.is_deleted = true;
			group_msg_handler_(gm);
		}
		/* Tombstone the forwarded Discord message too. */
		if (delete_forward_handler_)
			delete_forward_handler_(chat_id, sid);
	}
}

void TDLib::Impl::build_private_message(const td_api::message &message,
					models::PrivateMessage &out)
{
	out.chat_id = message.chat_id_;
	out.message_id = to_server_msg_id(message.id_);
	out.is_outgoing = message.is_outgoing_;
	out.date = message.date_;
	out.edit_date = message.edit_date_;
	out.is_deleted = false;
	if (message.media_album_id_ != 0)
		out.media_album_id = message.media_album_id_;

	/*
	 * Resolve the sender. Messages sent by the logged-in account are
	 * recorded with a NULL sender_id, as the telegram_private_messages schema
	 * prescribes. Incoming private-chat messages are always sent by the
	 * peer user; a messageSenderChat is not expected here and is ignored
	 * (its chat id is not a valid telegram_users.id).
	 */
	if (!message.is_outgoing_ && message.sender_id_ &&
	    message.sender_id_->get_id() == td_api::messageSenderUser::ID) {
		auto &s = static_cast<const td_api::messageSenderUser &>(
			*message.sender_id_);
		out.sender_id = s.user_id_;
	}

	extract_message_content(message, out.content);
	out.forward_info = extract_forward_info(message);
	out.meta = message_meta_json(message);
	out.is_pinned = message.is_pinned_;
	out.interaction = extract_interaction(message.interaction_info_.get());
}

void TDLib::Impl::build_group_message(const td_api::message &message,
				      models::GroupMessage &out)
{
	out.chat_id = message.chat_id_;
	out.message_id = to_server_msg_id(message.id_);
	out.is_channel_post = message.is_channel_post_;
	out.date = message.date_;
	out.edit_date = message.edit_date_;
	out.is_deleted = false;
	if (message.media_album_id_ != 0)
		out.media_album_id = message.media_album_id_;

	if (!message.author_signature_.empty())
		out.author_signature = message.author_signature_;

	/*
	 * A group message's sender may be a user or a chat/channel (channel
	 * posts, anonymous admins). Record whichever applies; the schema
	 * keeps them in separate foreign-key columns. The own account's sender
	 * is recorded like any other participant -- a group is public, so
	 * "outgoing" is not tracked.
	 */
	if (message.sender_id_) {
		if (message.sender_id_->get_id() ==
		    td_api::messageSenderUser::ID) {
			auto &s = static_cast<const td_api::messageSenderUser &>(
				*message.sender_id_);
			out.sender_user_id = s.user_id_;
		} else if (message.sender_id_->get_id() ==
			   td_api::messageSenderChat::ID) {
			auto &s = static_cast<const td_api::messageSenderChat &>(
				*message.sender_id_);
			out.sender_chat_id = s.chat_id_;
		}
	}

	extract_message_content(message, out.content);
	out.forward_info = extract_forward_info(message);
	out.meta = message_meta_json(message);
	out.is_pinned = message.is_pinned_;
	out.interaction = extract_interaction(message.interaction_info_.get());

	std::string topic_kind;
	int64_t topic_id = 0;
	if (message_topic(message.topic_id_.get(), topic_kind, topic_id)) {
		out.topic_kind = std::move(topic_kind);
		out.topic_id = topic_id;
	}
}

void TDLib::Impl::resolve_forward_origin(const models::ForwardInfo &info)
{
	/*
	 * A forwarded message may reference an entity we have not stored
	 * yet: the original sender (a user) or the original chat/channel.
	 * Memorize it so the *_message_fwd_info references point at real
	 * users/groups rows. Known entities are re-emitted (idempotent, and
	 * it satisfies the origin_sender_user_id foreign key before the
	 * message row is written); unknown ones are fetched from TDLib.
	 */
	if (info.origin_sender_user_id.has_value())
		ensure_user_saved(*info.origin_sender_user_id);
	if (info.origin_chat_id.has_value())
		ensure_chat_saved(*info.origin_chat_id);
}

void TDLib::Impl::ensure_user_saved(int64_t user_id)
{
	if (user_id == 0 || !user_handler_)
		return;

	auto it = users_.find(user_id);
	if (it != users_.end() && it->second) {
		/*
		 * Already known: persist synchronously so the forward-info FK
		 * to telegram_users.id resolves before the message row is written.
		 */
		user_handler_(map_user(*it->second));
		return;
	}

	/* Unknown: fetch it, then persist and cache when it arrives. */
	send_query(td_api::make_object<td_api::getUser>(user_id),
		[this](Object obj) {
			if (obj->get_id() != td_api::user::ID)
				return;
			auto u = td::move_tl_object_as<td_api::user>(obj);
			if (user_handler_)
				user_handler_(map_user(*u));
			maybe_download_profile_photo(*u);
			users_[u->id_] = std::move(u);
		});
}

void TDLib::Impl::ensure_chat_saved(int64_t chat_id)
{
	if (chat_id == 0 || !group_handler_)
		return;

	/*
	 * Only group, supergroup and channel chats (negative ids) map to a
	 * groups row; a private chat's peer is handled via the user path.
	 */
	if (is_private_chat(chat_id))
		return;

	auto it = chat_to_group_.find(chat_id);
	if (it != chat_to_group_.end()) {
		/* Already known: re-emit so the group is persisted. */
		emit_group(it->second);
		return;
	}

	/* Unknown: fetch the chat; handle_new_chat persists it. */
	send_query(td_api::make_object<td_api::getChat>(chat_id),
		[this](Object obj) {
			if (obj->get_id() != td_api::chat::ID)
				return;
			auto c = td::move_tl_object_as<td_api::chat>(obj);
			handle_new_chat(*c, false);
		});
}

/*
 * Record a message's link preview, and queue its image for download.
 *
 * Called after the message row exists, because the preview row keys off it.
 * The image goes through exactly the same pipeline as an attachment --
 * downloadFile, hash, content-address into telegram_files -- with one bit set
 * so the completion links it to the preview instead of to the message.
 */
void TDLib::Impl::maybe_store_link_preview(int64_t chat_id,
					   int64_t tdlib_msg_id,
					   const td_api::MessageContent &content,
					   bool is_group)
{
	const td_api::linkPreview *lp = message_link_preview(content);
	if (!lp)
		return;

	const char *type_name = "unsupported";
	const td_api::file *img = link_preview_image(lp->type_.get(), &type_name);

	if (link_preview_handler_) {
		models::LinkPreview m;
		m.chat_id = chat_id;
		m.message_id = to_server_msg_id(tdlib_msg_id);
		m.url = lp->url_;
		m.display_url = lp->display_url_;
		m.site_name = lp->site_name_;
		m.title = lp->title_;
		if (lp->description_) {
			m.description = lp->description_->text_;
			m.description_entities =
				formatted_text_entities_json(*lp->description_);
		}
		m.show_media_above_description =
			lp->show_media_above_description_;
		m.skip_confirmation = lp->skip_confirmation_;
		m.author = lp->author_;
		m.type = type_name;
		m.instant_view_version = lp->instant_view_version_;
		m.has_large_media = lp->has_large_media_;
		m.show_large_media = lp->show_large_media_;
		m.show_above_text = lp->show_above_text_;
		m.extra = link_preview_extra_json(*lp);
		link_preview_handler_(m, is_group);
	}

	if (!img || !message_file_handler_)
		return;

	PendingMsgFile ref{ chat_id, to_server_msg_id(tdlib_msg_id),
			    is_group, "photo", std::string() };
	ref.is_link_preview = true;

	const bool no_download = record_without_download(chat_id, *img);

	/* Already known: link the existing row instead of fetching again --
	 * unless only its metadata is known and this chat stores bytes. */
	if (file_lookup_ && message_file_link_handler_ && img->remote_ &&
	    (!img->remote_->id_.empty() || !img->remote_->unique_id_.empty())) {
		auto fid = file_lookup_(img->remote_->id_,
					img->remote_->unique_id_);
		if (fid.has_value() && (!fid->meta_only || no_download)) {
			message_file_link_handler_(MessageFileLink{
				ref.chat_id, ref.message_id, ref.is_group,
				true, fid->id });
			return;
		}
	}
	if (no_download) {
		emit_file_meta(ref, *img, false);
		return;
	}
	if (img->local_ && img->local_->is_downloading_completed_) {
		emit_message_file(ref, *img);
		return;
	}
	ref.dl_query = start_ingest_download(img->id_);
	pending_message_file_.emplace(img->id_, std::move(ref));
}

/* Pages of getForumTopics read per forum and run, at most; 100 each. */
constexpr int kMaxForumTopicPages = 20;

/*
 * Read a forum's topic list, a page at a time, and store each topic as it
 * stands. Errors end the sweep: a forum the account cannot read has no
 * topics to learn.
 */
void TDLib::Impl::sweep_forum_topics(int64_t chat_id, int32_t offset_date,
				     int64_t offset_message_id,
				     int32_t offset_topic_id, int pages)
{
	send_query(td_api::make_object<td_api::getForumTopics>(
			   chat_id, std::string(), offset_date,
			   offset_message_id, offset_topic_id, 100),
		[this, chat_id, pages](Object obj) {
			if (!obj || obj->get_id() != td_api::forumTopics::ID)
				return;
			auto &ft = static_cast<const td_api::forumTopics &>(*obj);
			for (const auto &t : ft.topics_) {
				if (!t || !t->info_)
					continue;
				forum_topic_handler_(extract_forum_topic(*t->info_),
						     false);
				if (forum_topic_pinned_handler_)
					forum_topic_pinned_handler_(
						chat_id, t->info_->forum_topic_id_,
						t->is_pinned_);
			}
			if (ft.topics_.empty() || pages + 1 >= kMaxForumTopicPages ||
			    (ft.next_offset_date_ == 0 &&
			     ft.next_offset_message_id_ == 0 &&
			     ft.next_offset_forum_topic_id_ == 0))
				return;
			sweep_forum_topics(chat_id, ft.next_offset_date_,
					   ft.next_offset_message_id_,
					   ft.next_offset_forum_topic_id_,
					   pages + 1);
		});
}

/*
 * Remember the latest interaction totals of a message until the next flush.
 * updateMessageReactions carries the reactions alone and only replaces
 * those; updateMessageInteractionInfo carries everything.
 */
void TDLib::Impl::note_interaction(int64_t chat_id, int64_t td_message_id,
				   const models::Interaction &ia,
				   bool reactions_only)
{
	if (!interaction_handler_ || interaction_flush_secs_ <= 0 ||
	    !is_server_msg_id(td_message_id))
		return;
	auto key = std::make_pair(chat_id, to_server_msg_id(td_message_id));
	++interaction_updates_;
	interaction_seen_.insert(key);
	auto &p = interaction_pending_[key];
	p.is_group = !is_private_chat(chat_id);
	if (reactions_only)
		p.ia.reactions = ia.reactions;
	else
		p.ia = ia;
}

/* Seconds between the reports of how busy interaction updates are. */
constexpr int kInteractionReportSecs = 600;

void TDLib::Impl::drain_interaction(void)
{
	const time_t now = std::time(nullptr);
	if (interaction_report_at_ == 0)
		interaction_report_at_ = now;
	if (now - interaction_report_at_ >= kInteractionReportSecs) {
		/* The measurement the flush interval is chosen from. */
		pr_info(l_, "interaction: %zu update(s) for %zu message(s) in"
			" the last %lld s, %zu write(s) at a %d s interval",
			interaction_updates_, interaction_seen_.size(),
			(long long)(now - interaction_report_at_),
			interaction_writes_, interaction_flush_secs_);
		interaction_report_at_ = now;
		interaction_updates_ = 0;
		interaction_writes_ = 0;
		interaction_seen_.clear();
	}

	if (interaction_pending_.empty() ||
	    now - interaction_last_flush_ < interaction_flush_secs_)
		return;
	interaction_last_flush_ = now;

	std::vector<InteractionUpdate> batch;
	batch.reserve(interaction_pending_.size());
	for (auto &kv : interaction_pending_)
		batch.push_back(InteractionUpdate{ kv.first.first,
			kv.first.second, kv.second.is_group,
			std::move(kv.second.ia) });
	interaction_pending_.clear();
	interaction_writes_ += batch.size();
	interaction_handler_(std::move(batch));
}

/*
 * Pass on a group's member count (0 = not reported) and whether the account
 * may list its members, unless they are what was last passed on for it.
 */
void TDLib::Impl::note_group_member_info(int64_t chat_id, int64_t count,
					 std::optional<bool> can_get_members)
{
	if (!group_member_info_handler_ || chat_id == 0)
		return;
	auto &last = group_member_info_last_[chat_id];
	const int can = can_get_members ? (*can_get_members ? 1 : 0) : -1;
	if ((count <= 0 || count == last.first) &&
	    (can < 0 || can == last.second))
		return;
	if (count > 0)
		last.first = count;
	if (can >= 0)
		last.second = can;
	group_member_info_handler_(chat_id,
		count > 0 ? std::optional<int64_t>(count) : std::nullopt,
		can_get_members);
}

/* Store the poll a message shows, as the message has it now. */
void TDLib::Impl::maybe_store_poll(const td_api::message &message)
{
	if (!poll_handler_ || !message.content_ ||
	    message.content_->get_id() != td_api::messagePoll::ID)
		return;
	auto &m = static_cast<const td_api::messagePoll &>(*message.content_);
	if (m.poll_)
		poll_handler_(extract_poll(m));
}

/*
 * A logged message or a chat's full info names community @community_id
 * (0: none). Make sure its row is stored, re-emitting the last
 * updateCommunity, and hand it to the receiver once per run, which asks
 * for its full info when the stored row has no chats yet. An
 * inaccessible community is skipped: TDLib takes its id in no method.
 */
void TDLib::Impl::note_community_named(int64_t community_id)
{
	if (community_id == 0 || !community_named_handler_)
		return;
	if (!community_checked_.insert(community_id).second)
		return;
	auto it = communities_.find(community_id);
	if (it != communities_.end()) {
		if (community_handler_)
			community_handler_(it->second);
		if (!it->second.have_access)
			return;
	}
	community_named_handler_(community_id);
}

/* Thread-safe: called from the serial DB worker. */
void TDLib::Impl::enqueue_community_full_info(int64_t community_id)
{
	if (community_id == 0)
		return;
	std::lock_guard<std::mutex> lk(refetch_mtx_);
	if (community_fetch_queued_.insert(community_id).second)
		community_fetch_pending_.push_back(community_id);
}

/*
 * getCommunityFullInfo is paced by community_pacer_ (CommunityPacer.hpp):
 * one at a time, a few a minute for the whole account, none during a
 * flood wait. A backfill can name many communities at once; each is
 * asked for once per run when it succeeds, and not again within
 * kCommunityRetrySecs after it failed for another reason than a flood
 * wait, which instead holds every ask back and retries it.
 */
constexpr time_t kCommunityRetrySecs = 3600;

/* At most this many getFullRichMessage calls per second, and a failed one
 * is not asked again for this long. */
constexpr int kRichFullPerSecond = 2;
constexpr time_t kRichFullRetrySecs = 3600;

void TDLib::Impl::drain_community_full_info(void)
{
	time_t now = std::time(nullptr);
	while (community_pacer_.mayAsk(now)) {
		int64_t id;
		{
			std::lock_guard<std::mutex> lk(refetch_mtx_);
			if (community_fetch_pending_.empty())
				return;
			id = community_fetch_pending_.front();
			community_fetch_pending_.pop_front();
			community_fetch_queued_.erase(id);
		}
		if (community_full_done_.count(id))
			continue;
		auto f = community_full_failed_.find(id);
		if (f != community_full_failed_.end() &&
		    now - f->second < kCommunityRetrySecs)
			continue;
		community_pacer_.asked(now);
		send_query(td_api::make_object<td_api::getCommunityFullInfo>(id),
			[this, id](Object obj) {
				if (obj->get_id() ==
				    td_api::communityFullInfo::ID) {
					community_pacer_.answered();
					emit_community_full_info(id,
						static_cast<const td_api::communityFullInfo &>(
							*obj));
					return;
				}
				std::string why = "unexpected answer";
				if (obj->get_id() == td_api::error::ID) {
					auto &e = static_cast<const td_api::error &>(
						*obj);
					if (e.code_ == 429 || e.code_ == 420) {
						const long secs =
							retryAfterSecs(
								e.message_, 60);
						community_pacer_.floodWait(
							std::time(nullptr), secs);
						pr_info(l_, "community full info:"
							" paused %lds by a flood"
							" wait (community %lld"
							" retried after it)",
							secs, (long long)id);
						std::lock_guard<std::mutex> lk(
							refetch_mtx_);
						if (community_fetch_queued_
							    .insert(id).second)
							community_fetch_pending_
								.push_front(id);
						return;
					}
					why = std::to_string(e.code_) + " " +
					      e.message_;
				}
				community_pacer_.answered();
				pr_info(l_, "community %lld: no full info (%s);"
					" not asked again for an hour",
					(long long)id, why.c_str());
				community_full_failed_[id] = std::time(nullptr);
				/* A line naming it after the hour asks again. */
				community_checked_.erase(id);
			});
	}
}

/*
 * A rich message can come without all of its blocks: richMessage.is_full
 * is false, and only getFullRichMessage returns the rest. Such a message
 * is stored as it came and queued here to be stored again in full.
 */
void TDLib::Impl::maybe_fetch_full_rich(const td_api::message &message)
{
	if (!message.content_ || !rich_message_is_partial(*message.content_) ||
	    !is_server_msg_id(message.id_))
		return;
	const auto key = std::make_pair(message.chat_id_, message.id_);
	auto f = rich_full_failed_.find(key);
	if (f != rich_full_failed_.end() &&
	    std::time(nullptr) - f->second < kRichFullRetrySecs)
		return;
	if (rich_full_seen_.insert(key).second)
		rich_full_pending_.push_back(key);
}

/*
 * Ask for each queued rich message in full, then re-read the message and
 * store it again through the normal path with the full blocks in place of
 * the partial ones: its text, summary and block files come out as for a
 * message delivered whole. A message not edited since keeps its edit
 * date, so the stored row takes the content and no edit version is
 * written. At most kRichFullPerSecond messages a second.
 */
void TDLib::Impl::drain_rich_full(void)
{
	time_t now = std::time(nullptr);
	if (now != rich_full_tick_) {
		rich_full_tick_ = now;
		rich_full_in_tick_ = 0;
	}
	while (rich_full_in_tick_ < kRichFullPerSecond &&
	       !rich_full_pending_.empty()) {
		const auto key = rich_full_pending_.front();
		rich_full_pending_.pop_front();
		rich_full_in_tick_++;

		auto fail = [this, key](const char *step, const Object &obj) {
			std::string why = "unexpected answer";
			if (obj && obj->get_id() == td_api::error::ID) {
				auto &e = static_cast<const td_api::error &>(*obj);
				why = std::to_string(e.code_) + " " + e.message_;
			}
			pr_info(l_, "rich message %lld/%lld: no full version (%s:"
				" %s); not asked again for an hour",
				(long long)key.first,
				(long long)to_server_msg_id(key.second), step,
				why.c_str());
			rich_full_failed_[key] = std::time(nullptr);
			rich_full_seen_.erase(key);
		};

		send_query(td_api::make_object<td_api::getFullRichMessage>(
				   key.first, key.second),
			[this, key, fail](Object obj) {
				if (!obj || obj->get_id() != td_api::richMessage::ID) {
					fail("getFullRichMessage", obj);
					return;
				}
				auto full = std::make_shared<
					td_api::object_ptr<td_api::richMessage>>(
					td::move_tl_object_as<td_api::richMessage>(obj));
				send_query(td_api::make_object<td_api::getMessage>(
						   key.first, key.second),
					[this, key, fail, full](Object m) {
						if (!m || m->get_id() != td_api::message::ID) {
							fail("getMessage", m);
							return;
						}
						auto msg = td::move_tl_object_as<
							td_api::message>(m);
						if (!msg->content_ ||
						    msg->content_->get_id() !=
							    td_api::messageRichMessage::ID)
							return; /* no longer rich */
						auto &c = static_cast<
							td_api::messageRichMessage &>(
							*msg->content_);
						(*full)->is_full_ = true;
						c.message_ = std::move(*full);
						if (is_private_chat(msg->chat_id_))
							handle_message_for_private_chat(*msg, 0);
						else
							handle_message_for_group_chat(*msg, 0);
					});
			});
	}
}

void TDLib::Impl::emit_community_full_info(int64_t community_id,
					   const td_api::communityFullInfo &fi)
{
	community_full_done_.insert(community_id);
	community_full_failed_.erase(community_id);
	if (community_full_info_handler_)
		community_full_info_handler_(
			community_full_info_of(community_id, fi));
}

void TDLib::Impl::maybe_download_message_file(const td_api::message &message,
					      bool is_group)
{
	if (!message_file_handler_ || !message.content_)
		return;

	const char *category = "unknown";
	std::string file_name;
	const td_api::file *f =
		message_content_file(*message.content_, &category, &file_name);
	if (f)
		ingest_message_file(PendingMsgFile{ message.chat_id_,
			to_server_msg_id(message.id_), is_group, category,
			file_name }, *f);

	for (const auto &x : message_extra_files(*message.content_)) {
		PendingMsgFile ref{ message.chat_id_,
			to_server_msg_id(message.id_), is_group, x.category,
			x.file_name };
		ref.role = x.role;
		ref.position = x.position;
		ingest_message_file(std::move(ref), *x.file);
	}
}

/*
 * Get one file of a message into the store, or at least recorded: link it
 * when it is already known, record it without its bytes when this chat does
 * not store media, else download it and remember the message for it.
 */
void TDLib::Impl::ingest_message_file(PendingMsgFile ref,
				      const td_api::file &file)
{
	const td_api::file *f = &file;

	/*
	 * Already recorded from an earlier download: link the existing files
	 * row by its remote id, else by its unique id (the same file often
	 * comes in another message under a different remote id), instead of
	 * downloading (and hashing) it again.
	 * This is what keeps the backfiller from re-fetching the whole history,
	 * and in particular from re-downloading oversized files that are only
	 * kept as metadata.
	 *
	 * A row recorded without its bytes is linked the same way, unless
	 * this chat stores its media: then the download below fills the row
	 * in (upsertFile matches it by unique id).
	 */
	const bool no_download = record_without_download(ref.chat_id, *f);
	if (file_lookup_ && message_file_link_handler_ && f->remote_ &&
	    (!f->remote_->id_.empty() || !f->remote_->unique_id_.empty())) {
		auto fid = file_lookup_(f->remote_->id_, f->remote_->unique_id_);
		if (fid.has_value() && (!fid->meta_only || no_download)) {
			MessageFileLink lk{ ref.chat_id, ref.message_id,
					    ref.is_group, ref.is_link_preview,
					    fid->id };
			lk.role = ref.role;
			lk.position = ref.position;
			message_file_link_handler_(lk);
			return;
		}
	}

	/* Not a member chat, or over the size cap: record, do not fetch. */
	if (no_download) {
		emit_file_meta(ref, *f, false);
		return;
	}

	/* Already downloaded: link it immediately. */
	if (f->local_ && f->local_->is_downloading_completed_) {
		emit_message_file(ref, *f);
		return;
	}

	/* Otherwise request the download and remember the message for it. */
	ref.dl_query = start_ingest_download(f->id_);
	pending_message_file_.emplace(f->id_, std::move(ref));
}

/*
 * Whether a chat file is recorded without downloading it
 * (storage-cache.md section 6): the chat is not a member chat and
 * TG_SKIP_NONMEMBER_MEDIA is on, or the file's size is known and at least
 * TG_MAX_STORE_FILE_SIZE, which would only be downloaded to be hashed. A
 * size of 0 is unknown; the store then applies the cap after the download.
 * A file without a remote id cannot be recorded, so it is downloaded.
 */
bool TDLib::Impl::record_without_download(int64_t chat_id,
					  const td_api::file &f)
{
	if (!file_meta_handler_ || !f.remote_ || f.remote_->id_.empty())
		return false;
	if (skip_nonmember_media_ && !is_member_chat(chat_id))
		return true;
	return max_store_file_size_ != 0 && f.size_ > 0 &&
	       (uint64_t)f.size_ >= max_store_file_size_;
}

void TDLib::Impl::emit_file_meta(const PendingMsgFile &ref,
				 const td_api::file &f, bool is_group_photo)
{
	if (!file_meta_handler_ || !f.remote_)
		return;

	FileMeta m;
	m.chat_id = ref.chat_id;
	m.message_id = ref.message_id;
	m.is_group = ref.is_group;
	m.is_link_preview = ref.is_link_preview;
	m.is_group_photo = is_group_photo;
	m.tg_file_id = f.remote_->id_;
	m.tg_unique_id = f.remote_->unique_id_;
	m.file_size = f.size_ > 0 ? f.size_ : 0;
	m.content_type = ref.content_type;
	m.orig_file_name = ref.orig_file_name;
	m.role = ref.role;
	m.position = ref.position;
	file_meta_handler_(m);
}

void TDLib::Impl::emit_message_file(const PendingMsgFile &ref,
				    const td_api::file &f)
{
	if (!message_file_handler_ || !f.local_ ||
	    !f.local_->is_downloading_completed_)
		return;

	MessageFile mf;
	mf.chat_id = ref.chat_id;
	mf.message_id = ref.message_id;
	mf.is_group = ref.is_group;
	mf.is_link_preview = ref.is_link_preview;
	mf.local_path = f.local_->path_;
	mf.tg_file_id = f.remote_ ? f.remote_->id_ : std::string();
	mf.tg_unique_id = f.remote_ ? f.remote_->unique_id_ : std::string();
	mf.tg_local_file_id = f.id_;
	mf.file_size = f.size_;
	mf.content_type = ref.content_type;
	mf.orig_file_name = ref.orig_file_name;
	mf.role = ref.role;
	mf.position = ref.position;
	message_file_handler_(mf);
}

void TDLib::Impl::request_user_full_info(int64_t user_id)
{
	if (!user_full_info_handler_)
		return;

	send_query(td_api::make_object<td_api::getUserFullInfo>(user_id),
		[this, user_id](Object obj) {
			if (obj->get_id() != td_api::userFullInfo::ID)
				return;
			auto fi = td::move_tl_object_as<td_api::userFullInfo>(
				obj);
			user_full_info_handler_(
				map_user_full_info(*fi, user_id));
			note_photo_video(user_id, false, fi->photo_.get());
		});
}

/* Thread-safe: called from the serial DB worker (see the message handlers). */
void TDLib::Impl::enqueue_refetch_user(int64_t user_id)
{
	if (user_id == 0)
		return;
	std::lock_guard<std::mutex> lk(refetch_mtx_);
	refetch_users_pending_.insert(user_id);
}

void TDLib::Impl::enqueue_refetch_group(int64_t group_id)
{
	if (group_id == 0)
		return;
	std::lock_guard<std::mutex> lk(refetch_mtx_);
	refetch_groups_pending_.insert(group_id);
}

/*
 * Drain the pending refetch sets on the loop thread (called once per loop()).
 * A per-entity cooldown keeps a burst of crossings (e.g. during backfill) from
 * hammering the API; the refetch itself re-runs the normal fetch paths, whose
 * upserts record any changed fields as history.
 */
/*
 * At most this many relink and repair re-fetches per second, together.
 * Each one is a getMessage plus, for media not seen before, a download;
 * TDLib batches the former per chat, but a burst of thousands still invites
 * FLOOD_WAIT.
 */
constexpr int kRelinkPerSecond = 10;

void TDLib::Impl::drain_relink(void)
{
	time_t now = std::time(nullptr);
	if (now != relink_tick_) {
		relink_tick_ = now;
		relink_in_tick_ = 0;
		recheck_in_tick_ = 0;
		repair_in_tick_ = 0;
	}
	while (relink_in_tick_ < kRelinkPerSecond) {
		std::pair<int64_t, int64_t> m;
		{
			std::lock_guard<std::mutex> lk(refetch_mtx_);
			if (relink_pending_.empty())
				break;
			m = relink_pending_.front();
			relink_pending_.pop_front();
		}
		/* TDLib addresses a server message as id << 20. */
		refetch_edited_message(m.first, m.second << 20, false);
		relink_in_tick_++;
		if (++relink_sent_ % 1000 == 0 || relink_sent_ == relink_total_)
			std::cerr << "relink: re-fetched " << relink_sent_
				  << " of " << relink_total_
				  << " message(s) missing their media\n";
	}
	/* Chat rechecks, from the same budget, ahead of the repair; the
	 * repair waits for them altogether (recheck::repairMayRun). */
	drain_recheck();
	{
		std::lock_guard<std::mutex> lk(refetch_mtx_);
		if (repair_pending_.empty())
			return;
		const auto now = recheck::Clock::now();
		const bool may = recheck::repairMayRun(
			recheck_q_.size(), recheck_inflight_,
			now - repair_last_sent_);
		if (may == repair_paused_) {
			repair_paused_ = !may;
			pr_info(l_, may ? "repair: resumed (%zu recheck(s) waiting)"
					: "repair: paused while %zu recheck(s) wait",
				recheck_q_.size() + (size_t)recheck_inflight_);
		}
		if (!may)
			return;
		repair_last_sent_ = now;
	}
	/*
	 * Old-row repair, after the relink queue and from what is left of the
	 * same budget. A re-fetched message is stored again through the
	 * normal path, which classifies it; one TDLib can no longer return
	 * keeps what it has. At most recheck::kRepairPerSecond a second.
	 */
	while (recheck::repairAllowance(kRelinkPerSecond, relink_in_tick_,
					repair_in_tick_) > 0) {
		std::pair<int64_t, int64_t> m;
		{
			std::lock_guard<std::mutex> lk(refetch_mtx_);
			if (repair_pending_.empty())
				return;
			m = repair_pending_.front();
			repair_pending_.pop_front();
		}
		refetch_edited_message(m.first, m.second << 20, false);
		relink_in_tick_++;
		repair_in_tick_++;
		if (++repair_sent_ % 1000 == 0 || repair_sent_ == repair_total_)
			pr_info(l_, "repair: re-fetched %zu of %zu old message(s)"
				" without their kind or detail", repair_sent_,
				repair_total_);
	}
}

/*
 * At most this many rechecks start per second, each counted against
 * kRelinkPerSecond too, and this many wait for their answer at once: a
 * getChatHistory that reaches the server is heavier than a getMessage.
 */
constexpr int kRecheckPerSecond = 2;
constexpr int kRecheckInflight = 2;

void TDLib::Impl::drain_recheck(void)
{
	if (!is_authorized_ || closing_)
		return;
	const time_t now = std::time(nullptr);
	if (now < recheck_paused_until_)
		return;
	while (relink_in_tick_ < kRelinkPerSecond &&
	       recheck_in_tick_ < kRecheckPerSecond &&
	       recheck_inflight_ < kRecheckInflight) {
		std::optional<recheck::Item> it;
		{
			std::lock_guard<std::mutex> lk(refetch_mtx_);
			/* One slot stays free for a recheck asked for:
			 * a background one starts only when none is in
			 * flight. */
			if (!recheck_more_.empty()) {
				/* A recheck under way finishes first. */
				it = recheck_more_.front();
				recheck_more_.pop_front();
			} else {
				it = recheck_q_.pop(recheck::Clock::now(),
						    recheck_inflight_ == 0);
			}
		}
		if (!it)
			return;
		relink_in_tick_++;
		recheck_in_tick_++;
		recheck_inflight_++;
		/*
		 * The stretch's shape is recheck::shapeOf's. only_local
		 * false: TDLib answers from its own database where it holds
		 * the stretch and asks Telegram for the gaps.
		 */
		recheck::Shape sh = recheck::shapeOf(it->around);
		if (it->page > 1)
			sh = recheck::Shape{ it->from_td, 0, recheck::kNewestLimit };
		recheck::Item item = *it;
		send_query(td_api::make_object<td_api::getChatHistory>(
				   it->chat_id, sh.from, sh.offset, sh.limit,
				   false),
			   [this, item](Object obj) {
				   on_recheck_page(item, std::move(obj));
			   });
	}
}

void TDLib::Impl::on_recheck_page(const recheck::Item &it, Object obj)
{
	if (recheck_inflight_ > 0)
		recheck_inflight_--;

	if (obj->get_id() == td_api::error::ID) {
		auto err = td::move_tl_object_as<td_api::error>(obj);
		/* A flood wait holds every recheck back for its time; the
		 * recheck itself is not repeated. */
		if (err->code_ == 429 || err->code_ == 420) {
			long secs = 30;
			auto pos = err->message_.find_last_of(' ');
			if (pos != std::string::npos)
				secs = std::max(1L, std::atol(err->message_.c_str() + pos + 1));
			recheck_paused_until_ = std::time(nullptr) + secs;
		}
		pr_info(l_, "recheck: chat %lld around %lld: %d %s",
			(long long)it.chat_id, (long long)it.around, err->code_,
			err->message_.c_str());
		return;
	}
	if (obj->get_id() != td_api::messages::ID)
		return;

	auto msgs = td::move_tl_object_as<td_api::messages>(obj);
	RecheckAnswer a;
	a.chat_id = it.chat_id;
	a.is_group = !is_private_chat(it.chat_id);
	a.page.around = it.around;
	a.page.cont = it.page > 1;
	a.item = it;
	a.seq = ++recheck_seq_;
	if (it.queued_at != recheck::Clock::time_point{})
		a.waited_secs = std::chrono::duration_cast<std::chrono::seconds>(
			recheck::Clock::now() - it.queued_at).count();

	/*
	 * A message found again that an earlier recheck marked deleted was
	 * not gone after all: undo that first. The handler queues the undo
	 * on the serial worker ahead of the stores below, which would
	 * otherwise replace meta and drop the flag that tells it apart.
	 */
	for (auto rit = msgs->messages_.rbegin(); rit != msgs->messages_.rend();
	     ++rit) {
		if (!*rit || (*rit)->chat_id_ != it.chat_id)
			continue;
		a.page.total++;
		if (!is_server_msg_id((*rit)->id_))
			continue;
		a.page.ids.push_back(to_server_msg_id((*rit)->id_));
		if (a.oldest_td == 0 || (*rit)->id_ < a.oldest_td)
			a.oldest_td = (*rit)->id_;
	}
	if (recheck_found_handler_ && !a.page.ids.empty())
		recheck_found_handler_(a);

	/*
	 * Oldest first through the same handlers as a live message, which
	 * store what is missing and record an edit as a new version. Not
	 * the forward handler: only updateNewMessage forwards, so nothing
	 * found late reaches Discord.
	 */
	for (auto rit = msgs->messages_.rbegin(); rit != msgs->messages_.rend();
	     ++rit) {
		if (!*rit || (*rit)->chat_id_ != it.chat_id)
			continue;
		td_api::message &m = **rit;
		if (!is_server_msg_id(m.id_))
			continue;
		if (a.is_group)
			handle_message_for_group_chat(m, 0);
		else
			handle_message_for_private_chat(m, 0);
	}
	if (recheck_handler_)
		recheck_handler_(a);
}

void TDLib::Impl::note_chat_last_message(int64_t chat_id,
					 const td_api::message *m)
{
	if (m && m->date_ > 0)
		chat_last_date_[chat_id] = m->date_;
	if (m && is_server_msg_id(m->id_))
		chat_last_id_[chat_id] = to_server_msg_id(m->id_);
}

void TDLib::Impl::queue_recheck_catchup(void)
{
	if (stopped_ || closing_)
		return;
	auto chats = recheck::catchUpChats(chat_last_date_, std::time(nullptr));
	/*
	 * Only the chats with a gap at the newest end are worth a history
	 * read, which under Telegram's flood limits takes a minute or more;
	 * telling them apart needs the database, on the serial worker.
	 */
	if (recheck_catchup_handler_) {
		std::unordered_map<int64_t, int64_t> last;
		for (int64_t c : chats) {
			auto it = chat_last_id_.find(c);
			if (it != chat_last_id_.end())
				last[c] = it->second;
		}
		pr_info(l_, "recheck: restart catch-up: %zu chat(s) with a"
			" message in the last 48 h; checking which have a gap",
			chats.size());
		recheck_catchup_handler_(std::move(chats), std::move(last));
		return;
	}
	size_t queued = 0, recent = 0, busy = 0;
	{
		std::lock_guard<std::mutex> lk(refetch_mtx_);
		const auto now = recheck::Clock::now();
		for (int64_t c : chats) {
			switch (recheck_q_.push({ c, 0, true }, now)) {
			case recheck::Admit::Queued:
				queued++;
				break;
			case recheck::Admit::Busy:
				busy++;
				break;
			default:
				recent++;
				break;
			}
		}
	}
	pr_info(l_, "recheck: restart catch-up queued %zu of %zu chat(s) with a"
		" message in the last 48 h (%zu rechecked already, %zu over"
		" the queue's bound)", queued, chats.size(), recent, busy);
}

void TDLib::Impl::drain_refetch(void)
{
	drain_relink();
	drain_community_full_info();
	drain_rich_full();

	std::unordered_set<int64_t> users, groups;
	{
		std::lock_guard<std::mutex> lk(refetch_mtx_);
		if (refetch_users_pending_.empty() &&
		    refetch_groups_pending_.empty())
			return;
		users.swap(refetch_users_pending_);
		groups.swap(refetch_groups_pending_);
	}

	time_t now = std::time(nullptr);
	int nu = 0, ng = 0;
	for (int64_t uid : users) {
		time_t &last = refetch_user_last_[uid];
		if (last != 0 && (double)(now - last) < refetch_cooldown_)
			continue;
		last = now;
		refetch_user_info(uid);
		nu++;
	}
	for (int64_t gid : groups) {
		time_t &last = refetch_group_last_[gid];
		if (last != 0 && (double)(now - last) < refetch_cooldown_)
			continue;
		last = now;
		refetch_group_info(gid);
		ng++;
	}
	if (nu || ng)
		std::cerr << "refetch: refreshing " << nu << " user(s), " << ng
			  << " group(s) after msg_count crossings\n";
}

/* Fresh fetch of a user's basic + full info (loop thread). The user/full-info
 * handlers upsert it, recording name/username/bio/photo changes as history. */
void TDLib::Impl::refetch_user_info(int64_t user_id)
{
	if (user_id == 0)
		return;

	send_query(td_api::make_object<td_api::getUser>(user_id),
		[this](Object obj) {
			if (obj->get_id() != td_api::user::ID)
				return;
			auto u = td::move_tl_object_as<td_api::user>(obj);
			if (user_handler_)
				user_handler_(map_user(*u));
			maybe_download_profile_photo(*u);
			users_[u->id_] = std::move(u);
		});
	request_user_full_info(user_id);
}

/* Fresh fetch of a group's chat + full info (loop thread). getChat ->
 * handle_new_chat re-emits the group and re-requests its full info, so the
 * group upsert records title/description/username/photo changes as history. */
void TDLib::Impl::refetch_group_info(int64_t chat_id)
{
	if (chat_id == 0)
		return;

	send_query(td_api::make_object<td_api::getChat>(chat_id),
		[this](Object obj) {
			if (obj->get_id() != td_api::chat::ID)
				return;
			auto c = td::move_tl_object_as<td_api::chat>(obj);
			handle_new_chat(*c, false);
		});
}

/*
 * Point @owner_id at the files row already recorded for photo @f, if there is
 * one, and return true; the caller then does not download it. A row recorded
 * without its bytes counts only with @meta_ok (the photo is not to be
 * downloaded anyway); otherwise the download fills it in (upsertFile matches
 * it by unique id), as for message media.
 */
bool TDLib::Impl::link_known_photo(int64_t owner_id, bool is_group,
				   const td_api::file &f, bool meta_ok)
{
	if (!file_lookup_ || !photo_link_handler_ || !f.remote_)
		return false;
	if (f.remote_->id_.empty() && f.remote_->unique_id_.empty())
		return false;

	auto known = file_lookup_(f.remote_->id_, f.remote_->unique_id_);
	if (!known.has_value() || (known->meta_only && !meta_ok))
		return false;

	photo_link_handler_(PhotoLink{ owner_id, is_group, known->id });
	++photo_links_;
	pr_debug(l_, "photo index: linked %s %lld to file %llu without a"
		 " download (%llu linked, %llu downloaded so far)",
		 is_group ? "group" : "user", (long long)owner_id,
		 (unsigned long long)known->id,
		 (unsigned long long)photo_links_,
		 (unsigned long long)photo_downloads_);
	return true;
}

void TDLib::Impl::maybe_download_profile_photo(const td_api::user &u)
{
	if (!photo_handler_ || !u.profile_photo_ || !u.profile_photo_->big_)
		return;

	const td_api::file &big = *u.profile_photo_->big_;

	/* Already downloaded: emit immediately. Storing it costs no download,
	 * and the store prunes TDLib's copy. */
	if (big.local_ && big.local_->is_downloading_completed_) {
		emit_photo(u.id_, big);
		return;
	}

	/* Already stored: link it. Profile photos are always downloaded, so
	 * a metadata-only row is filled in rather than linked. */
	if (link_known_photo(u.id_, false, big, false))
		return;

	/* Otherwise request the download and remember which user it is for. */
	++photo_downloads_;
	pending_photo_[big.id_] = u.id_;
	start_ingest_download(big.id_);
}

void TDLib::Impl::note_ingest_download(int32_t file_id)
{
	std::lock_guard<std::mutex> lk(ingest_dl_mtx_);
	ingest_dl_.insert(file_id);
}

/* Loop thread: drop @file_id from the set unless a pending map still has it. */
void TDLib::Impl::ingest_download_done(int32_t file_id)
{
	if (has_ingest_entries(file_id))
		return;
	ingest_query_.erase(file_id);
	ingest_displaced_.erase(file_id);
	std::lock_guard<std::mutex> lk(ingest_dl_mtx_);
	ingest_dl_.erase(file_id);
}

/*
 * Loop thread: download all of @file_id for a pending entry the caller
 * adds, and return the query id the entry waits on.
 *
 * The download is synchronous: TDLib answers when it completes, fails or
 * is cancelled, which is the only reliable failure signal. updateFile
 * cannot tell: TDLib reports is_downloading_active false for a download it
 * still holds but has not started (a file still loading from its
 * database), and for one a ranged downloadFile replaced and finished.
 */
std::uint64_t TDLib::Impl::start_ingest_download(int32_t file_id)
{
	note_ingest_download(file_id);
	const std::uint64_t q = next_query_id();
	ingest_query_[file_id] = q;
	ingest_displaced_.erase(file_id);
	handlers_.emplace(q, [this, file_id, q](Object o) {
		on_ingest_download_result(file_id, q, std::move(o));
	});
	client_manager_->send(client_id_, q,
		td_api::make_object<td_api::downloadFile>(file_id, 1, 0, 0,
							  true));
	return q;
}

/* Loop thread: TDLib's answer to the ingest download @query of @file_id. */
void TDLib::Impl::on_ingest_download_result(int32_t file_id,
					    std::uint64_t query, Object obj)
{
	if (!obj)
		return;
	if (obj->get_id() == td_api::file::ID) {
		const auto &f = static_cast<const td_api::file &>(*obj);
		if (f.local_ && f.local_->is_downloading_completed_) {
			/* Usually handled by its updateFile already. */
			handle_file_update(f);
			return;
		}
		/* Every byte is there: its updateFile completes it. */
		if (f.local_ && f.size_ > 0 &&
		    f.local_->downloaded_prefix_size_ >= f.size_)
			return;
		drop_failed_ingest(file_id, query,
				   "the download ended incomplete");
		return;
	}
	if (obj->get_id() != td_api::error::ID)
		return;
	const auto &e = static_cast<const td_api::error &>(*obj);

	/* Shutting down: TDLib aborts everything; nothing to report. */
	if (closing_ || stopped_)
		return;

	/*
	 * Another downloadFile for this file asked for a different range
	 * (the fetch service streaming part of it): TDLib cancelled this
	 * one, but nothing failed. Keep the entries and resume once TDLib
	 * stops downloading; getFile catches a range that is already done.
	 * If ingest has asked for the file again since, that download
	 * replaced the range in turn and serves these entries too.
	 */
	if (e.code_ == 200 &&
	    e.message_ == "Canceled by another downloadFile request") {
		auto qit = ingest_query_.find(file_id);
		if (qit == ingest_query_.end())
			return;
		if (qit->second != query) {
			auto range = pending_message_file_.equal_range(file_id);
			for (auto it = range.first; it != range.second; ++it)
				if (it->second.dl_query == query)
					it->second.dl_query = qit->second;
			return;
		}
		if (!ingest_displaced_.insert(file_id).second)
			return;
		pr_debug(l_, "ingest: download of file %d replaced by another"
			 " range; resuming it when that ends", (int)file_id);
		send_query(td_api::make_object<td_api::getFile>(file_id),
			[this](Object o) {
				if (o && o->get_id() == td_api::file::ID)
					handle_file_update(
						static_cast<const td_api::file &>(
							*o));
			});
		return;
	}

	drop_failed_ingest(file_id, query,
			   std::to_string(e.code_) + " " + e.message_);
}

/*
 * Loop thread: the ingest download @query of @file_id failed. Drop the
 * entries that waited on it, log each once, and keep ingest_dl_ in step.
 * Entries that a later download of the same file serves are left to that
 * download's own answer. Not retried: the relink sweep
 * (TG_RELINK_MEDIA_ON_START) picks such messages up.
 */
void TDLib::Impl::drop_failed_ingest(int32_t file_id, std::uint64_t query,
				     const std::string &why)
{
	auto qit = ingest_query_.find(file_id);
	const bool latest = qit != ingest_query_.end() && qit->second == query;
	bool any = false;

	if (latest) {
		auto it = pending_photo_.find(file_id);
		if (it != pending_photo_.end()) {
			pr_warn(l_, "ingest: download of file %d failed (%s):"
				" profile photo of user %lld not stored"
				" (%llu failed since start)",
				(int)file_id, why.c_str(),
				(long long)it->second,
				(unsigned long long)++ingest_failures_);
			pending_photo_.erase(it);
			any = true;
		}
		auto git = pending_group_photo_.find(file_id);
		if (git != pending_group_photo_.end()) {
			pr_warn(l_, "ingest: download of file %d failed (%s):"
				" photo of group %lld not stored"
				" (%llu failed since start)",
				(int)file_id, why.c_str(),
				(long long)git->second,
				(unsigned long long)++ingest_failures_);
			pending_group_photo_.erase(git);
			any = true;
		}
	}

	auto range = pending_message_file_.equal_range(file_id);
	for (auto it = range.first; it != range.second;) {
		if (it->second.dl_query != query) {
			++it;
			continue;
		}
		const PendingMsgFile &ref = it->second;
		pr_warn(l_, "ingest: download of file %d failed (%s): %s"
			" message %lld in chat %lld left without its %s"
			" (%llu failed since start)",
			(int)file_id, why.c_str(),
			ref.is_group ? "group" : "private",
			(long long)ref.message_id, (long long)ref.chat_id,
			ref.is_link_preview ? "link preview image"
					    : ref.content_type.c_str(),
			(unsigned long long)++ingest_failures_);
		it = pending_message_file_.erase(it);
		any = true;
	}

	if (any)
		ingest_download_done(file_id);
}

/* Loop thread: whether a pending map still waits on @file_id. */
bool TDLib::Impl::has_ingest_entries(int32_t file_id) const
{
	return pending_photo_.count(file_id) ||
	       pending_group_photo_.count(file_id) ||
	       pending_message_file_.count(file_id);
}

/*
 * Loop thread: download @file_id again for the entries whose download a
 * ranged downloadFile replaced, now that TDLib stopped downloading it.
 * Not a retry: the replaced download never failed. If the other range
 * failed instead, this download most likely fails too, and is logged.
 */
void TDLib::Impl::resume_ingest_download(int32_t file_id)
{
	pr_debug(l_, "ingest: resuming the download of file %d", (int)file_id);
	const std::uint64_t q = start_ingest_download(file_id);
	auto range = pending_message_file_.equal_range(file_id);
	for (auto it = range.first; it != range.second; ++it)
		it->second.dl_query = q;
}

void TDLib::Impl::handle_file_update(const td_api::file &f)
{
	if (!f.local_)
		return;
	if (!f.local_->is_downloading_completed_) {
		/* A replaced ingest download: the other range is done. */
		if (!f.local_->is_downloading_active_ &&
		    ingest_displaced_.count(f.id_))
			resume_ingest_download(f.id_);
		return;
	}

	/*
	 * Bracket the emits below with the batch hooks, so a store job that
	 * ends before the next emit cannot let go of the last hold on the
	 * file and get it pruned under the jobs still to be queued. Declared
	 * before ingest_sync, so the batch ends after the ingest bookkeeping.
	 */
	struct EmitBatch {
		Impl	*impl;
		int32_t	id;
		bool	on;
		~EmitBatch()
		{
			if (on && impl->file_batch_end_)
				impl->file_batch_end_(id);
		}
	} emit_batch{ this, f.id_,
		      pending_photo_.count(f.id_) ||
		      pending_group_photo_.count(f.id_) ||
		      pending_message_file_.count(f.id_) };
	if (emit_batch.on && file_batch_begin_)
		file_batch_begin_(f.id_);

	/* Whichever branch below handles it, the set follows the maps. */
	struct IngestSync {
		Impl	*impl;
		int32_t	id;
		~IngestSync() { impl->ingest_download_done(id); }
	} ingest_sync{ this, f.id_ };

	/*
	 * One file can wait in several maps: the message that set a chat's
	 * photo carries the same file as the photo itself. Every use gets
	 * it; each emit queues its own store job, and the store holds prune
	 * TDLib's copy after the last of them (and after the batch).
	 */
	auto it = pending_photo_.find(f.id_);
	if (it != pending_photo_.end()) {
		int64_t user_id = it->second;
		pending_photo_.erase(it);
		emit_photo(user_id, f);
	}

	auto git = pending_group_photo_.find(f.id_);
	if (git != pending_group_photo_.end()) {
		int64_t group_id = git->second;
		pending_group_photo_.erase(git);
		emit_group_photo(group_id, f);
	}

	auto range = pending_message_file_.equal_range(f.id_);
	if (range.first != range.second) {
		/* Every message waiting on this file gets it. */
		std::vector<PendingMsgFile> refs;
		for (auto it = range.first; it != range.second; ++it)
			refs.push_back(it->second);
		pending_message_file_.erase(range.first, range.second);
		for (const auto &ref : refs)
			emit_message_file(ref, f);
	}
}

void TDLib::Impl::emit_photo(int64_t user_id, const td_api::file &f)
{
	if (!photo_handler_ || !f.local_ ||
	    !f.local_->is_downloading_completed_)
		return;

	ProfilePhoto p;
	p.user_id = user_id;
	p.local_path = f.local_->path_;
	p.tg_file_id = f.remote_ ? f.remote_->id_ : std::string();
	p.tg_unique_id = f.remote_ ? f.remote_->unique_id_ : std::string();
	p.tg_local_file_id = f.id_;
	p.file_size = f.size_;
	photo_handler_(p);
}

void TDLib::Impl::handle_new_chat(const td_api::chat &chat, bool from_chat_list)
{
	if (!chat.type_)
		return;

	int64_t group_id = 0;
	models::GroupType type = models::GroupType::BasicGroup;

	switch (chat.type_->get_id()) {
	case td_api::chatTypeBasicGroup::ID: {
		auto &t = static_cast<const td_api::chatTypeBasicGroup &>(
			*chat.type_);
		group_id = t.basic_group_id_;
		type = models::GroupType::BasicGroup;
		break;
	}
	case td_api::chatTypeSupergroup::ID: {
		auto &t = static_cast<const td_api::chatTypeSupergroup &>(
			*chat.type_);
		group_id = t.supergroup_id_;
		type = t.is_channel_ ? models::GroupType::Channel :
				       models::GroupType::Supergroup;
		break;
	}
	default:
		/* Private and secret chats are not telegram_groups. */
		return;
	}

	bool first_sight = group_state_.find(group_id) == group_state_.end();

	GroupState &st = group_state_[group_id];
	st.type = type;
	st.chat_id = chat.id_;
	st.title = chat.title_;
	st.chat_seen = true;
	chat_to_group_[chat.id_] = group_id;

	/* Merge usernames already received via updateSupergroup. */
	auto sg = supergroups_.find(group_id);
	if (sg != supergroups_.end()) {
		st.active_usernames = sg->second.active;
		st.disabled_usernames = sg->second.disabled;
		st.collectible_usernames = sg->second.collectible;
		if (sg->second.is_channel)
			st.type = models::GroupType::Channel;
	}

	/*
	 * Membership cached from the supergroup or basic group update, which
	 * TDLib sends before the chat. Supergroup and basic group ids are
	 * separate id spaces, so take it only from the cache of this type.
	 */
	if (type == models::GroupType::BasicGroup) {
		auto bg = basic_groups_.find(group_id);
		if (bg != basic_groups_.end())
			set_membership(st, bg->second);
	} else if (sg != supergroups_.end()) {
		set_membership(st, sg->second.member);
	}

	emit_group(group_id);

	/* Request full info so the description arrives via its update. */
	if (type == models::GroupType::BasicGroup)
		send_query(td_api::make_object<td_api::getBasicGroupFullInfo>(
				   group_id), {});
	else
		send_query(td_api::make_object<td_api::getSupergroupFullInfo>(
				   group_id), {});

	maybe_download_group_photo(chat.id_, chat.photo_.get());

	/*
	 * Track administrators only for groups in our own chat list (not for
	 * chats merely referenced by a forward/reply). On first sight, start
	 * tracking and fetch the admin list now; supergroups/channels use
	 * getSupergroupMembers, basic groups get theirs from the
	 * basicGroupFullInfo requested above (see updateBasicGroupFullInfo).
	 */
	if (from_chat_list && first_sight &&
	    admin_poll_set_.find(chat.id_) == admin_poll_set_.end()) {
		admin_poll_set_.insert(chat.id_);
		admin_poll_queue_.push_back(chat.id_);
		if (type != models::GroupType::BasicGroup)
			fetch_group_admins(group_id, chat.id_);
	}
}

void TDLib::Impl::fetch_group_admins(int64_t supergroup_id, int64_t chat_id)
{
	if (!group_admins_handler_)
		return;

	send_query(td_api::make_object<td_api::getSupergroupMembers>(
			supergroup_id,
			td_api::make_object<
				td_api::supergroupMembersFilterAdministrators>(),
			0, 200),
		[this, chat_id](Object obj) {
			/*
			 * Data-loss guard: sync only on a real member list. An
			 * error (permission denied, FLOOD_WAIT, ...) must never
			 * be treated as "no admins" — that would remove them all.
			 * On a permission error (400/403, e.g. we left the group)
			 * count a miss and stop polling it after a few; transient
			 * errors (FLOOD_WAIT, server) don't count.
			 */
			if (obj->get_id() != td_api::chatMembers::ID) {
				if (obj->get_id() == td_api::error::ID) {
					auto &e = static_cast<td_api::error &>(*obj);
					if ((e.code_ == 400 || e.code_ == 403) &&
					    ++admin_poll_miss_[chat_id] >=
						    kAdminPollMaxMiss)
						admin_poll_set_.erase(chat_id);
				}
				return;
			}
			admin_poll_miss_.erase(chat_id);
			auto members =
				td::move_tl_object_as<td_api::chatMembers>(obj);

			models::GroupAdminList list;
			list.group_id = chat_id;
			for (const auto &m : members->members_) {
				if (!m)
					continue;
				models::GroupAdmin ga;
				if (!map_admin(*m, ga))
					continue;
				ensure_user_saved(ga.user_id);
				list.admins.push_back(std::move(ga));
			}

			/* A group always has at least a creator; an empty set
			 * means nothing usable was returned, so skip. */
			if (!list.admins.empty())
				group_admins_handler_(list);
		});
}

void TDLib::Impl::emit_basic_group_admins(int64_t chat_id,
					  const td_api::basicGroupFullInfo &fi)
{
	if (!group_admins_handler_)
		return;

	models::GroupAdminList list;
	list.group_id = chat_id;
	for (const auto &m : fi.members_) {
		if (!m)
			continue;
		models::GroupAdmin ga;
		if (!map_admin(*m, ga))
			continue;
		ensure_user_saved(ga.user_id);
		list.admins.push_back(std::move(ga));
	}

	if (!list.admins.empty())
		group_admins_handler_(list);
}

void TDLib::Impl::arm_admin_alarm(void)
{
	/*
	 * setAlarm's Ok response is delivered on this (the loop) thread, so
	 * the whole poll cycle runs where all TDLib state safely lives.
	 */
	send_query(td_api::make_object<td_api::setAlarm>(admin_poll_interval_),
		   [this](Object) { on_admin_alarm(); });
}

void TDLib::Impl::on_admin_alarm(void)
{
	/* Dying: let the alarm chain end (no re-arm). */
	if (stopped_ || closing_)
		return;

	/* Do work only while usable, but ALWAYS re-arm so a transient
	 * de-auth does not permanently kill the heartbeat. Exactly one alarm
	 * is in flight at any time. */
	if (is_authorized_ && group_admins_handler_)
		poll_admin_batch();

	arm_admin_alarm();
}

void TDLib::Impl::poll_admin_batch(void)
{
	/*
	 * Refresh up to admin_poll_batch_ tracked groups, round-robin: pop
	 * from the front, re-fetch, push to the rear. Scan at most the
	 * queue's current length so freshly re-pushed groups are not polled
	 * twice in one cycle; evicted groups (not in admin_poll_set_) are
	 * dropped instead of re-queued.
	 */
	int polled = 0;
	size_t scan = admin_poll_queue_.size();
	while (polled < admin_poll_batch_ && scan > 0 &&
	       !admin_poll_queue_.empty()) {
		int64_t chat_id = admin_poll_queue_.front();
		admin_poll_queue_.pop_front();
		scan--;

		if (!admin_poll_set_.count(chat_id))
			continue;	/* evicted: drop */

		admin_poll_queue_.push_back(chat_id);

		auto cit = chat_to_group_.find(chat_id);
		if (cit == chat_to_group_.end())
			continue;
		auto git = group_state_.find(cit->second);
		if (git == group_state_.end())
			continue;

		if (git->second.type == models::GroupType::BasicGroup)
			/* Admins arrive via the resulting updateBasicGroupFullInfo. */
			send_query(td_api::make_object<
					td_api::getBasicGroupFullInfo>(
					cit->second), {});
		else
			fetch_group_admins(cit->second, chat_id);
		polled++;
	}
}

/* --- Background message backfiller ------------------------------------- */

void TDLib::Impl::arm_backfill_alarm(void)
{
	/* One alarm in flight at a time; setAlarm's reply lands on the loop
	 * thread, where all backfill state safely lives (like the admin poll). */
	double delay = backfill_next_delay_ > 0.0 ? backfill_next_delay_
						  : backfill_interval_;
	backfill_next_delay_ = 0.0;
	send_query(td_api::make_object<td_api::setAlarm>(delay),
		   [this](Object) { on_backfill_alarm(); });
}

void TDLib::Impl::on_backfill_alarm(void)
{
	if (stopped_ || closing_)
		return;
	if (is_authorized_)
		backfill_tick();
	arm_backfill_alarm();
}

void TDLib::Impl::backfill_tick(void)
{
	/* Backfill reuses the real-time message handlers, so wait for them. */
	if (!group_msg_handler_ || !private_msg_handler_)
		return;
	if (backfill_inflight_ >= backfill_inflight_max_)
		return;

	size_t n = backfill_order_.size();
	if (n == 0)
		return;

	/*
	 * Round-robin from the last position: fetch one page for the next chat
	 * that is neither finished nor already in flight, so every chat makes
	 * steady progress instead of one draining first.
	 *
	 * Two tiers: the user's own chats (in their Main/Archive list) are
	 * served first, each tier with its own cursor for fairness within it. A
	 * low-priority chat is only picked once no high-priority chat still has
	 * work, so the incidental chats wait until the user's are done.
	 */
	auto pick = [&](bool want_priority, size_t &rr) -> int64_t {
		for (size_t i = 0; i < n; i++) {
			size_t idx = (rr + i) % n;
			auto it = backfill_.find(backfill_order_[idx]);
			if (it == backfill_.end())
				continue;
			BackfillEntry &e = it->second;
			if (e.done || e.in_flight || e.priority != want_priority)
				continue;
			rr = (idx + 1) % n;
			return backfill_order_[idx];
		}
		return 0;
	};

	int64_t chat_id = pick(true, backfill_rr_);
	if (chat_id == 0)
		chat_id = pick(false, backfill_rr_low_);
	if (chat_id == 0)
		return;	/* nothing left to fetch. */

	BackfillEntry &e = backfill_[chat_id];
	e.in_flight = true;
	backfill_inflight_++;

	/* from_message_id 0 = start at the newest message; only_local false so
	 * TDLib fetches older messages from the server. */
	send_query(td_api::make_object<td_api::getChatHistory>(
			   chat_id, e.cursor_msg_id, 0, backfill_page_, false),
		[this, chat_id](Object obj) {
			on_backfill_page(chat_id, std::move(obj));
		});
}

void TDLib::Impl::on_backfill_page(int64_t chat_id, Object obj)
{
	auto it = backfill_.find(chat_id);
	if (it == backfill_.end())
		return;
	BackfillEntry &e = it->second;
	e.in_flight = false;
	if (backfill_inflight_ > 0)
		backfill_inflight_--;

	if (obj->get_id() == td_api::error::ID) {
		auto err = td::move_tl_object_as<td_api::error>(obj);
		/*
		 * Flood wait: back the whole walk off for the requested time.
		 * Any other error: leave the cursor untouched and retry the chat
		 * on a later round.
		 */
		if (err->code_ == 420) {
			double secs = 30.0;
			auto pos = err->message_.find_last_of(' ');
			if (pos != std::string::npos)
				secs = atof(err->message_.c_str() + pos + 1);
			backfill_next_delay_ = secs >= 1.0 ? secs : 30.0;
		}
		return;
	}
	if (obj->get_id() != td_api::messages::ID)
		return;

	auto msgs = td::move_tl_object_as<td_api::messages>(obj);
	if (msgs->messages_.empty()) {
		/* Usually the history start; getChatHistory can also return an
		 * empty page transiently while loading, so tolerate a few. */
		if (++e.empty_retries >= kBackfillEmptyRetries) {
			e.done = true;
			emit_backfill_state(chat_id);
		}
		return;
	}
	e.empty_retries = 0;

	/*
	 * messages_ is newest -> oldest. Process oldest-first through the exact
	 * same handlers as the real-time path (which ensure entities are saved,
	 * download media, resolve replies and persist idempotently), then
	 * advance the cursor to the oldest id so the next call fetches strictly
	 * older messages.
	 */
	for (auto rit = msgs->messages_.rbegin(); rit != msgs->messages_.rend();
	     ++rit) {
		if (!*rit)
			continue;
		td_api::message &m = **rit;
		if (is_private_chat(m.chat_id_))
			handle_message_for_private_chat(m, 0);
		else
			handle_message_for_group_chat(m, 0);
	}

	int64_t oldest = e.cursor_msg_id;
	if (msgs->messages_.back())
		oldest = msgs->messages_.back()->id_;
	e.cursor_msg_id = oldest;
	emit_backfill_state(chat_id);
}

void TDLib::Impl::arm_backfill_discovery(void)
{
	send_query(td_api::make_object<td_api::setAlarm>(
			   backfill_discovery_interval_),
		   [this](Object) { on_backfill_discovery(); });
}

void TDLib::Impl::on_backfill_discovery(void)
{
	if (stopped_ || closing_)
		return;
	if (is_authorized_)
		/*
		 * Pull more chats into the main list; each newly loaded chat
		 * arrives via updateNewChat, which registers it for backfill.
		 * Returns Ok normally and error 404 once everything is loaded --
		 * both are fine, so no handler is needed.
		 */
		send_query(td_api::make_object<td_api::loadChats>(
				   td_api::make_object<td_api::chatListMain>(),
				   500), {});
	arm_backfill_discovery();
}

void TDLib::Impl::backfill_register_chat(int64_t chat_id, bool priority)
{
	if (!backfill_enabled_ || chat_id == 0)
		return;

	auto it = backfill_.find(chat_id);
	if (it != backfill_.end()) {
		/* Already known (from the DB or a prior sighting): keep its
		 * cursor/done, but promote it if it has since entered the user's
		 * chat list. Priority never drops back to low. */
		if (priority && !it->second.priority) {
			it->second.priority = true;
			emit_backfill_state(chat_id);
		}
		return;
	}

	BackfillEntry e{};
	e.priority = priority;
	backfill_[chat_id] = e;
	backfill_order_.push_back(chat_id);
	emit_backfill_state(chat_id);
}

void TDLib::Impl::emit_backfill_state(int64_t chat_id)
{
	if (!backfill_state_handler_)
		return;
	auto it = backfill_.find(chat_id);
	if (it == backfill_.end())
		return;

	const BackfillEntry &e = it->second;
	models::BackfillState st;
	st.chat_id = chat_id;
	st.scope = is_private_chat(chat_id) ? "private" : "group";
	if (e.cursor_msg_id != 0)
		st.cursor_msg_id = e.cursor_msg_id;
	st.done = e.done;
	st.priority = e.priority;
	backfill_state_handler_(st);
}

/* Record a group chat's member flag, logging when it changes. */
void TDLib::Impl::set_membership(GroupState &st, Membership m)
{
	if (st.member == m)
		return;
	pr_debug(l_, "membership: chat %lld: %s -> %s",
		 (long long)st.chat_id, membership_name(st.member),
		 membership_name(m));
	st.member = m;
}

/*
 * Apply a supergroup or basic group status update to its chat, if the chat
 * has been seen; otherwise handle_new_chat() picks it up from the cache.
 */
void TDLib::Impl::update_group_membership(int64_t group_id, bool is_basic,
					  Membership m)
{
	auto it = group_state_.find(group_id);
	if (it == group_state_.end() || !it->second.chat_seen)
		return;
	/* Supergroup and basic group ids share group_state_'s key space. */
	if ((it->second.type == models::GroupType::BasicGroup) != is_basic)
		return;
	set_membership(it->second, m);
}

/*
 * Whether the account is a member of a chat (storage-cache.md section 5).
 * Private chats always are; a group chat is unless its status says
 * otherwise, so a chat not seen yet or without a status counts as one.
 * Loop thread only.
 */
bool TDLib::Impl::is_member_chat(int64_t chat_id) const
{
	if (is_private_chat(chat_id))
		return true;

	auto cit = chat_to_group_.find(chat_id);
	if (cit == chat_to_group_.end())
		return true;
	auto git = group_state_.find(cit->second);
	if (git == group_state_.end() || git->second.chat_id != chat_id)
		return true;
	return git->second.member != Membership::NotMember;
}

void TDLib::Impl::maybe_download_group_photo(int64_t group_id,
					     const td_api::chatPhotoInfo *photo)
{
	if (!group_photo_handler_ || !photo || !photo->big_)
		return;

	const td_api::file &big = *photo->big_;
	const bool no_download = record_without_download(group_id, big);
	if (!no_download && big.local_ &&
	    big.local_->is_downloading_completed_) {
		emit_group_photo(group_id, big);
		return;
	}
	/* Already recorded: link it, unless only its metadata is known and
	 * this chat stores its photo (then the download fills the row in). */
	if (link_known_photo(group_id, true, big, no_download))
		return;
	if (no_download) {
		PendingMsgFile ref{ group_id, 0, true, "photo", std::string() };
		emit_file_meta(ref, big, true);
		return;
	}

	++photo_downloads_;
	pending_group_photo_[big.id_] = group_id;
	start_ingest_download(big.id_);
}

void TDLib::Impl::emit_group(int64_t group_id)
{
	auto it = group_state_.find(group_id);
	if (it == group_state_.end() || !it->second.chat_seen ||
	    !group_handler_)
		return;

	const GroupState &st = it->second;
	models::Group g;
	g.id = st.chat_id;
	g.type = st.type;
	g.title = st.title;
	g.description = st.description;
	g.active_usernames = st.active_usernames;
	g.disabled_usernames = st.disabled_usernames;
	g.collectible_usernames = st.collectible_usernames;
	group_handler_(g);
}

void TDLib::Impl::emit_group_photo(int64_t group_id, const td_api::file &f)
{
	if (!group_photo_handler_ || !f.local_ ||
	    !f.local_->is_downloading_completed_)
		return;

	GroupPhoto p;
	p.group_id = group_id;
	p.local_path = f.local_->path_;
	p.tg_file_id = f.remote_ ? f.remote_->id_ : std::string();
	p.tg_unique_id = f.remote_ ? f.remote_->unique_id_ : std::string();
	p.tg_local_file_id = f.id_;
	p.file_size = f.size_;
	group_photo_handler_(p);
}


/*
 * TDLib::resolveFile. Runs on the caller's thread, never the loop thread: it
 * touches no loop-thread state, only request_wait() and the extraction
 * helpers ingest uses, so a refetched source yields the same file ingest
 * would have taken from it.
 */
Object TDLib::Impl::resolve_file(const models::FileSource &src,
				 std::chrono::milliseconds timeout)
{
	using Kind = models::FileSource::Kind;
	using std::chrono::steady_clock;
	const auto deadline = steady_clock::now() + timeout;
	auto left = [deadline] {
		auto d = std::chrono::duration_cast<std::chrono::milliseconds>(
			deadline - steady_clock::now());
		return d.count() > 0 ? d : std::chrono::milliseconds(0);
	};

	td_api::object_ptr<td_api::Function> q;
	int32_t want = 0;
	switch (src.kind) {
	case Kind::Message:
	case Kind::LinkPreview:
	case Kind::MessageExtra:
	case Kind::RichMedia:
		/* Stored ids are server ids; TDLib wants its own. */
		q = td_api::make_object<td_api::getMessage>(
			src.chat_id, src.message_id * kTdMsgIdShift);
		want = td_api::message::ID;
		break;
	case Kind::GroupPhoto:
		q = td_api::make_object<td_api::getChat>(src.chat_id);
		want = td_api::chat::ID;
		break;
	case Kind::UserPhoto:
		q = td_api::make_object<td_api::getUser>(src.chat_id);
		want = td_api::user::ID;
		break;
	case Kind::UserPhotoHistory:
		q = td_api::make_object<td_api::getUserProfilePhotos>(
			src.chat_id, 0, 100);
		want = td_api::chatPhotos::ID;
		break;
	case Kind::GroupPhotoHistory:
		q = td_api::make_object<td_api::searchChatMessages>(
			src.chat_id, nullptr, std::string(), nullptr, 0, 0, 100,
			td_api::make_object<td_api::searchMessagesFilterChatPhoto>());
		want = td_api::foundChatMessages::ID;
		break;
	case Kind::CustomEmoji:
		q = td_api::make_object<td_api::getCustomEmojiStickers>(
			std::vector<int64_t>{ src.object_id });
		want = td_api::stickers::ID;
		break;
	case Kind::MessageEffect:
		q = td_api::make_object<td_api::getMessageEffect>(src.object_id);
		want = td_api::messageEffect::ID;
		break;
	case Kind::Community:
		q = td_api::make_object<td_api::getCommunityFullInfo>(
			src.object_id);
		want = td_api::communityFullInfo::ID;
		break;
	}

	Object obj = request_wait(std::move(q), left());
	if (!obj || obj->get_id() != want) {
		if (obj && obj->get_id() == td_api::error::ID)
			return obj;
		return td_api::make_object<td_api::error>(
			500, "unexpected answer while resolving a file");
	}

	const td_api::file *f = nullptr;
	const char *category = "unknown";
	std::string file_name;
	switch (src.kind) {
	case Kind::Message: {
		auto &m = static_cast<const td_api::message &>(*obj);
		if (m.content_)
			f = message_content_file(*m.content_, &category,
						 &file_name);
		break;
	}
	case Kind::LinkPreview: {
		auto &m = static_cast<const td_api::message &>(*obj);
		const td_api::linkPreview *lp =
			m.content_ ? message_link_preview(*m.content_) : nullptr;
		if (lp)
			f = link_preview_image(lp->type_.get(), &category);
		break;
	}
	case Kind::MessageExtra: {
		auto &m = static_cast<const td_api::message &>(*obj);
		if (!m.content_)
			break;
		for (const auto &x : message_extra_files(*m.content_))
			if (src.role == x.role && src.position == x.position)
				f = x.file;
		break;
	}
	case Kind::RichMedia: {
		/* The medium in the blocks: matched by unique id. */
		auto &m = static_cast<const td_api::message &>(*obj);
		if (!m.content_)
			break;
		auto files = message_extra_files(*m.content_);
		if (auto *x = rich_file_with_unique_id(files, src.unique_id))
			f = x->file;
		break;
	}
	case Kind::GroupPhoto: {
		auto &c = static_cast<const td_api::chat &>(*obj);
		if (c.photo_)
			f = c.photo_->big_.get();
		break;
	}
	case Kind::UserPhoto: {
		auto &u = static_cast<const td_api::user &>(*obj);
		if (u.profile_photo_)
			f = u.profile_photo_->big_.get();
		break;
	}
	case Kind::UserPhotoHistory: {
		/* The photo whose size is the file: matched by unique id. */
		auto &list = static_cast<const td_api::chatPhotos &>(*obj);
		for (const auto &ph : list.photos_)
			if (ph)
				if (auto *x = chat_photo_file_with_unique_id(
					    *ph, src.unique_id))
					f = x;
		break;
	}
	case Kind::GroupPhotoHistory: {
		auto &found = static_cast<const td_api::foundChatMessages &>(*obj);
		for (const auto &m : found.messages_) {
			if (!m || !m->content_ ||
			    m->content_->get_id() != td_api::messageChatChangePhoto::ID)
				continue;
			auto &c = static_cast<const td_api::messageChatChangePhoto &>(
				*m->content_);
			if (c.photo_)
				if (auto *x = chat_photo_file_with_unique_id(
					    *c.photo_, src.unique_id))
					f = x;
		}
		break;
	}
	case Kind::CustomEmoji: {
		auto &list = static_cast<const td_api::stickers &>(*obj);
		for (const auto &st : list.stickers_)
			if (st && st->sticker_ &&
			    sticker_custom_emoji_id(*st) == src.object_id)
				f = st->sticker_.get();
		break;
	}
	case Kind::MessageEffect: {
		/* The effect's file whose unique id is the row's. */
		auto &e = static_cast<const td_api::messageEffect &>(*obj);
		for (const auto &x : message_effect_files(e))
			if (x.file->remote_ &&
			    x.file->remote_->unique_id_ == src.unique_id)
				f = x.file;
		break;
	}
	case Kind::Community: {
		/* The photo size whose unique id is the row's: chatPhotoInfo's
		 * big and small files are the full photo's "c" and "a". */
		auto &fi = static_cast<const td_api::communityFullInfo &>(*obj);
		if (fi.photo_)
			for (const td_api::file *x : chat_photo_files(*fi.photo_))
				if (x->remote_ &&
				    x->remote_->unique_id_ == src.unique_id)
					f = x;
		break;
	}
	}
	if (!f)
		return td_api::make_object<td_api::error>(
			404, "the source no longer carries a file");

	/* The file object lives inside obj; ask for it on its own. */
	return request_wait(td_api::make_object<td_api::getFile>(f->id_), left());
}


TDLib::TDLib(uint32_t api_id, const char *api_hash, const char *data_dir)
	: impl_(std::make_unique<Impl>(api_id, api_hash, data_dir))
{
}

TDLib::~TDLib(void) = default;

void TDLib::setLogger(log_hd_t *l)
{
	impl_->l_ = l;
}

void TDLib::setMessageHandler(std::function<void(const TextMessage &)> cb)
{
	impl_->msg_handler_ = std::move(cb);
}

void TDLib::setForwardHandler(std::function<void(const ForwardMessage &)> cb)
{
	impl_->forward_handler_ = std::move(cb);
}

void TDLib::setEditForwardHandler(std::function<void(const ForwardMessage &)> cb)
{
	impl_->edit_forward_handler_ = std::move(cb);
}

void TDLib::setDeleteForwardHandler(std::function<void(int64_t, int64_t)> cb)
{
	impl_->delete_forward_handler_ = std::move(cb);
}

void TDLib::setPrivateMessageHandler(
	std::function<void(const models::PrivateMessage &)> cb)
{
	impl_->private_msg_handler_ = std::move(cb);
}

void TDLib::setForumTopicHandlers(
	std::function<void(const models::ForumTopic &, bool)> info,
	std::function<void(int64_t, int32_t, bool)> pinned)
{
	impl_->forum_topic_handler_ = std::move(info);
	impl_->forum_topic_pinned_handler_ = std::move(pinned);
}

void TDLib::setMessageMetaHandlers(
	std::function<void(int64_t, int64_t, bool, bool)> pinned,
	std::function<void(int64_t, int64_t, bool,
			   const std::optional<std::string> &)> fact_check)
{
	impl_->message_pinned_handler_ = std::move(pinned);
	impl_->fact_check_handler_ = std::move(fact_check);
}

void TDLib::setInteractionHandler(
	std::function<void(std::vector<InteractionUpdate>)> cb, int flush_secs)
{
	impl_->interaction_handler_ = std::move(cb);
	impl_->interaction_flush_secs_ = flush_secs;
}

void TDLib::setGroupMemberInfoHandler(
	std::function<void(int64_t, std::optional<int64_t>, std::optional<bool>)> cb)
{
	impl_->group_member_info_handler_ = std::move(cb);
}

void TDLib::setPollHandler(std::function<void(const models::Poll &)> cb)
{
	impl_->poll_handler_ = std::move(cb);
}

void TDLib::setLinkPreviewHandler(
	std::function<void(const models::LinkPreview &, bool)> cb)
{
	impl_->link_preview_handler_ = std::move(cb);
}

void TDLib::setContentVersionHandler(
	std::function<void(int64_t, int64_t, const models::MessageContent &,
			   bool)> cb)
{
	impl_->content_version_handler_ = std::move(cb);
}

void TDLib::setGroupMessageHandler(
	std::function<void(const models::GroupMessage &)> cb)
{
	impl_->group_msg_handler_ = std::move(cb);
}

void TDLib::setMessageFileHandler(std::function<void(const MessageFile &)> cb)
{
	impl_->message_file_handler_ = std::move(cb);
}

void TDLib::setFileEmitBatchHandlers(std::function<void(int32_t)> begin,
				     std::function<void(int32_t)> end)
{
	impl_->file_batch_begin_ = std::move(begin);
	impl_->file_batch_end_ = std::move(end);
}

void TDLib::setFileLookup(
	std::function<std::optional<models::KnownFile>(
		const std::string &, const std::string &)> cb)
{
	impl_->file_lookup_ = std::move(cb);
}

void TDLib::setMessageFileLinkHandler(
	std::function<void(const MessageFileLink &)> cb)
{
	impl_->message_file_link_handler_ = std::move(cb);
}

void TDLib::setPhotoLinkHandler(std::function<void(const PhotoLink &)> cb)
{
	impl_->photo_link_handler_ = std::move(cb);
}

void TDLib::setFileMetaHandler(std::function<void(const FileMeta &)> cb)
{
	impl_->file_meta_handler_ = std::move(cb);
}

void TDLib::setMediaPolicy(bool skip_nonmember, uint64_t max_store_file_size)
{
	impl_->skip_nonmember_media_ = skip_nonmember;
	impl_->max_store_file_size_ = max_store_file_size;
}

void TDLib::setMessageReplyHandler(std::function<void(const MessageReply &)> cb)
{
	impl_->message_reply_handler_ = std::move(cb);
}

void TDLib::setUserHandler(std::function<void(const models::User &)> cb)
{
	impl_->user_handler_ = std::move(cb);
}

void TDLib::setUserFullInfoHandler(
	std::function<void(const models::UserFullInfo &)> cb)
{
	impl_->user_full_info_handler_ = std::move(cb);
}

void TDLib::setPhotoVideoHandler(
	std::function<void(int64_t, bool, const models::File &,
			   const models::File &)> cb)
{
	impl_->photo_video_handler_ = std::move(cb);
}

/*
 * The video of an animated current photo, from a full info: hand its still
 * and its MPEG-4 to the photo video handler, which links them in the
 * photo's history (profile-photo-videos). A still photo does nothing.
 */
void TDLib::Impl::note_photo_video(int64_t owner_id, bool is_group,
				   const td_api::chatPhoto *photo)
{
	if (!photo_video_handler_ || !photo)
		return;
	const td_api::file *video = chat_photo_animation(photo);
	const td_api::file *still = chat_photo_best_size_of(photo->sizes_);
	auto meta = [](const td_api::file *f, const char *type, const char *ext,
		       models::File &out) {
		if (!f || !f->remote_ || f->remote_->id_.empty())
			return false;
		out.tg_file_id = f->remote_->id_;
		out.tg_unique_id = f->remote_->unique_id_;
		out.file_type = type;
		out.file_size = f->size_ > 0 ? (uint64_t)f->size_ : 0;
		out.on_disk = false;
		out.file_ext = ext;
		return true;
	};
	models::File s, v;
	if (meta(still, "photo", "jpg", s) && meta(video, "video", "mp4", v))
		photo_video_handler_(owner_id, is_group, s, v);
}

void TDLib::setProfilePhotoHandler(std::function<void(const ProfilePhoto &)> cb)
{
	impl_->photo_handler_ = std::move(cb);
}

void TDLib::setGroupHandler(std::function<void(const models::Group &)> cb)
{
	impl_->group_handler_ = std::move(cb);
}

void TDLib::setGroupPhotoHandler(std::function<void(const GroupPhoto &)> cb)
{
	impl_->group_photo_handler_ = std::move(cb);
}

void TDLib::setGroupAdminsHandler(
	std::function<void(const models::GroupAdminList &)> cb)
{
	impl_->group_admins_handler_ = std::move(cb);
}

void TDLib::setAdminPollConfig(double interval_seconds, int batch)
{
	impl_->admin_poll_interval_ = interval_seconds;
	impl_->admin_poll_batch_ = batch > 0 ? batch : 1;
}

void TDLib::setBackfillStateHandler(
	std::function<void(const models::BackfillState &)> cb)
{
	impl_->backfill_state_handler_ = std::move(cb);
}

void TDLib::loadBackfillState(const std::vector<models::BackfillState> &states)
{
	for (const auto &s : states) {
		Impl::BackfillEntry e;
		e.cursor_msg_id = s.cursor_msg_id.value_or(0);
		e.done = s.done;
		e.priority = s.priority;
		impl_->backfill_[s.chat_id] = e;
		impl_->backfill_order_.push_back(s.chat_id);
	}
}

void TDLib::setBackfillConfig(double tick_interval, double discovery_interval,
			      int page, int inflight)
{
	impl_->backfill_interval_ = tick_interval;
	impl_->backfill_enabled_ = tick_interval > 0.0;
	impl_->backfill_discovery_interval_ =
		discovery_interval > 0.0 ? discovery_interval : 300.0;
	impl_->backfill_page_ = (page > 0 && page <= 100) ? page : 100;
	impl_->backfill_inflight_max_ = inflight > 0 ? inflight : 1;
}

void TDLib::loop(int timeout)
{
	impl_->loop_thread_.store(std::this_thread::get_id(),
				  std::memory_order_relaxed);
	/* Issue any refetches queued by the DB worker, on this (the loop)
	 * thread where send_query and the TDLib caches live. */
	impl_->drain_refetch();
	impl_->drain_interaction();
	impl_->process_response(impl_->client_manager_->receive(timeout));
}

bool TDLib::isStopped(void) const
{
	return impl_->stopped_;
}

TDLib::Status TDLib::status(void) const
{
	std::lock_guard<std::mutex> lk(impl_->requests_mtx_);
	return { impl_->state_, impl_->state_since_,
		 impl_->status_user_id_ };
}

void TDLib::close(void)
{
	/* Stop re-arming the admin-poll alarm while TDLib shuts down. */
	impl_->closing_ = true;
	impl_->set_requests_open(false);
	impl_->set_state(State::Closing);
	impl_->send_query(td_api::make_object<td_api::close>(), {});
}

void TDLib::deleteLocalFile(int32_t file_id)
{
	impl_->delete_local_file(file_id);
}

std::future<TDLib::TdObject> TDLib::request(TdFunction f)
{
	std::uint64_t id;
	return impl_->request(std::move(f), &id);
}

TDLib::TdObject TDLib::request(TdFunction f, std::chrono::milliseconds timeout)
{
	return impl_->request_wait(std::move(f), timeout);
}

TDLib::TdObject TDLib::resolveFile(const models::FileSource &src,
				   std::chrono::milliseconds timeout)
{
	return impl_->resolve_file(src, timeout);
}

bool TDLib::isIngestDownloading(int32_t file_id) const
{
	std::lock_guard<std::mutex> lk(impl_->ingest_dl_mtx_);
	return impl_->ingest_dl_.count(file_id) != 0;
}

void TDLib::setPruneOnStart(bool on)
{
	impl_->purge_on_start_ = on;
}

void TDLib::refetchUser(int64_t user_id)
{
	impl_->enqueue_refetch_user(user_id);
}

void TDLib::refetchGroup(int64_t group_id)
{
	impl_->enqueue_refetch_group(group_id);
}

void TDLib::setCommunityHandlers(
	std::function<void(const models::Community &)> community,
	std::function<void(const models::CommunityFullInfo &)> full_info,
	std::function<void(int64_t)> named)
{
	impl_->community_handler_ = std::move(community);
	impl_->community_full_info_handler_ = std::move(full_info);
	impl_->community_named_handler_ = std::move(named);
}

void TDLib::fetchCommunityFullInfo(int64_t community_id)
{
	impl_->enqueue_community_full_info(community_id);
}

void TDLib::setRecheckCatchUpHandler(
	std::function<void(std::vector<int64_t>,
			   std::unordered_map<int64_t, int64_t>)> cb)
{
	impl_->recheck_catchup_handler_ = std::move(cb);
}

size_t TDLib::queueRecheckCatchUp(const std::vector<int64_t> &chats)
{
	size_t queued = 0;
	std::lock_guard<std::mutex> lk(impl_->refetch_mtx_);
	const auto now = recheck::Clock::now();
	for (int64_t c : chats)
		if (impl_->recheck_q_.push({ c, 0, true }, now) ==
		    recheck::Admit::Queued)
			queued++;
	return queued;
}

recheck::Admit TDLib::recheckChat(int64_t chat_id, int64_t around)
{
	std::lock_guard<std::mutex> lk(impl_->refetch_mtx_);
	return impl_->recheck_q_.push({ chat_id, around, false },
				      recheck::Clock::now());
}

void TDLib::setRecheckFoundHandler(
	std::function<void(const RecheckAnswer &)> cb)
{
	impl_->recheck_found_handler_ = std::move(cb);
}

void TDLib::recheckNextPage(const RecheckAnswer &a)
{
	recheck::Item it = a.item;
	it.page++;
	it.from_td = a.oldest_td;
	std::lock_guard<std::mutex> lk(impl_->refetch_mtx_);
	impl_->recheck_more_.push_back(it);
}

void TDLib::setRecheckHandler(std::function<void(const RecheckAnswer &)> cb)
{
	impl_->recheck_handler_ = std::move(cb);
}

void TDLib::repairMessages(std::vector<std::pair<int64_t, int64_t>> msgs)
{
	std::lock_guard<std::mutex> lk(impl_->refetch_mtx_);
	impl_->repair_total_ += msgs.size();
	for (auto &m : msgs)
		impl_->repair_pending_.push_back(m);
}

void TDLib::relinkMessageMedia(std::vector<std::pair<int64_t, int64_t>> msgs)
{
	std::lock_guard<std::mutex> lk(impl_->refetch_mtx_);
	impl_->relink_total_ += msgs.size();
	for (auto &m : msgs)
		impl_->relink_pending_.push_back(m);
}

} /* namespace tgloggerd */
