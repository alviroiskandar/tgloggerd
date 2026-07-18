/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * tgloggerd web — chat-history page enhancement. The page is fully functional
 * without it: it lands the viewport on the newest message and, where the
 * browser supports it, plays animated (.tgs / Lottie) stickers inline.
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

	initStickers();

	/*
	 * Animated stickers are gzip-compressed Lottie JSON (.tgs). Render them
	 * with the locally bundled lottie player, loaded on demand only when a
	 * chat actually has one. Each sticker is decompressed in the browser and
	 * rendered lazily as it scrolls into view, so a chat full of them does
	 * not decode everything at once. Browsers without DecompressionStream
	 * (or with JS disabled) keep the download-link fallback in the markup.
	 */
	function initStickers() {
		var stickers = document.querySelectorAll(".tgs-sticker");
		if (!stickers.length || typeof DecompressionStream === "undefined")
			return;

		var s = document.createElement("script");
		s.src = "/lottie.min.js";
		s.onload = function () {
			var io = new IntersectionObserver(function (entries) {
				entries.forEach(function (e) {
					if (!e.isIntersecting)
						return;
					io.unobserve(e.target);
					renderSticker(e.target);
				});
			}, { rootMargin: "200px" });
			stickers.forEach(function (el) { io.observe(el); });
		};
		document.head.appendChild(s);
	}

	function renderSticker(el) {
		var url = el.getAttribute("data-tgs");
		if (!url || !window.lottie)
			return;

		fetch(url)
			.then(function (r) {
				return r.body.pipeThrough(
					new DecompressionStream("gzip"));
			})
			.then(function (stream) {
				return new Response(stream).json();
			})
			.then(function (data) {
				el.textContent = "";	// drop the fallback link
				window.lottie.loadAnimation({
					container: el,
					renderer: "svg",
					loop: true,
					autoplay: true,
					animationData: data,
				});
			})
			.catch(function () { /* keep the fallback link */ });
	}
}());
