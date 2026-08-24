/* SPDX-License-Identifier: GPL-2.0-or-later */
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

	/*
	 * Floating date badge: while scrolling, show the day of the message at
	 * the top of the viewport; fade it out slowly 2 s after scrolling stops.
	 */
	(function initDateBadge() {
		var badge = document.getElementById("chat-datebadge");
		if (!badge)
			return;

		function currentDay() {
			/* Queried live so infinite-scroll messages are included. */
			var msgs = log.querySelectorAll("[data-day]");
			if (!msgs.length)
				return "";
			var edge = log.getBoundingClientRect().top + 12;
			var day = msgs[0].getAttribute("data-day") || "";
			for (var i = 0; i < msgs.length; i++) {
				if (msgs[i].getBoundingClientRect().top <= edge) {
					var d = msgs[i].getAttribute("data-day");
					if (d)
						day = d;
				} else {
					break;	/* messages run top-to-bottom */
				}
			}
			return day;
		}

		var hideTimer = null;
		var ticking = false;
		function refresh() {
			ticking = false;
			var day = currentDay();
			if (!day)
				return;
			badge.textContent = day;
			badge.classList.add("show");
			clearTimeout(hideTimer);
			hideTimer = setTimeout(function () {
				badge.classList.remove("show");
			}, 1000);
		}
		$log.on("scroll", function () {
			if (!ticking) {
				ticking = true;
				requestAnimationFrame(refresh);
			}
		});
	}());

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

	/* Inline stickers, rendered lazily as they scroll into view. Reusable so
	 * infinite-scroll / live-appended messages get their stickers observed
	 * too; only unobserved (fresh) ones are picked up. */
	var stickerIO = null;
	function observeStickers(root) {
		if (!canGunzip)
			return;
		if (!stickerIO) {
			stickerIO = new IntersectionObserver(function (entries) {
				entries.forEach(function (e) {
					if (!e.isIntersecting)
						return;
					stickerIO.unobserve(e.target);
					var el = e.target;
					ensureLottie().then(function () {
						renderTgs(el, el.getAttribute("data-tgs"))
							.catch(function () {});
					}).catch(function () {});
				});
			}, { rootMargin: "200px" });
		}
		(root || document).querySelectorAll(".tgs-sticker")
			.forEach(function (el) { stickerIO.observe(el); });
	}
	observeStickers(log);

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

	/*
	 * "Auto-load & live": infinite scroll + follow-the-tail, backed by the
	 * /v1 JSON API and the shared ChatRender (so injected messages match the
	 * server-rendered ones). Enabled by the checkbox or an ?auto=1 URL.
	 */
	(function initAuto() {
		var $chat = $(".chat");
		var $msgs = $("#chat-msgs");
		var $auto = $("#chat-auto");
		if (!$chat.length || !$msgs.length || !$auto.length || !window.ChatRender)
			return;
		var chat = $chat[0], msgsEl = $msgs[0];
		var $sd = $("#chat-scrolldown");

		var scope = $chat.attr("data-scope");
		var chatId = $chat.attr("data-chat-id");
		var limit = parseInt($chat.attr("data-limit"), 10) || 30;
		var api = "/v1/chats/" + scope + "/" + chatId + "/messages";

		function attrNum(name) {
			var v = chat.getAttribute(name);
			return (v === null || v === "") ? null : parseInt(v, 10);
		}
		var olderCursor = attrNum("data-older-after");
		var newerCursor = attrNum("data-newer-after");
		var oldestId = attrNum("data-oldest");
		var newestId = attrNum("data-newest");

		var autoOn = false, loadingOlder = false, loadingNewer = false;
		var liveTimer = null;

		function fresh(list) {
			return (list || []).filter(function (m) {
				return !document.getElementById("msg-" + m.msg_id);
			});
		}
		function insert(list, where) {
			var $new = $(window.ChatRender.messages(list));
			if (where === "prepend")
				$msgs.prepend($new);
			else
				$msgs.append($new);
			$new.each(function () { observeStickers(this); });
			return $new;
		}
		function nearBottom() {
			return log.scrollHeight - log.scrollTop - log.clientHeight < 40;
		}
		function preloadPx() {
			var n = msgsEl.querySelectorAll("[data-day]").length || 1;
			return Math.max((log.scrollHeight / n) * 50, 800);
		}

		function loadOlder() {
			if (loadingOlder || olderCursor === null || !autoOn)
				return;
			loadingOlder = true;
			$.getJSON(api, { limit: limit, after: olderCursor })
				.done(function (d) {
					var list = fresh(d.messages);
					if (list.length) {
						var before = log.scrollHeight;
						insert(list, "prepend");
						log.scrollTop += log.scrollHeight - before;
					}
					olderCursor = (d.older_after !== undefined) ? d.older_after : null;
					if (d.oldest_msg_id !== undefined) oldestId = d.oldest_msg_id;
					loadingOlder = false;
					if (autoOn && olderCursor !== null && log.scrollTop < preloadPx())
						loadOlder();	/* keep the buffer ahead */
				}).fail(function () { loadingOlder = false; });
		}

		function loadNewer() {
			if (loadingNewer || newerCursor === null || !autoOn)
				return;
			loadingNewer = true;
			$.getJSON(api, { limit: limit, after: newerCursor })
				.done(function (d) {
					var list = fresh(d.messages);
					if (list.length) {
						var atBottom = nearBottom();
						insert(list, "append");
						if (atBottom) log.scrollTop = log.scrollHeight;
					}
					newerCursor = (d.newer_after !== undefined) ? d.newer_after : null;
					if (d.newest_msg_id !== undefined) newestId = d.newest_msg_id;
					loadingNewer = false;
					if (autoOn && newerCursor !== null &&
					    log.scrollHeight - log.scrollTop - log.clientHeight < preloadPx())
						loadNewer();
					updateLive();
				}).fail(function () { loadingNewer = false; });
		}

		function liveTick() {
			if (!autoOn || newerCursor !== null || newestId === null)
				return;
			$.getJSON(api, { limit: limit, after: newestId }).done(function (d) {
				var list = fresh(d.messages);
				if (list.length) {
					var atBottom = nearBottom();
					insert(list, "append");
					if (atBottom) log.scrollTop = log.scrollHeight;
				}
				if (d.newest_msg_id !== undefined && d.newest_msg_id > newestId)
					newestId = d.newest_msg_id;
				if (d.newer_after !== undefined) {
					newerCursor = d.newer_after;	/* fell behind; page in */
					updateLive();
				}
			});
		}
		function updateLive() {
			var wantLive = autoOn && newerCursor === null && !document.hidden;
			if (wantLive && !liveTimer)
				liveTimer = setInterval(liveTick, 1000);
			else if (!wantLive && liveTimer) {
				clearInterval(liveTimer);
				liveTimer = null;
			}
		}

		/* Reply to an off-page message: load its centered page in place,
		 * scroll to it, flash it, and rewrite the URL so it is shareable and
		 * a reload's SSR reproduces the view. */
		function replyJump(after, lim, frag) {
			$.getJSON(api, { limit: lim, after: after }).done(function (d) {
				$msgs.html(window.ChatRender.messages(d.messages || []));
				observeStickers(msgsEl);
				olderCursor = (d.older_after !== undefined) ? d.older_after : null;
				newerCursor = (d.newer_after !== undefined) ? d.newer_after : null;
				if (d.oldest_msg_id !== undefined) oldestId = d.oldest_msg_id;
				if (d.newest_msg_id !== undefined) newestId = d.newest_msg_id;
				var id = frag.indexOf("#msg-") === 0 ? frag.slice(1) : null;
				var el = id && document.getElementById(id);
				if (el) {
					el.scrollIntoView({ block: "center" });
					el.classList.add("flash");
					setTimeout(function () { el.classList.remove("flash"); }, 2000);
				}
				history.replaceState(null, "", window.location.pathname +
					"?limit=" + lim + "&after=" + after +
					(autoOn ? "&auto=1" : "") + frag);
				updateLive();
			});
		}

		/* Jump to the newest message: scroll to the bottom if the tail is
		 * loaded, otherwise reload the newest page in place. */
		function goNewest() {
			if (newerCursor === null) {
				log.scrollTop = log.scrollHeight;
				return;
			}
			$.getJSON(api, { limit: limit }).done(function (d) {
				$msgs.html(window.ChatRender.messages(d.messages || []));
				observeStickers(msgsEl);
				olderCursor = (d.older_after !== undefined) ? d.older_after : null;
				newerCursor = (d.newer_after !== undefined) ? d.newer_after : null;
				if (d.oldest_msg_id !== undefined) oldestId = d.oldest_msg_id;
				if (d.newest_msg_id !== undefined) newestId = d.newest_msg_id;
				log.scrollTop = log.scrollHeight;
				history.replaceState(null, "", window.location.pathname +
					"?limit=" + limit + (autoOn ? "&auto=1" : ""));
				updateLive();
			});
		}

		/* Show the scroll-to-newest button (auto on) whenever the view is
		 * scrolled away from the bottom; hide it once the bottom is reached. */
		function updateScrollDown() {
			$sd.toggleClass("show", autoOn && !nearBottom());
		}
		$sd.on("click", goNewest);

		$msgs.on("click", ".reply-quote", function (e) {
			if (!autoOn)
				return;			/* let SSR navigation happen */
			var href = this.getAttribute("href") || "";
			if (href.charAt(0) !== "?")
				return;			/* in-page #msg: native/hashchange */
			e.preventDefault();
			var h = href.indexOf("#");
			var frag = h >= 0 ? href.slice(h) : "";
			var qs = new URLSearchParams(h >= 0 ? href.slice(1, h) : href.slice(1));
			replyJump(qs.get("after"), qs.get("limit") || limit, frag);
		});

		$log.on("scroll", function () {
			if (!autoOn)
				return;
			var px = preloadPx();
			if (log.scrollTop < px)
				loadOlder();
			if (log.scrollHeight - log.scrollTop - log.clientHeight < px)
				loadNewer();
			updateScrollDown();
		});

		document.addEventListener("visibilitychange", updateLive);

		function setAuto(on, writeUrl) {
			autoOn = on;
			$auto.prop("checked", on);
			$chat.toggleClass("auto", on);
			if (!on)
				$sd.removeClass("show");
			if (writeUrl) {
				var u = new URL(window.location.href);
				if (on) u.searchParams.set("auto", "1");
				else u.searchParams.delete("auto");
				history.replaceState(null, "", u.pathname + u.search + u.hash);
			}
			updateLive();
			if (on)
				$log.trigger("scroll");	/* kick an initial prefetch */
		}
		$auto.on("change", function () { setAuto(this.checked, true); });
		setAuto(new URL(window.location.href).searchParams.get("auto") === "1", false);
	}());
});
