/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * tgloggerd web — chat-history page enhancement. The page is functional
 * without it: it lands the viewport on the newest message, plays animated
 * (.tgs / Lottie) stickers inline, and opens preview-able media (photo,
 * video, gif, sticker) in a lightbox instead of navigating away.
 */
(function () {
	"use strict";
	var log = document.getElementById("chat-log");
	if (!log)
		return;

	// The message list is its own scroll container. Center a targeted
	// message (#msg-<id> from a reply link, same page or freshly loaded);
	// otherwise land on the newest message at the bottom.
	function focusHash() {
		var h = window.location.hash;
		if (h.indexOf("#msg-") !== 0)
			return false;
		var el = document.getElementById(h.slice(1));
		if (!el)
			return false;
		el.scrollIntoView({ block: "center" });
		return true;
	}
	if (!focusHash())
		log.scrollTop = log.scrollHeight;
	// Re-center when an in-page reply link changes the hash (no reload).
	window.addEventListener("hashchange", focusHash);

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
		var modal = document.getElementById("media-modal");
		var body = document.getElementById("media-modal-body");
		var closeBtn = document.getElementById("media-modal-close");
		if (!modal || !body)
			return;

		function fill(kind, z) {
			if (kind === "image") {
				var img = document.createElement("img");
				img.src = z.getAttribute("data-src");
				body.appendChild(img);
				return true;
			}
			if (kind === "video" || kind === "animation") {
				var v = document.createElement("video");
				v.src = z.getAttribute("data-src");
				v.controls = true;
				v.autoplay = true;
				v.playsInline = true;
				if (kind === "animation") {
					v.loop = true;
					v.muted = true;
				}
				body.appendChild(v);
				return true;
			}
			if (kind === "lottie") {
				if (!canGunzip)
					return false; /* let the fallback link work */
				var c = document.createElement("div");
				c.className = "mm-lottie";
				body.appendChild(c);
				ensureLottie().then(function () {
					renderTgs(c, z.getAttribute("data-tgs"))
						.catch(function () {});
				}).catch(function () {});
				return true;
			}
			return false;
		}

		function open(z) {
			body.textContent = "";
			if (!fill(z.getAttribute("data-zoom"), z))
				return false;
			modal.hidden = false;
			document.body.style.overflow = "hidden";
			return true;
		}

		function close() {
			modal.hidden = true;
			body.textContent = "";	/* stop any video / lottie */
			document.body.style.overflow = "";
		}

		log.addEventListener("click", function (e) {
			var z = e.target.closest(".zoomable");
			if (!z || !log.contains(z))
				return;
			if (open(z))
				e.preventDefault();
		});
		closeBtn.addEventListener("click", close);
		modal.addEventListener("click", function (e) {
			/* A click on the backdrop (not the media) closes it. */
			if (e.target === modal || e.target === body)
				close();
		});
		document.addEventListener("keydown", function (e) {
			if (e.key === "Escape" && !modal.hidden)
				close();
		});
	}());
}());
