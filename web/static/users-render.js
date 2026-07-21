/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * tgloggerd web -- client-side renderer for /v1/search/users rows. It mirrors
 * the SSR markup in views/templates/users.html so a JS-loaded page is identical
 * to a server-rendered one. Every string field in the JSON is already
 * HTML-escaped by the server (name, username, ...), so it is inserted verbatim;
 * file URLs arrive pre-tokenized as _photo_url (the browser has no key).
 *
 * Exposes window.UsersRender.rows(list) -> table-body HTML string.
 */
(function () {
	"use strict";

	function photo(r) {
		if (r._photo_url)
			return '<a href="/users/' + r.id + '"><img class="avatar-sm" src="' +
				r._photo_url + '" alt=""></a>';
		return '<span class="avatar-sm placeholder"></span>';
	}

	function badges(r) {
		var s = "";
		if (r.is_premium)  s += '<span class="badge">Premium</span>';
		if (r.is_verified) s += '<span class="badge">Verified</span>';
		if (r.is_scam)     s += '<span class="badge warn">Scam</span>';
		if (r.is_fake)     s += '<span class="badge warn">Fake</span>';
		return s;
	}

	function row(r) {
		var uname = (r.username && r.username !== "")
			? "@" + r.username
			: '<span class="muted">&mdash;</span>';
		return '<tr>' +
			'<td class="col-photo">' + photo(r) + '</td>' +
			'<td><a href="/users/' + r.id + '">' + r.id + '</a></td>' +
			'<td><a href="/users/' + r.id + '">' + r.name + '</a></td>' +
			'<td>' + uname + '</td>' +
			'<td>' + r.type + '</td>' +
			'<td>' + badges(r) + '</td>' +
			'<td class="nowrap">' + r.created_at + '</td>' +
			'<td class="nowrap"><a class="muted" href="/users/' + r.id +
				'/history">History</a></td>' +
			'</tr>';
	}

	function rows(list) {
		if (!list || !list.length)
			return '<tr><td colspan="8" class="muted">' +
				'No users match this search.</td></tr>';
		return list.map(row).join("");
	}

	window.UsersRender = { rows: rows, row: row };
}());
