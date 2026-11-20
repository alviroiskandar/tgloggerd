// Applies pageLive() to the current page: refetches it in place or
// raises the "new activity" notice the shell shows, polls while the
// stream is down, and follows session events. Signed out, only the
// public chat has a stream: it follows that chat's session events and
// nothing else (the chat view handles the rest).
import { createContext, useContext, useEffect, useState, type ReactNode } from 'react';
import { useNav } from '../nav.tsx';
import { useLive, useLiveState, useRefetcher } from './react.tsx';
import { pageLive } from './pages.ts';
import type { LiveEvent } from './events.ts';

interface Notice { shown: boolean; apply: () => void; dismiss: () => void }
const NoticeCtx = createContext<Notice>({ shown: false, apply: () => {}, dismiss: () => {} });
export const useLiveNotice = () => useContext(NoticeCtx);

export const POLL_MS = 30_000;

export function PageLive({ children }: { children: ReactNode }) {
	const { page, refresh } = useNav();
	const refetch = useRefetcher();
	const state = useLiveState();
	const signedOut = !page.viewer.signed_in;
	const spec = signedOut ? null : pageLive(page);
	const chat = signedOut && page.route === 'group_chat' ? (page.props as { chat?: { kind: string; group?: { id: number } } }).chat : undefined;
	const publicTopics = chat?.kind === 'group' && chat.group ? [`chat:${chat.group.id}`] : null;
	const [shown, setShown] = useState(false);
	useEffect(() => setShown(false), [page]);

	const doRefresh = () => {
		if (!spec) return;
		if (refetch) refetch.request('page', spec.interval, refresh);
		else void refresh();
	};

	useLive(spec?.topics ?? publicTopics, (e: LiveEvent) => {
		if (signedOut) {
			// A visitor has no session to lose: revoked is not for it. A
			// withdrawn group (scope_changed) reloads the page, whose answer
			// says whether the chat is still public.
			if (e.kind === 'session' && e.reason === 'scope_changed') void refresh();
			return;
		}
		if (e.kind === 'session') {
			if (e.reason === 'revoked') location.assign(`/login?next=${encodeURIComponent(page.url)}`);
			else void refresh();
			return;
		}
		if (!spec || (e.kind !== 'resync' && !spec.match(e))) return;
		if (spec.mode === 'refresh') doRefresh();
		else setShown(true);
	});

	// Without the stream, pages that refresh in place poll, and refetch
	// when the window gets focus; hidden tabs do not.
	const polls = spec?.mode === 'refresh' && state === 'polling';
	useEffect(() => {
		if (!polls) return;
		const tick = () => { if (!document.hidden) doRefresh(); };
		const t = setInterval(tick, POLL_MS);
		addEventListener('focus', tick);
		return () => { clearInterval(t); removeEventListener('focus', tick); };
	}, [polls, page.url]);

	const notice: Notice = {
		shown,
		apply: () => { setShown(false); void refresh(); },
		dismiss: () => setShown(false),
	};
	return <NoticeCtx.Provider value={notice}>{children}</NoticeCtx.Provider>;
}
