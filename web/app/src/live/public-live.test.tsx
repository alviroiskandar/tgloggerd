// @vitest-environment happy-dom
// Live streams for signed-out viewers: only an exposed group's public
// chat opens one, with that chat's topic alone, and a visitor's page
// never acts on a revoked session (it has none) but reloads on
// scope_changed (the group may have been withdrawn).
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { act, waitFor } from '@testing-library/react';
import { callsTo, fakeApi, fixture, renderPage, setup } from '../test/page.tsx';
import type { GroupsProps } from '../routes/groups.tsx';

// Signed-out streams are read with FetchSource; signed-in ones with
// EventSource. Both are this fake here, which records which was used.
const { FakeSource } = vi.hoisted(() => {
	class FakeSource {
		static all: FakeSource[] = [];
		readyState = 0;
		onopen: ((ev: Event) => unknown) | null = null;
		onerror: ((ev: Event) => unknown) | null = null;
		retryAfterMs: number | undefined;
		private ls = new Map<string, ((ev: { data: string }) => void)[]>();
		constructor(public url: string, public via = 'fetch') { FakeSource.all.push(this); }
		addEventListener(kind: string, fn: (ev: { data: string }) => void) { this.ls.set(kind, [...(this.ls.get(kind) ?? []), fn]); }
		removeEventListener() {}
		close() { this.readyState = 2; }
		open() { this.readyState = 1; this.onopen?.(new Event('open')); }
		emit(e: Record<string, unknown>) { for (const fn of this.ls.get(String(e.kind)) ?? []) fn({ data: JSON.stringify(e) }); }
	}
	return { FakeSource };
});
vi.mock('./fetchsource.ts', () => ({ FetchSource: FakeSource }));

class FakeEventSource extends FakeSource { constructor(url: string) { super(url, 'eventsource'); } }

beforeEach(setup);
beforeEach(() => {
	FakeSource.all = [];
	vi.stubGlobal('EventSource', FakeEventSource);
});
afterEach(() => vi.restoreAllMocks());

const tick = (ms = 30) => new Promise((r) => setTimeout(r, ms));
const exposed = () => fixture<GroupsProps>('/groups').props.rows.find((g) => g.exposed)!;
const topicsOf = (s: InstanceType<typeof FakeSource>) => new URL(s.url, 'http://x').searchParams.get('topics')!.split(',').sort();

async function openPublicChat() {
	const id = exposed().id;
	const calls = fakeApi('public', { 'GET /api/chats/group/:id/messages': () => ({ messages: [], has_older: true, has_newer: false }) });
	renderPage(`/groups/${id}/chat`, 'public');
	await waitFor(() => expect(FakeSource.all.length).toBeGreaterThan(0));
	const s = FakeSource.all.at(-1)!;
	act(() => s.open());
	return { id, s, calls };
}

describe('a signed-out visitor', () => {
	it('opens a signed-out stream on an exposed group\'s public chat, with its chat topic only', async () => {
		const { id, s } = await openPublicChat();
		expect(s.via).toBe('fetch');
		expect(topicsOf(s)).toEqual([`chat:${id}`]);
		expect(s.url).not.toContain('last=');
		expect(FakeSource.all).toHaveLength(1);
	});

	it('opens no stream on any other page', async () => {
		const id = exposed().id;
		for (const u of [`/groups/${id}`, `/groups/${id}/files`, '/login', '/groups']) {
			fakeApi('public', {});
			const { unmount } = renderPage(u, 'public');
			await tick(500);
			expect(FakeSource.all, u).toHaveLength(0);
			unmount();
		}
	});

	it('does not act on a revoked session', async () => {
		const assign = vi.fn();
		vi.stubGlobal('location', { ...location, assign, href: location.href, pathname: location.pathname, search: location.search, hash: '' });
		const { s, calls } = await openPublicChat();
		const pages = callsTo(calls, 'GET', /^\/api\/page\//).length;
		act(() => s.emit({ kind: 'session', reason: 'revoked' }));
		await tick(300);
		expect(assign).not.toHaveBeenCalled();
		expect(callsTo(calls, 'GET', /^\/api\/page\//)).toHaveLength(pages);
	});

	it('reloads the page data when the group is withdrawn', async () => {
		const { id, s, calls } = await openPublicChat();
		act(() => s.emit({ kind: 'session', reason: 'scope_changed' }));
		await waitFor(() => expect(callsTo(calls, 'GET', `/api/page/groups/${id}/chat`)).toHaveLength(1));
	});
});

describe('a signed-in viewer', () => {
	it('keeps its EventSource stream and follows a revoked session to the sign-in page', async () => {
		const assign = vi.fn();
		vi.stubGlobal('location', { ...location, assign, href: location.href, pathname: location.pathname, search: location.search, hash: '' });
		const id = exposed().id;
		fakeApi('superadmin', { 'GET /api/chats/group/:id/messages': () => ({ messages: [], has_older: true, has_newer: false }) });
		renderPage(`/groups/${id}`, 'superadmin');
		await waitFor(() => expect(FakeSource.all.length).toBeGreaterThan(0));
		const s = FakeSource.all.at(-1)!;
		expect(s.via).toBe('eventsource');
		expect(topicsOf(s)).toContain(`group:${id}`);
		act(() => s.open());
		act(() => s.emit({ seq: 1, kind: 'session', reason: 'revoked' }));
		await waitFor(() => expect(assign).toHaveBeenCalledWith(expect.stringMatching(/^\/login\?next=/)));
	});
});
