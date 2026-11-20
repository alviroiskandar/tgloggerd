import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { BACKOFF_MAX_MS, BACKOFF_MIN_MS, LiveManager, jittered, type Clock, type SourceLike } from './manager.ts';
import { Refetcher, type Visibility } from './refetch.ts';
import type { LiveEvent } from './events.ts';

const clock: Clock = { now: () => Date.now(), setTimeout: (fn, ms) => setTimeout(fn, ms), clearTimeout: (id) => clearTimeout(id as never) };

class FakeSource implements SourceLike {
	readyState = 0;
	onopen: ((ev: Event) => unknown) | null = null;
	onerror: ((ev: Event) => unknown) | null = null;
	closed = false;
	private ls = new Map<string, ((ev: { data: string }) => void)[]>();
	constructor(public url: string) {}
	addEventListener(kind: string, fn: (ev: { data: string }) => void) { this.ls.set(kind, [...(this.ls.get(kind) ?? []), fn]); }
	close() { this.closed = true; this.readyState = 2; }
	openNow() { this.readyState = 1; this.onopen?.(new Event("open")); }
	fail(closed = false) { this.readyState = closed ? 2 : 0; this.onerror?.(new Event("error")); }
	emit(e: LiveEvent) { for (const fn of this.ls.get(e.kind) ?? []) fn({ data: JSON.stringify(e) }); }
}

// random: the back-off's jitter; at 1 each wait is the whole step.
function setup(random: () => number = () => 1) {
	const sources: FakeSource[] = [];
	const m = new LiveManager({ open: (url) => { const s = new FakeSource(url); sources.push(s); return s; }, clock, random });
	const states: string[] = [];
	m.onState((s) => states.push(s));
	return { m, sources, states, last: () => sources[sources.length - 1]! };
}

beforeEach(() => vi.useFakeTimers());
afterEach(() => vi.useRealTimers());

describe('LiveManager', () => {
	it('opens one stream for the union of topics once they settle', () => {
		const { m, sources, last } = setup();
		m.subscribe(['chat:-1', 'user:7'], () => {});
		m.subscribe(['user:7', 'stats'], () => {});
		expect(sources).toHaveLength(0);
		vi.advanceTimersByTime(300);
		expect(sources).toHaveLength(1);
		expect(decodeURIComponent(last().url)).toBe('/api/events?topics=chat:-1,stats,user:7');
	});

	it('reconnects with last= on a topic change and closes the old stream only once the new one is open', () => {
		const { m, sources } = setup();
		const un = m.subscribe(['chat:-1'], () => {});
		vi.advanceTimersByTime(300);
		const a = sources[0]!;
		a.openNow();
		a.emit({ seq: 41, kind: 'message.new', chat_id: -1, message_id: 5 });
		un();
		m.subscribe(['chat:-2'], () => {});
		vi.advanceTimersByTime(300);
		const b = sources[1]!;
		expect(b.url).toContain('last=41');
		expect(a.closed).toBe(false);
		b.openNow();
		expect(a.closed).toBe(true);
	});

	it('closes the stream when nothing is subscribed', () => {
		const { m, sources, states } = setup();
		const un = m.subscribe(['stats'], () => {});
		vi.advanceTimersByTime(300);
		sources[0]!.openNow();
		un();
		vi.advanceTimersByTime(300);
		expect(sources[0]!.closed).toBe(true);
		expect(states.at(-1)).toBe('idle');
	});

	it('delivers events by topic and drops repeats', () => {
		const { m, last } = setup();
		const a = vi.fn(), b = vi.fn(), all = vi.fn();
		m.subscribe(['chat:-1'], a);
		m.subscribe(['user:7'], b);
		m.subscribe(['messages'], all);
		vi.advanceTimersByTime(300);
		last().openNow();
		const e: LiveEvent = { seq: 1, kind: 'message.new', chat_id: -1, message_id: 9 };
		last().emit(e);
		last().emit(e);
		last().emit({ seq: 2, kind: 'user.updated', user_id: 7 });
		last().emit({ seq: 3, kind: 'resync' });
		expect(a.mock.calls.map((c) => c[0].seq)).toEqual([1, 3]);
		expect(b.mock.calls.map((c) => c[0].seq)).toEqual([2, 3]);
		expect(all.mock.calls.map((c) => c[0].seq)).toEqual([1, 3]);
	});

	it('switches to polling after three failures in 30 s, backs off, and resyncs when the stream returns', () => {
		const { m, sources, states, last } = setup();
		const fn = vi.fn();
		m.subscribe(['chat:-1'], fn);
		vi.advanceTimersByTime(300);
		last().openNow();
		last().fail();
		last().fail();
		expect(states.at(-1)).toBe('reconnecting');
		last().fail();
		expect(states.at(-1)).toBe('polling');
		expect(sources).toHaveLength(1);
		vi.advanceTimersByTime(5000);
		expect(sources).toHaveLength(2);
		last().fail(true); // still down: wait 10 s now
		vi.advanceTimersByTime(9999);
		expect(sources).toHaveLength(2);
		vi.advanceTimersByTime(1);
		expect(sources).toHaveLength(3);
		last().openNow();
		expect(states.at(-1)).toBe('live');
		expect(fn).toHaveBeenCalledWith(expect.objectContaining({ kind: 'resync' }));
	});

	it('jitters each back-off wait between half its step and the whole step', () => {
		const rolls = [0, 0.5, 0.999];
		const { m, sources, last } = setup(() => rolls.shift() ?? 0);
		m.subscribe(['stats'], () => {});
		vi.advanceTimersByTime(300);
		// Refused three times: steps of 5, 10 and 20 s.
		const waits: number[] = [];
		for (let i = 0; i < 3; i++) {
			const n = sources.length;
			last().fail(true);
			let t = 0;
			while (sources.length === n && t < BACKOFF_MAX_MS) { vi.advanceTimersByTime(1); t++; }
			waits.push(t);
		}
		expect(waits).toEqual([2500, 7500, 19990]);
	});

	it('keeps every jittered wait within its bounds', () => {
		for (let step = BACKOFF_MIN_MS; step <= BACKOFF_MAX_MS; step *= 2) {
			for (const r of [0, 0.25, 0.5, 0.75, 0.9999, -1, 2]) {
				const w = jittered(step, () => r);
				expect(w).toBeGreaterThanOrEqual(step / 2);
				expect(w).toBeLessThanOrEqual(step);
			}
		}
		const many = new Set(Array.from({ length: 50 }, () => jittered(BACKOFF_MIN_MS, Math.random)));
		expect(many.size).toBeGreaterThan(1);
	});

	it('polls at once when the server refuses the stream', () => {
		const { m, states, last } = setup();
		m.subscribe(['stats'], () => {});
		vi.advanceTimersByTime(300);
		last().fail(true);
		expect(states.at(-1)).toBe('polling');
	});
});

function visibility(): Visibility & { set(h: boolean): void } {
	let hidden = false;
	let fns: (() => void)[] = [];
	return {
		hidden: () => hidden,
		onVisible: (fn) => { fns.push(fn); return () => { fns = fns.filter((f) => f !== fn); }; },
		set(h) { hidden = h; if (!h) fns.forEach((f) => f()); },
	};
}

describe('Refetcher', () => {
	it('turns a burst into one run and at most one trailing run per interval', async () => {
		const r = new Refetcher(clock, visibility());
		const fn = vi.fn(async () => {});
		for (let i = 0; i < 10; i++) r.request('page', 2000, fn);
		await vi.advanceTimersByTimeAsync(0);
		expect(fn).toHaveBeenCalledTimes(1);
		for (let i = 0; i < 10; i++) r.request('page', 2000, fn);
		await vi.advanceTimersByTimeAsync(1999);
		expect(fn).toHaveBeenCalledTimes(1);
		await vi.advanceTimersByTimeAsync(1);
		expect(fn).toHaveBeenCalledTimes(2);
		await vi.advanceTimersByTimeAsync(10_000);
		expect(fn).toHaveBeenCalledTimes(2);
	});

	it('defers while hidden and runs once on show', async () => {
		const v = visibility();
		const r = new Refetcher(clock, v);
		const fn = vi.fn(async () => {});
		v.set(true);
		r.request('a', 250, fn);
		r.request('a', 250, fn);
		await vi.advanceTimersByTimeAsync(5000);
		expect(fn).not.toHaveBeenCalled();
		v.set(false);
		await vi.advanceTimersByTimeAsync(0);
		expect(fn).toHaveBeenCalledTimes(1);
	});

	it('runs at most four at once', async () => {
		const r = new Refetcher(clock, visibility());
		const done: (() => void)[] = [];
		let active = 0, peak = 0;
		const fn = () => new Promise<void>((res) => { active++; peak = Math.max(peak, active); done.push(() => { active--; res(); }); });
		for (let i = 0; i < 6; i++) r.request(`k${i}`, 1000, fn);
		await vi.advanceTimersByTimeAsync(0);
		expect(active).toBe(4);
		done.shift()!();
		await vi.advanceTimersByTimeAsync(0);
		expect(active).toBe(4);
		while (done.length) { done.shift()!(); await vi.advanceTimersByTimeAsync(0); }
		expect(peak).toBe(4);
		expect(active).toBe(0);
	});

	it('runs again after a run that a request arrived during', async () => {
		const r = new Refetcher(clock, visibility());
		let release: () => void = () => {};
		const fn = vi.fn(() => new Promise<void>((res) => { release = res; }));
		r.request('k', 100, fn);
		await vi.advanceTimersByTimeAsync(0);
		r.request('k', 100, fn);
		release();
		await vi.advanceTimersByTimeAsync(100);
		expect(fn).toHaveBeenCalledTimes(2);
	});
});
