/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * tgloggerd web -- MCP admin pages. One file serving both /mcp-admin/groups
 * (the exposure allowlist, with a select2 group picker) and /mcp-admin/tokens
 * (mint and revoke bearer tokens); each block no-ops when its section is absent.
 *
 * The token plaintext is handled in exactly one place, and never stored,
 * re-fetched or logged: the server returns it once from the mint call, it is
 * shown, and it is dropped when the panel closes.
 */
(function () {
	"use strict";
	if (typeof jQuery === "undefined")
		return;
	var $ = jQuery;

	function postForm(url, params, csrf) {
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

	/* ---- /mcp-admin/groups ------------------------------------------- */
	var $groups = $("#mcp-groups");
	if ($groups.length) {
		var gcsrf = $groups.attr("data-csrf");
		var $gwrap = $("#mg-form-wrap"),
		    $gmsg  = $("#mg-form-msg"),
		    $gsel  = $("#mg-f-group"),
		    $gnote = $("#mg-f-note");

		$gsel.select2({
			placeholder: "Search a group by title…",
			width: "100%",
			minimumInputLength: 1,
			ajax: {
				url: "/mcp-admin/groups/search",
				dataType: "json",
				delay: 250,
				data: function (p) { return { q: p.term || "" }; },
				processResults: function (d) { return { results: d.results || [] }; },
				cache: true
			}
		});

		$("#mg-add").on("click", function () {
			$gmsg.text("");
			$gsel.val(null).trigger("change");
			$gnote.val("");
			$gwrap.prop("hidden", false);
		});
		$("#mg-cancel").on("click", function () { $gwrap.prop("hidden", true); });

		$("#mg-form").on("submit", function (e) {
			e.preventDefault();
			$gmsg.attr("class", "rt-form-msg").text("Saving…");
			postForm("/mcp-admin/groups/add", {
				group_id: $gsel.val() || "",
				note: $gnote.val() || ""
			}, gcsrf).then(function (res) {
				if (res.ok)
					location.reload();
				else
					$gmsg.attr("class", "rt-form-msg err")
					     .text(res.error || "Could not expose that group.");
			});
		});

		$("#mg-body").on("click", ".mg-del", function () {
			var $tr = $(this).closest("tr");
			if (!window.confirm("Stop exposing this group? MCP clients will no longer see its messages."))
				return;
			postForm("/mcp-admin/groups/remove",
				 { group_id: $tr.attr("data-group-id") }, gcsrf)
				.then(function (res) {
					if (res.ok)
						location.reload();
					else
						window.alert(res.error || "Could not remove it.");
				});
		});
	}

	/* ---- /mcp-admin/tokens ------------------------------------------- */
	var $tokens = $("#mcp-tokens");
	if ($tokens.length) {
		var tcsrf = $tokens.attr("data-csrf");
		var $twrap = $("#mt-form-wrap"),
		    $tmsg  = $("#mt-form-msg"),
		    $tname = $("#mt-f-name"),
		    $tnew  = $("#mt-new"),
		    $tval  = $("#mt-new-value");

		$("#mt-add").on("click", function () {
			$tmsg.text("");
			$tname.val("");
			$twrap.prop("hidden", false);
		});
		$("#mt-cancel").on("click", function () { $twrap.prop("hidden", true); });

		/* Closing the panel is what drops the plaintext; reloading then
		 * re-renders the list without it. */
		$("#mt-new-done").on("click", function () {
			$tval.text("");
			$tnew.prop("hidden", true);
			location.reload();
		});

		$("#mt-form").on("submit", function (e) {
			e.preventDefault();
			$tmsg.attr("class", "rt-form-msg").text("Minting…");
			postForm("/mcp-admin/tokens/mint",
				 { name: $tname.val() || "" }, tcsrf)
				.then(function (res) {
					if (!res.ok) {
						$tmsg.attr("class", "rt-form-msg err")
						     .text(res.error || "Could not mint a token.");
						return;
					}
					$twrap.prop("hidden", true);
					$tmsg.text("");
					/* .text(), never .html(): this value is
					 * shown verbatim and must never be
					 * interpreted as markup. */
					$tval.text(res.token || "");
					$tnew.prop("hidden", false);
				});
		});

		$("#mt-body").on("click", ".mt-revoke", function () {
			var $tr = $(this).closest("tr");
			if (!window.confirm("Revoke this token? Any client using it stops working immediately."))
				return;
			postForm("/mcp-admin/tokens/revoke",
				 { id: $tr.attr("data-id") }, tcsrf)
				.then(function (res) {
					if (res.ok)
						location.reload();
					else
						window.alert(res.error || "Could not revoke it.");
				});
		});
	}
}());
