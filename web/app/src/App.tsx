import { useEffect, type ReactNode } from 'react';
import { NavProvider, useNav } from './nav.tsx';
import { LiveProvider } from './live/react.tsx';
import { PageLive } from './live/PageLive.tsx';
import { EntityProvider } from './live/useEntities.tsx';
import { routes } from './routes/index.tsx';
import { LoadingPill } from './ui/LoadingPill.tsx';
import { UnknownRoute, type PageData } from './page.ts';
import { applyHead } from './head.ts';
import './styles/base.css';
import './styles/app.css';
import './styles/chat.css';

export function routeOf(page: PageData) {
	const r = routes[page.route];
	if (!r) throw new UnknownRoute(page.route);
	return r;
}

function LiveRoot({ children }: { children: ReactNode }) {
	const { viewer, page } = useNav();
	// Signed out, only an exposed group's public chat has a stream; every
	// other signed-out page has none.
	const mode = viewer.signed_in ? 'signed-in' : page.route === 'group_chat' ? 'signed-out' : null;
	return <LiveProvider mode={mode}><EntityProvider enabled={viewer.signed_in}><PageLive>{children}</PageLive></EntityProvider></LiveProvider>;
}

function Current() {
	const { page } = useNav();
	const route = routeOf(page);
	// The server writes the first page's head; a page the app moves to
	// without a reload, or back to, gets its own here.
	useEffect(() => applyHead(route.head(page)), [page]);
	const { Page } = route;
	return <Page page={page} />;
}

export default function App({ page }: { page: PageData }) {
	routeOf(page); // unknown routes fail before rendering anything
	return (
		<NavProvider initial={page}>
			<LiveRoot><Current /></LiveRoot>
			<LoadingPill />
		</NavProvider>
	);
}
