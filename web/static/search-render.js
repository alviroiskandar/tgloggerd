/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * tgloggerd web -- client renderer for /v1/search/<entity> result rows. It
 * mirrors the SSR markup in views/templates/search_page.html, driven by the
 * `cols` metadata and positional `rows` the API returns (like api2.php's
 * keys/data). Every string field is already HTML-escaped by the server, so it
 * is inserted verbatim; the photo cell is a pre-tokenized /files/<token> URL
 * (or ""). Entity-agnostic: the detail-page base ("/users", "/groups", ...) is
 * passed in.
 *
 * Exposes window.SearchRender.rows(cols, rows, detailBase) -> table-body HTML.
 */
(function () {
	"use strict";

	var DASH = '<span class="muted">&mdash;</span>';

	function idIndexOf(cols) {
		for (var i = 0; i < cols.length; i++)
			if (cols[i].type === "id")
				return i;
		return 1;
	}

	function cell(col, val, idVal, base) {
		switch (col.type) {
		case "photo":
			return '<td class="col-photo"><a href="' + base + '/' + idVal + '">' + (val
				? '<img class="avatar-sm" src="' + val + '" alt="">'
				: '<span class="avatar-sm placeholder"></span>') + '</a></td>';
		case "id":
			return '<td><a href="' + base + '/' + val + '">' + val + '</a></td>';
		case "name":
			return '<td class="cell-name"><a href="' + base + '/' + idVal + '">' +
				val + '</a></td>';
		case "username":
			return '<td>' + (val !== "" ? "@" + val : DASH) + '</td>';
		case "bool":
			return '<td class="cell-bool">' + (val
				? '<span class="bool-yes">Yes</span>'
				: '<span class="muted">No</span>') + '</td>';
		case "longtext":
			return '<td class="cell-long">' + (val !== "" ? val : DASH) + '</td>';
		default:
			return '<td class="nowrap">' + (val !== "" ? val : DASH) + '</td>';
		}
	}

	function row(cols, r, idIdx, base) {
		var idVal = r[idIdx];
		var s = "<tr>";
		for (var i = 0; i < cols.length; i++)
			s += cell(cols[i], r[i], idVal, base);
		return s + "</tr>";
	}

	function rows(cols, list, base) {
		if (!list || !list.length)
			return '<tr><td colspan="' + cols.length +
				'" class="muted">No results match this search.</td></tr>';
		var idIdx = idIndexOf(cols);
		var out = "";
		for (var i = 0; i < list.length; i++)
			out += row(cols, list[i], idIdx, base);
		return out;
	}

	window.SearchRender = { rows: rows };
}());
