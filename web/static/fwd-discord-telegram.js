/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * tgloggerd web -- Discord -> Telegram routes admin page. Wires the select2
 * chat picker (AJAX to /platform-fwd/discord-telegram/chats), the add/edit form (POST /platform-fwd/discord-telegram/save)
 * and per-row edit/delete (POST /platform-fwd/discord-telegram/delete). All mutating POSTs carry the
 * session CSRF token.
 *
 * Note what this file never does: read or display a bot token. The server does
 * not send one, and editing a route reuses the bot by id, so a token only ever
 * travels browser -> server, never back.
 */
(function () {
	"use strict";
	if (typeof jQuery === "undefined")
		return;
	var $ = jQuery;
	var $root = $("#routes");
	if (!$root.length)
		return;
	var csrf = $root.attr("data-csrf");

	var $form    = $("#rt-form"),
	    $wrap    = $("#rt-form-wrap"),
	    $title   = $("#rt-form-title"),
	    $msg     = $("#rt-form-msg"),
	    $id      = $("#rt-f-id"),
	    $channel = $("#rt-f-channel"),
	    $chat    = $("#rt-f-chat"),
	    $bot     = $("#rt-f-bot"),
	    $token   = $("#rt-f-token"),
	    $enabled = $("#rt-f-enabled");

	$chat.select2({
		placeholder: "Search a group, channel, or user…",
		width: "100%",
		minimumInputLength: 1,
		ajax: {
			url: "/platform-fwd/discord-telegram/chats",
			dataType: "json",
			delay: 250,
			data: function (params) { return { q: params.term || "" }; },
			processResults: function (data) { return { results: data.results }; },
			cache: true
		}
	});

	function setMsg(text, kind) {
		$msg.text(text || "").attr("class", "rt-form-msg" + (kind ? " " + kind : ""));
	}

	function openForm(mode, row) {
		setMsg("");
		/* Never prefilled: the server does not disclose stored tokens. */
		$token.val("");
		if (mode === "edit" && row) {
			$title.text("Edit route");
			$id.val(row.id);
			$channel.val(row.channelId);
			var opt = new Option(row.chatTitle, row.chatId, true, true);
			$chat.empty().append(opt).trigger("change");
			$bot.val(row.botId);
			$enabled.prop("checked", String(row.enabled) === "1");
		} else {
			$title.text("Add route");
			$id.val("");
			$channel.val("");
			$chat.val(null).trigger("change");
			$bot.val("");
			$enabled.prop("checked", true);
		}
		$wrap.prop("hidden", false);
		$("html, body").animate({ scrollTop: $wrap.offset().top - 20 }, 150);
	}

	function postForm(url, params) {
		params.csrf = csrf;
		return fetch(url, {
			method: "POST",
			headers: { "Content-Type": "application/x-www-form-urlencoded" },
			body: new URLSearchParams(params).toString()
		}).then(function (r) {
			return r.json().catch(function () {
				return { ok: false, error: "Bad response (HTTP " + r.status + ")." };
			});
		}).catch(function () {
			return { ok: false, error: "Network error." };
		});
	}

	$("#rt-add").on("click", function () { openForm("add"); });
	$("#rt-cancel").on("click", function () { $wrap.prop("hidden", true); });

	$("#rt-body").on("click", ".rt-edit", function () {
		var $tr = $(this).closest("tr");
		openForm("edit", {
			id: $tr.attr("data-id"),
			channelId: $tr.attr("data-channel-id"),
			chatId: $tr.attr("data-chat-id"),
			chatTitle: $tr.attr("data-chat-title"),
			botId: $tr.attr("data-bot-id"),
			enabled: $tr.attr("data-enabled")
		});
	});

	$("#rt-body").on("click", ".rt-del", function () {
		var $tr = $(this).closest("tr");
		if (!window.confirm("Delete this route? Forwarding from this Discord channel will stop."))
			return;
		postForm("/platform-fwd/discord-telegram/delete", { id: $tr.attr("data-id") }).then(function (res) {
			if (res.ok)
				location.reload();
			else
				window.alert(res.error || "Could not delete.");
		});
	});

	$form.on("submit", function (e) {
		e.preventDefault();
		setMsg("Saving…");
		postForm("/platform-fwd/discord-telegram/save", {
			id: $id.val(),
			discord_channel_id: ($channel.val() || "").trim(),
			chat_id: $chat.val() || "",
			telegram_bot_id: $bot.val() || "",
			bot_token: $token.val() || "",
			enabled: $enabled.prop("checked") ? "1" : "0"
		}).then(function (res) {
			if (res.ok)
				location.reload();
			else
				setMsg(res.error || "Could not save.", "err");
		});
	});
}());
