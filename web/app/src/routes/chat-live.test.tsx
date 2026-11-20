// @vitest-environment happy-dom
// The chat's live events for messages inside the window: edits and
// deletions update the message in place, and messages the daemon
// stores late (a recheck against Telegram found them missing) come as
// message.new for ids inside the window: each is put in its place by
// message id, not appended, and the reader's view stays where it was.
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { act, configure, screen, waitFor } from '@testing-library/react';
import { callsTo, fakeApi, fixture, renderPage, setup, type Call } from '../test/page.tsx';
import type { ChatProps } from './chat.tsx';
import type { GroupsProps } from './groups.tsx';
import type { ChatMessage } from '../types.ts';

beforeEach(setup);
configure({ asyncUtilTimeout: 5000 });

// A stand-in for the browser's EventSource: the page's live stream.
class FakeSource {
	static last: FakeSource | null = null;
	readyState = 0;
	onopen: ((ev: Event) => unknown) | null = null;
	onerror: ((ev: Event) => unknown) | null = null;
	private ls = new Map<string, ((ev: { data: string }) => void)[]>();
	constructor(public url: string) { FakeSource.last = this; }
	addEventListener(kind: string, fn: (ev: { data: string }) => void) { this.ls.set(kind, [...(this.ls.get(kind) ?? []), fn]); }
	removeEventListener() {}
	close() { this.readyState = 2; }
	open() { this.readyState = 1; this.onopen?.(new Event('open')); }
	emit(e: Record<string, unknown>) { for (const fn of this.ls.get(String(e.kind)) ?? []) fn({ data: JSON.stringify(e) }); }
}

beforeEach(() => {
	FakeSource.last = null;
	vi.stubGlobal('EventSource', FakeSource);
});
afterEach(() => vi.restoreAllMocks());

const gid = () => fixture<GroupsProps>('/groups').props.rows[0]!.id;
const url = () => `/groups/${gid()}/chat`;
const windowCalls = (calls: Call[]) => callsTo(calls, 'GET', /^\/api\/chats\/group\/-?\d+\/messages$/);
const around = (calls: Call[]) => windowCalls(calls).filter((c) => c.query.get('around'));
const after = (calls: Call[]) => windowCalls(calls).filter((c) => c.query.get('after'));
const tick = (ms = 30) => new Promise((r) => setTimeout(r, ms));

// The chat's whole window, as the server has it now.
const full = () => fixture<ChatProps>(url()).props.messages;
// The ids of the window's rows, in DOM order.
const order = () => [...document.querySelectorAll('.c-feed > [id^="m"]')].map((x) => Number(x.id.slice(1)));

// Answers "around" from the whole window as the server would; the tail
// has nothing newer.
function server(msgs: () => ChatMessage[]) {
	return fakeApi('superadmin', {
		'GET /api/chats/group/:id/messages': (c) => {
			const all = msgs();
			const a = c.query.get('around');
			if (a) {
				const i = all.findIndex((m) => m.message_id >= Number(a));
				const lim = Number(c.query.get('limit'));
				const from = Math.max(0, i - Math.floor(lim / 2));
				return { messages: all.slice(from, from + lim), has_older: true, has_newer: true };
			}
			return { messages: [], has_older: true, has_newer: false };
		},
	});
}

// The page opened, without some of its messages if given, and its
// live stream open.
async function open(missing: number[] = []) {
	const all = full();
	const shown = all.filter((m) => !missing.includes(m.message_id));
	const page = fixture<ChatProps>(url());
	renderPage(url(), 'superadmin', { props: { ...page.props, messages: shown } });
	await waitFor(() => expect(FakeSource.last?.url).toMatch(/chat%3A|chat:/));
	act(() => FakeSource.last!.open());
	return FakeSource.last!;
}

describe('messages filled in later', () => {
	it('puts a message filled mid-window between its neighbours', async () => {
		const all = full();
		const gap = all[20]!;
		const calls = server(full);
		const s = await open([gap.message_id]);
		expect(document.getElementById('m' + gap.message_id)).toBeNull();
		const tails = after(calls).length;
		act(() => s.emit({ seq: 1, kind: 'message.new', chat_id: gid(), message_id: gap.message_id }));
		await waitFor(() => expect(document.getElementById('m' + gap.message_id)).toBeTruthy());
		const o = order();
		const at = o.indexOf(gap.message_id);
		expect(o[at - 1]).toBe(all[19]!.message_id);
		expect(o[at + 1]).toBe(all[21]!.message_id);
		expect(o.filter((x) => x === gap.message_id)).toHaveLength(1);
		expect(around(calls)[0]!.query.get('around')).toBe(String(gap.message_id));
		// Not news: no tail fetch, nothing counted as unseen.
		expect(after(calls)).toHaveLength(tails);
		expect(screen.queryByRole('button', { name: /new$/ })).toBeNull();
	});

	it('fills a burst of two with one fetch, each in its place', async () => {
		const all = full();
		const a = all[30]!, b = all[31]!;
		const calls = server(full);
		const s = await open([a.message_id, b.message_id]);
		act(() => {
			s.emit({ seq: 1, kind: 'message.new', chat_id: gid(), message_id: a.message_id });
			s.emit({ seq: 2, kind: 'message.new', chat_id: gid(), message_id: b.message_id });
		});
		await waitFor(() => expect(document.getElementById('m' + b.message_id)).toBeTruthy());
		const o = order();
		const at = o.indexOf(a.message_id);
		expect(o.slice(at - 1, at + 3)).toEqual([all[29]!.message_id, a.message_id, b.message_id, all[32]!.message_id]);
		await tick(300);
		expect(around(calls)).toHaveLength(1);
	});

	it('does not fetch a message already in the window', async () => {
		const all = full();
		const calls = server(full);
		const s = await open();
		act(() => s.emit({ seq: 1, kind: 'message.new', chat_id: gid(), message_id: all[10]!.message_id }));
		await tick(300);
		expect(around(calls)).toHaveLength(0);
		expect(order().filter((x) => x === all[10]!.message_id)).toHaveLength(1);
	});

	it('ignores one older than the window, which the older page brings', async () => {
		const page = fixture<ChatProps>(url());
		expect(page.props.has_older).toBe(true);
		const calls = server(full);
		const s = await open();
		const before = windowCalls(calls).length;
		act(() => s.emit({ seq: 1, kind: 'message.new', chat_id: gid(), message_id: page.props.messages[0]!.message_id - 1 }));
		await tick(300);
		expect(windowCalls(calls)).toHaveLength(before);
	});

	it('puts one older than the window first when the window holds the oldest', async () => {
		const all = full();
		const calls = server(full);
		const page = fixture<ChatProps>(url());
		renderPage(url(), 'superadmin', { props: { ...page.props, has_older: false, messages: all.slice(1) } });
		await waitFor(() => expect(FakeSource.last?.url).toMatch(/chat%3A|chat:/));
		act(() => FakeSource.last!.open());
		act(() => FakeSource.last!.emit({ seq: 1, kind: 'message.new', chat_id: gid(), message_id: all[0]!.message_id }));
		await waitFor(() => expect(order()[0]).toBe(all[0]!.message_id));
		expect(around(calls)).toHaveLength(1);
	});

	it('still fetches the tail for a message at or after the newest', async () => {
		const last = full().at(-1)!;
		const calls = server(full);
		const s = await open();
		const tails = after(calls).length;
		act(() => s.emit({ seq: 1, kind: 'message.new', chat_id: gid(), message_id: last.message_id + 1 }));
		await waitFor(() => expect(after(calls).length).toBe(tails + 1));
		expect(after(calls).at(-1)!.query.get('after')).toBe(String(last.message_id));
		expect(around(calls)).toHaveLength(0);
	});

	it('keeps the message the reader looks at in place when one lands above it', async () => {
		const all = full();
		const gap = all[5]!;
		// Each row 100 px tall in DOM order, the view scrolled to 3000.
		let top = 3000;
		vi.spyOn(HTMLElement.prototype, 'scrollTop', 'get').mockImplementation(function (this: HTMLElement) { return this.classList.contains('c-scroll') ? top : 0; });
		vi.spyOn(HTMLElement.prototype, 'scrollTop', 'set').mockImplementation(function (this: HTMLElement, v: number) { if (this.classList.contains('c-scroll')) top = v; });
		vi.spyOn(HTMLElement.prototype, 'scrollHeight', 'get').mockImplementation(function (this: HTMLElement) { return this.classList.contains('c-scroll') ? 10_000 : 0; });
		vi.spyOn(HTMLElement.prototype, 'clientHeight', 'get').mockImplementation(function (this: HTMLElement) { return this.classList.contains('c-scroll') ? 800 : 0; });
		vi.spyOn(HTMLElement.prototype, 'getBoundingClientRect').mockImplementation(function (this: HTMLElement) {
			const rows = [...document.querySelectorAll('.c-feed > *')];
			const i = rows.indexOf(this);
			const y = i < 0 ? 0 : i * 100 - top;
			return { top: y, bottom: y + 100, left: 0, right: 0, width: 0, height: 100, x: 0, y, toJSON: () => ({}) } as DOMRect;
		});
		const calls = server(full);
		const s = await open([gap.message_id]);
		top = 3000;
		act(() => s.emit({ seq: 1, kind: 'message.new', chat_id: gid(), message_id: gap.message_id }));
		await waitFor(() => expect(document.getElementById('m' + gap.message_id)).toBeTruthy());
		expect(around(calls)).toHaveLength(1);
		// One row went in above the view: the view moved down with it.
		expect(top).toBe(3100);
	});
});

describe('edits and deletions in the window', () => {
	it('updates an edited message in place', async () => {
		const all = full();
		const m = all[15]!;
		const edited = () => full().map((x) => (x.id === m.id ? { ...x, content_type: 'text' as const, text: 'changed on recheck', media: null, album: [], detail: null, service: null, edited_at: x.date } : x));
		const calls = server(edited);
		const s = await open();
		act(() => s.emit({ seq: 1, kind: 'message.edited', chat_id: gid(), message_id: m.message_id }));
		await screen.findByText('changed on recheck');
		const o = order();
		expect(o.indexOf(m.message_id)).toBe(all.findIndex((x) => x.message_id === m.message_id));
		expect(around(calls)[0]!.query.get('around')).toBe(String(m.message_id));
	});

	it('marks a deleted message in place', async () => {
		const all = full();
		const m = all.find((x, i) => i > 10 && !x.deleted_at && x.content_type !== 'service')!;
		const gone = () => full().map((x) => (x.id === m.id ? { ...x, deleted_at: x.date } : x));
		server(gone);
		const s = await open();
		const row = () => document.getElementById('m' + m.message_id)!;
		expect(row().querySelector('.c-deltag')).toBeNull();
		act(() => s.emit({ seq: 1, kind: 'message.deleted', chat_id: gid(), message_id: m.message_id }));
		await waitFor(() => expect(row().querySelector('.c-deltag')).toBeTruthy());
		expect(order()).toHaveLength(all.length);
	});
});

// A window that does not end at the newest (opened at a message or a
// date, or scrolled back): it still follows the chat's events for the
// messages it holds, but leaves what comes after it to the newer pages.
describe('a window with newer pages', () => {
	// Answers "around" as server() does, and "after" from the whole
	// window, with more always beyond it.
	function behindServer(msgs: () => ChatMessage[]) {
		return fakeApi('superadmin', {
			'GET /api/chats/group/:id/messages': (c) => {
				const all = msgs();
				const lim = Number(c.query.get('limit'));
				const a = c.query.get('around');
				if (a) {
					const i = all.findIndex((m) => m.message_id >= Number(a));
					const from = Math.max(0, i - Math.floor(lim / 2));
					return { messages: all.slice(from, from + lim), has_older: true, has_newer: true };
				}
				const af = Number(c.query.get('after'));
				return { messages: all.filter((m) => m.message_id > af).slice(0, lim), has_older: true, has_newer: true };
			},
		});
	}

	// Opened with has_newer and without auto-load, so nothing loads the
	// newer pages by itself during the test.
	async function openBehind(missing: number[] = []) {
		const page = fixture<ChatProps>(url());
		const shown = full().filter((m) => !missing.includes(m.message_id));
		renderPage(url(), 'superadmin', { props: { ...page.props, messages: shown, has_newer: true }, viewer: { ...page.viewer, chat: { ...page.viewer.chat, auto_load: false } } });
		await waitFor(() => expect(FakeSource.last?.url).toMatch(/chat%3A|chat:/));
		act(() => FakeSource.last!.open());
		return FakeSource.last!;
	}

	it('subscribes to the chat', async () => {
		behindServer(full);
		const s = await openBehind();
		expect(decodeURIComponent(s.url)).toContain(`chat:${gid()}`);
		expect(screen.getByRole('button', { name: 'Load newer messages' })).toBeTruthy();
	});

	it('puts a message filled mid-window in its place', async () => {
		const all = full();
		const gap = all[20]!;
		const calls = behindServer(full);
		const s = await openBehind([gap.message_id]);
		act(() => s.emit({ seq: 1, kind: 'message.new', chat_id: gid(), message_id: gap.message_id }));
		await waitFor(() => expect(document.getElementById('m' + gap.message_id)).toBeTruthy());
		const o = order();
		const at = o.indexOf(gap.message_id);
		expect(o[at - 1]).toBe(all[19]!.message_id);
		expect(o[at + 1]).toBe(all[21]!.message_id);
		expect(after(calls)).toHaveLength(0);
	});

	it('updates an edited message in place', async () => {
		const m = full()[15]!;
		const edited = () => full().map((x) => (x.id === m.id ? { ...x, content_type: 'text' as const, text: 'edited while scrolled back', media: null, album: [], detail: null, service: null, edited_at: x.date } : x));
		const calls = behindServer(edited);
		const s = await openBehind();
		act(() => s.emit({ seq: 1, kind: 'message.edited', chat_id: gid(), message_id: m.message_id }));
		await screen.findByText('edited while scrolled back');
		expect(around(calls)[0]!.query.get('around')).toBe(String(m.message_id));
	});

	it('fetches and counts nothing for a new message after the window', async () => {
		const last = full().at(-1)!;
		const calls = behindServer(full);
		const s = await openBehind();
		const before = windowCalls(calls).length;
		act(() => {
			s.emit({ seq: 1, kind: 'message.new', chat_id: gid(), message_id: last.message_id + 1 });
			s.emit({ seq: 2, kind: 'message.new', chat_id: gid(), message_id: last.message_id });
		});
		await tick(400);
		expect(windowCalls(calls)).toHaveLength(before);
		expect(screen.queryByRole('button', { name: /new$/ })).toBeNull();
		expect(screen.getByRole('button', { name: 'Jump to latest' })).toBeTruthy();
	});

	it('refetches the window itself on a resync, not the tail', async () => {
		const all = full();
		const gap = all[25]!, m = all[10]!;
		const now = () => full().map((x) => (x.id === m.id ? { ...x, content_type: 'text' as const, text: 'changed while events were missed', media: null, album: [], detail: null, service: null, edited_at: x.date } : x));
		const calls = behindServer(now);
		const s = await openBehind([gap.message_id]);
		act(() => s.emit({ seq: 1, kind: 'resync' }));
		await screen.findByText('changed while events were missed');
		await waitFor(() => expect(document.getElementById('m' + gap.message_id)).toBeTruthy());
		const cursors = after(calls).map((c) => c.query.get('after'));
		expect(cursors[0]).toBe(String(all[0]!.message_id - 1));
		expect(cursors).not.toContain(String(all.at(-1)!.message_id));
		const o = order();
		expect(o).toEqual(all.map((x) => x.message_id));
		// The window stays a window: its newer pages are still to load.
		expect(screen.getByRole('button', { name: 'Load newer messages' })).toBeTruthy();
	});

	it('does not poll the tail while the stream is down', async () => {
		const calls = behindServer(full);
		const s = await openBehind();
		// Refused: the manager polls until its retry.
		act(() => { s.readyState = 2; s.onerror?.(new Event('error')); });
		await screen.findByText(/Live updates unavailable/);
		await tick(1500);
		expect(windowCalls(calls)).toHaveLength(0);
	});
});

// A signed-out visitor's public chat: chat-live-public.test.tsx.
