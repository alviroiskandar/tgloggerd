// One event stream per browser tab (web-live-client.md sections 3 and
// 6). Components subscribe with the topics they show; the manager keeps
// one EventSource for the union, reconnects when the union changes, and
// falls back to polling when the stream keeps failing. It never
// refetches anything itself: subscribers do, through the API.
//
// A signed-out stream (signedOut) has no seqs: the manager sends no
// last= and keeps none, and the server starts every open with a resync,
// so each reconnect is one refetch. Its subscribers never poll the tail
// every second; the chat refetches its window every 30 s meanwhile.
import { LIVE_KINDS, topicsOf, type LiveEvent } from './events.ts';

export type LiveState = 'idle' | 'connecting' | 'live' | 'reconnecting' | 'polling';

// The part of EventSource the manager uses, so tests can fake it.
export interface SourceLike {
	readonly readyState: number; // 0 connecting, 1 open, 2 closed
	onopen: ((ev: Event) => unknown) | null;
	onerror: ((ev: Event) => unknown) | null;
	addEventListener(kind: string, fn: (ev: { data: string; lastEventId?: string }) => void): void;
	close(): void;
	// A refusal's Retry-After, when the source can read it: the next
	// attempt waits at least this long.
	readonly retryAfterMs?: number;
}

export interface Clock {
	now(): number;
	setTimeout(fn: () => void, ms: number): unknown;
	clearTimeout(id: unknown): void;
}

export interface ManagerOptions {
	open: (url: string) => SourceLike;
	url?: string;
	clock?: Clock;
	settleMs?: number;
	// A number in [0, 1) for the back-off's jitter; Math.random unless
	// a test needs it fixed.
	random?: () => number;
	// A signed-out viewer's stream: no seqs (see above).
	signedOut?: boolean;
}

type Handler = (e: LiveEvent) => void;
interface Sub { topics: ReadonlySet<string>; fn: Handler }

const realClock: Clock = { now: () => Date.now(), setTimeout: (fn, ms) => setTimeout(fn, ms), clearTimeout: (id) => clearTimeout(id as ReturnType<typeof setTimeout>) };

export const FAILS_TO_POLL = 3;
export const FAIL_WINDOW_MS = 30_000;
export const BACKOFF_MIN_MS = 5_000;
export const BACKOFF_MAX_MS = 60_000;

// Equal jitter: each wait is half its back-off step plus a random part
// of the other half, so tabs refused or cut off at the same moment do
// not all retry at the same moment.
export function jittered(step: number, random: () => number): number {
	const r = Math.min(Math.max(random(), 0), 1);
	return Math.round(step / 2 + (step / 2) * r);
}

export class LiveManager {
	private readonly open: (url: string) => SourceLike;
	private readonly url: string;
	private readonly clock: Clock;
	private readonly settleMs: number;
	private readonly random: () => number;
	private readonly signedOut: boolean;
	private subs = new Set<Sub>();
	private stateFns = new Set<(s: LiveState) => void>();
	private src: SourceLike | null = null;
	private srcKey = '';
	private settle: unknown = null;
	private retry: unknown = null;
	private fails: number[] = [];
	private backoff = BACKOFF_MIN_MS;
	private seq = 0;
	private disposed = false;
	state: LiveState = 'idle';

	constructor(o: ManagerOptions) {
		this.open = o.open;
		this.url = o.url ?? '/api/events';
		this.clock = o.clock ?? realClock;
		this.settleMs = o.settleMs ?? 300;
		this.random = o.random ?? Math.random;
		this.signedOut = !!o.signedOut;
	}

	subscribe(topics: string[], fn: Handler): () => void {
		const sub: Sub = { topics: new Set(topics), fn };
		this.subs.add(sub);
		this.topicsChanged();
		return () => { this.subs.delete(sub); this.topicsChanged(); };
	}

	onState(fn: (s: LiveState) => void): () => void {
		this.stateFns.add(fn);
		return () => this.stateFns.delete(fn);
	}

	dispose(): void {
		this.disposed = true;
		this.clock.clearTimeout(this.settle);
		this.clock.clearTimeout(this.retry);
		this.src?.close();
		this.src = null;
		this.subs.clear();
		this.stateFns.clear();
	}

	private key(): string {
		const all = new Set<string>();
		for (const s of this.subs) for (const t of s.topics) all.add(t);
		return [...all].sort().join(',');
	}

	private setState(s: LiveState): void {
		if (s === this.state) return;
		this.state = s;
		for (const fn of this.stateFns) fn(s);
	}

	// Navigation changes the topics in steps; wait for them to settle.
	private topicsChanged(): void {
		this.clock.clearTimeout(this.settle);
		this.settle = this.clock.setTimeout(() => this.reconcile(), this.settleMs);
	}

	private reconcile(): void {
		if (this.disposed) return;
		const key = this.key();
		if (!key) {
			this.src?.close();
			this.src = null;
			this.srcKey = '';
			this.clock.clearTimeout(this.retry);
			this.setState('idle');
			return;
		}
		if (key === this.srcKey && (this.src || this.state === 'polling')) return;
		if (this.state === 'polling') { this.srcKey = key; return; } // the retry opens with the new key
		this.connect(key);
	}

	private connect(key: string): void {
		const q = new URLSearchParams({ topics: key });
		if (this.seq && !this.signedOut) q.set('last', String(this.seq));
		const old = this.src;
		const src = this.open(`${this.url}?${q}`);
		this.src = src;
		this.srcKey = key;
		const wasPolling = this.state === 'polling';
		if (!old) this.setState(wasPolling ? 'polling' : 'connecting');
		src.onopen = () => {
			if (src !== this.src) return;
			// The old stream stays until the new one is open, so topics that
			// stay are never dropped.
			old?.close();
			this.backoff = BACKOFF_MIN_MS;
			const back = this.state === 'polling';
			this.setState('live');
			// A signed-out stream starts with the server's own resync.
			if (back && !this.signedOut) this.dispatch({ seq: this.seq, kind: 'resync' });
		};
		src.onerror = () => {
			if (src !== this.src) { src.close(); return; }
			const now = this.clock.now();
			this.fails = [...this.fails.filter((t) => now - t < FAIL_WINDOW_MS), now];
			if (this.state === 'polling' || src.readyState === 2 || this.fails.length >= FAILS_TO_POLL) this.toPolling(src.retryAfterMs);
			else this.setState('reconnecting');
		};
		const onEvent = (ev: { data: string }) => {
			if (src !== this.src && src !== old) return;
			let e: LiveEvent;
			try { e = JSON.parse(ev.data) as LiveEvent; } catch { return; }
			if (typeof e.seq === 'number' && !this.signedOut) {
				// Both streams may deliver an event while they overlap.
				if (e.seq <= this.seq && e.kind !== 'resync') return;
				this.seq = Math.max(this.seq, e.seq);
			}
			this.dispatch(e);
		};
		for (const k of LIVE_KINDS) src.addEventListener(k, onEvent);
	}

	private toPolling(retryAfterMs = 0): void {
		this.src?.close();
		this.src = null;
		this.fails = [];
		this.setState('polling');
		this.clock.clearTimeout(this.retry);
		// Never before the server's Retry-After, jitter on top of it.
		const wait = Math.max(jittered(this.backoff, this.random), retryAfterMs ? retryAfterMs + jittered(BACKOFF_MIN_MS, this.random) / 2 : 0);
		this.backoff = Math.min(BACKOFF_MAX_MS, this.backoff * 2);
		this.retry = this.clock.setTimeout(() => {
			const key = this.key();
			if (!this.disposed && key) this.connect(key);
		}, wait);
	}

	private dispatch(e: LiveEvent): void {
		const topics = topicsOf(e);
		for (const s of [...this.subs]) {
			if (topics === 'all' || topics.some((t) => s.topics.has(t))) {
				try { s.fn(e); } catch (err) { console.error('live handler', err); }
			}
		}
	}
}
