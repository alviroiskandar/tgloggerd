/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * tgloggerd web -- Discord webhook integrations admin page. Wires the select2
 * chat picker (AJAX to /platform-fwd/telegram-discord/chats), the add/edit form (POST
 * /platform-fwd/telegram-discord/save), per-row edit/delete, and the "send test message" button
 * (POST /platform-fwd/telegram-discord/test). All mutating POSTs carry the session CSRF token.
 */
(function () {
	"use strict";
	if (typeof jQuery === "undefined")
		return;
	var $ = jQuery;
	var $root = $("#integrations");
	if (!$root.length)
		return;
	var csrf = $root.attr("data-csrf");

	var $form    = $("#int-form"),
	    $wrap    = $("#int-form-wrap"),
	    $title   = $("#int-form-title"),
	    $msg     = $("#int-form-msg"),
	    $id      = $("#int-f-id"),
	    $chat    = $("#int-f-chat"),
	    $url     = $("#int-f-url"),
	    $enabled = $("#int-f-enabled");

	$chat.select2({
		placeholder: "Search a group, channel, or user…",
		width: "100%",
		minimumInputLength: 1,
		ajax: {
			url: "/platform-fwd/telegram-discord/chats",
			dataType: "json",
			delay: 250,
			data: function (params) { return { q: params.term || "" }; },
			processResults: function (data) { return { results: data.results }; },
			cache: true
		}
	});

	function setMsg(text, kind) {
		$msg.text(text || "").attr("class", "int-form-msg" + (kind ? " " + kind : ""));
	}

	function openForm(mode, row) {
		setMsg("");
		if (mode === "edit" && row) {
			$title.text("Edit integration");
			$id.val(row.id);
			var opt = new Option(row.chatTitle + "  ·  " + row.chatType,
					     row.chatId, true, true);
			$chat.empty().append(opt).trigger("change");
			$url.val(row.url);
			$enabled.prop("checked", String(row.enabled) === "1");
		} else {
			$title.text("Add integration");
			$id.val("");
			$chat.val(null).trigger("change");
			$url.val("");
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

	$("#int-add").on("click", function () { openForm("add"); });
	$("#int-cancel").on("click", function () { $wrap.prop("hidden", true); });

	$("#int-body").on("click", ".int-edit", function () {
		var $tr = $(this).closest("tr");
		openForm("edit", {
			id: $tr.attr("data-id"),
			chatId: $tr.attr("data-chat-id"),
			chatType: $tr.attr("data-chat-type"),
			chatTitle: $tr.attr("data-chat-title"),
			url: $tr.attr("data-url"),
			enabled: $tr.attr("data-enabled")
		});
	});

	$("#int-body").on("click", ".int-del", function () {
		var $tr = $(this).closest("tr");
		if (!window.confirm("Delete this integration? Forwarding for this chat will stop."))
			return;
		postForm("/platform-fwd/telegram-discord/delete", { id: $tr.attr("data-id") }).then(function (res) {
			if (res.ok)
				location.reload();
			else
				window.alert(res.error || "Could not delete.");
		});
	});

	$form.on("submit", function (e) {
		e.preventDefault();
		setMsg("Saving…");
		postForm("/platform-fwd/telegram-discord/save", {
			id: $id.val(),
			chat_id: $chat.val() || "",
			webhook_url: $url.val(),
			enabled: $enabled.prop("checked") ? "1" : "0"
		}).then(function (res) {
			if (res.ok)
				location.reload();
			else
				setMsg(res.error || "Could not save.", "err");
		});
	});

	$("#int-test").on("click", function () {
		var url = $url.val();
		if (!url) { setMsg("Enter a webhook URL first.", "err"); return; }
		setMsg("Sending test…");
		postForm("/platform-fwd/telegram-discord/test", { webhook_url: url }).then(function (res) {
			setMsg(res.ok ? (res.message || "Sent.") : (res.error || "Test failed."),
			       res.ok ? "ok" : "err");
		});
	});
}());
