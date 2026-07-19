/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * tgloggerd web — chat-history page enhancement (jQuery). The page is
 * functional without it: it lands on the newest message, plays animated
 * (.tgs / Lottie) stickers inline, opens preview-able media (photo, video,
 * gif, sticker) in a lightbox, and offers a date/time jump. DOM/events use
 * jQuery; the browser-only bits (DecompressionStream, Lottie, the viewport
 * observer) stay native since jQuery adds nothing there.
 */
jQuery(function ($) {
	"use strict";
	var $log = $("#chat-log");
	if (!$log.length)
		return;
	var log = $log[0];

	/*
	 * The message list is its own scroll container. A URL fragment steers
	 * the initial position: #msg-<id> centers that message (a reply jump),
	 * #top/#bottom go to the ends (the Oldest/Newest buttons), and anything
	 * else lands on the newest message at the bottom.
	 */
	function focusHash() {
		var h = window.location.hash;
		if (h === "#top") {
			log.scrollTop = 0;
			return true;
		}
		if (h === "#bottom") {
			log.scrollTop = log.scrollHeight;
			return true;
		}
		if (h.indexOf("#msg-") === 0) {
			var el = document.getElementById(h.slice(1));
			if (el) {
				el.scrollIntoView({ block: "center" });
				return true;
			}
		}
		return false;
	}
	if (!focusHash())
		log.scrollTop = log.scrollHeight;
	$(window).on("hashchange", focusHash);

	/* Date/time jump: pick a moment, navigate to ?after_ts=<unix seconds>. */
	(function initDateJump() {
		var input = document.getElementById("chat-date");
		if (!input || typeof window.flatpickr === "undefined")
			return;
		var fp = window.flatpickr(input, {
			enableTime: true,
			time_24hr: true,
			dateFormat: "Y-m-d H:i",
		});
		function jump() {
			var d = fp.selectedDates[0];
			if (!d)
				return;
			var ts = Math.floor(d.getTime() / 1000);
			var limit = $(".chat").data("limit") || 30;
			window.location = window.location.pathname +
				"?limit=" + limit + "&after_ts=" + ts;
		}
		$("#chat-date-go").on("click", jump);
	}());

	var canGunzip = typeof DecompressionStream !== "undefined";

	/* Load the bundled lottie player on demand; resolves once available. */
	var lottieReady = null;
	function ensureLottie() {
		if (window.lottie)
			return Promise.resolve();
		if (!lottieReady) {
			lottieReady = new Promise(function (resolve, reject) {
				var s = document.createElement("script");
				s.src = "/lottie.min.js";
				s.onload = resolve;
				s.onerror = reject;
				document.head.appendChild(s);
			});
		}
		return lottieReady;
	}

	/* Decompress a .tgs and play it inside el. Returns a Promise. */
	function renderTgs(el, url) {
		return fetch(url)
			.then(function (r) {
				return r.body.pipeThrough(
					new DecompressionStream("gzip"));
			})
			.then(function (stream) {
				return new Response(stream).json();
			})
			.then(function (data) {
				el.textContent = "";
				window.lottie.loadAnimation({
					container: el,
					renderer: "svg",
					loop: true,
					autoplay: true,
					animationData: data,
				});
			});
	}

	/* Inline stickers, rendered lazily as they scroll into view. */
	(function initStickers() {
		var stickers = document.querySelectorAll(".tgs-sticker");
		if (!stickers.length || !canGunzip)
			return;
		var io = new IntersectionObserver(function (entries) {
			entries.forEach(function (e) {
				if (!e.isIntersecting)
					return;
				io.unobserve(e.target);
				var el = e.target;
				ensureLottie().then(function () {
					renderTgs(el, el.getAttribute("data-tgs"))
						.catch(function () {});
				}).catch(function () {});
			});
		}, { rootMargin: "200px" });
		stickers.forEach(function (el) { io.observe(el); });
	}());

	/* Lightbox: enlarge preview-able media on click. */
	(function initLightbox() {
		var $modal = $("#media-modal");
		var $body = $("#media-modal-body");
		if (!$modal.length || !$body.length)
			return;

		function fill(kind, el) {
			var src = el.getAttribute("data-src");
			if (kind === "image") {
				$body.append($("<img>").attr("src", src));
				return true;
			}
			if (kind === "video" || kind === "animation") {
				var v = document.createElement("video");
				v.src = src;
				v.controls = true;
				v.autoplay = true;
				v.playsInline = true;
				if (kind === "animation") {
					v.loop = true;
					v.muted = true;
				}
				$body.append(v);
				return true;
			}
			if (kind === "lottie") {
				if (!canGunzip)
					return false; /* let the fallback link work */
				var c = document.createElement("div");
				c.className = "mm-lottie";
				$body.append(c);
				ensureLottie().then(function () {
					renderTgs(c, el.getAttribute("data-tgs"))
						.catch(function () {});
				}).catch(function () {});
				return true;
			}
			return false;
		}

		function open(el) {
			$body.empty();
			if (!fill(el.getAttribute("data-zoom"), el))
				return false;
			$modal.prop("hidden", false);
			$("body").css("overflow", "hidden");
			return true;
		}

		function close() {
			$modal.prop("hidden", true);
			$body.empty();		/* stop any video / lottie */
			$("body").css("overflow", "");
		}

		$log.on("click", ".zoomable", function (e) {
			if (open(this))
				e.preventDefault();
		});
		$("#media-modal-close").on("click", close);
		$modal.on("click", function (e) {
			/* A click on the backdrop (not the media) closes it. */
			if (e.target === this || e.target === $body[0])
				close();
		});
		$(document).on("keydown", function (e) {
			if (e.key === "Escape" && !$modal.prop("hidden"))
				close();
		});
	}());
});
