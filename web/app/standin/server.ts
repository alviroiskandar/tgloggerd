// Development stand-in for the Go service's page side: routes a path to
// fixture page data, renders it with the app's render entry and serves
// the document Go would assemble, plus /api/page/<path> for browser
// navigation. Vite serves the client modules with hot reload. Never
// deployed; the real server is webapi/.
//
//   ?as=superadmin|admin|member|public   view as a sample account, or signed out
//                                 (remembered in a cookie)
//   ?ssr=0                        skip server rendering, as when the render service is down
//
// POST /api/_standin/reset puts the fixtures back as loaded.
//
// Live updates: GET /api/events is a Server-Sent Events stream as
// web-live.md specifies. Events come from POST /api/_standin/emit
// {kind, chat_id?, message_id?, user_id?}, which also makes the change
// in the fixtures (message.new with a free message_id below the newest
// fills that gap); POST /api/_standin/events {"off": true} makes the
// stream refuse and drops open ones, to exercise the polling fallback.
// Signed out, a stream may ask only chat:<id> topics of exposed groups
// (at most 4; anything else is 401), carries no ids and no seq, starts
// with a resync (with refetch_interval) on every open and leaves out
// file_id, as Go's does;
// POST /api/_standin/events {"public_full": <Retry-After seconds>}
// refuses signed-out streams with 503 as when their caps are reached
// (0 lifts it), and withdrawing a group's exposure ends its streams
// with a session scope_changed event.
// TGD_STANDIN_TICK=<ms> emits a new message into a subscribed chat that
// often.
import { createServer as createHttp, type IncomingMessage, type ServerResponse } from 'node:http';
import { createServer as createVite } from 'vite';
import { documentHtml } from './document.ts';
import { docBytes, docHeaders } from './docpreview.ts';
import { gzipSync } from 'node:zlib';

// PORT=0 picks a free port (the browser tests start one stand-in per
// worker). TGD_STANDIN_TEST=<worker> runs hot reload over this server
// instead of Vite's fixed port, which two stand-ins cannot share, and
// gives each worker its own Vite cache, reused by the next run.
const PORT = process.env.PORT !== undefined ? Number(process.env.PORT) : 5173;
const TEST = !!process.env.TGD_STANDIN_TEST;
const server = createHttp((req, res) => {
	handle(req, res).catch((e) => {
		console.error(e);
		res.writeHead(500, { 'content-type': 'text/plain' });
		res.end(String(e));
	});
});
const vite = await createVite({
	server: { middlewareMode: true, fs: { allow: ['..'] }, hmr: TEST ? { server } : true },
	...(TEST ? { cacheDir: `node_modules/.vite-standin-${process.env.TGD_STANDIN_TEST}`, logLevel: 'warn' as const } : {}),
	appType: 'custom',
});

type Persona = 'superadmin' | 'admin' | 'member' | 'noemail' | 'newpw' | 'public';
const PERSONAS: Persona[] = ['superadmin', 'admin', 'member', 'noemail', 'newpw', 'public'];
function persona(req: IncomingMessage, url: URL): Persona {
	const q = url.searchParams.get('as') as Persona | null;
	if (q && PERSONAS.includes(q)) return q;
	const c = /(?:^|; )tgd_as=(\w+)/.exec(req.headers.cookie || '')?.[1] as Persona | undefined;
	return c && PERSONAS.includes(c) ? c : 'superadmin';
}

// The page's own URL, without the stand-in's switches. Works on a copy:
// the caller still reads ?ssr= from its URL afterwards.
function cleanUrl(url: URL): string {
	const u = new URL(url);
	u.searchParams.delete('as');
	u.searchParams.delete('ssr');
	const q = u.searchParams.toString();
	return u.pathname + (q ? '?' + q : '');
}

function send(res: ServerResponse, status: number, body: unknown, cookies: string[] = []): void {
	res.writeHead(status, { 'content-type': 'application/json', 'cache-control': 'no-store', ...(cookies.length ? { 'set-cookie': cookies } : {}) });
	res.end(JSON.stringify(body));
}

function readBody(req: IncomingMessage): Promise<string> {
	return new Promise((resolve, reject) => {
		let s = '';
		req.on('data', (c) => { s += c; if (s.length > 1 << 16) req.destroy(); });
		req.on('end', () => resolve(s));
		req.on('error', reject);
	});
}

// The live event stream.
interface LiveClient { res: ServerResponse; who: Persona; topics: Set<string> }
type Ev = { seq: number; kind: string; chat_id?: number; user_id?: number; [k: string]: unknown };
const liveClients = new Set<LiveClient>();
const liveRing: Ev[] = [];
let liveSeq = 1000;
let liveOff = false;
// Seconds of Retry-After while signed-out streams are refused; 0 lets
// them in.
let publicFull = 0;
const PUBLIC_TOPICS = 4;
const PUBLIC_REFETCH_SECS = 5;

function evTopics(e: Ev): string[] | null {
	if (e.kind.startsWith('message.') || e.kind === 'file.stored') return [`chat:${e.chat_id}`, 'messages'];
	if (e.kind === 'user.updated') return [`user:${e.user_id}`];
	if (e.kind === 'group.updated') return [`group:${e.chat_id}`];
	if (e.kind === 'stats.changed') return ['stats'];
	if (e.kind === 'bot.updated') return ['bots'];
	return null;
}

function sendEv(c: LiveClient, e: Ev): void {
	if (c.who === 'public') {
		// No seq, no file_id.
		const { seq: _s, file_id: _f, ...rest } = e as Ev & { file_id?: number };
		c.res.write(`event: ${e.kind}\ndata: ${JSON.stringify(rest)}\n\n`);
		return;
	}
	c.res.write(`id: ${e.seq}\nevent: ${e.kind}\ndata: ${JSON.stringify(e)}\n\n`);
}

// The topics a signed-out stream may ask: chat:<id> of exposed groups.
function publicTopicsOk(fx: Record<string, any>, topics: Set<string>): boolean {
	if (!topics.size || topics.size > PUBLIC_TOPICS) return false;
	return [...topics].every((t) => /^chat:-\d+$/.test(t) && fx.exposedGroup(Number(t.slice(5))));
}

// Signed-out streams of a group no longer exposed end, as Go's periodic
// check ends them: scope_changed, never revoked.
async function endWithdrawn(): Promise<void> {
	const fx = await vite.ssrLoadModule('/standin/fixtures.ts');
	for (const c of [...liveClients]) {
		if (c.who !== 'public' || publicTopicsOk(fx, c.topics)) continue;
		c.res.write(`event: session\ndata: ${JSON.stringify({ kind: 'session', reason: 'scope_changed' })}\n\n`);
		c.res.end();
		liveClients.delete(c);
	}
}

async function publish(e: Omit<Ev, 'seq'>): Promise<Ev> {
	const fx = await vite.ssrLoadModule('/standin/fixtures.ts');
	const ev = { ...e, seq: ++liveSeq } as Ev;
	liveRing.push(ev);
	if (liveRing.length > 500) liveRing.shift();
	const ts = evTopics(ev);
	for (const c of liveClients) {
		if (ts && !ts.some((t) => c.topics.has(t))) continue;
		if (!fx.liveVisible(c.who, ev)) continue;
		sendEv(c, ev);
	}
	return ev;
}

async function openStream(req: IncomingMessage, res: ServerResponse, url: URL, who: Persona): Promise<void> {
	const fx = await vite.ssrLoadModule('/standin/fixtures.ts');
	const c: LiveClient = { res, who, topics: new Set((url.searchParams.get('topics') || '').split(',').filter(Boolean)) };
	if (who === 'public') {
		if (!publicTopicsOk(fx, c.topics)) { send(res, 401, { error: 'unauthenticated' }); return; }
		if (liveOff || publicFull) {
			res.writeHead(503, { 'content-type': 'application/json', 'cache-control': 'no-store', 'retry-after': String(publicFull || 5) });
			res.end(JSON.stringify({ error: 'unavailable', message: 'Too many live streams are open.' }));
			return;
		}
		// No seqs: every open starts with a resync, Last-Event-ID or not,
		// naming the least seconds between two refetches of a window.
		res.writeHead(200, { 'content-type': 'text/event-stream', 'cache-control': 'no-store', 'x-accel-buffering': 'no' });
		res.write('retry: 5000\n\n');
		sendEv(c, { seq: 0, kind: 'resync', refetch_interval: PUBLIC_REFETCH_SECS });
		liveClients.add(c);
		const beat = setInterval(() => res.write(': heartbeat\n\n'), 15000);
		req.on('close', () => { clearInterval(beat); liveClients.delete(c); });
		return;
	}
	if (liveOff) { send(res, 503, { error: 'live_off', message: 'Live updates are off.' }); return; }
	res.writeHead(200, { 'content-type': 'text/event-stream', 'cache-control': 'no-store', 'x-accel-buffering': 'no' });
	res.write('retry: 2000\n\n');
	const last = Number(req.headers['last-event-id'] || url.searchParams.get('last') || 0);
	if (last) {
		if (liveRing.length && last < liveRing[0]!.seq - 1) sendEv(c, { seq: liveSeq, kind: 'resync' });
		else for (const e of liveRing) if (e.seq > last && (evTopics(e) ?? []).some((t) => c.topics.has(t)) && fx.liveVisible(who, e)) sendEv(c, e);
	}
	liveClients.add(c);
	const beat = setInterval(() => res.write(': heartbeat\n\n'), 15000);
	req.on('close', () => { clearInterval(beat); liveClients.delete(c); });
}

const TICK = Number(process.env.TGD_STANDIN_TICK) || 0;
if (TICK) {
	setInterval(async () => {
		const chats = [...new Set([...liveClients].flatMap((c) => [...c.topics]).filter((t) => t.startsWith('chat:')))];
		if (!chats.length) return;
		const fx = await vite.ssrLoadModule('/standin/fixtures.ts');
		const e = fx.liveChange('message.new', { chat_id: Number(chats[Math.floor(Math.random() * chats.length)]!.slice(5)) });
		if (e) await publish(e);
	}, TICK);
}

// Session CSRF tokens. Pages carry standin-csrf; a password change
// issues a new one, as the Go service does when it reissues the session.
const csrfTokens = new Set(['standin-csrf']);
const csrfOk = (req: IncomingMessage) => csrfTokens.has(String(req.headers['x-csrf-token'] || ''));

// An HSL colour (0..1 each) as Lottie's RGB (0..1 each).
function hsl(h: number, s: number, l: number): number[] {
	const f = (n: number) => { const k = (n + h * 12) % 12; return l - s * Math.min(l, 1 - l) * Math.max(-1, Math.min(k - 3, 9 - k, 1)); };
	return [f(0), f(8), f(4)];
}

async function handle(req: IncomingMessage, res: ServerResponse): Promise<void> {
	const url = new URL(req.url || '/', 'http://x');
	// A file's bytes (/files/<token>): a picture in a colour of its id,
	// whatever the file's type, so the media in the pages have something
	// to show. Videos and audio do not play.
	// A photo no longer stored, which Telegram could not give back.
	if (/^\/files\/standin-gone-\d+$/.test(url.pathname)) {
		send(res, 502, { error: 'fetch_failed', message: 'The file could not be fetched from Telegram.' });
		return;
	}
	// A previewed document (docpreview.ts): its bytes with Go's headers,
	// a byte range when asked (the text view reads the first 512 KiB).
	const dm = url.pathname.match(/^\/files\/standin-(\d+)\.(?!tgs$)([a-z0-9]+)$/);
	if (dm) {
		const body = Buffer.from(docBytes(Number(dm[1]), dm[2]!));
		const headers = docHeaders(Number(dm[1]), dm[2]!, url.searchParams.get('download') === '1');
		const range = /^bytes=(\d+)-(\d*)$/.exec(String(req.headers.range ?? ''));
		if (range && Number(range[1]) < body.length) {
			const from = Number(range[1]);
			const to = Math.min(range[2] ? Number(range[2]) : body.length - 1, body.length - 1);
			res.writeHead(206, { ...headers, 'content-range': `bytes ${from}-${to}/${body.length}`, 'accept-ranges': 'bytes' });
			res.end(body.subarray(from, to + 1));
			return;
		}
		res.writeHead(200, { ...headers, 'accept-ranges': 'bytes' });
		res.end(body);
		return;
	}
	const fm = url.pathname.match(/^\/files\/standin-(\d+)(\.tgs)?$/);
	if (fm?.[2]) {
		// A Lottie sticker: a circle that pulses, gzipped as .tgs files are.
		const fill = hsl(((Number(fm[1]) * 47) % 360) / 360, 0.6, 0.5);
		const anim = { v: '5.5.2', fr: 30, ip: 0, op: 30, w: 512, h: 512, layers: [{ ty: 4, ind: 1, ip: 0, op: 30, st: 0, ks: { o: { a: 0, k: 100 }, r: { a: 0, k: 0 }, p: { a: 0, k: [256, 256, 0] }, a: { a: 0, k: [0, 0, 0] }, s: { a: 1, k: [{ t: 0, s: [80, 80, 100], i: { x: [0.5], y: [1] }, o: { x: [0.5], y: [0] } }, { t: 15, s: [100, 100, 100], i: { x: [0.5], y: [1] }, o: { x: [0.5], y: [0] } }, { t: 30, s: [80, 80, 100] }] } }, shapes: [{ ty: 'el', p: { a: 0, k: [0, 0] }, s: { a: 0, k: [360, 360] } }, { ty: 'fl', c: { a: 0, k: [...fill, 1] }, o: { a: 0, k: 100 } }] }] };
		res.writeHead(200, { 'content-type': 'application/x-tgsticker', 'cache-control': 'no-store' });
		res.end(gzipSync(JSON.stringify(anim)));
		return;
	}
	if (fm) {
		const id = Number(fm[1]);
		const hue = (id * 47) % 360;
		// Landscape, portrait or wide by the id, so a page that crops a
		// picture to one shape shows it.
		const [w, h] = [[400, 300], [300, 400], [480, 240]][id % 3]!;
		res.writeHead(200, { 'content-type': 'image/svg+xml', 'cache-control': 'no-store', ...(url.searchParams.get('download') === '1' ? { 'content-disposition': `attachment; filename="file_${id}"` } : {}) });
		res.end(`<svg xmlns="http://www.w3.org/2000/svg" width="${w}" height="${h}" viewBox="0 0 400 300" preserveAspectRatio="none"><rect width="400" height="300" fill="hsl(${hue} 55% 60%)"/><circle cx="300" cy="80" r="40" fill="hsl(${hue} 70% 85%)"/><path d="M0 300 L140 150 L240 250 L300 200 L400 300 Z" fill="hsl(${hue} 45% 35%)"/></svg>`);
		return;
	}
	if (url.pathname.startsWith('/app/') || url.pathname.startsWith('/@') || url.pathname.startsWith('/node_modules/')) {
		vite.middlewares(req, res);
		return;
	}
	if (url.pathname === '/favicon.ico') {
		res.writeHead(204);
		res.end();
		return;
	}
	const who = persona(req, url);
	const cookie = `tgd_as=${who}; Path=/; SameSite=Lax`;
	const fx = await vite.ssrLoadModule('/standin/fixtures.ts');
	// Sign-in, as webapi/README "Authentication" describes it, with
	// fixed tokens. Usernames admin, gw_admin and researcher sign in as
	// the sample superadmin, admin and member; the password is "standin".
	if (url.pathname === '/api/login' && req.method === 'GET') {
		send(res, 200, who === 'public' ? { signed_in: false, csrf: 'standin-login' } : { signed_in: true }, ['tgw_csrf=standin-login; Path=/api/login; HttpOnly; SameSite=Lax']);
		return;
	}
	// Email links and the forgotten-password form. The stand-in's link
	// token is "standin-token"; any other answers bad_link.
	if (url.pathname === '/api/password/forgot' && req.method === 'POST') {
		if (req.headers['x-csrf-token'] !== 'standin-login') { send(res, 403, { error: 'csrf' }); return; }
		const body = JSON.parse((await readBody(req)) || '{}') as { username?: string };
		if (!body.username) { send(res, 400, { error: 'bad_request', message: 'Enter a username or email address.' }); return; }
		res.writeHead(202); res.end(); return;
	}
	if ((url.pathname === '/api/password/set' || url.pathname === '/api/account/email/verify') && req.method === 'POST') {
		const body = JSON.parse((await readBody(req)) || '{}') as { token?: string; new_password?: string };
		if (body.token !== 'standin-token') { send(res, 400, { error: 'bad_link', message: 'The link is unknown, used or expired.' }); return; }
		if (url.pathname === '/api/password/set' && (body.new_password ?? '').length < 8) { send(res, 400, { error: 'weak_password', message: 'The password is too short.' }); return; }
		if (url.pathname === '/api/account/email/verify') fx.verifyPendingEmails();
		res.writeHead(204); res.end(); return;
	}
	if (url.pathname === '/api/login' && req.method === 'POST') {
		if (req.headers['x-csrf-token'] !== 'standin-login') { send(res, 403, { error: 'csrf' }); return; }
		const body = JSON.parse((await readBody(req)) || '{}') as { username?: string; password?: string };
		const as = ({ admin: 'superadmin', gw_admin: 'admin', researcher: 'member', ops: 'noemail', mod_kernel: 'newpw' } as Record<string, Persona>)[body.username ?? ''];
		if (!as || body.password !== 'standin') {
			send(res, 401, { error: 'invalid_credentials', message: 'Wrong username or password.', csrf: 'standin-login' });
			return;
		}
		send(res, 200, { user: { id: 1, username: body.username, role: as, must_change_password: fx.needsPasswordChange(as) }, csrf: 'standin-csrf', landing: fx.landingOf(as) }, [`tgd_as=${as}; Path=/; SameSite=Lax`]);
		return;
	}
	if (url.pathname === '/api/logout' && req.method === 'POST') {
		if (!csrfOk(req)) { send(res, 403, { error: 'csrf' }); return; }
		res.writeHead(204, { 'set-cookie': 'tgd_as=public; Path=/; SameSite=Lax' });
		res.end();
		return;
	}
	// A temporary password: only these calls work until the account chose
	// its own (the spec's Gates); the page data answers password_change.
	if (url.pathname.startsWith('/api/') && fx.needsPasswordChange(who) && !/^\/api\/(session|logout|login|page(\/.*)?|account\/password|account\/email\/verify|password\/(forgot|set)|_standin\/.*)$/.test(url.pathname)) {
		send(res, 403, { error: 'password_change_required', message: 'Choose your own password first.' }); return;
	}
	if (url.pathname === '/api/session') {
		if (who === 'public') send(res, 401, { error: 'unauthenticated' });
		else send(res, 200, { user: { ...fx.viewerOf(who).account, must_change_password: fx.needsPasswordChange(who) }, csrf: 'standin-csrf', landing: fx.landingOf(who) });
		return;
	}
	// The account endpoints proposed for the Go service (settings pages).
	if (url.pathname.startsWith('/api/account/')) {
		if (who === 'public') { send(res, 401, { error: 'unauthenticated' }); return; }
		if (!csrfOk(req)) { send(res, 403, { error: 'csrf' }); return; }
		// Fixture state is untyped JavaScript-shaped data.
		const st: any = fx.settingsState(who);
		const body = req.method === 'GET' || req.method === 'DELETE' ? {} : JSON.parse((await readBody(req)) || '{}');
		const p = url.pathname.slice('/api/account/'.length);
		let am: RegExpMatchArray | null;
		if (p === 'password' && req.method === 'POST') {
			if (body.current_password !== 'standin') { send(res, 400, { error: 'wrong_password', message: 'The current password is wrong.' }); return; }
			st.sessions = st.sessions.filter((x: { current: boolean }) => x.current);
			const csrf = `standin-csrf-${csrfTokens.size}`;
			csrfTokens.add(csrf);
			fx.passwordChanged(who);
			send(res, 200, { csrf }); return;
		}
		if (p === 'email' && req.method === 'POST') {
			if (body.password !== 'standin') { send(res, 400, { error: 'wrong_password', message: 'The current password is wrong.' }); return; }
			// The stand-in has mail set up: a new address waits for its link;
			// an empty one removes the address at once.
			const sent = fx.changeEmail(who, String(body.email ?? ''));
			send(res, 200, { verification_sent: sent }); return;
		}
		if (p === 'preferences' && req.method === 'PATCH') {
			// Only the keys named change; null resets one to the site default.
			const bad = fx.badPref(body);
			if (bad) { send(res, 400, { error: 'bad_request', message: `${bad} does not take that value.`, field: bad }); return; }
			const range = fx.badRange(st.prefs, body);
			if (range) { send(res, 400, { error: 'bad_request', message: 'The dashboard chart\'s range does not fit its period.', field: range }); return; }
			for (const [k, v] of Object.entries(body)) { if (v === null) delete st.prefs[k]; else st.prefs[k] = v; }
			send(res, 200, { prefs: st.prefs }); return;
		}
		if (p === 'preferences' && req.method === 'PUT') {
			st.prefs = Object.fromEntries(Object.entries(body).filter(([, v]) => v !== null));
			send(res, 200, { prefs: st.prefs }); return;
		}
		if ((am = p.match(/^pinned\/(-?\d+)$/)) && req.method === 'DELETE') { st.pinned = st.pinned.filter((g: { id: number }) => String(g.id) !== am![1]); res.writeHead(204); res.end(); return; }
		if (/^pinned\/-?\d+$/.test(p) && req.method === 'PUT' && st.pinned.length >= 50 && !st.pinned.some((x: { id: number }) => `pinned/${x.id}` === p)) { send(res, 400, { error: 'too_many', message: 'You have 50 pinned groups, the most there can be. Unpin one first.' }); return; }
		if ((am = p.match(/^pinned\/(-?\d+)$/)) && req.method === 'PUT') {
			const g = fx.groupFor(who, am[1]);
			if (!g) { send(res, 404, { error: 'not_found' }); return; }
			if (!st.pinned.some((x: { id: number }) => x.id === g.id)) st.pinned.push(g);
			res.writeHead(204); res.end(); return;
		}
		if (p === 'saved-searches' && req.method === 'POST' && st.saved.length >= 100) { send(res, 400, { error: 'too_many', message: 'You have 100 saved searches, the most there can be. Delete one first.' }); return; }
		if (p === 'saved-searches' && req.method === 'POST') {
			if (!body.title || typeof body.url !== 'string' || !body.url.startsWith('/')) { send(res, 400, { error: 'bad_request', message: 'A title and a search path are required.' }); return; }
			const id = Math.max(0, ...st.saved.map((x: { id: number }) => x.id)) + 1;
			st.saved.push({ id, title: String(body.title), url: body.url });
			send(res, 201, { id }); return;
		}
		if ((am = p.match(/^saved-searches\/(\d+)$/)) && req.method === 'DELETE') { st.saved = st.saved.filter((x: { id: number }) => String(x.id) !== am![1]); res.writeHead(204); res.end(); return; }
		if ((am = p.match(/^sessions\/(\w+)$/)) && req.method === 'DELETE') { st.sessions = st.sessions.filter((x: { id: string; current: boolean }) => x.current || x.id !== am![1]); res.writeHead(204); res.end(); return; }
		if (p === 'sessions/sign-out-others' && req.method === 'POST') {
			st.sessions = st.sessions.filter((x: { current: boolean }) => x.current);
			// This session's cookie is reissued, as the Go service does.
			const csrf = `standin-csrf-${csrfTokens.size}`;
			csrfTokens.add(csrf);
			send(res, 200, { csrf }); return;
		}
		send(res, 404, { error: 'not_found' }); return;
	}
	if (url.pathname === '/api/dashboard/kpis' && req.method === 'GET') {
		if (who === 'public') { send(res, 401, { error: 'unauthenticated' }); return; }
		send(res, 200, { kpis: fx.dashboardKpis(who) }); return;
	}
	if (url.pathname === '/api/dashboard/series' && req.method === 'GET') {
		if (who === 'public') { send(res, 401, { error: 'unauthenticated' }); return; }
		const [status, body] = fx.dashboardSeries(who, url.searchParams);
		send(res, status, body); return;
	}
	if (url.pathname === '/api/events' && req.method === 'GET') { await openStream(req, res, url, who); return; }
	if (url.pathname === '/api/_standin/emit' && req.method === 'POST') {
		const b = JSON.parse((await readBody(req)) || '{}');
		const e = fx.liveChange(String(b.kind), b);
		if (!e) { send(res, 400, { error: 'bad_request' }); return; }
		send(res, 200, await publish(e));
		return;
	}
	// Back to the fixtures as loaded: every module is evaluated afresh on
	// its next use, and the stand-in's own state is cleared. The browser
	// tests call it before each test.
	if (url.pathname === '/api/_standin/reset' && req.method === 'POST') {
		for (const env of Object.values(vite.environments)) env.moduleGraph.invalidateAll();
		for (const c of liveClients) c.res.end();
		liveClients.clear();
		liveRing.length = 0;
		liveOff = false;
		publicFull = 0;
		csrfTokens.clear();
		csrfTokens.add('standin-csrf');
		send(res, 200, { reset: true });
		return;
	}
	// The search engine of the lists (engine.ts): off, available
	// or behind. It lives in the fixtures' module, so a reset turns it off.
	if (url.pathname === '/api/_standin/engine' && req.method === 'POST') {
		const st = String(JSON.parse((await readBody(req)) || '{}').state ?? 'off');
		if (!['off', 'available', 'behind'].includes(st)) { send(res, 400, { error: 'bad_request' }); return; }
		fx.setEngine(st as 'off' | 'available' | 'behind');
		send(res, 200, { state: st });
		return;
	}
	// GET /api/search/capabilities, as Go answers it.
	if (url.pathname === '/api/search/capabilities' && req.method === 'GET') {
		send(res, 200, fx.searchCapabilities(who));
		return;
	}
	if (url.pathname === '/api/_standin/events' && req.method === 'POST') {
		const b = JSON.parse((await readBody(req)) || '{}');
		if ('public_full' in b) {
			publicFull = Math.max(0, Number(b.public_full) || 0);
			if (publicFull) for (const c of [...liveClients]) if (c.who === 'public') { c.res.end(); liveClients.delete(c); }
			send(res, 200, { public_full: publicFull });
			return;
		}
		liveOff = !!b.off;
		if (liveOff) for (const c of liveClients) c.res.end();
		send(res, 200, { off: liveOff });
		return;
	}
	if (/^\/api\/(accounts|federations|forwarding|join-chat|mcp|bots)(\/|$)/.test(url.pathname)) {
		if (who === 'public') { send(res, 401, { error: 'unauthenticated' }); return; }
		if (req.method !== 'GET' && !csrfOk(req)) { send(res, 403, { error: 'csrf' }); return; }
		const body = req.method === 'GET' ? Object.fromEntries(url.searchParams) : JSON.parse((await readBody(req)) || '{}');
		const [status, out] = fx.adminApi(req.method || 'GET', url.pathname.slice('/api/'.length), body, who);
		if (status === 204) { res.writeHead(204); res.end(); } else send(res, status, out);
		if (req.method !== 'GET' && url.pathname.startsWith('/api/mcp/')) await endWithdrawn();
		return;
	}
	const lk = url.pathname.match(/^\/api\/users\/(\d+)\/lookup-name$/);
	if (lk && req.method === 'POST') {
		if (who === 'public') { send(res, 401, { error: 'unauthenticated' }); return; }
		if (!csrfOk(req)) { send(res, 403, { error: 'csrf' }); return; }
		const [status, out, retry] = fx.lookupName(lk[1], who);
		// The daemon's cycle takes a while; long enough here to see it wait.
		await new Promise((r) => setTimeout(r, TEST ? 300 : 1500));
		res.writeHead(status, { 'content-type': 'application/json', 'cache-control': 'no-store', ...(retry ? { 'retry-after': String(retry) } : {}) });
		res.end(JSON.stringify(out));
		return;
	}
	// "Fetch from Telegram" (fetchFile): for accounts that may fetch; the
	// daemon takes a moment, long enough here to see the button wait.
	const ff = url.pathname.match(/^\/api\/files\/([^/]+)\/fetch$/);
	if (ff && req.method === 'POST') {
		if (who === 'public') { send(res, 401, { error: 'unauthenticated' }); return; }
		if (!csrfOk(req)) { send(res, 403, { error: 'csrf' }); return; }
		if (!fx.canFetch(who)) { send(res, 403, { error: 'fetch_not_allowed', message: 'This account may not fetch files from Telegram.' }); return; }
		const [status, out] = fx.fetchFileApi(decodeURIComponent(ff[1]!));
		await new Promise((r) => setTimeout(r, TEST ? 300 : 1200));
		send(res, status, out);
		return;
	}
	// Signed out is 401, even for an exposed group's photos.
	const ph = url.pathname.match(/^\/api\/(users|groups)\/(-?\d+)\/photos$/);
	if (ph && req.method === 'GET') {
		if (who === 'public') { send(res, 401, { error: 'unauthenticated' }); return; }
		const [status, out] = ph[1] === 'users' ? fx.userPhotos(ph[2], who) : fx.groupPhotos(ph[2], who);
		send(res, status, out);
		return;
	}
	// A group's member list and the switch that hides a channel's
	// (members.ts). Signed out is 401, members get 404.
	const mm = url.pathname.match(/^\/api\/groups\/(-?\d+)(\/members)?$/);
	if (mm && (mm[2] ? req.method === 'GET' : req.method === 'PATCH')) {
		if (who === 'public') { send(res, 401, { error: 'unauthenticated' }); return; }
		const role = fx.viewerOf(who).account.role;
		const sees = (g: { id: number }) => !!fx.groupFor(who, String(g.id));
		if (mm[2]) { const [st, out] = fx.membersApi(mm[1], url.searchParams, role, sees); send(res, st, out); return; }
		if (!csrfOk(req)) { send(res, 403, { error: 'csrf' }); return; }
		const [st, out] = fx.patchGroup(mm[1], JSON.parse((await readBody(req)) || '{}'), role, sees);
		send(res, st, out);
		return;
	}
	const hm = url.pathname.match(/^\/api\/(users|groups)\/(-?\d+)\/history$/);
	if (hm && req.method === 'GET') {
		if (who === 'public') { send(res, 401, { error: 'unauthenticated' }); return; }
		const [status, out] = fx.historyApi(hm[1] as 'users' | 'groups', hm[2], url.searchParams, who);
		send(res, status, out);
		return;
	}
	// A group's files and polls, a page at a time (groupfiles.ts).
	const gt = url.pathname.match(/^\/api\/groups\/(-?\d+)\/(files|polls)$/);
	if (gt && req.method === 'GET') {
		const [status, out] = fx.groupTabApi(gt[2] as 'files' | 'polls', gt[1], url.searchParams, who);
		send(res, status, out);
		return;
	}
	if (url.pathname === '/api/entities' && req.method === 'GET') {
		const [status, out] = fx.entities(who, url.searchParams);
		send(res, status, out);
		return;
	}
	if (url.pathname === '/api/chats/search' && req.method === 'GET') {
		const [status, out] = fx.searchChats(who, url.searchParams);
		send(res, status, out);
		return;
	}
	let cm;
	if ((cm = url.pathname.match(/^\/api\/chats\/(group|private)\/(-?\d+)\/messages$/))) {
		const chat = fx.chatFor(cm[1], cm[2], who);
		if (!chat) { res.writeHead(404, { 'content-type': 'application/json' }); res.end('{"error":"not_found"}'); return; }
		const num = (k: string) => (url.searchParams.get(k) ? Number(url.searchParams.get(k)) : undefined);
		// A forum topic: groups only, as in Go.
		const topic = num('topic');
		if (topic !== undefined && cm[1] === 'private') { send(res, 400, { error: 'bad_request', message: 'A private chat has no topics.', field: 'topic' }); return; }
		const w = fx.chatWindow(chat, { before: num('before'), after: num('after'), at: num('around'), after_ts: num('after_ts'), topic }, Math.min(100, Math.max(1, num('limit') || 60)), fx.viewOf(who, cm[1] === 'group' ? chat.id : undefined));
		res.writeHead(200, { 'content-type': 'application/json', 'cache-control': 'no-store' });
		res.end(JSON.stringify(w));
		return;
	}
	if (url.pathname.startsWith('/api/page/') || url.pathname === '/api/page') {
		// The path's slashes may come encoded as %2F, as the client sends them.
		const target = cleanUrl(new URL(url.pathname.slice('/api/page'.length).replace(/%2F/gi, '/') + url.search || '/', 'http://x'));
		const page = fx.pageData(target, who);
		res.writeHead(page.status, { 'content-type': 'application/json', 'set-cookie': cookie, 'cache-control': 'no-store' });
		res.end(JSON.stringify(page));
		return;
	}
	const target = cleanUrl(url);
	const page = fx.pageData(target, who);
	// Signed out: every page but the public chats sends to the sign-in page.
	if (who === 'public' && page.route === 'not_found' && url.pathname !== '/login') {
		res.writeHead(302, { location: `/login?next=${encodeURIComponent(target)}`, 'set-cookie': cookie });
		res.end();
		return;
	}
	let head: string | null = null;
	let html: string | null = null;
	if (url.searchParams.get('ssr') !== '0') {
		try {
			const entry = await vite.ssrLoadModule('/src/entry-server.tsx');
			({ head, html } = entry.render(page));
		} catch (e) {
			vite.ssrFixStacktrace(e as Error);
			console.error(e);
		}
	}
	// Vite's dev transform puts its base (/app/) before absolute URLs in
	// the document, which would change the media URLs in the rendered
	// app and break hydration. Transform the document around a marker and
	// put the app in afterwards.
	const MARK = '<!--standin-app-->';
	let doc = documentHtml({ page, head, html: html === null ? null : MARK, csrf: 'standin-csrf', assets: { css: [], preload: [], entry: '/src/entry-client.tsx' } });
	doc = await vite.transformIndexHtml(target, doc);
	if (html !== null) doc = doc.replace(MARK, () => html);
	res.writeHead(page.status, { 'content-type': 'text/html; charset=utf-8', 'set-cookie': cookie, 'cache-control': 'no-store' });
	res.end(doc);
}

server.listen(PORT, TEST ? '127.0.0.1' : '0.0.0.0', () => {
	const a = server.address();
	console.log(`stand-in on :${typeof a === 'object' && a ? a.port : PORT}`);
});
