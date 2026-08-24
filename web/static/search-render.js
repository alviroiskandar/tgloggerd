/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * tgloggerd web -- client renderer for /v1/search/<entity> result rows. It
 * mirrors the SSR markup in views/templates/search_page.html, driven by the
 * `cols` metadata and positional `rows` the API returns (like api2.php's
 * keys/data). Every string field is already HTML-escaped by the server, so it
 * is inserted verbatim; image cells (photo/filethumb) are a pre-tokenized
 * /files/<token> URL (or ""). The detail-page base ("/users", "/groups", ...)
 * is passed in; when it is empty (files) the id/thumbnail link to the row's
 * media URL instead of a detail page.
 *
 * Exposes window.SearchRender.rows(cols, rows, detailBase) -> table-body HTML.
 */
(function () {
	"use strict";

	var DASH = '<span class="muted">&mdash;</span>';
	var FILE_EMOJI = {
		video: "🎬", animation: "🎞️", sticker: "🌟",
		document: "📄", audio: "🎵", voice: "🎤"
	};

	function indexOfType(cols, type) {
		for (var i = 0; i < cols.length; i++)
			if (cols[i].type === type)
				return i;
		return -1;
	}
	function indexOfKey(cols, key) {
		for (var i = 0; i < cols.length; i++)
			if (cols[i].key === key)
				return i;
		return -1;
	}

	function fileThumb(url, type, stored) {
		var inner;
		if (type === "photo" && stored)
			inner = '<img class="avatar-sm" src="' + url +
				'" alt="" loading="lazy">';
		else
			inner = '<span class="file-icon">' +
				(FILE_EMOJI[type] || "📎") + '</span>';
		return '<td class="col-photo"><a href="' + url + '">' + inner + '</a></td>';
	}

	/* A "party" cell: {kind, id, name, username, photo(url)} -> a clickable
	 * avatar + name linking to the user/group detail page. */
	function party(p) {
		if (!p || typeof p !== "object")
			return '<td class="cell-party">' + DASH + '</td>';
		var av = p.photo
			? '<img class="avatar-sm" src="' + p.photo + '" alt="">'
			: '<span class="avatar-sm placeholder"></span>';
		var inner;
		if (p.kind === "user") {
			inner = '<a class="party" href="/users/' + p.id + '">' +
				av + ' ' + p.name + '</a>';
			if (p.username)
				inner += ' <span class="muted">@' + p.username + '</span>';
		} else if (p.kind === "group") {
			inner = '<a class="party" href="/groups/' + p.id + '">' +
				av + ' ' + p.name + '</a>';
		} else {
			inner = '<span class="party muted">' + av + ' ' + p.name + '</span>';
		}
		return '<td class="cell-party">' + inner + '</td>';
	}

	function cell(col, val, ctx) {
		switch (col.type) {
		case "party":
			return party(val);
		case "photo":
			return '<td class="col-photo"><a href="' + ctx.base + '/' + ctx.idVal + '">' +
				(val
					? '<img class="avatar-sm" src="' + val + '" alt="">'
					: '<span class="avatar-sm placeholder"></span>') +
				'</a></td>';
		case "filethumb":
			return fileThumb(val, ctx.fileType, ctx.stored);
		case "id":
			return '<td><a href="' +
				(ctx.base ? ctx.base + '/' + val : ctx.mediaUrl) +
				'">' + val + '</a></td>';
		case "name":
			return '<td class="cell-name"><a href="' + ctx.base + '/' + ctx.idVal + '">' +
				val + '</a></td>';
		case "username":
			return '<td>' + (val !== "" ? "@" + val : DASH) + '</td>';
		case "bool":
			return '<td class="cell-bool">' + (val
				? '<span class="bool-yes">Yes</span>'
				: '<span class="muted">No</span>') + '</td>';
		case "longtext":
			return '<td class="cell-long">' + (val !== "" ? val : DASH) + '</td>';
		case "fileid":
			return '<td class="cell-long cell-fileid">' + (val !== "" ? val : DASH) + '</td>';
		case "hash":
			return '<td class="cell-hash">' + (val !== "" ? val : DASH) + '</td>';
		default:
			return '<td class="nowrap">' + (val !== "" ? val : DASH) + '</td>';
		}
	}

	function row(cols, r, idx) {
		var ctx = {
			base:      idx.base,
			idVal:     r[idx.id],
			mediaUrl:  idx.photo >= 0 ? r[idx.photo] : "",
			fileType:  idx.type  >= 0 ? r[idx.type]  : "",
			stored:    idx.stored >= 0 ? r[idx.stored] : false
		};
		var s = "<tr>";
		for (var i = 0; i < cols.length; i++)
			s += cell(cols[i], r[i], ctx);
		return s + "</tr>";
	}

	function rows(cols, list, base) {
		if (!list || !list.length)
			return '<tr><td colspan="' + cols.length +
				'" class="muted">No results match this search.</td></tr>';
		var idIdx = indexOfType(cols, "id");
		var idx = {
			base:   base || "",
			id:     idIdx < 0 ? 1 : idIdx,
			photo:  Math.max(indexOfType(cols, "photo"), indexOfType(cols, "filethumb")),
			type:   indexOfKey(cols, "file_type"),
			stored: indexOfKey(cols, "stored")
		};
		var out = "";
		for (var i = 0; i < list.length; i++)
			out += row(cols, list[i], idx);
		return out;
	}

	window.SearchRender = { rows: rows };
}());
