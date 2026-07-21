/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * tgloggerd web -- advanced-search UI for /users. Progressive enhancement over
 * the server-rendered first page: build a condition builder from the embedded
 * field registry, then drive sorting/paging/searching through the
 * /v1/search/users JSON API without full reloads, keeping the URL shareable
 * (so a reload SSR-reproduces the same page). Depends on jQuery (global) and
 * window.UsersRender.
 */
jQuery(function ($) {
	"use strict";

	var $page = $("#users-page");
	if (!$page.length || !window.UsersRender)
		return;

	var schema = [];
	try {
		schema = JSON.parse($("#search-schema").text() || "[]");
	} catch (e) {
		schema = [];
	}
	if (!schema.length)
		return;

	var byKey = {};
	schema.forEach(function (f) { byKey[f.key] = f; });

	var $body    = $("#results-body");
	var $pager   = $("#pager");
	var $meta    = $(".search-meta");
	var $builder = $("#search-builder");
	var api      = "/v1/search/users";
	var NCOLS    = 8;

	function attrNum(name, def) {
		var v = parseInt($page.attr(name), 10);
		return isNaN(v) ? def : v;
	}

	var urlp = new URLSearchParams(window.location.search);

	var state = {
		total:  attrNum("data-total", 0),
		limit:  attrNum("data-limit", 50),
		offset: attrNum("data-offset", 0),
		sort:   $page.attr("data-sort") || "",
		order:  $page.attr("data-order") || "desc",
		debug:  urlp.get("debug") === "1",
		conds:  []
	};
	var sraw = urlp.get("search");
	if (sraw) {
		try {
			var parsed = JSON.parse(sraw);
			if (Array.isArray(parsed))
				state.conds = parsed;
		} catch (e) { /* ignore */ }
	}

	/* ---- condition builder ------------------------------------------- */

	function isNullOp(o) { return o === "IS NULL" || o === "IS NOT NULL"; }

	function fillOps($op, field, selected) {
		$op.empty();
		field.operators.forEach(function (o) {
			$op.append($("<option>").val(o).text(o));
		});
		if (selected)
			$op.val(selected);
	}

	function buildRow(cond) {
		var $row = $('<div class="cond-row"></div>');
		var $col = $('<select class="cond-col" aria-label="Field"></select>');
		schema.forEach(function (f) {
			$col.append($("<option>").val(f.key).text(f.label));
		});
		var $op   = $('<select class="cond-op" aria-label="Operator"></select>');
		var $val  = $('<input class="cond-val" type="text" placeholder="value">');
		var $conn = $('<select class="cond-conn" aria-label="Connector">' +
			      '<option>AND</option><option>OR</option></select>');
		var $del  = $('<button type="button" class="cond-del" ' +
			      'title="Remove condition">&times;</button>');

		function syncVal() {
			var f = byKey[$col.val()];
			var nullish = isNullOp($op.val());
			$val.prop("disabled", nullish).css("visibility",
				nullish ? "hidden" : "visible");
			$val.attr("placeholder",
				(f && f.type === "enum" && f["enum"])
					? f["enum"].split(",").join(" | ") : "value");
		}

		$col.on("change", function () {
			fillOps($op, byKey[$col.val()]);
			syncVal();
		});
		$op.on("change", syncVal);

		$row.append($col, $op, $val, $conn, $del);

		if (cond && byKey[cond.c]) {
			$col.val(cond.c);
			fillOps($op, byKey[cond.c], cond.o);
			$val.val(cond.v || "");
			$conn.val(cond.n === "OR" ? "OR" : "AND");
		} else {
			fillOps($op, byKey[$col.val()]);
		}
		syncVal();

		$del.on("click", function () { $row.remove(); });
		return $row;
	}

	function renderBuilder() {
		$builder.empty();
		var $rows = $('<div class="cond-rows"></div>');
		if (state.conds.length)
			state.conds.forEach(function (c) { $rows.append(buildRow(c)); });
		else
			$rows.append(buildRow(null));

		var $bar = $('<div class="cond-bar"></div>');
		var $add = $('<button type="button" class="cond-add">+ Add condition</button>');
		var $go  = $('<button type="button" class="cond-apply">Search</button>');
		var $clr = $('<button type="button" class="cond-clear">Clear</button>');
		var $dbg = $('<label class="cond-debug"><input type="checkbox"> Debug</label>');
		$dbg.find("input").prop("checked", state.debug);

		$add.on("click", function () { $rows.append(buildRow(null)); });
		$go.on("click",  function () { apply(); });
		$clr.on("click", function () {
			state.conds = [];
			renderBuilder();
			state.offset = 0;
			load();
		});
		$dbg.find("input").on("change", function () {
			state.debug = this.checked;
		});

		$bar.append($add, $go, $clr, $dbg);
		$builder.append($rows, $bar);
	}

	function collect() {
		var out = [];
		$builder.find(".cond-row").each(function () {
			var $r = $(this);
			var c = $r.find(".cond-col").val();
			var o = $r.find(".cond-op").val();
			var v = $.trim($r.find(".cond-val").val());
			var n = $r.find(".cond-conn").val();
			if (!c || !o)
				return;
			if (!isNullOp(o) && v === "")
				return;
			var cond = { c: c, o: o, n: n };
			if (!isNullOp(o))
				cond.v = v;
			out.push(cond);
		});
		return out;
	}

	/* ---- fetch + render ---------------------------------------------- */

	function searchStr() {
		return state.conds.length ? JSON.stringify(state.conds) : "";
	}

	function syncUrl() {
		var u = new URL(window.location.href);
		var p = u.searchParams;
		var s = searchStr();
		if (s) p.set("search", s); else p.delete("search");
		if (state.offset) p.set("offset", state.offset); else p.delete("offset");
		if (state.sort) p.set("sort", state.sort); else p.delete("sort");
		p.set("order", state.order);
		p.set("limit", state.limit);
		if (state.debug) p.set("debug", "1"); else p.delete("debug");
		window.history.pushState(null, "", u.pathname + "?" + p.toString());
	}

	function renderMeta() {
		var pages = state.limit > 0
			? Math.max(1, Math.ceil(state.total / state.limit)) : 1;
		var cur = state.limit > 0 ? Math.floor(state.offset / state.limit) + 1 : 1;
		$meta.html('<span class="muted"><strong>' + state.total +
			"</strong> result" + (state.total === 1 ? "" : "s") +
			'</span> <span class="muted">· page ' + cur + " / " +
			pages + "</span>");
	}

	function pageBtn(label, target, opts) {
		opts = opts || {};
		var $b = $('<button type="button" class="pager-btn"></button>')
			.html(label);
		if (opts.active) $b.addClass("active");
		if (opts.disabled) $b.prop("disabled", true);
		else $b.on("click", function () { goTo(target); });
		return $b;
	}

	function renderPager() {
		var pages = state.limit > 0
			? Math.max(1, Math.ceil(state.total / state.limit)) : 1;
		var cur = state.limit > 0 ? Math.floor(state.offset / state.limit) : 0;
		$pager.empty();
		if (pages <= 1)
			return;
		$pager.append(pageBtn("&laquo; First", 0, { disabled: cur === 0 }));
		$pager.append(pageBtn("Prev", cur - 1, { disabled: cur === 0 }));

		var chunk = 10;
		var start = Math.floor(cur / chunk) * chunk;
		var end = Math.min(start + chunk, pages);
		for (var i = start; i < end; i++)
			$pager.append(pageBtn(String(i + 1), i, { active: i === cur }));

		$pager.append(pageBtn("Next", cur + 1, { disabled: cur >= pages - 1 }));
		$pager.append(pageBtn("Last &raquo;", pages - 1,
			{ disabled: cur >= pages - 1 }));
	}

	function renderDebug(d) {
		$(".debug-panel").remove();
		if (!d || !d.debug)
			return;
		var $p = $('<details class="debug-panel" open>' +
			'<summary>Debug (SQL / bind / EXPLAIN)</summary></details>');
		$p.append($("<pre>").text(JSON.stringify(d.debug, null, 2)));
		$pager.after($p);
	}

	function goTo(pageIdx) {
		state.offset = Math.max(0, pageIdx) * state.limit;
		load();
	}

	function apply() {
		state.conds = collect();
		state.offset = 0;
		load();
	}

	function load() {
		var params = { limit: state.limit, offset: state.offset,
			       order: state.order };
		if (state.sort) params.sort = state.sort;
		params.search = searchStr();
		if (state.debug) params.debug = 1;

		$body.html('<tr><td colspan="' + NCOLS +
			'" class="muted">Loading&hellip;</td></tr>');

		$.getJSON(api, params).done(function (d) {
			state.total  = d.total;
			state.limit  = d.limit;
			state.offset = d.offset;
			state.sort   = d.sort || "";
			state.order  = d.order || "desc";
			$body.html(window.UsersRender.rows(d.rows));
			renderMeta();
			renderPager();
			renderDebug(d);
			markSort();
			syncUrl();
		}).fail(function (xhr) {
			var msg = (xhr.responseJSON && xhr.responseJSON.error) ||
				"Request failed";
			$body.html('<tr><td colspan="' + NCOLS +
				'" class="search-error">' + $("<i>").text(msg).html() +
				"</td></tr>");
		});
	}

	/* ---- sortable headers -------------------------------------------- */

	function markSort() {
		$("th[data-sort-key]").each(function () {
			var $th = $(this);
			var key = $th.attr("data-sort-key");
			$th.toggleClass("sort-active", key === state.sort);
			$th.attr("data-dir", key === state.sort ? state.order : "");
		});
	}

	$("table.grid thead").on("click", "th[data-sort-key]", function () {
		var key = $(this).attr("data-sort-key");
		if (state.sort === key)
			state.order = (state.order === "asc") ? "desc" : "asc";
		else {
			state.sort = key;
			state.order = "asc";
		}
		state.offset = 0;
		load();
	});

	window.addEventListener("popstate", function () {
		window.location.reload();
	});

	/* ---- init: SSR already rendered the first page; just enhance ------ */
	renderBuilder();
	renderPager();
	markSort();
});
