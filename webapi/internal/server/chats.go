// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */

package server

import (
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"strconv"
	"strings"
	"time"

	"tgloggerd/webapi/internal/auth"
	"tgloggerd/webapi/internal/logdb"
	"tgloggerd/webapi/internal/scope"
)

// The chat API (GET /api/chats/{scope}/{id}/messages), GET
// /api/entities, and the JSON shapes of users, groups and messages the
// pages share (webapi/openapi.yaml: UserSummary, GroupSummary,
// ChatMessage). The window semantics are the Drogon /v1 chat API's
// (web/src/controllers/ApiController.cpp): the same message ids as
// cursors, replies resolved within the chat, albums folded into one
// message, media with the same stored and fetchable rules
// (web/docs/media.md).

// view turns rows into JSON for one request: its scope decides which
// fields a user or a group shows, and whether it has a session which
// files it may fetch.
type view struct {
	s       *Server
	sc      scope.Scope
	session bool
}

func (s *Server) viewFor(sc scope.Scope, session bool) view {
	return view{s: s, sc: sc, session: session}
}

// full is whether the viewer sees users and groups in full: their
// counts, contact names and exposure (web-accounts.md section 5).
func (v view) full() bool {
	return v.sc.SeesAllUsers()
}

// iso writes a unix time as the API's timestamps are written.
func iso(t int64) string {
	return time.Unix(t, 0).UTC().Format(time.RFC3339)
}

func isoPtr(t int64) *string {
	if t == 0 {
		return nil
	}
	s := iso(t)
	return &s
}

// file is how a file is linked for this request (Server.fileLink),
// and whether its bytes are stored.
func (v view) file(f logdb.FileRef) (l fileLink, stored bool) {
	if f.ID == 0 || !f.Exists {
		return fileLink{}, false
	}
	return v.s.fileLink(v.sc, v.session, f.ID, f.Size, f.OnDisk), f.OnDisk
}

// photo is a picture's URL, or nil.
func (v view) photo(f logdb.FileRef) *string {
	l, _ := v.file(f)
	return l.URL
}

type userSummaryJSON struct {
	ID          int64   `json:"id"`
	Name        string  `json:"name"`
	Username    string  `json:"username"`
	Type        string  `json:"type"`
	Photo       *string `json:"photo"`
	ContactName string  `json:"contact_name,omitempty"`
	*userCountsJSON
}

// userCountsJSON are the fields only a full-access viewer gets.
type userCountsJSON struct {
	MsgCount   uint64 `json:"msg_count"`
	IsVerified bool   `json:"is_verified"`
	IsPremium  bool   `json:"is_premium"`
	IsScam     bool   `json:"is_scam"`
	IsFake     bool   `json:"is_fake"`
	IsSupport  bool   `json:"is_support"`
	Language   string `json:"language"`
	CreatedAt  string `json:"created_at"`
	UpdatedAt  string `json:"updated_at"`
}

// user is a user's summary as the viewer may see it: in full, or name
// and photo only.
func (v view) user(u logdb.User) userSummaryJSON {
	j := userSummaryJSON{ID: u.ID, Name: u.Name(), Username: u.Username, Type: u.Type,
		Photo: v.photo(u.Photo)}
	if v.full() {
		j.ContactName = u.ContactName()
		j.userCountsJSON = &userCountsJSON{MsgCount: u.MsgCount, IsVerified: u.IsVerified,
			IsPremium: u.IsPremium, IsScam: u.IsScam, IsFake: u.IsFake, IsSupport: u.IsSupport,
			Language: u.Language, CreatedAt: iso(u.CreatedAt), UpdatedAt: iso(u.UpdatedAt)}
	}
	return j
}

// userOf is the user with this id from a loaded set; one whose row is
// missing (never seen in full) still gets a summary.
func (v view) userOf(users map[int64]logdb.User, id int64) userSummaryJSON {
	if u, ok := users[id]; ok {
		return v.user(u)
	}
	return userSummaryJSON{ID: id, Name: "#" + strconv.FormatInt(id, 10), Type: "unknown"}
}

type groupSummaryJSON struct {
	ID       int64   `json:"id"`
	Title    string  `json:"title"`
	Username string  `json:"username"`
	Type     string  `json:"type"`
	Photo    *string `json:"photo"`
	MsgCount *uint64 `json:"msg_count,omitempty"`
	Exposed  *bool   `json:"exposed,omitempty"`
	// Telegram's member count as the daemon last stored it, and when;
	// null before it has. Every viewer of the group sees it: it is
	// what Telegram shows anyone who opens the group.
	MemberCount   *uint64 `json:"member_count"`
	MemberCountAt *string `json:"member_count_at"`
}

// group is a group's summary; its count and exposure only for a
// full-access viewer.
func (v view) group(g logdb.Group) groupSummaryJSON {
	j := groupSummaryJSON{ID: g.ID, Title: g.Title, Username: g.Username, Type: g.Type,
		Photo: v.photo(g.Photo), MemberCount: g.MemberCount}
	if g.MemberCount != nil {
		j.MemberCountAt = isoPtr(g.MemberCountAt)
	}
	if v.full() {
		n, e := g.MsgCount, g.Exposed
		j.MsgCount, j.Exposed = &n, &e
	}
	return j
}

type chatMediaJSON struct {
	Stored    bool    `json:"stored"`
	Fetchable bool    `json:"fetchable"`
	Fetch     *string `json:"fetch"`
	Thumb     *string `json:"thumb"`
	URL       *string `json:"url"`
	Name      string  `json:"name,omitempty"`
	Size      uint64  `json:"size,omitempty"`
	mediaDetailJSON
	// The item's own message, for the items of an album (the album
	// message's media and album), which may differ in kind and in
	// whether they were deleted.
	MessageID        int64   `json:"message_id,omitempty"`
	Kind             string  `json:"kind,omitempty"`
	DeletedAt        *string `json:"deleted_at,omitempty"`
	DeletedOnRecheck bool    `json:"deleted_on_recheck,omitempty"`
}

// poster adds the picture a video (an animation, a video note, a
// video sticker) shows before it plays to its media, which is then
// also its thumb: its cover when stored, else Telegram's thumbnail of
// it when stored, else its cover when it may be fetched. The minithumb
// stays as it is, for a blurred placeholder.
func (v view) poster(md *chatMediaJSON, m logdb.Message) {
	if md == nil || (md.Render != "video" && md.Render != "animation") {
		return
	}
	var u *string
	switch {
	case storedFile(m.Cover):
		u = v.photo(m.Cover)
	case storedFile(m.Thumbnail):
		u = v.photo(m.Thumbnail)
	case m.Cover.ID != 0:
		u = v.photo(m.Cover)
	}
	if u != nil {
		md.Poster, md.Thumb = u, u
	}
}

// storedFile is whether a file's bytes are in the store.
func storedFile(f logdb.FileRef) bool {
	return f.ID != 0 && f.Exists && f.OnDisk
}

// media is a message's media: its file and its detail. thumb is the
// file's own URL for media that render as an image, and otherwise its
// blurred preview (minithumb) when the row records one.
func (v view) media(ctype string, f logdb.FileRef, extra []byte) *chatMediaJSON {
	if f.ID == 0 {
		return nil
	}
	l, stored := v.file(f)
	url := l.URL
	m := &chatMediaJSON{Stored: stored, Fetchable: l.Fetchable, Fetch: l.Fetch, URL: url,
		Name: f.Name, Size: f.Size,
		mediaDetailJSON: mediaDetail(ctype, f, extra)}
	switch {
	case m.Render == "image":
		m.Thumb = url
	case m.Minithumb != "":
		t := m.Minithumb
		m.Thumb = &t
	}
	return m
}

type chatReplyJSON struct {
	MessageID  int64  `json:"message_id"`
	SenderID   int64  `json:"sender_id"`
	SenderName string `json:"sender_name"`
	Snippet    string `json:"snippet"`
	// ChatID is set when the target is in another chat.
	ChatID *int64 `json:"chat_id,omitempty"`
	// Resolved: the target is logged and the fields above describe
	// it; otherwise only its id is known ("message #N").
	Resolved bool `json:"resolved"`
	Deleted  bool `json:"deleted"`
}

type editJSON struct {
	At          string           `json:"at"`
	Text        string           `json:"text"`
	Entities    []textEntityJSON `json:"entities"`
	ContentType string           `json:"content_type,omitempty"`
	Media       *chatMediaJSON   `json:"media,omitempty"`
	// Detail is the version's MessageDetail (view.editDetail).
	Detail any `json:"detail"`
}

// forwardJSON is a forwarded message's origin (ForwardOrigin), with
// the ids a client links to.
type forwardJSON struct {
	Type      string  `json:"type"`
	Name      string  `json:"name"`
	UserID    *int64  `json:"user_id,omitempty"`
	ChatID    *int64  `json:"chat_id,omitempty"`
	MessageID *int64  `json:"message_id,omitempty"`
	Date      *string `json:"date,omitempty"`
}

type chatMessageJSON struct {
	ID          uint64           `json:"id"`
	MessageID   int64            `json:"message_id"`
	Date        string           `json:"date"`
	Scope       string           `json:"scope"`
	Sender      *userSummaryJSON `json:"sender"`
	ContentType string           `json:"content_type"`
	Text        string           `json:"text"`
	Entities    []textEntityJSON `json:"entities"`
	Media       *chatMediaJSON   `json:"media"`
	Album       []chatMediaJSON  `json:"album"`
	ReplyTo     *chatReplyJSON   `json:"reply_to"`
	Forward     *string          `json:"forward"`
	Edits       []editJSON       `json:"edits"`
	EditedAt    *string          `json:"edited_at"`
	DeletedAt   *string          `json:"deleted_at"`
	// DeletedOnRecheck: the deletion was found by a recheck against
	// Telegram (onRecheck); sent only when true.
	DeletedOnRecheck bool             `json:"deleted_on_recheck,omitempty"`
	LinkPreview      *linkPreviewJSON `json:"link_preview"`
	messageExtraJSON
	messageKindsJSON
}

// onRecheck is whether a message's deletion is to be shown as found by
// a recheck: the flag counts only on a message marked deleted.
func onRecheck(m logdb.Message) bool {
	return m.DeletedAt != 0 && m.DeletedOnRecheck
}

// messageKindsJSON are the fields of MessageKinds, which ChatMessage
// and MessageFull share: the kind's detail (kinds.go), a service
// message's action (service.go), the forum topic, and the pinned state
// and latest interaction totals (docs/design/message-types.md
// sections 4.5 to 4.7).
type messageKindsJSON struct {
	Detail    any            `json:"detail"`
	Service   *serviceJSON   `json:"service"`
	TopicID   *int64         `json:"topic_id"`
	Pinned    bool           `json:"pinned"`
	Views     *int64         `json:"views"`
	Forwards  *int64         `json:"forwards"`
	Replies   *int64         `json:"replies"`
	Reactions []reactionJSON `json:"reactions"`
	// EffectID and Effect are the message's effect (emoji.go).
	EffectID *string            `json:"effect_id"`
	Effect   *messageEffectJSON `json:"effect"`
}

// channelCounts is whether a message's view and forward counts mean
// anything: Telegram counts them for channel posts, and a message
// forwarded from a channel carries its post's. A group's own messages
// get them stored as 0, which the chat would show as "0 views".
func channelCounts(m logdb.Message) bool {
	return m.IsChannelPost || (m.Forward != nil && m.Forward.Type == "channel")
}

// reactionJSON is one reaction's total, as the reactions column
// stores it: {emoji | custom_emoji_id | paid: true, count}.
type reactionJSON struct {
	Emoji         string `json:"emoji,omitempty"`
	CustomEmojiID string `json:"custom_emoji_id,omitempty"`
	Paid          bool   `json:"paid,omitempty"`
	Count         int64  `json:"count"`
	// CustomEmoji is present (an object or null) exactly when
	// CustomEmojiID is; never read from the column.
	CustomEmoji json.RawMessage `json:"custom_emoji,omitempty"`
}

// reactions reads the reactions column; nil, malformed or withdrawn
// all give none.
func reactions(raw []byte) []reactionJSON {
	out := []reactionJSON{}
	if len(raw) == 0 {
		return out
	}
	var rs []reactionJSON
	if json.Unmarshal(raw, &rs) != nil {
		return out
	}
	for _, r := range rs {
		r.CustomEmoji = nil
		if r.Count > 0 && (r.Emoji != "" || r.CustomEmojiID != "" || r.Paid) {
			out = append(out, r)
		}
	}
	return out
}

func (v view) messageKinds(k kinds, m logdb.Message) messageKindsJSON {
	j := messageKindsJSON{Detail: v.detail(k, m), Service: v.service(k, m), Pinned: m.Pinned,
		Replies: m.Replies, Reactions: reactions(m.Reactions),
		EffectID: idString(m.EffectID), Effect: v.effect(k, m.EffectID)}
	for i, r := range j.Reactions {
		if r.CustomEmojiID != "" {
			j.Reactions[i].CustomEmoji = jsonOrNull(v.customEmoji(k, r.CustomEmojiID))
		}
	}
	if channelCounts(m) {
		j.Views, j.Forwards = m.Views, m.Forwards
	}
	if m.TopicID != 0 {
		id := m.TopicID
		j.TopicID = &id
	}
	return j
}

// text is a message's text for the viewer: its text or caption, and
// for a service message its description (view.serviceText).
func (v view) text(k kinds, m logdb.Message) string {
	if m.ContentType == "service" {
		return v.serviceText(k, m, k.service[m.RowID])
	}
	return m.Text
}

// messageExtraJSON are the fields ChatMessage and MessageFull share
// beyond their own.
type messageExtraJSON struct {
	// SenderChat is the chat a message was posted as (a channel, an
	// anonymous admin's group), which sender shows like a user.
	SenderChat      *groupSummaryJSON `json:"sender_chat,omitempty"`
	ForwardFrom     *forwardJSON      `json:"forward_from,omitempty"`
	AuthorSignature string            `json:"author_signature,omitempty"`
	IsChannelPost   bool              `json:"is_channel_post,omitempty"`
	ServiceType     string            `json:"service_type,omitempty"`
	TdlibType       string            `json:"tdlib_type,omitempty"`
}

func (v view) messageExtra(n names, m logdb.Message) messageExtraJSON {
	j := messageExtraJSON{ForwardFrom: v.forwardFrom(n, m), AuthorSignature: m.AuthorSignature,
		IsChannelPost: m.IsChannelPost, ServiceType: m.ServiceType, TdlibType: m.TdlibType}
	// A chat the viewer may not see is not named, nor linked.
	if m.SenderChat != 0 && !m.Outgoing && v.sc.SeesChat(m.SenderChat) {
		g, ok := n.groups[m.SenderChat]
		if !ok {
			g = logdb.Group{ID: m.SenderChat, Title: "#" + strconv.FormatInt(m.SenderChat, 10),
				Type: "unknown"}
		}
		gj := v.group(g)
		j.SenderChat = &gj
	}
	return j
}

type chatWindowJSON struct {
	Messages []chatMessageJSON `json:"messages"`
	HasOlder bool              `json:"has_older"`
	HasNewer bool              `json:"has_newer"`
}

func scopeName(group bool) string {
	if group {
		return "group"
	}
	return "private"
}

// names are the users and groups a set of messages refers to.
type names struct {
	users  map[int64]logdb.User
	groups map[int64]logdb.Group
}

// sender is a message's sender: nil for the logging account's own
// messages, a user, or a chat posting in its own name (a channel, an
// anonymous admin), which is shown like a user with the chat's id.
func (v view) sender(n names, m logdb.Message) *userSummaryJSON {
	switch {
	case m.Outgoing:
		return nil
	case m.SenderChat != 0 && !v.sc.SeesChat(m.SenderChat):
		// Posted as a chat the viewer may not see: not named.
		return &userSummaryJSON{Name: hiddenChat, Type: "unknown"}
	case m.SenderChat != 0:
		j := userSummaryJSON{ID: m.SenderChat, Name: "#" + strconv.FormatInt(m.SenderChat, 10),
			Type: "unknown"}
		if g, ok := n.groups[m.SenderChat]; ok {
			j.Name, j.Username, j.Photo = g.Title, g.Username, v.photo(g.Photo)
		}
		return &j
	case m.SenderUser != 0:
		j := v.userOf(n.users, m.SenderUser)
		return &j
	}
	name := m.AuthorSignature
	if name == "" {
		name = "(unknown)"
	}
	return &userSummaryJSON{Name: name, Type: "unknown"}
}

// nameOf is the display name of a user or a chat, "" when unknown.
func (n names) nameOf(user, chat int64) string {
	if chat != 0 {
		if g, ok := n.groups[chat]; ok {
			return g.Title
		}
		return ""
	}
	if u, ok := n.users[user]; ok {
		return u.Name()
	}
	return ""
}

func (n names) forward(m logdb.Message) *string {
	f := m.Forward
	if f == nil {
		return nil
	}
	name := f.SenderName
	if name == "" {
		name = n.nameOf(f.UserID, 0)
	}
	if name == "" {
		name = n.nameOf(0, f.ChatID)
	}
	if name == "" {
		name = "Unknown"
	}
	return &name
}

// forwardName is names.forward for the viewer: an origin chat it may not
// see is not named.
func (v view) forwardName(n names, m logdb.Message) *string {
	if f := m.Forward; f != nil && f.ChatID != 0 && !v.sc.SeesChat(f.ChatID) {
		s := hiddenChat
		return &s
	}
	return n.forward(m)
}

// hiddenChat names a chat the viewer may not see, where a message
// refers to one (posted as it, forwarded from it, replying from it).
const hiddenChat = "(hidden chat)"

// forwardFrom is the origin with its ids, named as forward names it; an
// origin chat the viewer may not see keeps only its kind and date.
func (v view) forwardFrom(n names, m logdb.Message) *forwardJSON {
	f := m.Forward
	if f == nil {
		return nil
	}
	if f.ChatID != 0 && !v.sc.SeesChat(f.ChatID) {
		return &forwardJSON{Type: f.Type, Name: hiddenChat, Date: isoPtr(f.Date)}
	}
	j := &forwardJSON{Type: f.Type, Name: *n.forward(m), Date: isoPtr(f.Date)}
	if f.UserID != 0 {
		id := f.UserID
		j.UserID = &id
	}
	if f.ChatID != 0 {
		id := f.ChatID
		j.ChatID = &id
	}
	if f.MessageID != 0 {
		id := f.MessageID
		j.MessageID = &id
	}
	return j
}

// replySnippetRunes is how much of its target a reply quotes.
const replySnippetRunes = 120

// reply is what a message replies to, as the viewer may see it: a
// target in another chat is shown only to a viewer that sees that
// chat, and otherwise not at all.
func (v view) reply(n names, k kinds, chatID int64, m logdb.Message) *chatReplyJSON {
	r := m.Reply
	if r == nil {
		return nil
	}
	j := &chatReplyJSON{MessageID: r.MessageID}
	if r.ChatID != chatID {
		if !v.sc.SeesChat(r.ChatID) {
			return nil
		}
		c := r.ChatID
		j.ChatID = &c
	}
	if !r.Resolved {
		return j
	}
	j.Resolved, j.Deleted = true, r.Deleted
	switch {
	case r.SenderChat != 0 && !v.sc.SeesChat(r.SenderChat):
		j.SenderName = hiddenChat
	case r.SenderChat != 0:
		j.SenderID, j.SenderName = r.SenderChat, n.nameOf(0, r.SenderChat)
	case r.SenderUser != 0:
		j.SenderID, j.SenderName = r.SenderUser, n.nameOf(r.SenderUser, 0)
	case r.Outgoing:
		j.SenderName = "You"
	}
	// As much of a summary as of a text (messageSelect).
	j.Snippet, _ = cutRunes(v.snippetOf(k, r.Snippet, r.RowID, r.ContentType, r.FileName),
		replySnippetRunes)
	return j
}

func (v view) edits(k kinds, m logdb.Message) []editJSON {
	out := []editJSON{}
	for i, e := range m.Edits {
		out = append(out, editJSON{At: iso(e.At), Text: e.Text,
			Entities:    v.messageEntities(k, m, e.Text, e.Entities),
			ContentType: chatType(e.ContentType), Media: v.media(e.ContentType, e.File, e.Extra),
			Detail: v.editDetail(k, m, i)})
	}
	return out
}

func (v view) chatMessage(n names, k kinds, group bool, chatID int64, m logdb.Message) chatMessageJSON {
	j := chatMessageJSON{
		ID: m.RowID, MessageID: m.MessageID, Date: iso(m.Date), Scope: scopeName(group),
		Sender: v.sender(n, m), ContentType: chatType(m.ContentType), Text: v.text(k, m),
		Entities: v.messageEntities(k, m, m.Text, m.Entities),
		Media:    v.media(m.ContentType, m.File, m.Extra), Album: []chatMediaJSON{},
		ReplyTo: v.reply(n, k, chatID, m), Forward: v.forwardName(n, m), Edits: v.edits(k, m),
		DeletedAt: isoPtr(m.DeletedAt), DeletedOnRecheck: onRecheck(m),
		messageExtraJSON: v.messageExtra(n, m), messageKindsJSON: v.messageKinds(k, m),
	}
	v.poster(j.Media, m)
	if m.EditDate > 0 {
		j.EditedAt = isoPtr(m.EditDate)
	}
	j.LinkPreview = v.linkPreview(k, m)
	return j
}

// chatMessages turns a window's rows into messages, folding each run
// of two or more consecutive rows of one album into one message, as
// Browse.cpp's chatHistory does: the first item's ids, date and sender,
// its media and the others' as the album, the caption (and its edits)
// of the item that has one, the first reply and forward found. Such a
// message is deleted only when all its items are; each item's media
// says its own message, kind and deletion, since an album may mix
// photos and videos (or documents, or audio) and lose items one by one.
func (v view) chatMessages(n names, kd kinds, group bool, chatID int64, ms []logdb.Message) []chatMessageJSON {
	out := []chatMessageJSON{}
	for i := 0; i < len(ms); {
		k := i + 1
		if a := ms[i].AlbumID; a != 0 {
			for k < len(ms) && ms[k].AlbumID == a {
				k++
			}
		}
		j := v.chatMessage(n, kd, group, chatID, ms[i])
		if k-i > 1 {
			var lastDeleted int64
			allDeleted, anyRecheck := true, false
			for x := i; x < k; x++ {
				item := v.chatMessage(n, kd, group, chatID, ms[x])
				if md := item.Media; md != nil {
					md.MessageID, md.Kind, md.DeletedAt = ms[x].MessageID, item.ContentType,
						item.DeletedAt
					md.DeletedOnRecheck = item.DeletedOnRecheck
					if x == i {
						j.Media = md
					} else {
						j.Album = append(j.Album, *md)
					}
				}
				if j.Text == "" && item.Text != "" {
					j.Text, j.Entities, j.Edits, j.EditedAt = item.Text, item.Entities, item.Edits,
						item.EditedAt
				}
				if j.ReplyTo == nil {
					j.ReplyTo = item.ReplyTo
				}
				if j.Forward == nil {
					j.Forward, j.ForwardFrom = item.Forward, item.ForwardFrom
				}
				if ms[x].DeletedAt == 0 {
					allDeleted = false
				}
				lastDeleted = max(lastDeleted, ms[x].DeletedAt)
				anyRecheck = anyRecheck || onRecheck(ms[x])
			}
			j.DeletedAt, j.DeletedOnRecheck = nil, false
			if allDeleted {
				j.DeletedAt, j.DeletedOnRecheck = isoPtr(lastDeleted), anyRecheck
			}
		}
		out = append(out, j)
		i = k
	}
	return out
}

func (v view) window(c logdb.Chat, w logdb.Window, k kinds) chatWindowJSON {
	return chatWindowJSON{
		Messages: v.chatMessages(names{w.Users, w.Groups}, k, c.Group, c.ID, w.Messages),
		HasOlder: w.HasOlder, HasNewer: w.HasNewer,
	}
}

// The window sizes of getChatMessages.
const (
	chatDefaultLimit = 60
	chatMaxLimit     = 100
)

var errBadAnchor = errors.New("at most one of before, after, around and after_ts")

// chatAnchor reads before, after, around and limit; limit is clamped
// to 1..chatMaxLimit, as the Drogon API clamps it.
func chatAnchor(q map[string][]string) (logdb.Anchor, int, error) {
	var (
		a   logdb.Anchor
		set int
	)
	for _, p := range []struct {
		name string
		dst  **int64
	}{{"before", &a.Before}, {"after", &a.After}, {"around", &a.Around}} {
		vs := q[p.name]
		if len(vs) == 0 || vs[0] == "" {
			continue
		}
		n, err := strconv.ParseInt(vs[0], 10, 64)
		if err != nil {
			return a, 0, errors.New(p.name + " is not a message id")
		}
		*p.dst = &n
		set++
	}
	if set > 1 {
		return a, 0, errBadAnchor
	}
	limit := chatDefaultLimit
	if vs := q["limit"]; len(vs) > 0 && vs[0] != "" {
		n, err := strconv.Atoi(vs[0])
		if err != nil {
			return a, 0, errors.New("limit is not a number")
		}
		limit = min(max(n, 1), chatMaxLimit)
	}
	return a, limit, nil
}

// chatTopic reads ?topic=, a forum topic of a group: 0 without one.
func chatTopic(v string, c logdb.Chat) (int64, error) {
	if v == "" {
		return 0, nil
	}
	n, err := strconv.ParseInt(v, 10, 64)
	if err != nil || n <= 0 {
		return 0, errors.New("topic is not a topic id")
	}
	if !c.Group {
		return 0, errors.New("a private chat has no topics")
	}
	return n, nil
}

// errServer wraps a database error of chatTimeAnchor.
var errServer = errors.New("server error")

// chatTimeAnchor reads after_ts, a unix time: the window starts at the
// first message sent then or later, as the Drogon /v1 chat API's
// after_ts did, and is the newest messages when none is that recent.
// It takes the place of before, after and around, so with one of them
// it is a bad request.
func (s *Server) chatTimeAnchor(r *http.Request, c logdb.Chat, v string, a *logdb.Anchor) error {
	if v == "" {
		return nil
	}
	ts, err := strconv.ParseInt(v, 10, 64)
	if err != nil {
		return errors.New("after_ts is not a unix time")
	}
	if a.Before != nil || a.After != nil || a.Around != nil {
		return errBadAnchor
	}
	id, ok, err := c.DateAnchor(r.Context(), s.ro, ts)
	if err != nil {
		return fmt.Errorf("%w: %w", errServer, err)
	}
	if ok {
		after := id - 1
		a.After = &after
	}
	return nil
}

// optionalSession is the session of a request that signed-out visitors
// may make too: nil without one. A gate of a signed-in account answers
// as RequireSession answers it; ok is false when the answer is sent.
func (s *Server) optionalSession(w http.ResponseWriter, r *http.Request) (sess *auth.Session, sc scope.Scope, ok bool) {
	ps, valid := s.pageSession(w, r)
	if !valid {
		noStore(w)
		writeError(w, http.StatusServiceUnavailable, "unavailable", nil)
		return nil, sc, false
	}
	sc = s.requestScope(r, ps)
	if g := sc.Gate(); g != scope.None {
		noStore(w)
		writeError(w, http.StatusForbidden, string(g), map[string]any{"message": gateMessages[g]})
		return nil, sc, false
	}
	return ps, sc, true
}

// getChatMessages is GET /api/chats/{scope}/{id}/messages. Signed-out
// visitors may read the chats of exposed groups, as the public chat
// page does; for anything else they get 401, whether it exists or not,
// so they learn nothing about it. A signed-in viewer gets 404 for what
// its scope hides. Visitors are answered from a cache they share
// (publicwindow.go), since their live pages all refetch the same
// windows at once.
func (s *Server) getChatMessages(w http.ResponseWriter, r *http.Request) {
	sess, sc, ok := s.optionalSession(w, r)
	if !ok {
		return
	}
	noStore(w)
	if sess != nil {
		s.chatMessages(w, r, sess, sc)
		return
	}
	key := windowKey(r.PathValue("scope"), r.PathValue("id"), r.URL.Query())
	s.public().window.serve(w, r, key, func(w http.ResponseWriter, r *http.Request) {
		s.chatMessages(w, r, nil, sc)
	})
}

// chatMessages answers a chat window for getChatMessages; sess is nil
// for a signed-out visitor.
func (s *Server) chatMessages(w http.ResponseWriter, r *http.Request, sess *auth.Session, sc scope.Scope) {
	hidden := func() {
		if sess == nil {
			writeError(w, http.StatusUnauthorized, "unauthenticated", nil)
			return
		}
		notFound(w)
	}
	kind := r.PathValue("scope")
	id, err := strconv.ParseInt(r.PathValue("id"), 10, 64)
	if (kind != "group" && kind != "private") || err != nil {
		hidden()
		return
	}
	c, err := logdb.OpenChat(r.Context(), s.ro, sc, kind == "group", id)
	if errors.Is(err, logdb.ErrNotFound) {
		hidden()
		return
	}
	if err != nil {
		s.internalError(w, "chat lookup", err)
		return
	}
	q := r.URL.Query()
	a, limit, err := chatAnchor(q)
	if err == nil {
		c.Topic, err = chatTopic(q.Get("topic"), c)
	}
	if err == nil {
		err = s.chatTimeAnchor(r, c, q.Get("after_ts"), &a)
	}
	if err != nil {
		if errors.Is(err, errServer) {
			s.internalError(w, "chat time anchor", err)
			return
		}
		badRequest(w, "bad_request", err.Error())
		return
	}
	win, err := c.Window(r.Context(), s.ro, a, limit)
	if err != nil {
		s.internalError(w, "chat window", err)
		return
	}
	k, err := s.loadKinds(r.Context(), sc, c.ID, win.Messages)
	if err != nil {
		s.internalError(w, "chat window kinds", err)
		return
	}
	s.rechecks.window(c.ID, a, win, sess != nil)
	writeJSON(w, http.StatusOK, s.viewFor(sc, sess != nil).window(c, win, k))
}

// entitiesMax is how many ids each list of GET /api/entities takes.
const entitiesMax = 200

func parseIDs(s string) ([]int64, bool) {
	if s == "" {
		return nil, true
	}
	parts := strings.Split(s, ",")
	if len(parts) > entitiesMax {
		return nil, false
	}
	ids := make([]int64, 0, len(parts))
	for _, p := range parts {
		if p == "" {
			continue
		}
		n, err := strconv.ParseInt(p, 10, 64)
		if err != nil {
			return nil, false
		}
		ids = append(ids, n)
	}
	return ids, true
}

type entitiesJSON struct {
	Users  []userSummaryJSON  `json:"users"`
	Groups []groupSummaryJSON `json:"groups"`
}

// getEntities is GET /api/entities: the summaries of the users and
// groups asked for that the scope sees; the others are left out.
func (s *Server) getEntities(w http.ResponseWriter, r *http.Request) {
	noStore(w)
	sc := ScopeFrom(r.Context())
	q := r.URL.Query()
	uids, ok1 := parseIDs(q.Get("users"))
	gids, ok2 := parseIDs(q.Get("groups"))
	if !ok1 || !ok2 {
		badRequest(w, "bad_request", "At most 200 numeric ids per list.")
		return
	}
	us, err := logdb.VisibleUsers(r.Context(), s.ro, sc, uids)
	if err != nil {
		s.internalError(w, "entities", err)
		return
	}
	gs, err := logdb.VisibleGroups(r.Context(), s.ro, sc, gids)
	if err != nil {
		s.internalError(w, "entities", err)
		return
	}
	v := s.viewFor(sc, true)
	out := entitiesJSON{Users: []userSummaryJSON{}, Groups: []groupSummaryJSON{}}
	for _, u := range us {
		out.Users = append(out.Users, v.user(u))
	}
	for _, g := range gs {
		out.Groups = append(out.Groups, v.group(g))
	}
	writeJSON(w, http.StatusOK, out)
}
