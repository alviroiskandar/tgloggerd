/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * tgloggerd web -- advanced-search UI for the listing pages (/users, /groups,
 * ...). Progressive enhancement over the server-rendered first page: a
 * condition builder driven by the embedded field registry, then
 * sorting/paging/searching through the entity's /v1/search/<entity> JSON API
 * (cols + positional rows) without full reloads, keeping the URL shareable. The
 * API URL and detail-page base come from the page's data-api/data-detail-base.
 * Depends on jQuery (global) and window.SearchRender.
 */
jQuery(function ($) {
	"use strict";

	var $page = $("#search-page");
	if (!$page.length || !window.SearchRender)
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

	var $body      = $("#results-body");
	var $meta      = $(".search-meta");
	var $builder   = $("#search-builder");
	var api        = $page.attr("data-api") || "/v1/search/users";
	var detailBase = $page.attr("data-detail-base") || "/users";
	var NCOLS      = $("#results-table thead th").length || 20;

	function attrNum(name, def) {
		var v = parseInt($page.attr(name), 10);
		return isNaN(v) ? def : v;
	}
	function trim(v) { return ("" + (v == null ? "" : v)).trim(); }

	var urlp = new URLSearchParams(window.location.search);
	var state = {
		total:     attrNum("data-total", 0),
		limit:     attrNum("data-limit", 50),
		offset:    attrNum("data-offset", 0),
		maxOffset: attrNum("data-max-offset", 500000),
		sort:      $page.attr("data-sort") || "",
		order:     $page.attr("data-order") || "desc",
		debug:     urlp.get("debug") === "1",
		conds:     []
	};

	/*
	 * Total pages, capped at the deepest reachable page. The API clamps
	 * offset to maxOffset, so pages past that would return the same rows;
	 * capping keeps every page button on a distinct offset.
	 */
	function totalPages() {
		if (state.limit <= 0)
			return 1;
		var p = Math.max(1, Math.ceil(state.total / state.limit));
		var reach = Math.floor(state.maxOffset / state.limit) + 1;
		return Math.min(p, reach);
	}
	var sraw = urlp.get("search");
	if (sraw) {
		try {
			var parsed = JSON.parse(sraw);
			if (Array.isArray(parsed))
				state.conds = parsed;
		} catch (e) { /* ignore */ }
	}

	function isNullOp(o) { return o === "IS NULL" || o === "IS NOT NULL"; }

	/* ---- condition builder ------------------------------------------- */

	function fillOps($op, field, selected) {
		$op.empty();
		field.operators.forEach(function (o) {
			$op.append($("<option>").val(o).text(o));
		});
		if (selected)
			$op.val(selected);
	}

	/* The value control adapts to the field type: a Yes/No dropdown for a
	 * boolean, a value dropdown for an enum, nothing for IS [NOT] NULL, else
	 * a text input. */
	function valueControl(field, op, preset) {
		if (isNullOp(op))
			return $('<span class="cond-val cond-val-none muted">(no value)</span>');
		var $c;
		if (field && field.type === "bool") {
			$c = $('<select class="cond-val">' +
			       '<option value="1">Yes</option>' +
			       '<option value="0">No</option></select>');
		} else if (field && field.type === "enum" && field["enum"]) {
			$c = $('<select class="cond-val"></select>');
			field["enum"].split(",").forEach(function (o) {
				$c.append($("<option>").val(o).text(o));
			});
		} else {
			$c = $('<input class="cond-val" type="text" placeholder="value">');
		}
		if (preset !== undefined && preset !== null)
			$c.val(preset);
		return $c;
	}

	function buildRow(cond) {
		var $row  = $('<div class="cond-row"></div>');
		var $col  = $('<select class="cond-col" aria-label="Field"></select>');
		schema.forEach(function (f) {
			$col.append($("<option>").val(f.key).text(f.label));
		});
		var $op   = $('<select class="cond-op" aria-label="Operator"></select>');
		var $conn = $('<select class="cond-conn" aria-label="Connector">' +
			      '<option>AND</option><option>OR</option></select>');
		var $del  = $('<button type="button" class="cond-del" ' +
			      'title="Remove condition">&times;</button>');
		var $val  = $('<input class="cond-val" type="text">');

		function rebuildVal(preset) {
			var $nv = valueControl(byKey[$col.val()], $op.val(), preset);
			$val.replaceWith($nv);
			$val = $nv;
		}
		$col.on("change", function () {
			fillOps($op, byKey[$col.val()]);
			rebuildVal();
		});
		$op.on("change", function () { rebuildVal(); });

		$row.append($col, $op, $val, $conn, $del);

		if (cond && byKey[cond.c]) {
			$col.val(cond.c);
			fillOps($op, byKey[cond.c], cond.o);
			rebuildVal(cond.v);
			$conn.val(cond.n === "OR" ? "OR" : "AND");
		} else {
			fillOps($op, byKey[$col.val()]);
			rebuildVal();
		}
		$del.on("click", function () { $row.remove(); });
		return $row;
	}

	function renderBuilder() {
		$builder.empty();

		/* Toolbar on top, so adding a condition never shifts it down. */
		var $bar = $('<div class="cond-bar"></div>');
		var $add = $('<button type="button" class="cond-add">+ Add condition</button>');
		var $go  = $('<button type="button" class="cond-apply">Search</button>');
		var $clr = $('<button type="button" class="cond-clear">Clear</button>');
		var $dbg = $('<label class="cond-debug"><input type="checkbox"> Debug</label>');
		$dbg.find("input").prop("checked", state.debug);

		var $rows = $('<div class="cond-rows"></div>');
		if (state.conds.length)
			state.conds.forEach(function (c) { $rows.append(buildRow(c)); });
		else
			$rows.append(buildRow(null));

		$add.on("click", function () { $rows.append(buildRow(null)); });
		$go.on("click",  function () { apply(); });
		$clr.on("click", function () {
			state.conds = [];
			renderBuilder();
			state.offset = 0;
			load();
		});
		$dbg.find("input").on("change", function () { state.debug = this.checked; });

		$bar.append($add, $go, $clr, $dbg);
		$builder.append($bar, $rows);
	}

	function collect() {
		var out = [];
		$builder.find(".cond-row").each(function () {
			var $r = $(this);
			var c = $r.find(".cond-col").val();
			var o = $r.find(".cond-op").val();
			var $v = $r.find(".cond-val");
			var v = $v.is("input, select") ? trim($v.val()) : "";
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
		var pages = totalPages();
		var cur = state.limit > 0 ? Math.floor(state.offset / state.limit) + 1 : 1;
		$("#meta-text").html('<span class="muted"><strong>' + state.total +
			"</strong> result" + (state.total === 1 ? "" : "s") +
			'</span> <span class="muted">· page ' + cur + " / " +
			pages + "</span>");
	}

	function pagerHtml() {
		var pages = totalPages();
		var cur = state.limit > 0 ? Math.floor(state.offset / state.limit) : 0;
		if (pages <= 1)
			return "";
		function btn(label, target, opts) {
			opts = opts || {};
			var cls = "pager-btn" + (opts.active ? " active" : "");
			if (opts.disabled)
				return '<button class="' + cls + '" disabled>' + label + "</button>";
			return '<button class="' + cls + '" data-page="' + target + '">' +
				label + "</button>";
		}
		var h = btn("&laquo; First", 0, { disabled: cur === 0 }) +
			btn("Prev", cur - 1, { disabled: cur === 0 });
		var chunk = 10;
		var start = Math.floor(cur / chunk) * chunk;
		var end = Math.min(start + chunk, pages);
		for (var i = start; i < end; i++)
			h += btn(String(i + 1), i, { active: i === cur });
		h += btn("Next", cur + 1, { disabled: cur >= pages - 1 }) +
		     btn("Last &raquo;", pages - 1, { disabled: cur >= pages - 1 });
		return h;
	}

	function renderPagers() {
		$("#pager-top, #pager-bottom").html(pagerHtml());
	}

	/* Debug panel as HTML tables (fetch info + EXPLAIN), above the results.
	 * All values are already server-escaped, so they are inserted verbatim. */
	function renderDebug(d) {
		var $area = $("#debug-area");
		if (!d || !d.debug) { $area.empty(); return; }
		var g = d.debug;
		var h = '<details class="debug-panel" open><summary>Debug</summary>' +
			'<table class="kv debug-kv">' +
			"<tr><th>Fetch query</th><td><code>" + g.sql + "</code></td></tr>" +
			"<tr><th>Count query</th><td><code>" + g.count_sql + "</code></td></tr>" +
			"<tr><th>Total rows</th><td>" + d.total + "</td></tr>" +
			"<tr><th>Bind data</th><td>" +
			(g.bind || []).map(function (b) {
				return '<span class="bind">' + b + "</span>";
			}).join(" ") + "</td></tr></table>";
		if (g.explain) {
			h += '<div class="table-wrap"><table class="grid"><thead><tr>';
			(g.explain.columns || []).forEach(function (c) { h += "<th>" + c + "</th>"; });
			h += "</tr></thead><tbody>";
			(g.explain.rows || []).forEach(function (r) {
				h += "<tr>";
				r.forEach(function (v) { h += "<td>" + v + "</td>"; });
				h += "</tr>";
			});
			h += "</tbody></table></div>";
		}
		$area.html(h + "</details>");
	}

	function markSort() {
		$("th[data-sort-key]").each(function () {
			var $th = $(this), key = $th.attr("data-sort-key");
			$th.toggleClass("sort-active", key === state.sort);
			$th.attr("data-dir", key === state.sort ? state.order : "");
		});
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
		var params = { limit: state.limit, offset: state.offset, order: state.order };
		if (state.sort) params.sort = state.sort;
		params.search = searchStr();
		if (state.debug) params.debug = 1;

		$body.html('<tr><td colspan="' + NCOLS +
			'" class="muted">Loading&hellip;</td></tr>');

		$.getJSON(api, params).done(function (d) {
			state.total  = d.total;
			state.limit  = d.limit;
			state.offset = d.offset;
			if (d.max_offset)
				state.maxOffset = d.max_offset;
			state.sort   = d.sort || "";
			state.order  = d.order || "desc";
			$body.html(window.SearchRender.rows(d.cols, d.rows, detailBase));
			renderMeta();
			renderPagers();
			renderDebug(d);
			markSort();
			syncTopScroll();
			syncUrl();
		}).fail(function (xhr) {
			var msg = (xhr.responseJSON && xhr.responseJSON.error) ||
				"Request failed";
			$body.html('<tr><td colspan="' + NCOLS +
				'" class="search-error">' + $("<i>").text(msg).html() +
				"</td></tr>");
		});
	}

	/* ---- long-text modal (truncated cells) --------------------------- */

	function showModal(text) {
		var $ov = $('<div class="modal-overlay"></div>');
		$ov.html('<div class="modal-box"><button type="button" ' +
			'class="modal-close" aria-label="Close">&times;</button>' +
			'<div class="modal-text"></div></div>');
		$ov.find(".modal-text").text(text);
		function close() { $ov.remove(); $(document).off("keydown.umodal"); }
		$ov.on("click", function (e) {
			if (e.target === $ov[0] || $(e.target).hasClass("modal-close"))
				close();
		});
		$(document).on("keydown.umodal", function (e) {
			if (e.key === "Escape") close();
		});
		$("body").append($ov);
	}

	$body.on("click", ".cell-long", function () {
		if (this.scrollWidth <= this.clientWidth + 1)
			return; /* not truncated: nothing hidden */
		showModal($(this).text());
	});

	/* ---- delegated pager + sort + history ---------------------------- */

	$("#pager-top, #pager-bottom").on("click", ".pager-btn[data-page]", function () {
		goTo(parseInt($(this).attr("data-page"), 10));
	});

	$("#results-table thead").on("click", "th[data-sort-key]", function () {
		var key = $(this).attr("data-sort-key");
		if (state.sort === key)
			state.order = (state.order === "asc") ? "desc" : "asc";
		else {
			state.sort = key;
			state.order = "asc";
		}
		/* Keep the current page when re-sorting (do not reset offset). */
		load();
	});

	$("#limit-input").on("change", function () {
		var v = parseInt(this.value, 10);
		if (isNaN(v) || v < 1) v = 1;
		if (v > 1000) v = 1000;
		this.value = v;
		if (v === state.limit)
			return;
		state.limit = v;
		state.offset = 0; /* page size changed -> back to page 1 */
		load();
	});

	window.addEventListener("popstate", function () { window.location.reload(); });

	/* ---- dual horizontal scrollbar (a top bar mirrors the bottom) ----- */
	var syncTopScroll = (function () {
		var $top   = $("#results-scroll-top");
		var $inner = $top.children().first();
		var $wrap  = $("#results-wrap");
		if (!$top.length || !$wrap.length)
			return function () {};
		var lock = false;
		$top.on("scroll", function () {
			if (lock) return;
			lock = true; $wrap[0].scrollLeft = $top[0].scrollLeft; lock = false;
		});
		$wrap.on("scroll", function () {
			if (lock) return;
			lock = true; $top[0].scrollLeft = $wrap[0].scrollLeft; lock = false;
		});
		function sync() { $inner.css("width", $wrap[0].scrollWidth + "px"); }
		$(window).on("resize", sync);
		return sync;
	}());

	/* ---- init: SSR already rendered the first page; just enhance ------ */
	renderBuilder();
	renderPagers();
	markSort();
	syncTopScroll();
});
