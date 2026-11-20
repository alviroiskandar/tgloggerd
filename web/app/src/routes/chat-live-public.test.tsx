// @vitest-environment happy-dom
// The public chat of an exposed group, read signed out, follows the
// chat's live events through a signed-out stream: no seqs, no tail
// polling every second, refetches at most every refetch_interval
// (5 s unless the stream's first resync says otherwise), and every
// 30 s while the stream is refused or down, with the footer saying so.
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { act, configure, screen, waitFor } from '@testing-library/react';
import { callsTo, fakeApi, fixture, renderPage, setup, type Call } from '../test/page.tsx';
import { PUBLIC_POLL_MS } from './chat.tsx';
import type { ChatProps } from './chat.tsx';
import type { GroupsProps } from './groups.tsx';
import type { ChatMessage } from '../types.ts';

const { FakeSource } = vi.hoisted(() => {
	class FakeSource {
		static all: FakeSource[] = [];
		readyState = 0;
		onopen: ((ev: Event) => unknown) | null = null;
		onerror: ((ev: Event) => unknown) | null = null;
		retryAfterMs: number | undefined;
		private ls = new Map<string, ((ev: { data: string }) => void)[]>();
		constructor(public url: string) { FakeSource.all.push(this); }
		addEventListener(kind: string, fn: (ev: { data: string }) => void) { this.ls.set(kind, [...(this.ls.get(kind) ?? []), fn]); }
		removeEventListener() {}
		close() { this.readyState = 2; }
		open() { this.readyState = 1; this.onopen?.(new Event('open')); }
		emit(e: Record<string, unknown>) { for (const fn of this.ls.get(String(e.kind)) ?? []) fn({ data: JSON.stringify(e) }); }
		refuse(retryAfterMs?: number) { this.retryAfterMs = retryAfterMs; this.readyState = 2; this.onerror?.(new Event('error')); }
	}
	return { FakeSource };
});
vi.mock('../live/fetchsource.ts', () => ({ FetchSource: FakeSource }));

beforeEach(setup);
beforeEach(() => {
	FakeSource.all = [];
	// Defined, so the app opens streams; signed out they are FetchSource.
	vi.stubGlobal('EventSource', class { constructor() { throw new Error('a visitor\'s stream is not an EventSource'); } });
});
afterEach(() => { vi.restoreAllMocks(); vi.useRealTimers(); setHidden(false); });
configure({ asyncUtilTimeout: 5000 });

function setHidden(h: boolean) {
	Object.defineProperty(document, 'hidden', { value: h, configurable: true });
	Object.defineProperty(document, 'visibilityState', { value: h ? 'hidden' : 'visible', configurable: true });
}

const gid = () => fixture<GroupsProps>('/groups').props.rows.find((g) => g.exposed)!.id;
const url = () => `/groups/${gid()}/chat`;
const full = () => fixture<ChatProps>(url(), 'public').props.messages;
const windowCalls = (calls: Call[]) => callsTo(calls, 'GET', /^\/api\/chats\/group\/-?\d+\/messages$/);
const after = (calls: Call[]) => windowCalls(calls).filter((c) => c.query.get('after'));
const tail = (calls: Call[]) => after(calls).filter((c) => c.query.get('after') === String(full().at(-1)!.message_id));
const tick = (ms = 30) => new Promise((r) => setTimeout(r, ms));
const footer = () => document.querySelector('.c-readonly')!.textContent!;

// Answers "after" from the whole chat as the server would, with newer
// pages beyond the window when `behind`.
function server(msgs: () => ChatMessage[], behind = false) {
	return fakeApi('public', {
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
			const got = all.filter((m) => m.message_id > af).slice(0, lim);
			return { messages: got, has_older: true, has_newer: behind || got.length === lim };
		},
	});
}

async function open(o: { behind?: boolean; missing?: number[] } = {}) {
	const page = fixture<ChatProps>(url(), 'public');
	const shown = full().filter((m) => !(o.missing ?? []).includes(m.message_id));
	renderPage(url(), 'public', { props: { ...page.props, messages: shown, has_newer: !!o.behind }, viewer: { ...page.viewer, chat: { ...page.viewer.chat, auto_load: false } } });
	await waitFor(() => expect(FakeSource.all.length).toBeGreaterThan(0));
	const s = FakeSource.all.at(-1)!;
	act(() => s.open());
	return s;
}

describe('a signed-out visitor at the newest', () => {
	it('subscribes to the chat alone, without resuming', async () => {
		server(full);
		const s = await open();
		expect(new URL(s.url, 'http://x').searchParams.get('topics')).toBe(`chat:${gid()}`);
		expect(s.url).not.toContain('last=');
		expect(footer()).toContain('New messages appear as they are logged.');
	});

	it('fetches the tail for a new message, and a burst as one refetch per interval', async () => {
		const calls = server(full);
		const s = await open();
		act(() => s.emit({ kind: 'resync', refetch_interval: 1 }));
		await waitFor(() => expect(tail(calls)).toHaveLength(1));
		act(() => { for (let i = 1; i <= 5; i++) s.emit({ kind: 'message.new', chat_id: gid(), message_id: full().at(-1)!.message_id + i }); });
		await tick(500);
		expect(tail(calls)).toHaveLength(1);
		await waitFor(() => expect(tail(calls)).toHaveLength(2));
		await tick(1300);
		expect(tail(calls)).toHaveLength(2);
	});

	it('refetches at most every 5 s when the stream names no interval', async () => {
		const calls = server(full);
		const s = await open();
		act(() => s.emit({ kind: 'resync' }));
		await waitFor(() => expect(tail(calls)).toHaveLength(1));
		act(() => s.emit({ kind: 'message.new', chat_id: gid(), message_id: full().at(-1)!.message_id + 1 }));
		await tick(2000);
		expect(tail(calls)).toHaveLength(1);
	});

	it('refetches the window for an edit, a deletion and a filled message together', async () => {
		const all = full();
		const gap = all[20]!, m = all[10]!, d = all[12]!;
		const now = () => full().map((x) => (x.id === m.id ? { ...x, content_type: 'text' as const, text: 'edited for the visitors', media: null, album: [], detail: null, service: null, edited_at: x.date } : x));
		const calls = server(now);
		const s = await open({ missing: [gap.message_id] });
		act(() => {
			s.emit({ kind: 'message.edited', chat_id: gid(), message_id: m.message_id });
			s.emit({ kind: 'message.deleted', chat_id: gid(), message_id: d.message_id });
			s.emit({ kind: 'message.new', chat_id: gid(), message_id: gap.message_id });
		});
		await screen.findByText('edited for the visitors');
		await waitFor(() => expect(document.getElementById('m' + gap.message_id)).toBeTruthy());
		await tick(300);
		// One pass over the window from its first message; nothing around
		// each message.
		expect(windowCalls(calls).filter((c) => c.query.get('around'))).toHaveLength(0);
		expect(after(calls).filter((c) => c.query.get('after') === String(all[0]!.message_id - 1))).toHaveLength(1);
	});

	it('never polls the tail every second while refused', async () => {
		const calls = server(full);
		const s = await open();
		act(() => s.refuse(600_000));
		await waitFor(() => expect(footer()).toContain('Live updates unavailable'));
		expect(footer()).not.toContain('New messages appear');
		await tick(2500);
		expect(windowCalls(calls)).toHaveLength(0);
	});
});

describe('a signed-out visitor whose stream is down', () => {
	it('refetches every 30 s while visible, pauses while hidden, refetches on showing, and goes back to live', async () => {
		vi.useFakeTimers({ shouldAdvanceTime: true });
		const calls = server(full);
		const s = await open();
		act(() => s.refuse(PUBLIC_POLL_MS * 4));
		await vi.advanceTimersByTimeAsync(100);
		expect(footer()).toContain('Live updates unavailable; checking for new messages every 30 seconds.');
		const c0 = windowCalls(calls).length;
		await vi.advanceTimersByTimeAsync(PUBLIC_POLL_MS - 1000);
		expect(windowCalls(calls)).toHaveLength(c0);
		await vi.advanceTimersByTimeAsync(1500);
		const c1 = windowCalls(calls).length;
		expect(c1).toBeGreaterThan(c0);
		// Hidden: nothing for two rounds.
		setHidden(true);
		document.dispatchEvent(new Event('visibilitychange'));
		await vi.advanceTimersByTimeAsync(PUBLIC_POLL_MS * 2);
		expect(windowCalls(calls)).toHaveLength(c1);
		// Shown again: one refetch at once.
		setHidden(false);
		await act(async () => { document.dispatchEvent(new Event('visibilitychange')); });
		await vi.advanceTimersByTimeAsync(6000);
		const c2 = windowCalls(calls).length;
		expect(c2).toBeGreaterThan(c1);
		// The manager tries again after the Retry-After; once open, live.
		await vi.advanceTimersByTimeAsync(PUBLIC_POLL_MS * 2);
		expect(FakeSource.all.length).toBeGreaterThan(1);
		act(() => FakeSource.all.at(-1)!.open());
		await vi.advanceTimersByTimeAsync(10);
		expect(footer()).toContain('New messages appear as they are logged.');
	});
});

describe('a signed-out visitor with newer pages', () => {
	it('ignores a new message after the window and refetches the window itself on a resync', async () => {
		const all = full();
		const calls = server(full, true);
		const s = await open({ behind: true });
		act(() => s.emit({ kind: 'message.new', chat_id: gid(), message_id: all.at(-1)!.message_id + 1 }));
		await tick(400);
		expect(windowCalls(calls)).toHaveLength(0);
		act(() => s.emit({ kind: 'resync' }));
		await waitFor(() => expect(after(calls).length).toBeGreaterThan(0));
		expect(after(calls)[0]!.query.get('after')).toBe(String(all[0]!.message_id - 1));
		expect(tail(calls)).toHaveLength(0);
		expect(screen.getByRole('button', { name: 'Load newer messages' })).toBeTruthy();
	});
});
