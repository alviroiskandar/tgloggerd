// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */

package logdb

import (
	"context"
	"database/sql"
	"encoding/base64"
	"errors"
	"strconv"
	"strings"
)

// The bots of Discord to Telegram forwarding (daemon migration 000040,
// web/docs/platform-forwarding.md): Telegram bots (telegram_bots) send
// into Telegram chats, Discord bots (discord_bots) read Discord
// channels. discordd logs each enabled bot in and writes back its
// status; the web adds, renames, enables, disables and removes them.
//
// The ids of the two tables never meet: discord_bots counts from
// DiscordBotIDBase, so an id alone names one bot of one kind, and both
// are addressed as /api/bots/{id}.
//
// The token is write-only: it is written here and compared on a re-add,
// never selected for an answer.

// The kinds of bot.
const (
	KindTelegram = "telegram"
	KindDiscord  = "discord"
)

// DiscordBotIDBase is the first id of discord_bots.
const DiscordBotIDBase = 1000000000

// KindOf is the kind of bot an id names.
func KindOf(id uint64) string {
	if id >= DiscordBotIDBase {
		return KindDiscord
	}
	return KindTelegram
}

func botTable(kind string) string {
	if kind == KindDiscord {
		return "discord_bots"
	}
	return "telegram_bots"
}

// MaxBotName is the longest name, in characters (the columns').
const MaxBotName = 64

// ValidTelegramToken checks a Telegram bot token's shape:
// <bot user id>:<secret>, the secret 30 or more of [A-Za-z0-9_-].
// Whether it works is discordd's to find out when it logs the bot in.
func ValidTelegramToken(t string) bool {
	id, secret, ok := strings.Cut(t, ":")
	if !ok || id == "" || len(t) > 255 || len(secret) < 30 {
		return false
	}
	for _, c := range id {
		if c < '0' || c > '9' {
			return false
		}
	}
	return base64URLChars(secret)
}

// ValidDiscordToken checks a Discord bot token's shape: three
// dot-separated base64url parts, the first of which decodes to the
// bot's id, a snowflake.
func ValidDiscordToken(t string) bool {
	parts := strings.Split(t, ".")
	if len(t) > 255 || len(parts) != 3 {
		return false
	}
	for _, p := range parts {
		if p == "" || !base64URLChars(p) {
			return false
		}
	}
	raw, err := base64.RawURLEncoding.DecodeString(strings.TrimRight(parts[0], "="))
	if err != nil || len(raw) == 0 || len(raw) > 20 {
		return false
	}
	n, err := strconv.ParseUint(string(raw), 10, 64)
	return err == nil && n != 0 && strings.TrimLeft(string(raw), "0123456789") == ""
}

func base64URLChars(s string) bool {
	for _, c := range s {
		switch {
		case c >= 'A' && c <= 'Z', c >= 'a' && c <= 'z', c >= '0' && c <= '9', c == '-', c == '_':
		default:
			return false
		}
	}
	return true
}

// BotRow is a bot as the pages show it: never its token.
type BotRow struct {
	ID      uint64
	Kind    string
	Name    string
	Enabled bool
	// Status as stored: what discordd last saw, or disabled.
	Status       string
	StatusDetail sql.NullString
	StatusAt     sql.NullInt64 // unix seconds
	// The bot's own user id on its platform (0 until discordd has
	// logged it in) and username.
	UserID   string
	Username string
	Routes   int // Discord to Telegram routes using it
	// HasUser is whether the archive has the telegram_users row of a
	// Telegram bot's user id; never for a Discord bot.
	HasUser bool
	// Photo is a Telegram bot's own profile photo: that of the
	// telegram_users row of its user id, when the archive has one.
	// None for a Discord bot.
	Photo FileRef
}

func botQuery(kind, where string) string {
	col := "telegram_bot_id"
	if kind == KindDiscord {
		col = "discord_bot_id"
	}
	// The photo comes in the same query, so a list of bots costs one
	// query per kind however many there are.
	photo := "0, 0, NULL, NULL, NULL, NULL, NULL, NULL, NULL"
	join := ""
	if kind == KindTelegram {
		photo = "bu.id IS NOT NULL, COALESCE(bu.profile_photo_file_id, 0), " + fileColumnsOf("bf")
		join = " LEFT JOIN telegram_users bu ON b.bot_user_id <> 0 AND bu.id = b.bot_user_id" +
			" LEFT JOIN telegram_files bf ON bf.id = bu.profile_photo_file_id"
	}
	return "SELECT b.id, b.name, b.enabled, b.status, b.status_detail, " +
		"UNIX_TIMESTAMP(b.status_at), CAST(b.bot_user_id AS CHAR), b.username, " +
		"(SELECT COUNT(*) FROM discord_telegram_routes r WHERE r." + col + " = b.id), " +
		photo + " FROM " + botTable(kind) + " b" + join + where
}

func scanBots(rows *sql.Rows, kind string) ([]BotRow, error) {
	defer rows.Close()
	out := []BotRow{}
	for rows.Next() {
		b := BotRow{Kind: kind}
		var (
			at    sql.NullFloat64
			photo uint64
			f     nullFile
		)
		if err := rows.Scan(append([]any{&b.ID, &b.Name, &b.Enabled, &b.Status, &b.StatusDetail, &at,
			&b.UserID, &b.Username, &b.Routes, &b.HasUser, &photo}, f.dest()...)...); err != nil {
			return nil, err
		}
		b.Photo = f.ref(photo)
		if at.Valid {
			b.StatusAt = sql.NullInt64{Int64: int64(at.Float64), Valid: true}
		}
		if b.UserID == "0" {
			b.UserID = ""
		}
		out = append(out, b)
	}
	return out, rows.Err()
}

// ListBots is every bot of kind ("" for both), Telegram ones first,
// each by id.
func ListBots(ctx context.Context, db *sql.DB, kind string) ([]BotRow, error) {
	out := []BotRow{}
	for _, k := range []string{KindTelegram, KindDiscord} {
		if kind != "" && kind != k {
			continue
		}
		rows, err := db.QueryContext(ctx, botQuery(k, " ORDER BY b.id"))
		if err != nil {
			return nil, err
		}
		bs, err := scanBots(rows, k)
		if err != nil {
			return nil, err
		}
		out = append(out, bs...)
	}
	return out, nil
}

// GetBot is one bot by id, of the kind the id names.
func GetBot(ctx context.Context, db *sql.DB, id uint64) (BotRow, bool, error) {
	k := KindOf(id)
	rows, err := db.QueryContext(ctx, botQuery(k, " WHERE b.id = ?"), id)
	if err != nil {
		return BotRow{}, false, err
	}
	bs, err := scanBots(rows, k)
	if err != nil || len(bs) == 0 {
		return BotRow{}, false, err
	}
	return bs[0], true, nil
}

// ErrTokenInUse: another bot row holds the token.
var ErrTokenInUse = errors.New("the token is another bot's")

// ErrBotIDRange: a table handed out an id of the other kind's range;
// the bot is not kept, as it could not be addressed.
var ErrBotIDRange = errors.New("bot id outside its kind's range")

// BotByToken is the id of the bot of kind holding token, if any: for a
// re-add, which answers that bot to its owner.
func BotByToken(ctx context.Context, db *sql.DB, kind, token string) (uint64, bool, error) {
	var id uint64
	err := db.QueryRowContext(ctx, "SELECT id FROM "+botTable(kind)+" WHERE token = ?",
		token).Scan(&id)
	if errors.Is(err, sql.ErrNoRows) {
		return 0, false, nil
	}
	return id, err == nil, err
}

// CreateBot adds a bot, connecting: discordd logs it in on its next
// reload. ErrTokenInUse when the token is stored already.
func CreateBot(ctx context.Context, db *sql.DB, kind, name, token string) (uint64, error) {
	res, err := db.ExecContext(ctx, "INSERT INTO "+botTable(kind)+
		" (name, token, enabled, status) VALUES (?, ?, 1, 'connecting')", name, token)
	if isDuplicate(err) {
		return 0, ErrTokenInUse
	}
	if err != nil {
		return 0, err
	}
	id64, err := res.LastInsertId()
	if err != nil {
		return 0, err
	}
	id := uint64(id64)
	if KindOf(id) != kind {
		_, _ = db.ExecContext(ctx, "DELETE FROM "+botTable(kind)+" WHERE id = ?", id)
		return 0, ErrBotIDRange
	}
	return id, nil
}

// BotChange is what a PATCH changes; nil fields stay.
type BotChange struct {
	Name    *string
	Enabled *bool
	Token   *string
}

// UpdateBot applies a change. Switching a bot off sets it disabled;
// switching it on, or giving it a new token, sets it connecting with no
// detail, so discordd tries it once more on its next reload (a bot
// Discord refused is not retried otherwise). A new token also forgets
// the identity, which discordd learns again. ErrTokenInUse when
// another bot holds the token.
func UpdateBot(ctx context.Context, db *sql.DB, id uint64, c BotChange) error {
	var set []string
	var args []any
	if c.Name != nil {
		set = append(set, "name = ?")
		args = append(args, *c.Name)
	}
	if c.Token != nil {
		set = append(set, "token = ?", "bot_user_id = 0", "username = ''")
		args = append(args, *c.Token)
	}
	switch {
	case c.Enabled != nil && !*c.Enabled:
		set = append(set, "enabled = 0", "status = 'disabled'", "status_detail = NULL",
			"status_at = NOW()")
	case c.Enabled != nil || c.Token != nil:
		// A new token for a disabled bot keeps it off.
		on := "enabled"
		if c.Enabled != nil {
			on = "1"
		}
		set = append(set, "enabled = "+on,
			"status = IF("+on+", 'connecting', 'disabled')", "status_detail = NULL",
			"status_at = NOW()")
	}
	if len(set) == 0 {
		return nil
	}
	_, err := db.ExecContext(ctx, "UPDATE "+botTable(KindOf(id))+" SET "+
		strings.Join(set, ", ")+" WHERE id = ?", append(args, id)...)
	if isDuplicate(err) {
		return ErrTokenInUse
	}
	return err
}

// DeleteBot removes a bot that no route uses; the caller checks
// BotRow.Routes first, and the foreign keys refuse it anyway.
func DeleteBot(ctx context.Context, db *sql.DB, id uint64) error {
	_, err := db.ExecContext(ctx, "DELETE FROM "+botTable(KindOf(id))+" WHERE id = ?", id)
	return err
}

// AllDiscordBotIDs is every Discord bot, disabled ones too, for the
// assign-owner command.
func AllDiscordBotIDs(ctx context.Context, db *sql.DB) ([]int64, error) {
	return allIDs(ctx, db, "SELECT id FROM discord_bots ORDER BY id")
}

// ImportDiscordBot stores the bot of token as name, unless a bot holds
// the token already, and points every route that names no Discord bot
// at it, in one transaction. It returns the bot's id, whether it was
// created, and how many routes it was given.
func ImportDiscordBot(ctx context.Context, db *sql.DB, name, token string) (uint64, bool, int64, error) {
	tx, err := db.BeginTx(ctx, nil)
	if err != nil {
		return 0, false, 0, err
	}
	defer tx.Rollback()
	var id uint64
	created := false
	err = tx.QueryRowContext(ctx, "SELECT id FROM discord_bots WHERE token = ? FOR UPDATE",
		token).Scan(&id)
	switch {
	case errors.Is(err, sql.ErrNoRows):
		res, err := tx.ExecContext(ctx, "INSERT INTO discord_bots (name, token, enabled, status) "+
			"VALUES (?, ?, 1, 'connecting')", name, token)
		if err != nil {
			return 0, false, 0, err
		}
		n, err := res.LastInsertId()
		if err != nil {
			return 0, false, 0, err
		}
		id, created = uint64(n), true
		if KindOf(id) != KindDiscord {
			return 0, false, 0, ErrBotIDRange
		}
	case err != nil:
		return 0, false, 0, err
	}
	res, err := tx.ExecContext(ctx, "UPDATE discord_telegram_routes SET discord_bot_id = ? "+
		"WHERE discord_bot_id IS NULL", id)
	if err != nil {
		return 0, false, 0, err
	}
	n, err := res.RowsAffected()
	if err != nil {
		return 0, false, 0, err
	}
	return id, created, n, tx.Commit()
}

// UnresolvedBot is a Telegram bot whose identity is known but whose
// user the archive does not have.
type UnresolvedBot struct {
	ID       uint64
	UserID   int64
	Username string
}

// UnresolvedTelegramBots is every Telegram bot that has logged in (a
// user id and a username) and whose telegram_users row is missing, by
// id. Discord bots are never among them: their ids are no Telegram
// users.
func UnresolvedTelegramBots(ctx context.Context, db *sql.DB) ([]UnresolvedBot, error) {
	rows, err := db.QueryContext(ctx, "SELECT b.id, b.bot_user_id, b.username FROM telegram_bots b "+
		"LEFT JOIN telegram_users u ON u.id = b.bot_user_id "+
		"WHERE b.bot_user_id <> 0 AND b.username <> '' AND u.id IS NULL ORDER BY b.id")
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []UnresolvedBot
	for rows.Next() {
		var b UnresolvedBot
		if err := rows.Scan(&b.ID, &b.UserID, &b.Username); err != nil {
			return nil, err
		}
		out = append(out, b)
	}
	return out, rows.Err()
}
