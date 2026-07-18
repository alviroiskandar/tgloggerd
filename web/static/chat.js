/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * tgloggerd web — chat-history page enhancement. The page is fully functional
 * without it; this only lands the viewport on the newest message. It is the
 * hook point for future infinite scrolling (load older on scroll-to-top).
 */
(function () {
	"use strict";
	var log = document.getElementById("chat-log");
	if (!log)
		return;

	// Jump to the newest message unless the URL targets a specific one
	// (e.g. a reply anchor), which the browser scrolls to itself.
	if (!window.location.hash)
		window.scrollTo(0, document.body.scrollHeight);
}());
