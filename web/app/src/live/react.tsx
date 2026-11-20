// React side of live updates: one manager and one refetcher per tab,
// created in the browser only (the server render has no stream): for
// signed-in viewers, and for a signed-out one only on an exposed group's
// public chat, whose stream (signed-out mode, read with FetchSource)
// may carry nothing but that chat's events.
import { createContext, useContext, useEffect, useRef, useState, type ReactNode } from 'react';
import { LiveManager, type LiveState } from './manager.ts';
import { Refetcher } from './refetch.ts';
import { FetchSource } from './fetchsource.ts';
import type { LiveEvent } from './events.ts';

interface Live { manager: LiveManager; refetch: Refetcher }

const Ctx = createContext<Live | null>(null);
const StateCtx = createContext<LiveState>('idle');

const clock = { now: () => Date.now(), setTimeout: (fn: () => void, ms: number) => setTimeout(fn, ms), clearTimeout: (id: unknown) => clearTimeout(id as ReturnType<typeof setTimeout>) };

export type LiveMode = 'signed-in' | 'signed-out' | null;

export function LiveProvider({ mode, children }: { mode: LiveMode; children: ReactNode }) {
	const [live, setLive] = useState<Live | null>(null);
	const [state, setState] = useState<LiveState>('idle');
	useEffect(() => {
		// Without EventSource (and in page tests, which take it away) no
		// stream at all, signed out included.
		if (!mode || typeof EventSource === 'undefined') return;
		const manager = mode === 'signed-out'
			? new LiveManager({ open: (url) => new FetchSource(url), clock, signedOut: true })
			: new LiveManager({ open: (url) => new EventSource(url, { withCredentials: true }), clock });
		const refetch = new Refetcher(clock);
		const off = manager.onState(setState);
		setLive({ manager, refetch });
		return () => { off(); manager.dispose(); refetch.dispose(); setLive(null); setState('idle'); };
	}, [mode]);
	return <Ctx.Provider value={live}><StateCtx.Provider value={state}>{children}</StateCtx.Provider></Ctx.Provider>;
}

export function useLiveState(): LiveState {
	return useContext(StateCtx);
}

export function useRefetcher(): Refetcher | null {
	return useContext(Ctx)?.refetch ?? null;
}

// Subscribe to topics while mounted. The handler may change on every
// render; the subscription changes only with the topics.
export function useLive(topics: string[] | null, handler: (e: LiveEvent) => void): void {
	const live = useContext(Ctx);
	const fn = useRef(handler);
	fn.current = handler;
	const key = topics ? [...topics].sort().join(',') : '';
	useEffect(() => {
		if (!live || !key) return;
		return live.manager.subscribe(key.split(','), (e) => fn.current(e));
	}, [live, key]);
}
