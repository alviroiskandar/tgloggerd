// An exposed group's public chat, read signed out, follows the chat's
// live events through a signed-out stream.
import { test, expect } from './fixtures.ts';
import type { APIRequestContext, Page } from '@playwright/test';

async function exposedGroup(request: APIRequestContext): Promise<number> {
	const rows = (await (await request.get('/api/page/groups?q=CTF', { headers: { cookie: 'tgd_as=superadmin' } })).json()).props.rows as { id: number; title: string; exposed?: boolean }[];
	const g = rows.find((r) => r.title === 'CTF Practice' && r.exposed);
	expect(g, 'an exposed sample group in the stand-in').toBeTruthy();
	return g!.id;
}

const streams = (page: Page) => {
	const urls: string[] = [];
	page.on('request', (r) => { if (new URL(r.url()).pathname === '/api/events') urls.push(r.url()); });
	return urls;
};
const footer = (page: Page) => page.locator('.c-readonly');

test('a message filled into an exposed group\'s public chat appears without a reload', async ({ page, as, emit, request }) => {
	const G = await exposedGroup(request);
	await as('public');
	const urls = streams(page);
	await page.goto(`/groups/${G}/chat`);
	await expect(page.locator('.c-publicpage, .c-chat.public').first()).toBeVisible();
	await expect(footer(page)).toContainText('New messages appear as they are logged.');
	await page.evaluate(() => { (window as unknown as { __mark: number }).__mark = 1; });
	// A free message id between two messages of the window.
	const msgs = (await (await page.request.get(`/api/page/groups/${G}/chat`)).json()).props.messages as { message_id: number }[];
	const i = msgs.findIndex((m, k) => k > 0 && k < msgs.length - 2 && msgs[k + 1]!.message_id - m.message_id > 1);
	expect(i, 'a gap in the window').toBeGreaterThan(0);
	const gap = msgs[i]!.message_id + 1;
	await emit({ kind: 'message.new', chat_id: G, message_id: gap });
	// At most one refetch per refetch_interval (5 s), so it may wait.
	await expect(page.locator(`#m${gap}`)).toContainText('Found on recheck', { timeout: 15_000 });
	const ids = await page.locator('.c-feed > [id^="m"]').evaluateAll((els) => els.map((el) => el.id));
	const at = ids.indexOf(`m${gap}`);
	expect([ids[at - 1], ids[at + 1]]).toEqual([`m${msgs[i]!.message_id}`, `m${msgs[i + 1]!.message_id}`]);
	const e = await emit({ kind: 'message.new', chat_id: G });
	await expect(page.locator(`#m${e.message_id}`)).toBeAttached({ timeout: 15_000 });
	expect(await page.evaluate(() => (window as unknown as { __mark?: number }).__mark)).toBe(1);
	// One stream, for this chat alone, never resuming.
	expect(urls.length).toBeGreaterThan(0);
	for (const u of urls) {
		const q = new URL(u).searchParams;
		expect(q.get('topics')).toBe(`chat:${G}`);
		expect(q.has('last')).toBe(false);
	}
});

test('a refused visitor is told live updates are unavailable, and goes live again', async ({ page, as, request, standinUrl }) => {
	test.setTimeout(90_000);
	const G = await exposedGroup(request);
	await as('public');
	await page.goto(`/groups/${G}/chat`);
	await expect(footer(page)).toContainText('New messages appear as they are logged.');
	const tails: string[] = [];
	page.on('request', (r) => { if (/\/api\/chats\/group\/-?\d+\/messages/.test(r.url())) tails.push(r.url()); });
	await fetch(`${standinUrl}/api/_standin/events`, { method: 'POST', body: JSON.stringify({ public_full: 3 }) });
	await expect(footer(page)).toContainText('Live updates unavailable');
	// No tail polling every second while refused.
	await page.waitForTimeout(2500);
	expect(tails.length).toBeLessThanOrEqual(1);
	await fetch(`${standinUrl}/api/_standin/events`, { method: 'POST', body: JSON.stringify({ public_full: 0 }) });
	await expect(footer(page)).toContainText('New messages appear as they are logged.', { timeout: 60_000 });
});

test('a withdrawn group\'s public chat reloads and leaves the archive', async ({ page, as, request }) => {
	const G = await exposedGroup(request);
	await as('public');
	await page.goto(`/groups/${G}/chat`);
	await expect(footer(page)).toContainText('New messages appear as they are logged.');
	const r = await request.delete(`/api/mcp/groups/${G}`, { headers: { cookie: 'tgd_as=superadmin', 'x-csrf-token': 'standin-csrf' } });
	expect(r.status()).toBe(204);
	await expect(page.locator('.c-chat.public')).toHaveCount(0, { timeout: 10_000 });
	expect(page.url()).not.toContain('/login');
});

test('signed out, other pages open no stream', async ({ page, as, request }) => {
	const G = await exposedGroup(request);
	await as('public');
	const urls = streams(page);
	await page.goto(`/groups/${G}`);
	await expect(page.locator('.c-publicpage')).toBeVisible();
	await page.goto('/login');
	await page.waitForTimeout(1000);
	expect(urls).toEqual([]);
});
