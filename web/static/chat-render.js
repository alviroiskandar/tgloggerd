/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * tgloggerd web — client-side renderer for /v1 chat messages. It mirrors the
 * server template (web/views/templates/chat.html) so an infinite-scroll /
 * live-appended message is byte-identical to a server-rendered one, which lets
 * the lightbox, sticker observer and date badge work on both the same way.
 *
 * Every string field in the /v1 JSON is already HTML-escaped by the server
 * (names, text, snippets, ...), so it is inserted verbatim -- exactly as the
 * inja template does. File URLs arrive pre-tokenized (media.url, photo_url,
 * item.media.url, edit.url) because the client has no key to mint them.
 *
 * Exposes window.ChatRender.messages(list) -> HTML string.
 */
(function () {
	"use strict";

	function avatar(s) {
		if (s && s.photo_url) {
			if (s.kind === "user")
				return '<a href="/users/' + s.id + '"><img class="avatar-sm msg-avatar" src="' + s.photo_url + '" alt=""></a>';
			if (s.kind === "group")
				return '<a href="/groups/' + s.id + '"><img class="avatar-sm msg-avatar" src="' + s.photo_url + '" alt=""></a>';
			return '<img class="avatar-sm msg-avatar" src="' + s.photo_url + '" alt="">';
		}
		return '<span class="avatar-sm placeholder msg-avatar"></span>';
	}

	function senderLine(m) {
		if (!m.show_sender)
			return "";
		var s = m.sender, inner;
		if (s.kind === "user") {
			inner = '<a href="/users/' + s.id + '">' + s.name + "</a>";
			if (s.username)
				inner += ' <span class="muted">@' + s.username + "</span>";
		} else if (s.kind === "group") {
			inner = '<a href="/groups/' + s.id + '">' + s.name + "</a>";
		} else {
			inner = s.name;
		}
		return '<div class="msg-sender">' + inner + "</div>";
	}

	function forward(m) {
		if (!m.forward)
			return "";
		var f = m.forward, inner;
		if (f.user_id !== undefined)
			inner = '<a href="/users/' + f.user_id + '">' + (f.sender_name !== "" ? f.sender_name : "user #" + f.user_id) + "</a>";
		else if (f.chat_id !== undefined)
			inner = '<a href="/groups/' + f.chat_id + '">' + (f.sender_name !== "" ? f.sender_name : "chat #" + f.chat_id) + "</a>";
		else if (f.sender_name !== "")
			inner = '<span class="fwd-name">' + f.sender_name + "</span>";
		else
			inner = '<span class="fwd-name">' + f.type + "</span>";
		return '<div class="fwd"><span class="fwd-label muted">Forwarded from</span> ' + inner + "</div>";
	}

	function reply(m) {
		if (!m.reply)
			return "";
		var r = m.reply, body;
		if (r.in_chat) {
			var name = r.sender_name !== "" ? r.sender_name : "In reply to";
			var text = r.deleted
				? '<span class="muted">(deleted message)</span>'
				: (r.snippet !== "" ? r.snippet : '<span class="muted">' + r.content_type + "</span>");
			body = '<span class="reply-name">' + name + '</span><span class="reply-text">' + text + "</span>";
		} else {
			body = '<span class="reply-name">In reply to</span><span class="reply-text muted">message #' + r.msg_id + "</span>";
		}
		return '<a class="reply-quote" href="' + r.href + '">' + body + "</a>";
	}

	function singleMedia(m) {
		if (!m.media)
			return "";
		var md = m.media, u = md.url;
		switch (md.render) {
		case "image":
			return '<a class="msg-media-link zoomable" data-zoom="image" data-src="' + u + '" href="' + u + '"><img class="msg-media" src="' + u + '" alt="' + m.content_type + '" loading="lazy"></a>';
		case "video":
			return '<a class="media-thumb zoomable" data-zoom="video" data-src="' + u + '" href="' + u + '"><video class="msg-media" src="' + u + '" preload="metadata" muted playsinline></video><span class="play-badge" aria-hidden="true"></span></a>';
		case "animation":
			return '<a class="media-thumb zoomable" data-zoom="animation" data-src="' + u + '" href="' + u + '"><video class="msg-media" src="' + u + '" autoplay loop muted playsinline preload="metadata"></video></a>';
		case "lottie":
			return '<div class="msg-media tgs-sticker zoomable" data-zoom="lottie" data-tgs="' + u + '"><a class="msg-file" href="' + u + '">🎞️ ' + (md.name !== "" ? md.name : "animated sticker") + "</a></div>";
		case "audio":
			return '<audio class="msg-audio" src="' + u + '" controls preload="none"></audio>';
		default:
			return '<a class="msg-file" href="' + u + '">📎 ' + (md.name !== "" ? md.name : m.content_type + " file") + (md.size !== undefined ? ' <span class="muted">(' + md.size + " bytes)</span>" : "") + "</a>";
		}
	}

	function albumItem(it) {
		if (it.is_deleted)
			return '<span class="album-item wide deleted" id="msg-' + it.msg_id + '"><span class="muted">(deleted)</span></span>';
		if (!it.media)
			return "";
		var u = it.media.url;
		switch (it.media.render) {
		case "image":
			return '<a class="album-item zoomable" id="msg-' + it.msg_id + '" data-zoom="image" data-src="' + u + '" href="' + u + '"><img src="' + u + '" alt="' + it.content_type + '" loading="lazy"></a>';
		case "video":
			return '<a class="album-item media-thumb zoomable" id="msg-' + it.msg_id + '" data-zoom="video" data-src="' + u + '" href="' + u + '"><video src="' + u + '" preload="metadata" muted playsinline></video><span class="play-badge" aria-hidden="true"></span></a>';
		case "animation":
			return '<a class="album-item media-thumb zoomable" id="msg-' + it.msg_id + '" data-zoom="animation" data-src="' + u + '" href="' + u + '"><video src="' + u + '" autoplay loop muted playsinline preload="metadata"></video></a>';
		case "audio":
			return '<audio class="album-item wide" id="msg-' + it.msg_id + '" src="' + u + '" controls preload="none"></audio>';
		default:
			return '<a class="album-item wide msg-file" id="msg-' + it.msg_id + '" href="' + u + '">📎 ' + (it.media.name !== "" ? it.media.name : it.content_type + " file") + "</a>";
		}
	}

	function edits(m, withFile) {
		if (!m.is_edited)
			return "";
		var inner;
		if (!m.edits || m.edits.length === 0) {
			inner = '<p class="muted">No prior versions recorded.</p>';
		} else {
			inner = '<ol class="edit-list">' + m.edits.map(function (e) {
				var when = '<div class="muted edit-when">' + (e.edit_date !== "" ? e.edit_date : "—") + "</div>";
				var body;
				if (e.text !== "") {
					body = '<div class="edit-text">' + e.text + "</div>";
				} else {
					var file = (withFile && e.file_id !== undefined)
						? ', <a href="' + e.url + '">file #' + e.file_id + "</a>" : "";
					body = '<div class="muted">(' + e.content_type + file + ")</div>";
				}
				return "<li>" + when + body + "</li>";
			}).join("") + "</ol>";
		}
		return '<details class="edit-history"><summary>edited</summary>' + inner + "</details>";
	}

	function service(m) {
		var who = "";
		var s = m.sender;
		if (s && s.name) {
			if (s.kind === "user")
				who = '<a class="service-who" href="/users/' + s.id + '">' + s.name + "</a> ";
			else if (s.kind === "group")
				who = '<a class="service-who" href="/groups/' + s.id + '">' + s.name + "</a> ";
			else
				who = '<span class="service-who">' + s.name + "</span> ";
		}
		var text = m.text !== "" ? m.text : (m.service_type || "");
		var time = '<span class="service-time muted"> · ' + (m.date !== "" ? m.date : "—") + "</span>";
		var del = m.is_deleted ? ' <span class="badge warn">deleted</span>' : "";
		return '<div class="msg-service" id="msg-' + m.msg_id + '" data-day="' + m.day + '"><span class="service-bubble">' + who + text + time + del + "</span></div>";
	}

	function album(m) {
		var av = m.is_outgoing ? "" : avatar(m.sender);
		var items = m.items.map(albumItem).join("");
		var text = m.text !== "" ? '<div class="msg-text">' + m.text + "</div>" : "";
		var badge = m.any_deleted ? ' <span class="badge warn">contains deleted</span>' : "";
		var meta = '<div class="msg-meta"><span class="msg-time">' + (m.date !== "" ? m.date : "—") +
			'</span><span class="muted">· album · ' + m.items.length + " items</span>" + edits(m, false) + badge + "</div>";
		return '<div class="msg ' + (m.is_outgoing ? "out" : "in") + '" data-day="' + m.day + '">' + av +
			'<div class="bubble">' + senderLine(m) + forward(m) + reply(m) +
			'<div class="album">' + items + "</div>" + text + meta + "</div></div>";
	}

	function normal(m) {
		var cls = "msg " + (m.is_outgoing ? "out" : "in") + (m.is_deleted ? " deleted" : "");
		var av = m.is_outgoing ? "" : avatar(m.sender);
		var sig = m.author_signature !== undefined ? '<div class="msg-sig muted">— ' + m.author_signature + "</div>" : "";
		var text = m.text !== "" ? '<div class="msg-text">' + m.text + "</div>" : "";
		var del = m.is_deleted
			? ' <span class="badge warn">deleted' + (m.deleted_at !== undefined ? " · " + m.deleted_at : "") + "</span>" : "";
		var meta = '<div class="msg-meta"><span class="msg-time">' + (m.date !== "" ? m.date : "—") + "</span>" + edits(m, true) + del + "</div>";
		return '<div class="' + cls + '" id="msg-' + m.msg_id + '" data-day="' + m.day + '">' + av +
			'<div class="bubble">' + senderLine(m) + forward(m) + reply(m) + singleMedia(m) + text + sig + meta + "</div></div>";
	}

	function one(m) {
		if (m.is_service)
			return service(m);
		if (m.is_album)
			return album(m);
		return normal(m);
	}

	window.ChatRender = {
		message: one,
		messages: function (list) {
			return list.map(one).join("");
		},
	};
}());
