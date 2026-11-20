// The chat view in a real browser: scrolling back and live updates.
import { test, expect } from './fixtures.ts';

const G = -1001337794619;
const bubbles = (page: import('@playwright/test').Page) => page.locator('.c-row, .c-service');

test('scrolling to the top loads older messages without moving the view', async ({ page }) => {
	await page.goto(`/groups/${G}/chat`);
	// Wait for the page to take over (its scroll listener attached).
	await expect(page.locator('.a-livestate')).toBeVisible();
	const before = await bubbles(page).count();
	// Scroll to the top until older messages come: the scroll listener may
	// be attached a moment after the first scroll to the bottom.
	await expect.poll(async () => {
		await page.locator('.c-scroll').evaluate((el) => { el.scrollTop = 0; el.dispatchEvent(new Event('scroll')); });
		return bubbles(page).count();
	}).toBeGreaterThan(before);
	// The view is put back on the next frame, so the reader stays put.
	await expect.poll(() => page.locator('.c-scroll').evaluate((el) => el.scrollTop)).toBeGreaterThan(0);
});

test('new messages follow the live edge; scrolled up, they are counted instead', async ({ page, emit }) => {
	await page.goto(`/groups/${G}/chat`);
	await expect(page.locator('.a-livestate.is-live')).toBeVisible();
	const e1 = await emit({ kind: 'message.new', chat_id: G });
	await expect(page.locator(`#m${e1.message_id}`)).toBeVisible();
	await page.locator('.c-scroll').evaluate((el) => { el.scrollTop = el.scrollHeight - el.clientHeight - 1500; });
	const top = await page.locator('.c-scroll').evaluate((el) => el.scrollTop);
	await emit({ kind: 'message.new', chat_id: G });
	await emit({ kind: 'message.new', chat_id: G });
	await expect(page.locator('.c-unseen')).toHaveText('2');
	expect(await page.locator('.c-scroll').evaluate((el) => el.scrollTop)).toBe(top);
});

test('edits and deletions change the bubble in place', async ({ page, emit }) => {
	await page.goto(`/groups/${G}/chat`);
	await expect(page.locator('.a-livestate.is-live')).toBeVisible();
	const e = await emit({ kind: 'message.new', chat_id: G });
	await expect(page.locator(`#m${e.message_id}`)).toBeVisible();
	await emit({ kind: 'message.edited', chat_id: G, message_id: e.message_id });
	await expect(page.locator(`#m${e.message_id}`)).toContainText('(edited live)');
	await emit({ kind: 'message.deleted', chat_id: G, message_id: e.message_id });
	await expect(page.locator(`#m${e.message_id} .c-bubble.deleted`)).toBeVisible();
});

test('a message filled in later lands in its place, not at the bottom', async ({ page, emit }) => {
	await page.goto(`/groups/${G}/chat`);
	await expect(page.locator('.a-livestate.is-live')).toBeVisible();
	// A free message id between two messages of the window.
	const msgs = (await (await page.request.get(`/api/page/groups/${G}/chat`)).json()).props.messages as { message_id: number }[];
	const i = msgs.findIndex((m, k) => k > 0 && k < msgs.length - 2 && msgs[k + 1]!.message_id - m.message_id > 1);
	expect(i, 'a gap in the window').toBeGreaterThan(0);
	const gap = msgs[i]!.message_id + 1;
	await emit({ kind: 'message.new', chat_id: G, message_id: gap });
	await expect(page.locator(`#m${gap}`)).toContainText('Found on recheck');
	// Between its neighbours (a day line may sit between rows).
	const ids = await page.locator('.c-feed > [id^="m"]').evaluateAll((els) => els.map((el) => el.id));
	const at = ids.indexOf(`m${gap}`);
	expect([ids[at - 1], ids[at + 1]]).toEqual([`m${msgs[i]!.message_id}`, `m${msgs[i + 1]!.message_id}`]);
	await expect(page.locator('.c-unseen')).toHaveCount(0);
});

test('a window scrolled back to the oldest still shows edits live', async ({ page, emit }) => {
	await page.goto(`/groups/${G}/chat?after=0`);
	await expect(page.locator('.a-livestate.is-live')).toBeVisible();
	const msgs = (await (await page.request.get(`/api/page/groups/${G}/chat?after=0`)).json()).props as { has_newer: boolean; messages: { message_id: number; content_type: string }[] };
	expect(msgs.has_newer, 'the oldest window has newer pages').toBe(true);
	const m = msgs.messages.find((x, k) => k > 2 && x.content_type === 'text')!;
	await emit({ kind: 'message.edited', chat_id: G, message_id: m.message_id });
	await expect(page.locator(`#m${m.message_id}`)).toContainText('(edited live)');
});

test('without the stream the chat polls, and goes live again when it returns', async ({ page, emit, standinUrl }) => {
	await page.goto(`/groups/${G}/chat`);
	await expect(page.locator('.a-livestate.is-live')).toBeVisible();
	await fetch(`${standinUrl}/api/_standin/events`, { method: 'POST', body: JSON.stringify({ off: true }) });
	await expect(page.locator('.a-livestate.is-polling')).toBeVisible();
	const e = await emit({ kind: 'message.new', chat_id: G });
	await expect(page.locator(`#m${e.message_id}`)).toBeAttached();
	await fetch(`${standinUrl}/api/_standin/events`, { method: 'POST', body: JSON.stringify({ off: false }) });
	await expect(page.locator('.a-livestate.is-live')).toBeVisible({ timeout: 15_000 });
});

test('a lone photo whose size is not stored is drawn whole, at its own shape', async ({ page }) => {
	// A lone photo that is not 4:3, the box an unknown size used to get.
	type M = { message_id: number; content_type: string; album: unknown[]; media: { w?: number; h?: number; render?: string } | null };
	const msgs = (await (await page.request.get(`/api/page/groups/${G}/chat`)).json()).props.messages as M[];
	const m = msgs.find((x) => x.content_type === 'photo' && !x.album.length && x.media?.render === 'image' && x.media.w && x.media.h && Math.abs(x.media.w / x.media.h - 4 / 3) > 0.1)!;
	expect(m, 'a lone photo that is not 4:3').toBeTruthy();
	// Most older photos come without their size: take the sizes out of
	// the page data the chat opens with.
	const strip = async (route: import('@playwright/test').Route) => {
		const res = await route.fetch();
		await route.fulfill({ response: res, body: (await res.text()).replace(/"w":\d+,"h":\d+,/g, '') });
	};
	// Open it in the app (not a document load: the stand-in's server
	// render would bring the sizes along), from page data without them.
	await page.goto('/users');
	await expect(page.getByRole('heading', { name: 'Users', level: 1 })).toBeVisible();
	// Page data and chat windows only: the live event stream never ends.
	await page.route((u) => u.pathname.startsWith('/api/page/') || u.pathname.startsWith('/api/chats/'), strip);
	await page.evaluate((to) => { history.pushState(null, '', to); dispatchEvent(new PopStateEvent('popstate')); }, `/groups/${G}/chat?at=${m.message_id}`);
	const box = page.locator(`#m${m.message_id}`).getByRole('link', { name: 'Photo' });
	const img = box.locator('img');
	await expect(img).toHaveJSProperty('complete', true);
	expect(await box.getAttribute('style') ?? '').not.toContain('aspect-ratio');
	const [natural, drawn] = await img.evaluate((i: HTMLImageElement) => [i.naturalWidth / i.naturalHeight, i.clientWidth / i.clientHeight]);
	expect(natural).toBeGreaterThan(0);
	expect(Math.abs(drawn - natural) / natural).toBeLessThan(0.02);
});

test('on its message page, a lone photo whose size is not stored is drawn whole', async ({ page }) => {
	type M = { id: number; content_type: string; album: unknown[]; media: { w?: number; h?: number; render?: string } | null };
	const msgs = (await (await page.request.get(`/api/page/groups/${G}/chat`)).json()).props.messages as M[];
	const m = msgs.find((x) => x.content_type === 'photo' && !x.album.length && x.media?.render === 'image' && x.media.w && x.media.h && Math.abs(x.media.w / x.media.h - 4 / 3) > 0.1)!;
	expect(m, 'a lone photo that is not 4:3').toBeTruthy();
	await page.goto('/users');
	await expect(page.getByRole('heading', { name: 'Users', level: 1 })).toBeVisible();
	await page.route((u) => u.pathname.startsWith('/api/page/'), async (route) => {
		const res = await route.fetch();
		await route.fulfill({ response: res, body: (await res.text()).replace(/"w":\d+,"h":\d+,/g, '') });
	});
	await page.evaluate((to) => { history.pushState(null, '', to); dispatchEvent(new PopStateEvent('popstate')); }, `/messages/group/${m.id}`);
	const box = page.locator('.a-msgmedia').getByRole('link', { name: 'Photo' });
	const img = box.locator('img');
	await expect(img).toHaveJSProperty('complete', true);
	expect(await box.getAttribute('style') ?? '').not.toContain('aspect-ratio');
	const [natural, drawn] = await img.evaluate((i: HTMLImageElement) => [i.naturalWidth / i.naturalHeight, i.clientWidth / i.clientHeight]);
	expect(Math.abs(drawn - natural) / natural).toBeLessThan(0.02);
});

test('an album\'s photos sit in cells of one size, each drawn whole', async ({ page }) => {
	await page.goto(`/groups/${G}/chat?at=555422`);
	const cells = page.locator('#m555422 .c-album .c-media');
	await expect(cells.first().locator('img')).toHaveJSProperty('complete', true);
	const boxes = await cells.evaluateAll((els) => els.map((e) => {
		const r = e.getBoundingClientRect();
		const img = e.querySelector('img:not(.c-mini)');
		return { w: Math.round(r.width), h: Math.round(r.height), fit: img ? getComputedStyle(img).objectFit : '' };
	}));
	expect(boxes.length).toBeGreaterThan(1);
	for (const b of boxes) expect(b).toEqual({ w: boxes[0]!.w, h: boxes[0]!.w, fit: 'contain' });
});

test('on a wide screen the chat column leaves half the side margins of a 760px one', async ({ page }) => {
	await page.setViewportSize({ width: 1600, height: 900 });
	await page.goto(`/groups/${G}/chat`);
	const [outer, feed] = await page.locator('.c-feed').evaluate((f) => [f.parentElement!.clientWidth, f.getBoundingClientRect().width]);
	expect(outer).toBeGreaterThan(1000);
	// Each margin is half of what a 760px column would leave.
	expect(Math.abs((outer - feed) / 2 - (outer - 760) / 4)).toBeLessThan(2);
});

test('a photo opens full size in the viewer above the chat; Esc closes it', async ({ page }) => {
	await page.goto(`/groups/${G}/chat?at=555422`);
	await expect(page.locator('.a-livestate')).toBeVisible();
	const photo = page.locator('#m555422').getByRole('link', { name: 'Photo' }).first();
	await expect(photo.locator('img')).toHaveJSProperty('complete', true);
	expect(await photo.locator('img').evaluate((i: HTMLImageElement) => i.naturalWidth)).toBeGreaterThan(0);
	await photo.click();
	const viewer = page.getByRole('dialog', { name: 'Media viewer' });
	await expect(viewer.locator('img')).toBeVisible();
	// Above everything: the image's centre is the topmost element there.
	const top = await viewer.locator('img').evaluate((i) => {
		const r = i.getBoundingClientRect();
		return document.elementFromPoint(r.x + r.width / 2, r.y + r.height / 2) === i;
	});
	expect(top).toBe(true);
	await page.keyboard.press('Escape');
	await expect(viewer).toHaveCount(0);
});

test('a video shows its poster and fetches none of its bytes until it is played', async ({ page, request }) => {
	const asked: string[] = [];
	page.on('request', (r) => asked.push(new URL(r.url()).pathname));
	// The newest stored, plain video with a poster, a page back at a time.
	type M = { message_id: number; content_type: string; album: unknown[]; media: { stored: boolean; url: string | null; poster?: string } | null };
	let v: M | undefined;
	for (let i = 0, before = ''; i < 20 && !v; i++) {
		const msgs = (await (await request.get(`/api/chats/group/${G}/messages?limit=100${before}`, { headers: { cookie: 'tgd_as=superadmin' } })).json()).messages as M[];
		if (!msgs.length) break;
		v = msgs.filter((m) => m.content_type === 'video' && !m.album.length && m.media?.stored && m.media.poster).at(-1);
		before = `&before=${msgs[0]!.message_id}`;
	}
	expect(v, 'a stored video with a poster').toBeTruthy();
	await page.goto(`/groups/${G}/chat?at=${v!.message_id}`);
	await expect(page.locator('.a-livestate')).toBeVisible();
	const box = page.locator(`#m${v!.message_id}`).getByRole('link', { name: 'Video' });
	const video = box.locator('video');
	await expect(video).toHaveAttribute('poster', v!.media!.poster!);
	await expect(video).toHaveAttribute('preload', 'none');
	await expect(box.locator('.c-playbig')).toBeVisible();
	await expect.poll(() => asked.includes(v!.media!.poster!)).toBe(true);
	expect(asked.filter((u) => u === v!.media!.url)).toEqual([]);
	await box.click();
	await expect(page.getByRole('dialog', { name: 'Media viewer' }).locator('video')).toHaveAttribute('src', v!.media!.url!);
});

test('a voice note fetches nothing until it is played', async ({ page }) => {
	const asked: string[] = [];
	page.on('request', (r) => r.url().includes('/files/') && asked.push(r.url()));
	await page.goto(`/groups/${G}/chat?at=555323`);
	await expect(page.locator('.a-livestate')).toBeVisible();
	const voice = page.locator('#m555323 .c-voice');
	await expect(voice.locator('.c-wave span').first()).toBeVisible();
	const src = await voice.locator('audio').getAttribute('src');
	expect(asked.filter((u) => u.endsWith(src!))).toEqual([]);
	await voice.getByRole('button', { name: 'Play' }).click();
	await expect.poll(() => asked.filter((u) => u.endsWith(src!)).length).toBeGreaterThan(0);
});

test('an animated sticker plays once in view, and only its chat loads the player', async ({ page }) => {
	const player: string[] = [];
	// The player itself, not the app's own loader module (lottie.ts).
	page.on('request', (r) => /lottie-web|lottie_light/.test(r.url()) && player.push(r.url()));
	await page.goto('/users');
	await expect(page.getByRole('heading', { level: 1 })).toBeVisible();
	expect(player).toEqual([]);
	await page.goto(`/groups/${G}/chat?at=555533`);
	const sticker = page.locator('#m555533 .c-lottie');
	await expect(sticker.locator('.c-lottie-box svg')).toBeVisible();
	await expect(sticker).toHaveClass(/ready/);
	await expect(sticker.locator('.c-lottie-emoji')).toHaveCount(0);
	expect(player.length).toBeGreaterThan(0);
});

test('an opened edit history sits clear of the edited mark and the time', async ({ page }) => {
	await page.goto(`/groups/${G}/chat`);
	await expect(page.locator('.a-livestate')).toBeVisible();
	const edited = page.locator('button.c-edited').last();
	await edited.scrollIntoViewIfNeeded();
	await edited.click();
	const bubble = edited.locator('xpath=ancestor::*[contains(@class,"c-bubble")][1]');
	const meta = await bubble.locator('.c-meta').boundingBox();
	const box = await bubble.locator('.c-edits').boundingBox();
	expect(box!.y - (meta!.y + meta!.height)).toBeGreaterThanOrEqual(8);
});

test('a chat shows each message kind, rendered by the server and in the browser', async ({ page }) => {
	// Assembly Lounge holds the stand-in's sample of each kind.
	const res = await page.request.get('/groups/-1001775585202/chat');
	const html = await res.text();
	for (const s of ['Which editor do you use?', 'Release checklist', 'Video chat . live now', 'This archive does not run widgets.']) expect(html).toContain(s);
	await page.goto('/groups/-1001775585202/chat');
	await expect(page.getByText('Which editor do you use?')).toBeVisible();
	await expect(page.locator('.c-videowrap.round')).toHaveCount(1);
	await expect(page.locator('.k-reaction').first()).toBeVisible();
	await expect(page.getByRole('link', { name: 'pinned a message' })).toBeVisible();
});

test('a forum chat shows one topic at a time', async ({ page }) => {
	await page.goto('/groups/-1001126016141/chat');
	const strip = page.getByRole('navigation', { name: 'Forum topics' });
	await expect(strip.getByRole('link', { name: 'All topics' })).toHaveAttribute('aria-current', 'page');
	expect(await page.locator('.k-topictag').count()).toBeGreaterThan(0);
	await strip.getByRole('link', { name: 'Help and questions' }).click();
	await expect(page).toHaveURL(/\?topic=12$/);
	await expect(strip.getByRole('link', { name: 'Help and questions' })).toHaveAttribute('aria-current', 'page');
	await expect(page.locator('.k-topictag')).toHaveCount(0);
	// The window's own API keeps to the topic.
	const w = await (await page.request.get('/api/chats/group/-1001126016141/messages?topic=12&limit=10')).json();
	expect(w.messages.every((m: { topic_id: number }) => m.topic_id === 12)).toBe(true);
	// A private chat has no topics.
	const pm = (await (await page.request.get('/api/page/private_messages')).json()).props.rows[0].chat.id;
	expect((await page.request.get(`/api/chats/private/${pm}/messages`)).status()).toBe(200);
	expect((await page.request.get(`/api/chats/private/${pm}/messages?topic=12`)).status()).toBe(400);
});

test('a chat opened at its oldest messages loads on downwards to the newest', async ({ page, request }) => {
	const G = -1001337794619;
	const newest = (await (await request.get(`/api/chats/group/${G}/messages?limit=1`, { headers: { cookie: 'tgd_as=superadmin' } })).json()).messages[0].message_id as number;
	await page.goto(`/groups/${G}/chat?after=0`);
	const bubbles = page.locator('.c-feed [id^="m"]');
	await expect(bubbles.first()).toBeVisible();
	const first = await bubbles.count();
	const scroller = page.locator('.c-scroll');
	// Scroll to the bottom until the newest message is there; each time
	// the window grows below.
	for (let i = 0; i < 12 && !(await page.locator(`#m${newest}`).count()); i++) {
		await scroller.evaluate((e) => { e.scrollTop = e.scrollHeight; });
		await page.waitForTimeout(250);
	}
	await expect(page.locator(`#m${newest}`)).toBeAttached();
	expect(await bubbles.count()).toBeGreaterThan(first + 60);
	await expect(page.getByText('Loading newer messages...')).toHaveCount(0);
	await expect(page.getByText('Beginning of the archived history')).toBeAttached();
});

test('a chat draws text entities, by the server and in the browser', async ({ page }) => {
	// Assembly Lounge holds the stand-in's formatted samples.
	const html = await (await page.request.get('/groups/-1001775585202/chat')).text();
	expect(html).toContain('<a class="rt-link" href="https://example.org/changelog" target="_blank" rel="noopener noreferrer nofollow">the changelog</a>');
	expect(html).toContain('<strong>Bold with <em>italic</em> inside</strong>');
	expect(html).not.toContain('href="javascript:');
	await page.goto('/groups/-1001775585202/chat');
	await expect(page.locator('.a-livestate')).toBeVisible();
	const text = page.locator('.c-text', { hasText: 'Release notes:' });
	await text.scrollIntoViewIfNeeded();
	await expect(text.getByRole('link', { name: 'the changelog' })).toHaveAttribute('target', '_blank');
	await expect(text.getByText('a sneaky link')).not.toHaveAttribute('href');
	// The spoiler opens from the keyboard.
	const spoiler = text.getByRole('button', { name: /spoiler/i });
	await expect(spoiler).toHaveCSS('color', 'rgba(0, 0, 0, 0)');
	await spoiler.focus();
	await page.keyboard.press('Enter');
	await expect(text.locator('.rt-spoiler.shown')).toHaveText('a hidden ending');
	// A folded quote opens.
	const quote = page.locator('blockquote.rt-expandable');
	const folded = (await quote.boundingBox())!.height;
	await quote.getByRole('button', { name: 'Show more' }).click();
	await expect.poll(async () => (await quote.boundingBox())!.height).toBeGreaterThan(folded);
	await expect(page.locator('pre.rt-pre code.language-rust')).toContainText('println!');
	// A hashtag searches the chat's messages.
	await text.getByRole('link', { name: '#release' }).click();
	await expect(page).toHaveURL(/\/group_messages\?q=%23release&f=/);
});

test('the message page draws text entities', async ({ page }) => {
	const w = await (await page.request.get('/api/chats/group/-1001775585202/messages?limit=60')).json();
	const m = w.messages.find((x: { text: string }) => x.text.startsWith('Release notes:'));
	await page.goto(`/messages/group/${m.id}`);
	const text = page.locator('.a-msgtext');
	await expect(text.getByRole('link', { name: 'https://example.org/notes' })).toHaveAttribute('rel', 'noopener noreferrer nofollow');
	await expect(text.locator('code.rt-code')).toHaveText('inline code');
	await expect(text.getByRole('link', { name: '@nobody_here_x' })).toHaveAttribute('href', 'https://t.me/nobody_here_x');
});

test('the message page shows a widget\'s HTML as text behind Show source, never running it', async ({ page }) => {
	type M = { id: number; detail: { kind: string; widget_id?: string | null } | null };
	const w = await (await page.request.get('/api/chats/group/-1001775585202/messages?limit=100')).json();
	const m = (w.messages as M[]).find((x) => x.detail?.kind === 'widget' && !!x.detail.widget_id)!;
	await page.goto(`/messages/group/${m.id}`);
	const src = page.locator('details.k-widgetsrc');
	await expect(src.locator('pre')).toBeHidden();
	await src.getByText('Show source').click();
	await expect(src.locator('pre code')).toContainText('<script>document.title');
	// Neither the script nor the img's onerror ran, and no element of the
	// widget's HTML is in the page.
	expect(await page.title()).not.toBe('widget ran');
	expect(await page.locator('.a-msgkind script, .a-msgkind img, .a-msgkind iframe').count()).toBe(0);
});

test('a reply\'s quote far outside the window opens the chat at the replied message, as ?at= does', async ({ page }) => {
	type M = { message_id: number; content_type: string; deleted_at: string | null; reply_to: unknown };
	const api = async (q: string) => (await (await page.request.get(`/api/page/groups/${G}/chat${q}`)).json()).props.messages as M[];
	// A message from the oldest window, far before the newest one, and the
	// newest window's last message made a reply to it.
	const oldest = await api('?after=0');
	const target = oldest.filter((m) => m.content_type !== 'service' && !m.deleted_at)[30]!;
	const newest = await api('');
	expect(newest.some((m) => m.message_id === target.message_id), 'the target is outside the newest window').toBe(false);
	const reply = newest.filter((m) => m.content_type !== 'service' && !m.deleted_at).at(-1)!;
	await page.route((u) => decodeURIComponent(u.pathname) === `/api/page/groups/${G}/chat` && !u.searchParams.has('at'), async (route) => {
		const res = await route.fetch();
		const body = await res.json();
		const m = body.props.messages.find((x: M) => x.message_id === reply.message_id);
		m.reply_to = { message_id: target.message_id, sender_id: 0, sender_name: 'Far sender', snippet: 'a far message', resolved: true };
		await route.fulfill({ response: res, json: body });
	});
	// Opened in the app, so the page data comes through the route above.
	await page.goto('/users');
	await expect(page.getByRole('heading', { name: 'Users', level: 1 })).toBeVisible();
	await page.evaluate((to) => { history.pushState(null, '', to); dispatchEvent(new PopStateEvent('popstate')); }, `/groups/${G}/chat`);
	await expect(page.locator('.a-livestate')).toBeVisible();
	await page.locator(`#m${reply.message_id} .c-reply`).click();
	await expect(page).toHaveURL(new RegExp(`\\?at=${target.message_id}$`));
	const t = page.locator(`#m${target.message_id}`);
	await expect(t).toHaveClass(/flash/);
	// In the middle of the view, where a direct ?at= load puts it.
	const offset = () => t.evaluate((el) => {
		const s = el.closest('.c-scroll')!.getBoundingClientRect();
		const r = el.getBoundingClientRect();
		return Math.abs((r.top + r.bottom) / 2 - (s.top + s.bottom) / 2) / s.height;
	});
	await expect.poll(offset).toBeLessThan(0.25);
	// And it stays there once the new window has settled.
	await page.waitForTimeout(500);
	expect(await offset()).toBeLessThan(0.25);
});
