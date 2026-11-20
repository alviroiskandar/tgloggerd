// An event stream read with fetch, for signed-out viewers. EventSource
// hides a refusal's status and headers, so a visitor refused for the
// caps could not honour the Retry-After the server sends; this one keeps
// it (retryAfterMs) for the manager's next attempt. It never reconnects
// on its own and sends no Last-Event-ID: a signed-out stream has no ids,
// and every open is a resync anyway.
import type { SourceLike } from './manager.ts';

type Listener = (ev: { data: string }) => void;

export class FetchSource implements SourceLike {
	readyState = 0;
	onopen: ((ev: Event) => unknown) | null = null;
	onerror: ((ev: Event) => unknown) | null = null;
	// Set on a refusal that came with Retry-After, before onerror.
	retryAfterMs: number | undefined;
	private ls = new Map<string, Listener[]>();
	private ac = new AbortController();

	constructor(url: string, fetchFn: typeof fetch = (...a) => fetch(...a)) {
		void this.run(url, fetchFn);
	}

	addEventListener(kind: string, fn: Listener): void {
		this.ls.set(kind, [...(this.ls.get(kind) ?? []), fn]);
	}

	close(): void {
		this.readyState = 2;
		this.ac.abort();
	}

	private fail(): void {
		if (this.readyState === 2 && this.ac.signal.aborted) return;
		this.readyState = 2;
		this.ac.abort();
		this.onerror?.(new Event('error'));
	}

	private async run(url: string, fetchFn: typeof fetch): Promise<void> {
		let res: Response;
		try {
			res = await fetchFn(url, { headers: { accept: 'text/event-stream' }, cache: 'no-store', credentials: 'same-origin', signal: this.ac.signal });
		} catch { this.fail(); return; }
		if (this.ac.signal.aborted) return;
		if (!res.ok || !res.body) {
			const ra = Number(res.headers.get('retry-after') ?? '');
			if (Number.isFinite(ra) && ra > 0) this.retryAfterMs = ra * 1000;
			void res.body?.cancel().catch(() => undefined);
			this.fail();
			return;
		}
		this.readyState = 1;
		this.onopen?.(new Event('open'));
		const reader = res.body.getReader();
		const dec = new TextDecoder();
		let buf = '';
		let kind = '';
		let data: string[] = [];
		try {
			for (;;) {
				const { value, done } = await reader.read();
				if (done) break;
				buf += dec.decode(value, { stream: true });
				const lines = buf.split(/\r\n|\r|\n/);
				buf = lines.pop() ?? '';
				for (const line of lines) {
					if (line === '') {
						if (data.length) for (const fn of this.ls.get(kind || 'message') ?? []) fn({ data: data.join('\n') });
						kind = '';
						data = [];
						continue;
					}
					if (line.startsWith(':')) continue;
					const i = line.indexOf(':');
					const field = i < 0 ? line : line.slice(0, i);
					const val = i < 0 ? '' : line.slice(i + 1).replace(/^ /, '');
					if (field === 'event') kind = val;
					else if (field === 'data') data.push(val);
				}
				if (this.ac.signal.aborted) return;
			}
		} catch { /* cut off: below */ }
		this.fail();
	}
}
