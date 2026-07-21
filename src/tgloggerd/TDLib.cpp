// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 Ammar Faizi <ammarfaizi2@gnuweeb.org>
 */
#include <tgloggerd/TDLib.hpp>

#include <td/telegram/Client.h>
#include <td/telegram/td_api.h>
#include <td/telegram/td_api.hpp>

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
 * logged to private_messages, whose chat_id foreign key references
 * users.id; routing group/channel messages there (negative chat ids) is
 * what violates that constraint.
 */
bool is_private_chat(int64_t chat_id)
{
	return chat_id > 0;
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

/* Append @s to @out as a JSON string literal (quotes included), escaping
 * per RFC 8259. Control characters below 0x20 become \u00XX. */
void json_append_string(std::string &out, const std::string &s)
{
	static const char hex[] = "0123456789abcdef";
	out += '"';
	for (unsigned char c : s) {
		switch (c) {
		case '"':  out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\b': out += "\\b"; break;
		case '\f': out += "\\f"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if (c < 0x20) {
				out += "\\u00";
				out += hex[(c >> 4) & 0xf];
				out += hex[c & 0xf];
			} else {
				out += (char)c;
			}
		}
	}
	out += '"';
}

/* The stable string stored for a td_api::TextEntityType, matching the
 * subtype without the "textEntityType" prefix, lower-snake-cased. */
const char *entity_type_string(const td_api::TextEntityType &t)
{
	switch (t.get_id()) {
	case td_api::textEntityTypeBold::ID:		return "bold";
	case td_api::textEntityTypeItalic::ID:		return "italic";
	case td_api::textEntityTypeUnderline::ID:	return "underline";
	case td_api::textEntityTypeStrikethrough::ID:	return "strikethrough";
	case td_api::textEntityTypeSpoiler::ID:		return "spoiler";
	case td_api::textEntityTypeCode::ID:		return "code";
	case td_api::textEntityTypePre::ID:		return "pre";
	case td_api::textEntityTypePreCode::ID:		return "pre_code";
	case td_api::textEntityTypeBlockQuote::ID:	return "block_quote";
	case td_api::textEntityTypeExpandableBlockQuote::ID:
							return "expandable_block_quote";
	case td_api::textEntityTypeTextUrl::ID:		return "text_url";
	case td_api::textEntityTypeUrl::ID:		return "url";
	case td_api::textEntityTypeMention::ID:		return "mention";
	case td_api::textEntityTypeMentionName::ID:	return "mention_name";
	case td_api::textEntityTypeHashtag::ID:		return "hashtag";
	case td_api::textEntityTypeCashtag::ID:		return "cashtag";
	case td_api::textEntityTypeBotCommand::ID:	return "bot_command";
	case td_api::textEntityTypeEmailAddress::ID:	return "email";
	case td_api::textEntityTypePhoneNumber::ID:	return "phone_number";
	case td_api::textEntityTypeBankCardNumber::ID:	return "bank_card_number";
	case td_api::textEntityTypeCustomEmoji::ID:	return "custom_emoji";
	case td_api::textEntityTypeMediaTimestamp::ID:	return "media_timestamp";
	default:					return "unknown";
	}
}

/*
 * Serialize a formattedText's entities to a compact JSON array. Each entry
 * is {"offset","length","type", ...type-specific}; offsets/lengths are the
 * UTF-16 code-unit spans TDLib reports. Returns nullopt when there are no
 * entities (so a plain-text message stores SQL NULL).
 */
std::optional<std::string> format_entities_json(const td_api::formattedText &ft)
{
	if (ft.entities_.empty())
		return std::nullopt;

	std::string out = "[";
	bool first = true;
	for (const auto &e : ft.entities_) {
		if (!e || !e->type_)
			continue;
		if (!first)
			out += ',';
		first = false;

		out += "{\"offset\":" + std::to_string(e->offset_);
		out += ",\"length\":" + std::to_string(e->length_);
		out += ",\"type\":";
		json_append_string(out, entity_type_string(*e->type_));

		switch (e->type_->get_id()) {
		case td_api::textEntityTypeTextUrl::ID: {
			auto &t = static_cast<const td_api::textEntityTypeTextUrl &>(
				*e->type_);
			out += ",\"url\":";
			json_append_string(out, t.url_);
			break;
		}
		case td_api::textEntityTypePreCode::ID: {
			auto &t = static_cast<const td_api::textEntityTypePreCode &>(
				*e->type_);
			out += ",\"language\":";
			json_append_string(out, t.language_);
			break;
		}
		case td_api::textEntityTypeMentionName::ID: {
			auto &t = static_cast<const td_api::textEntityTypeMentionName &>(
				*e->type_);
			out += ",\"user_id\":" + std::to_string(t.user_id_);
			break;
		}
		case td_api::textEntityTypeCustomEmoji::ID: {
			auto &t = static_cast<const td_api::textEntityTypeCustomEmoji &>(
				*e->type_);
			out += ",\"custom_emoji_id\":" +
				std::to_string(t.custom_emoji_id_);
			break;
		}
		default:
			break;
		}
		out += '}';
	}
	out += ']';

	/* All entries were malformed and skipped. */
	if (first)
		return std::nullopt;
	return out;
}

/* Store a formattedText (message text or media caption) into @out: the raw
 * text (nullopt if empty) and its formatting entities. */
void set_formatted_text(const td_api::formattedText *ft,
			models::MessageContent &out)
{
	if (!ft)
		return;
	if (!ft->text_.empty())
		out.text = ft->text_;
	out.entities = format_entities_json(*ft);
}

/* The user id of a message's sender, or 0 if the sender is a chat/channel
 * or absent. Used to tell a self-action (joined/left) from one done to
 * someone else (added/removed). */
int64_t sender_user_id(const td_api::message &message)
{
	if (message.sender_id_ &&
	    message.sender_id_->get_id() == td_api::messageSenderUser::ID)
		return static_cast<const td_api::messageSenderUser &>(
			*message.sender_id_).user_id_;
	return 0;
}

/*
 * Describe a system/service message (a member joined, the title changed,
 * ...). On a recognized service content, sets @service_type to a stable
 * action id and returns a human-readable description (from the account's
 * point of view; specific names are resolved later by the web UI). Returns
 * nullopt when @content is not a service message, so the caller records it
 * as ordinary/unknown content instead.
 */
std::optional<std::string>
describe_service_message(const td_api::message &message,
			 std::string &service_type)
{
	const auto &content = *message.content_;
	switch (content.get_id()) {
	case td_api::messageChatAddMembers::ID: {
		auto &c = static_cast<const td_api::messageChatAddMembers &>(content);
		service_type = "chat_add_members";
		/* Exactly one added member, equal to the sender: a self-join. */
		if (c.member_user_ids_.size() == 1 &&
		    c.member_user_ids_[0] == sender_user_id(message))
			return "joined the group";
		if (c.member_user_ids_.size() == 1)
			return "added a member to the group";
		return "added " + std::to_string(c.member_user_ids_.size()) +
			" members to the group";
	}
	case td_api::messageChatJoinByLink::ID:
		service_type = "chat_join_by_link";
		return "joined the group via invite link";
	case td_api::messageChatJoinByRequest::ID:
		service_type = "chat_join_by_request";
		return "was accepted into the group";
	case td_api::messageChatDeleteMember::ID: {
		auto &c = static_cast<const td_api::messageChatDeleteMember &>(content);
		service_type = "chat_delete_member";
		if (c.user_id_ == sender_user_id(message))
			return "left the group";
		return "removed a member from the group";
	}
	case td_api::messageChatChangeTitle::ID: {
		auto &c = static_cast<const td_api::messageChatChangeTitle &>(content);
		service_type = "chat_change_title";
		return "changed the group name to \"" + c.title_ + "\"";
	}
	case td_api::messageChatChangePhoto::ID:
		service_type = "chat_change_photo";
		return "changed the group photo";
	case td_api::messageChatDeletePhoto::ID:
		service_type = "chat_delete_photo";
		return "removed the group photo";
	case td_api::messagePinMessage::ID:
		service_type = "pin_message";
		return "pinned a message";
	case td_api::messageBasicGroupChatCreate::ID:
	case td_api::messageSupergroupChatCreate::ID:
		service_type = "chat_create";
		return "created the group";
	case td_api::messageChatUpgradeTo::ID:
		service_type = "chat_upgrade_to";
		return "the group was upgraded to a supergroup";
	case td_api::messageChatUpgradeFrom::ID:
		service_type = "chat_upgrade_from";
		return "the supergroup was created from a group";
	case td_api::messageChatSetMessageAutoDeleteTime::ID: {
		auto &c = static_cast<
			const td_api::messageChatSetMessageAutoDeleteTime &>(content);
		service_type = "chat_set_message_auto_delete_time";
		if (c.message_auto_delete_time_ > 0)
			return "set messages to auto-delete after " +
				std::to_string(c.message_auto_delete_time_) +
				" seconds";
		return "disabled auto-delete for messages";
	}
	case td_api::messageChatSetTheme::ID:
		service_type = "chat_set_theme";
		return "changed the chat theme";
	case td_api::messageChatSetBackground::ID:
		service_type = "chat_set_background";
		return "changed the chat background";
	case td_api::messageForumTopicCreated::ID: {
		auto &c = static_cast<const td_api::messageForumTopicCreated &>(content);
		service_type = "forum_topic_created";
		return "created topic \"" + c.name_ + "\"";
	}
	case td_api::messageForumTopicEdited::ID:
		service_type = "forum_topic_edited";
		return "edited a topic";
	case td_api::messageForumTopicIsClosedToggled::ID:
		service_type = "forum_topic_is_closed_toggled";
		return "changed a topic's closed state";
	case td_api::messageForumTopicIsHiddenToggled::ID:
		service_type = "forum_topic_is_hidden_toggled";
		return "changed a topic's hidden state";
	case td_api::messageVideoChatStarted::ID:
		service_type = "video_chat_started";
		return "started a video chat";
	case td_api::messageVideoChatEnded::ID:
		service_type = "video_chat_ended";
		return "ended the video chat";
	case td_api::messageVideoChatScheduled::ID:
		service_type = "video_chat_scheduled";
		return "scheduled a video chat";
	case td_api::messageInviteVideoChatParticipants::ID:
		service_type = "invite_video_chat_participants";
		return "invited participants to the video chat";
	case td_api::messageChatBoost::ID:
		service_type = "chat_boost";
		return "boosted the group";
	case td_api::messageScreenshotTaken::ID:
		service_type = "screenshot_taken";
		return "took a screenshot";
	case td_api::messageContactRegistered::ID:
		service_type = "contact_registered";
		return "joined Telegram";
	case td_api::messageGiftedPremium::ID:
		service_type = "gifted_premium";
		return "gifted a Telegram Premium subscription";
	case td_api::messagePaymentSuccessful::ID:
	case td_api::messagePaymentSuccessfulBot::ID:
		service_type = "payment_successful";
		return "made a payment";
	case td_api::messageCustomServiceAction::ID: {
		auto &c = static_cast<const td_api::messageCustomServiceAction &>(content);
		service_type = "custom_service_action";
		return c.text_;
	}
	default:
		return std::nullopt;
	}
}

/*
 * Map a td_api::message's content to the coarse MessageContent used by both
 * private and group messages. Media files are not linked here; the content
 * type, text/caption (with its formatting entities), and — for system
 * messages — the service type and a human-readable description are captured.
 */
void extract_message_content(const td_api::message &message,
			     models::MessageContent &out)
{
	if (!message.content_)
		return;

	switch (message.content_->get_id()) {
	case td_api::messageText::ID: {
		auto &c = static_cast<const td_api::messageText &>(
			*message.content_);
		out.content_type = models::MessageContentType::Text;
		set_formatted_text(c.text_.get(), out);
		break;
	}
	case td_api::messagePhoto::ID: {
		auto &c = static_cast<const td_api::messagePhoto &>(*message.content_);
		out.content_type = models::MessageContentType::Photo;
		set_formatted_text(c.caption_.get(), out);
		break;
	}
	case td_api::messageVideo::ID: {
		auto &c = static_cast<const td_api::messageVideo &>(*message.content_);
		out.content_type = models::MessageContentType::Video;
		set_formatted_text(c.caption_.get(), out);
		break;
	}
	case td_api::messageDocument::ID: {
		auto &c = static_cast<const td_api::messageDocument &>(*message.content_);
		out.content_type = models::MessageContentType::Document;
		set_formatted_text(c.caption_.get(), out);
		break;
	}
	case td_api::messageAudio::ID: {
		auto &c = static_cast<const td_api::messageAudio &>(*message.content_);
		out.content_type = models::MessageContentType::Audio;
		set_formatted_text(c.caption_.get(), out);
		break;
	}
	case td_api::messageVoiceNote::ID: {
		auto &c = static_cast<const td_api::messageVoiceNote &>(*message.content_);
		out.content_type = models::MessageContentType::Voice;
		set_formatted_text(c.caption_.get(), out);
		break;
	}
	case td_api::messageAnimation::ID: {
		auto &c = static_cast<const td_api::messageAnimation &>(*message.content_);
		out.content_type = models::MessageContentType::Animation;
		set_formatted_text(c.caption_.get(), out);
		break;
	}
	case td_api::messageSticker::ID:
		out.content_type = models::MessageContentType::Sticker;
		break;
	default: {
		/* A system/service message (member joined, title changed, ...)
		 * is a real, replyable message; record it as such. Anything
		 * else is genuinely unknown content. */
		std::string service_type;
		auto text = describe_service_message(message, service_type);
		if (text.has_value()) {
			out.content_type = models::MessageContentType::Service;
			out.service_type = std::move(service_type);
			if (!text->empty())
				out.text = std::move(*text);
		} else {
			out.content_type = models::MessageContentType::Unknown;
		}
		break;
	}
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

/*
 * Return the primary downloadable file of a message's content, or nullptr
 * if the content carries no file. On success, *category is set to the
 * matching files.file_type value. For photos, the largest size is chosen.
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

	/*
	 * Query-id counter for fire-and-forget requests issued off the loop
	 * thread (deleteLocalFile). Seeded high so it never collides with the
	 * loop thread's monotonic current_query_id_; no handler is registered
	 * for these, so the response is simply dropped by process_response.
	 */
	std::atomic<std::uint64_t>	aux_query_id_{1ull << 62};

	/* One-time startup optimizeStorage (reclaim TDLib's cached backlog). */
	bool		purge_on_start_ = false;
	bool		purge_started_ = false;

	std::function<void(const TextMessage &)>	msg_handler_;
	std::function<void(const models::PrivateMessage &)> private_msg_handler_;
	std::function<void(const models::GroupMessage &)> group_msg_handler_;
	std::function<void(const MessageFile &)>	message_file_handler_;
	std::function<std::optional<uint64_t>(const std::string &)> file_lookup_;
	std::function<void(const MessageFileLink &)>	message_file_link_handler_;
	std::function<void(const MessageReply &)>	message_reply_handler_;
	std::function<void(const models::User &)>	user_handler_;
	std::function<void(const models::UserFullInfo &)> user_full_info_handler_;
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
	};

	std::unordered_map<int64_t, SgInfo>		supergroups_;
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
	};
	std::unordered_map<int32_t, PendingMsgFile>	pending_message_file_;

	/* Reply targets already fetched, so a chain is not re-walked. */
	std::set<std::pair<int64_t, int64_t>>		resolved_reply_targets_;

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

	Impl(uint32_t api_id, const char *api_hash, const char *data_dir);

	std::uint64_t next_query_id(void) { return ++current_query_id_; }

	void send_query(td_api::object_ptr<td_api::Function> f,
			std::function<void(Object)> handler);
	void delete_local_file(int32_t file_id);
	void process_response(td::ClientManager::Response response);
	void process_update(td_api::object_ptr<td_api::Object> update);
	void on_authorization_state_update(void);
	void check_authentication_error(Object object);
	std::function<void(Object)> create_authentication_query_handler(void);
	void handle_new_message(td_api::message &message);
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
	void maybe_download_message_file(const td_api::message &message,
					 bool is_group);
	void emit_message_file(const PendingMsgFile &ref, const td_api::file &f);
	void maybe_download_profile_photo(const td_api::user &u);
	void request_user_full_info(int64_t user_id);
	void enqueue_refetch_user(int64_t user_id);
	void enqueue_refetch_group(int64_t group_id);
	void drain_refetch(void);
	void refetch_user_info(int64_t user_id);
	void refetch_group_info(int64_t chat_id);
	void handle_file_update(const td_api::file &f);
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
	}
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
				 * user_extra_info.personal_chat_id (FK to groups).
				 * Make sure it is saved first so the link resolves;
				 * for a known chat this persists it synchronously
				 * (ahead of the full-info upsert on the serial
				 * queue), for an unknown one it fetches it for a
				 * later refresh.
				 */
				if (fi.personal_chat_id != 0)
					ensure_chat_saved(fi.personal_chat_id);
				user_full_info_handler_(fi);
			},
			[this](td_api::updateFile &u) {
				if (u.file_)
					handle_file_update(*u.file_);
			},
			[this](td_api::updateNewChat &u) {
				if (u.chat_) {
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
				supergroups_[sg.id_] = info;

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
			},
			[this](td_api::updateBasicGroupFullInfo &u) {
				if (!u.basic_group_full_info_)
					return;
				auto &st = group_state_[u.basic_group_id_];
				st.description =
					u.basic_group_full_info_->description_;
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
				handle_new_message(*u.message_);
			},
			[this](td_api::updateMessageContent &u) {
				handle_update_message_content(u.chat_id_,
					u.message_id_, u.new_content_.get());
			},
			[this](td_api::updateMessageEdited &u) {
				/*
				 * updateMessageEdited fires with edit_date
				 * but not the content. Request the full
				 * message; when it arrives we rebuild and
				 * upsert it as if it were a new message,
				 * routing to the private or group path by
				 * chat kind.
				 */
				send_query(
					td_api::make_object<td_api::getMessage>(
						u.chat_id_, u.message_id_),
					[this](Object obj) {
						if (obj->get_id() !=
						    td_api::message::ID)
							return;
						auto msg =
							td::move_tl_object_as<
								td_api::message>(obj);
						if (is_private_chat(msg->chat_id_))
							handle_message_for_private_chat(
								*msg, 0);
						else
							handle_message_for_group_chat(
								*msg, 0);
					});
			},
			[this](td_api::updateDeleteMessages &u) {
				handle_delete_messages(
					u.chat_id_,
					u.message_ids_,
					u.is_permanent_);
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
					});
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
	 * private_messages (chat_id is a users.id); group, supergroup and
	 * channel chats (negative ids) go to group_messages (chat_id is a
	 * groups.id).
	 */
	if (is_private_chat(message.chat_id_))
		handle_message_for_private_chat(message, 0);
	else
		handle_message_for_group_chat(message, 0);

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

	/* Resolve the sender name and username from the cached users. */
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

void TDLib::Impl::handle_update_message_content(int64_t chat_id,
						int64_t message_id,
						const td_api::MessageContent *content)
{
	if (!private_msg_handler_)
		return;

	/*
	 * For message edits we need the full message object to get
	 * edit_date and sender. TDLib sends a separate updateMessageEdited
	 * or we can request getMessage. However, updateMessageContent
	 * only has chat_id, message_id, and new_content.
	 *
	 * For now, we send a getMessage query to fetch the full message
	 * so we can determine edit_date. But this adds latency and
	 * complexity. Simpler: just record the content update as a
	 * potential edit. We check private_messages for the existing
	 * row and compare content.
	 *
	 * Actually, the simplest approach for now: we know the message
	 * already exists in private_messages. We can just update the
	 * content fields. But we need edit_date. Let's use a simpler
	 * strategy: request getMessage to get the full object.
	 *
	 * For now, skip updateMessageContent handling; content changes
	 * that TDLib delivers via updateMessageContent without a
	 * corresponding edit_date are initial loads. Real edits come
	 * via updateMessageEdited which we'll handle below, or via
	 * updateNewMessage with an edit_date > 0.
	 *
	 * TODO: implement getMessage-based edit tracking.
	 */
	(void)chat_id;
	(void)message_id;
	(void)content;
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
	 * recorded with a NULL sender_id, as the private_messages schema
	 * prescribes. Incoming private-chat messages are always sent by the
	 * peer user; a messageSenderChat is not expected here and is ignored
	 * (its chat id is not a valid users.id).
	 */
	if (!message.is_outgoing_ && message.sender_id_ &&
	    message.sender_id_->get_id() == td_api::messageSenderUser::ID) {
		auto &s = static_cast<const td_api::messageSenderUser &>(
			*message.sender_id_);
		out.sender_id = s.user_id_;
	}

	extract_message_content(message, out.content);
	out.forward_info = extract_forward_info(message);
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
		 * to users.id resolves before the message row is written.
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

void TDLib::Impl::maybe_download_message_file(const td_api::message &message,
					      bool is_group)
{
	if (!message_file_handler_ || !message.content_)
		return;

	const char *category = "unknown";
	std::string file_name;
	const td_api::file *f =
		message_content_file(*message.content_, &category, &file_name);
	if (!f)
		return;

	PendingMsgFile ref{ message.chat_id_, to_server_msg_id(message.id_),
			    is_group, category, file_name };

	/*
	 * Already recorded from an earlier download: link the existing files
	 * row by its remote id instead of downloading (and hashing) it again.
	 * This is what keeps the backfiller from re-fetching the whole history,
	 * and in particular from re-downloading oversized files that are only
	 * kept as metadata.
	 */
	if (file_lookup_ && message_file_link_handler_ && f->remote_ &&
	    !f->remote_->id_.empty()) {
		auto fid = file_lookup_(f->remote_->id_);
		if (fid.has_value()) {
			message_file_link_handler_(MessageFileLink{
				ref.chat_id, ref.message_id, ref.is_group,
				*fid });
			return;
		}
	}

	/* Already downloaded: link it immediately. */
	if (f->local_ && f->local_->is_downloading_completed_) {
		emit_message_file(ref, *f);
		return;
	}

	/* Otherwise request the download and remember the message for it. */
	pending_message_file_[f->id_] = std::move(ref);
	send_query(td_api::make_object<td_api::downloadFile>(
			   f->id_, 1, 0, 0, false), {});
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
	mf.local_path = f.local_->path_;
	mf.tg_file_id = f.remote_ ? f.remote_->id_ : std::string();
	mf.tg_local_file_id = f.id_;
	mf.file_size = f.size_;
	mf.content_type = ref.content_type;
	mf.orig_file_name = ref.orig_file_name;
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
void TDLib::Impl::drain_refetch(void)
{
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
	for (int64_t uid : users) {
		time_t &last = refetch_user_last_[uid];
		if (last != 0 && (double)(now - last) < refetch_cooldown_)
			continue;
		last = now;
		refetch_user_info(uid);
	}
	for (int64_t gid : groups) {
		time_t &last = refetch_group_last_[gid];
		if (last != 0 && (double)(now - last) < refetch_cooldown_)
			continue;
		last = now;
		refetch_group_info(gid);
	}
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

void TDLib::Impl::maybe_download_profile_photo(const td_api::user &u)
{
	if (!photo_handler_ || !u.profile_photo_ || !u.profile_photo_->big_)
		return;

	const td_api::file &big = *u.profile_photo_->big_;

	/* Already downloaded: emit immediately. */
	if (big.local_ && big.local_->is_downloading_completed_) {
		emit_photo(u.id_, big);
		return;
	}

	/* Otherwise request the download and remember which user it is for. */
	pending_photo_[big.id_] = u.id_;
	send_query(td_api::make_object<td_api::downloadFile>(
			   big.id_, 1, 0, 0, false), {});
}

void TDLib::Impl::handle_file_update(const td_api::file &f)
{
	if (!f.local_ || !f.local_->is_downloading_completed_)
		return;

	auto it = pending_photo_.find(f.id_);
	if (it != pending_photo_.end()) {
		int64_t user_id = it->second;
		pending_photo_.erase(it);
		emit_photo(user_id, f);
		return;
	}

	auto git = pending_group_photo_.find(f.id_);
	if (git != pending_group_photo_.end()) {
		int64_t group_id = git->second;
		pending_group_photo_.erase(git);
		emit_group_photo(group_id, f);
		return;
	}

	auto mit = pending_message_file_.find(f.id_);
	if (mit != pending_message_file_.end()) {
		PendingMsgFile ref = mit->second;
		pending_message_file_.erase(mit);
		emit_message_file(ref, f);
		return;
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
		/* Private and secret chats are not groups. */
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

void TDLib::Impl::maybe_download_group_photo(int64_t group_id,
					     const td_api::chatPhotoInfo *photo)
{
	if (!group_photo_handler_ || !photo || !photo->big_)
		return;

	const td_api::file &big = *photo->big_;
	if (big.local_ && big.local_->is_downloading_completed_) {
		emit_group_photo(group_id, big);
		return;
	}

	pending_group_photo_[big.id_] = group_id;
	send_query(td_api::make_object<td_api::downloadFile>(
			   big.id_, 1, 0, 0, false), {});
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
	p.tg_local_file_id = f.id_;
	p.file_size = f.size_;
	group_photo_handler_(p);
}


TDLib::TDLib(uint32_t api_id, const char *api_hash, const char *data_dir)
	: impl_(std::make_unique<Impl>(api_id, api_hash, data_dir))
{
}

TDLib::~TDLib(void) = default;

void TDLib::setMessageHandler(std::function<void(const TextMessage &)> cb)
{
	impl_->msg_handler_ = std::move(cb);
}

void TDLib::setPrivateMessageHandler(
	std::function<void(const models::PrivateMessage &)> cb)
{
	impl_->private_msg_handler_ = std::move(cb);
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

void TDLib::setFileLookup(
	std::function<std::optional<uint64_t>(const std::string &)> cb)
{
	impl_->file_lookup_ = std::move(cb);
}

void TDLib::setMessageFileLinkHandler(
	std::function<void(const MessageFileLink &)> cb)
{
	impl_->message_file_link_handler_ = std::move(cb);
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
	/* Issue any refetches queued by the DB worker, on this (the loop)
	 * thread where send_query and the TDLib caches live. */
	impl_->drain_refetch();
	impl_->process_response(impl_->client_manager_->receive(timeout));
}

bool TDLib::isStopped(void) const
{
	return impl_->stopped_;
}

void TDLib::close(void)
{
	/* Stop re-arming the admin-poll alarm while TDLib shuts down. */
	impl_->closing_ = true;
	impl_->send_query(td_api::make_object<td_api::close>(), {});
}

void TDLib::deleteLocalFile(int32_t file_id)
{
	impl_->delete_local_file(file_id);
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

} /* namespace tgloggerd */
