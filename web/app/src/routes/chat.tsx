// The Telegram-like chat view (group or private chat), inside the shell,
// or on its own as the public page of a group exposed through MCP.
import { useCallback, useEffect, useLayoutEffect, useRef, useState, type ReactElement, type ReactNode } from 'react';
import { track } from '../ui/busy.ts';
import { ArrowLeft, CalendarDays, ChevronDown, ChevronsUp, CloudOff, Download, Forward, Globe, Info, Pencil, Play, Radio, Search, Trash2, X } from 'lucide-react';
import Shell from '../ui/Shell.tsx';
import { PublicFrame } from '../ui/PublicFrame.tsx';
import { Link, setQuery, useNav } from '../nav.tsx';
import { Avatar } from '../ui/primitives.tsx';
import { MessageMedia } from './chat-media.tsx';
import { FetchButton, needsFetch, useFetched } from '../ui/fetchfile.tsx';
import { LinkPreview } from './chat-preview.tsx';
import { Counts, EffectMark, KindDetail, KindFallback, Reactions, TopicStrip, TopicTag, richPage, textInside } from './chat-kinds.tsx';
import { fetchChatWindow } from '../api/chat.ts';
import { RichText, type RichChat } from '../ui/richtext.tsx';
import { useLive, useLiveState, useRefetcher } from '../live/react.tsx';
import { useFreshGroup, useFreshUser } from '../live/useEntities.tsx';
import { bytes, fmt } from '../format.ts';
import { TYPE_LABEL } from './groups.tsx';
import { contentLabel, serviceLabel } from '../ui/labels.ts';
import { isSentence } from '../ui/serviceline.ts';
import { ServiceText } from '../ui/serviceparts.tsx';
import { ServiceDetailView, topicInText } from '../ui/servicedetail.tsx';
import { loggerName } from '../ui/logger.ts';
import { EditDetail, useEditList } from '../ui/editdetail.tsx';
import { deletedTag, deletedTitle } from '../ui/deletion.ts';
import type { PageData, Viewer } from '../page.ts';
import type { ChatMedia, ChatMessage, GroupSummary } from '../types.ts';
import type { RouteDef } from './index.tsx';
import type { S } from '../api/schema.ts';

export type ChatProps = S['ChatProps'];

// Telegram's palette for sender names, picked per user id.
const NAME_COLORS = ['#e17076', '#eda86c', '#a695e7', '#7bc862', '#6ec9cb', '#65aadd', '#ee7aae'];
const nameColor = (id: number) => NAME_COLORS[Math.abs(id) % NAME_COLORS.length];

function dayKey(iso: string, v: Viewer): string {
	return new Intl.DateTimeFormat('en-CA', { timeZone: v.timezone || 'Asia/Jakarta' }).format(new Date(iso));
}
function dayLabel(iso: string, v: Viewer): string {
	return new Intl.DateTimeFormat('en-US', { weekday: 'short', month: 'long', day: 'numeric', year: 'numeric', timeZone: v.timezone || 'Asia/Jakarta' }).format(new Date(iso));
}
function hm(iso: string, v: Viewer): string {
	return new Intl.DateTimeFormat('en-GB', { hour: '2-digit', minute: '2-digit', hour12: v.date_format === '12h', timeZone: v.timezone || 'Asia/Jakarta' }).format(new Date(iso));
}

// The viewer's chat preferences (Settings, Preferences); each one's site
// default is on, except media, which loads automatically.
const prefs = (v: Viewer) => ({
	autoLoad: v.chat?.auto_load !== false,
	mediaOnClick: v.chat?.media === 'click',
	showDeleted: v.chat?.show_deleted !== false,
	showEdited: v.chat?.show_edited !== false,
	showService: v.chat?.show_service !== false,
});

// A chat's page, opened at a message when given: a negative id is a
// group, a positive one a private chat.
const chatPath = (id: number, at?: number) => `${id < 0 ? '/groups' : '/users'}/${id}/chat${at !== undefined ? `?at=${at}` : ''}`;

// A service message's action as words, when it came without its text.
const serviceText = (t?: string) => (t ? serviceLabel(t) : '');

// At most this many pages of 100 refetched for a window's resync; a
// window longer than that is refreshed from its start only.
const RESYNC_PAGES = 10;

// Where a forward came from: a link to the user, or to the channel's
// post (or the channel), else just the name.
function ForwardLine({ m }: { m: ChatMessage }) {
	const f = m.forward_from;
	const name = f?.name ?? m.forward ?? '';
	const to = f?.user_id !== undefined ? `/users/${f.user_id}` : f?.chat_id !== undefined ? (f.message_id !== undefined ? chatPath(f.chat_id, f.message_id) : `/groups/${f.chat_id}`) : null;
	return (
		<div className="c-fwd" title={f?.date ? `Sent ${f.date}` : undefined}>
			<Forward size={13} />Forwarded from {to ? <Link to={to} className="c-fwdlink"><b>{name}</b></Link> : <b>{name}</b>}
		</div>
	);
}

// An earlier version's media in the edit list: its picture, small, or
// its type and name.
// An earlier version's file: its picture, else its name; a big one not
// stored has "Fetch from Telegram" beside it.
function EditMedia({ m: given, type }: { m: NonNullable<ChatMessage['edits'][number]['media']>; type: ChatMessage['content_type'] }) {
	const m = useFetched(given);
	const fetch = needsFetch(m) ? <FetchButton path={m.fetch!} small /> : null;
	if (m.thumb) return <><a className="c-editmedia" href={m.url ?? undefined} target="_blank" rel="noopener" aria-label={`Earlier ${contentLabel(type).toLowerCase()}`}><img src={m.thumb} alt="" loading="lazy" /></a>{fetch}</>;
	return <div className="c-editfile c-sub">{contentLabel(type)}{m.name ? `: ${m.name}` : ''}{m.url ? '' : fetch ? ' (on Telegram)' : ' (not stored)'}{fetch}</div>;
}

function Bubble({ m, v, isGroup, first, last, flash, onReply, tag, chat }: { m: ChatMessage; v: Viewer; isGroup: boolean; first: boolean; last: boolean; flash: boolean; onReply: (id: number) => void; tag?: ReactNode; chat?: RichChat }) {
	const out = !m.sender;
	const ct = m.content_type;
	const sticker = ct === 'sticker';
	const mediaOnly = (ct === 'photo' || ct === 'video' || ct === 'animation' || ct === 'video_note') && !m.text && !m.reply_to && !m.forward;
	const [edits, setEdits] = useState(false);
	const editList = useEditList(m.edits, 'c-editmore');
	const s = useFreshUser(m.sender);
	const pref = prefs(v);
	// A message posted as a chat (a channel, an anonymous admin) links to
	// that chat, not to a user page.
	const asChat = m.sender_chat;
	// A chat the viewer may not see comes as a sender without an id
	// ("(hidden chat)"): named, but linked nowhere.
	const senderTo = asChat ? `/groups/${asChat.id}` : s && s.id !== 0 ? `/users/${s.id}` : '';
	const r = m.reply_to;
	const replyTo = r?.chat_id !== undefined ? chatPath(r.chat_id, r.message_id) : r ? `?at=${r.message_id}` : '';
	return (
		<div id={`m${m.message_id}`} className={`c-row ${out ? 'out' : 'in'} ${last ? 'last' : ''} ${flash ? 'flash' : ''}`}>
			{isGroup && !out && <div className="c-avslot">{last && s && (senderTo
					? <Link to={senderTo} aria-label={asChat ? asChat.title : s.name}>{asChat ? <Avatar id={asChat.id} name={asChat.title} photo={asChat.photo} size={34} square /> : <Avatar id={s.id} name={s.name} photo={s.photo} size={34} />}</Link>
					: <Avatar id={0} name={s.name} photo={null} size={34} square />)}</div>}
			<div className={`c-bubble ${sticker ? 'sticker' : ''} ${mediaOnly ? 'mediaonly' : ''} ${m.deleted_at ? 'deleted' : ''} ${last ? 'tail' : ''}`}>
				{isGroup && s && first && !sticker && (senderTo
					? <Link to={senderTo} className="c-sender" style={{ color: nameColor(s.id) }}>{asChat ? asChat.title : s.name}</Link>
					: <span className="c-sender c-sender-hidden">{s.name}</span>)}
				{tag}
				{(m.forward_from || m.forward) && <ForwardLine m={m} />}
				{r && (r.chat_id !== undefined
					// A reply to another chat opens that chat at the message.
					? <Link to={replyTo} className="c-reply c-reply-other" style={{ ['--rc' as string]: nameColor(r.sender_id) }}>
						<b>{r.resolved === false ? `message #${r.message_id}` : r.sender_name}<span className="c-sub"> . in another chat</span></b><span>{r.snippet}{r.deleted ? ' (deleted)' : ''}</span>
					</Link>
					: <a href={replyTo} className="c-reply" style={{ ['--rc' as string]: nameColor(r.sender_id) }}
						onClick={(e) => { e.preventDefault(); onReply(r.message_id); }}>
						<b>{r.resolved === false ? `message #${r.message_id}` : r.sender_name}</b>
						{r.resolved === false ? <span className="c-sub">not in the archive</span> : <span className={r.deleted ? 'c-replydel' : ''}>{r.snippet}{r.deleted ? ' (deleted)' : ''}</span>}
					</a>)}
				<MessageMedia msg={m} onClick={pref.mediaOnClick} />
				{m.detail ? <KindDetail d={m.detail} text={m.text} entities={m.entities} chat={chat} v={v} tdlib={m.tdlib_type} page={`/messages/${m.scope}/${m.id}`} /> : <KindFallback ct={m.content_type} tdlib={m.tdlib_type} />}
				{m.text && !textInside(m.detail) && <div className="c-text"><RichText text={m.text} entities={m.entities} chat={chat} /></div>}
				{m.link_preview && <LinkPreview lp={m.link_preview} v={v} onClick={pref.mediaOnClick} />}
				{m.author_signature && <div className="c-sign">- {m.author_signature}</div>}
				<Reactions list={m.reactions} />
				<div className="c-meta">
					<EffectMark id={m.effect_id} effect={m.effect} />
					<Counts k={m} />
					{m.deleted_at && <span className="c-deltag" title={deletedTitle(m, m.deleted_at)}><Trash2 size={11} />{deletedTag(m)}</span>}
					{pref.showEdited && m.edits.length > 0 && <button type="button" className="c-edited" onClick={() => setEdits((x) => !x)} aria-expanded={edits}><Pencil size={10} />edited</button>}
					{/* Edited, but no earlier version was recorded. */}
					{pref.showEdited && m.edits.length === 0 && m.edited_at && <span className="c-edited" title={`Edited ${m.edited_at}; no earlier version recorded`}><Pencil size={10} />edited</span>}
					<Link to={`/messages/${m.scope}/${m.id}`} className="c-time" title={m.date}>{hm(m.date, v)}</Link>
				</div>
				{edits && (
					<div className="c-editswrap"><div className="c-edits">
						{editList.more}
						{editList.shown.map((e, i) => (
							<div key={`${i}:${e.at}`}>
								<span className="c-sub">{hm(e.at, v)}{e.content_type && e.content_type !== m.content_type ? ` . was ${contentLabel(e.content_type)}` : ''}</span>
								{e.media && <EditMedia m={e.media} type={e.content_type ?? m.content_type} />}
								{e.text && !richPage(e.detail) ? <div><RichText text={e.text} entities={e.entities} chat={chat} /></div> : !e.media && !e.detail && <div className="c-sub">(no text)</div>}
								{e.detail && <EditDetail d={e.detail} text={e.text} entities={e.entities} chat={chat} v={v} />}
							</div>
						))}
						<div><span className="c-sub">{m.edited_at ? hm(m.edited_at, v) : ''} (current)</span><div><RichText text={m.text} entities={m.entities} chat={chat} /></div></div>
					</div></div>
				)}
			</div>
		</div>
	);
}

function Feed({ msgs: all, v, isGroup, flash, onReply, tag, chat, topicHref }: { msgs: ChatMessage[]; v: Viewer; isGroup: boolean; flash: number | null; onReply: (id: number) => void; tag?: (m: ChatMessage) => ReactNode; chat?: RichChat; topicHref: (id: number) => string }) {
	const pref = prefs(v);
	const msgs = all.filter((m) => (pref.showDeleted || !m.deleted_at) && (pref.showService || m.content_type !== 'service'));
	const out: ReactElement[] = [];
	let day = '';
	msgs.forEach((m, i) => {
		const d = dayKey(m.date, v);
		if (d !== day) {
			day = d;
			out.push(<div key={'d' + d} className="c-day"><span>{dayLabel(m.date, v)}</span></div>);
		}
		if (m.content_type === 'service') {
			// The actor links to its user, or to its chat when it acted as
			// one; the text falls back to the action's type.
			// The text comes rebuilt for the viewer (whom it may name); a pin,
			// a changed poll or a giveaway links to the message it is about.
			// No sender is the logging account itself, as on the message
			// page ("You" when signed in); a text that is a whole sentence has no actor.
			const svcType = m.service?.type ?? m.service_type;
			const sentence = isSentence(svcType, m.text);
			const svcAbout = m.service?.refs.message_id ?? null;
			// The names the text gives link to their pages; the rest of it
			// to the message the action is about.
			const about = (s: string) => (svcAbout ? <a href={`?at=${svcAbout}`} onClick={(e) => { e.preventDefault(); onReply(svcAbout); }}>{s}</a> : s);
			// The stored detail adds to the line; a topic the text quotes
			// becomes its link.
			const svcText = m.text || serviceText(svcType);
			const plain = (s: string) => topicInText(s, m.service?.detail, topicHref) ?? about(s);
			const actor = sentence ? null : m.sender_chat ? <Link to={`/groups/${m.sender_chat.id}`}>{m.sender_chat.title}</Link> : m.sender ? (m.sender.id !== 0 ? <Link to={`/users/${m.sender.id}`}>{m.sender.name}</Link> : <span>{m.sender.name}</span>) : <span>{loggerName(v)}</span>;
			out.push(
				<div key={m.id} id={`m${m.message_id}`} className={`c-service ${m.deleted_at ? 'deleted' : ''}`}>
					<span>{actor} <ServiceText service={m.service} text={svcText} capital={sentence} plain={plain} /><ServiceDetailView service={m.service} text={svcText} v={v} topicHref={topicHref} self={!sentence && !m.sender_chat && !m.sender && v.signed_in} /> <Link to={`/messages/${m.scope}/${m.id}`} className="c-svctime" title={m.date}>{hm(m.date, v)}</Link>{m.deleted_at && <span className="c-deltag" title={deletedTitle(m, m.deleted_at)}><Trash2 size={11} />{deletedTag(m)}</span>}</span>
				</div>,
			);
			return;
		}
		const same = (a?: ChatMessage) => !!a && a.content_type !== 'service' && dayKey(a.date, v) === d && (a.sender?.id ?? 0) === (m.sender?.id ?? 0);
		out.push(<Bubble key={m.id} m={m} v={v} isGroup={isGroup} first={!same(msgs[i - 1])} last={!same(msgs[i + 1])} flash={flash === m.message_id} onReply={onReply} tag={tag?.(m)} chat={chat} />);
	});
	return <>{out}</>;
}

function Chat({ page }: { page: PageData<ChatProps> }) {
	const p = page.props;
	const v = page.viewer;
	const { navigate } = useNav();
	const publicView = !v.signed_in;
	const isGroup = p.chat.kind === 'group';
	const chatId = p.chat.kind === 'group' ? p.chat.group.id : p.chat.user.id;
	const group = useFreshGroup(p.chat.kind === 'group' ? p.chat.group : null);
	const scope = isGroup ? 'group' : 'private';
	// A forum's one topic, when the page asks for it (?topic=).
	const topic = p.topic;
	// While a forum shows every topic, each message says its own.
	const tagOf = (m: ChatMessage) => {
		const t = topic === null && m.topic_id !== null ? p.topics.find((x) => x.id === m.topic_id) : undefined;
		return t ? <TopicTag t={t} href={topicHref(t.id)} /> : null;
	};
	const topicHref = (id: number | null) => setQuery(page.url, { topic: id ?? undefined, at: undefined, date: undefined, after: undefined, after_ts: undefined });
	const [msgs, setMsgs] = useState(p.messages);
	const [older, setOlder] = useState(p.has_older);
	const [newer, setNewer] = useState(p.has_newer);
	const [live, setLive] = useState(v.chat?.live ?? true);
	const [atBottom, setAtBottom] = useState(true);
	const [searching, setSearching] = useState(false);
	const [flash, setFlash] = useState<number | null>(p.at);
	const [unseen, setUnseen] = useState(0);
	// A new page (another chat, or ?at=) replaces the window in the same
	// render, so the scroll to p.at below finds the new window's target:
	// set from an effect, the messages came a commit late, after the
	// scroll had already fallen back to the old window's bottom.
	const [shown, setShown] = useState(p);
	if (shown !== p) {
		setShown(p);
		setMsgs(p.messages); setOlder(p.has_older); setNewer(p.has_newer); setFlash(p.at); setUnseen(0);
	}
	const scroller = useRef<HTMLDivElement>(null);
	// The window's loads (older, newer, the live tail) belong to it: when
	// a new page replaces the window they are aborted and their answers
	// dropped, and the busy guard, held by the load running, is free for
	// the new window's own loads.
	const win = useRef<AbortController | null>(null);
	const busy = useRef<AbortSignal | null>(null);
	// The last message after which a page came back empty: a window too
	// short to scroll does not ask again by itself (a scroll may).
	const emptyAfter = useRef<number | null>(null);
	// Ids of messages filled into the window's stretch, to fetch (below).
	const gaps = useRef(new Set<number>());
	const msgsRef = useRef(msgs);
	msgsRef.current = msgs;
	// A scroll to make once the next messages are rendered: keep the view
	// where it was after older ones went in above it, or follow new ones
	// at the bottom, or keep the message the reader looks at in place
	// when one went in between. Done in a layout effect, not a frame
	// callback, which can run before React has rendered them.
	const afterRender = useRef<{ prepend: number } | { bottom: true } | { anchor: string; top: number } | null>(null);
	useLayoutEffect(() => {
		const el = scroller.current;
		const a = afterRender.current;
		afterRender.current = null;
		if (!el || !a) return;
		if ('prepend' in a) el.scrollTop += el.scrollHeight - a.prepend;
		else if ('anchor' in a) {
			const t = document.getElementById(a.anchor);
			if (t) el.scrollTop += t.getBoundingClientRect().top - a.top;
		} else el.scrollTop = el.scrollHeight;
	}, [msgs]);
	const liveState = useLiveState();
	const refetch = useRefetcher();

	// The Drogon chat's #msg-<message id> anchors (bookmarks, links from
	// elsewhere) never reach the server: open the chat at that message.
	useEffect(() => {
		const h = /^#msg-(\d+)$/.exec(location.hash);
		if (h) navigate(setQuery(page.url, { at: Number(h[1]), date: undefined, after: undefined, after_ts: undefined }), { replace: true, scroll: false });
	}, []);

	// A new page (another chat, or ?at=) replaces the window.
	useEffect(() => {
		const ac = new AbortController();
		win.current = ac;
		busy.current = null;
		emptyAfter.current = null;
		gaps.current.clear();
		return () => ac.abort();
	}, [p]);

	useLayoutEffect(() => {
		const el = scroller.current;
		if (!el) return;
		const t = p.at !== null ? document.getElementById('m' + p.at) : null;
		if (t) t.scrollIntoView({ block: 'center' });
		else el.scrollTop = el.scrollHeight;
	}, [p]);

	const loadOlder = useCallback(async () => {
		const el = scroller.current;
		const first = msgs[0];
		const sig = win.current?.signal;
		if (!el || !first || !older || busy.current || !sig) return;
		busy.current = sig;
		try {
			const w = await track(fetchChatWindow(scope, chatId, { before: first.message_id }, 60, sig, topic));
			if (sig.aborted) return;
			// Once the older messages are rendered, keep the reader where
			// they were (see the layout effect below).
			afterRender.current = { prepend: el.scrollHeight };
			setMsgs((cur) => [...w.messages, ...cur]);
			setOlder(w.has_older);
		} catch { /* aborted, or failed: the next scroll asks again */ } finally { if (busy.current === sig) busy.current = null; }
	}, [msgs, older, scope, chatId, topic]);

	// The messages after the window, when it does not end at the newest
	// (opened at the oldest, at a message, a date or a time, or left
	// behind by the live tail): fetched from the last message's id, the
	// cursor, never a row id, which later backfills leave out of order.
	// Appended below, they leave the reader where it was. Once the newest
	// is reached, the view follows the live tail (following, below).
	const loadNewer = useCallback(async () => {
		const last = msgs[msgs.length - 1];
		const sig = win.current?.signal;
		if (!last || !newer || busy.current || !sig) return;
		busy.current = sig;
		try {
			const w = await track(fetchChatWindow(scope, chatId, { after: last.message_id }, 60, sig, topic));
			if (sig.aborted) return;
			const have = new Set(msgsRef.current.map((c) => c.id));
			const add = w.messages.filter((x) => !have.has(x.id));
			if (add.length) {
				msgsRef.current = [...msgsRef.current, ...add];
				setMsgs(msgsRef.current);
			} else emptyAfter.current = last.message_id;
			setNewer(w.has_newer);
		} catch { /* aborted, or failed: the next scroll asks again */ } finally { if (busy.current === sig) busy.current = null; }
	}, [msgs, newer, scope, chatId, topic]);

	useEffect(() => {
		const el = scroller.current;
		if (!el) return;
		const on = () => {
			const fromBottom = el.scrollHeight - el.scrollTop - el.clientHeight;
			const bottom = fromBottom < 80;
			setAtBottom(bottom);
			if (bottom) setUnseen(0);
			if (!prefs(v).autoLoad) return;
			// Both ends may be near in a short window; the busy guard runs one
			// at a time, and the next scroll asks again.
			if (el.scrollTop < 300) void loadOlder();
			if (fromBottom < 300) void loadNewer();
		};
		el.addEventListener('scroll', on, { passive: true });
		return () => el.removeEventListener('scroll', on);
	}, [loadOlder, loadNewer]);

	// A window too short to scroll gives no scroll event: load on, until
	// a page adds nothing.
	useEffect(() => {
		const el = scroller.current;
		const last = msgs[msgs.length - 1];
		if (last && last.message_id === emptyAfter.current) return;
		if (el && newer && prefs(v).autoLoad && el.scrollHeight - el.clientHeight < 300) void loadNewer();
	}, [msgs, newer, loadNewer]);

	// The live tail (web-live-client.md section 4). New messages are
	// fetched from the cursor and appended: at the bottom the view
	// follows them, scrolled up nothing moves and the jump button counts
	// them. Edits, deletions and stored files replace the message in
	// place. The stream drives it; while it is down, poll every second.
	const fetchNewer = useCallback(async () => {
		const sig = win.current?.signal;
		if (!sig) return;
		for (let pages = 0; pages < 3; pages++) {
			const last = msgsRef.current[msgsRef.current.length - 1];
			if (!last) return;
			const w = await fetchChatWindow(scope, chatId, { after: last.message_id }, 60, sig, topic);
			if (sig.aborted) return;
			const el = scroller.current;
			const stick = el ? el.scrollHeight - el.scrollTop - el.clientHeight < 80 : false;
			const have = new Set(msgsRef.current.map((c) => c.id));
			const add = w.messages.filter((x) => !have.has(x.id));
			if (add.length) {
				msgsRef.current = [...msgsRef.current, ...add];
				if (stick && el) afterRender.current = { bottom: true };
				else setUnseen((n) => n + add.length);
				setMsgs(msgsRef.current);
			}
			if (!w.has_newer) return;
		}
		// Too far behind to follow: leave the window, as when scrolled back.
		setNewer(true);
	}, [scope, chatId, topic]);

	// Keep the message at the top of the view where it is once the next
	// messages are rendered (see the layout effect above).
	const keepPlace = () => {
		const el = scroller.current;
		if (!el) return;
		const top = el.getBoundingClientRect().top;
		const seen = [...el.querySelectorAll<HTMLElement>('.c-feed > [id^="m"]')].find((x) => x.getBoundingClientRect().bottom > top);
		if (seen) afterRender.current = { anchor: seen.id, top: seen.getBoundingClientRect().top };
	};

	const replaceMessage = useCallback(async (messageId: number) => {
		const w = await fetchChatWindow(scope, chatId, { around: messageId }, 3, undefined, topic);
		const got = new Map(w.messages.map((m) => [m.id, m]));
		setMsgs((cur) => cur.map((m) => got.get(m.id) ?? m));
	}, [scope, chatId, topic]);

	// Messages filled into the window's stretch later (a recheck found
	// them missing): fetched around their ids and put in their place by
	// message id. They are not new to the reader, so they neither count
	// as unseen nor move the view.
	const fillGaps = useCallback(async () => {
		const sig = win.current?.signal;
		if (!sig) return;
		const ids = [...gaps.current].sort((a, b) => a - b);
		gaps.current.clear();
		for (const id of ids) {
			// An earlier answer of this run may have brought it already.
			if (msgsRef.current.some((m) => m.message_id === id)) continue;
			const w = await fetchChatWindow(scope, chatId, { around: id }, 5, sig, topic);
			if (sig.aborted) return;
			const cur = msgsRef.current;
			const first = cur[0], last = cur[cur.length - 1];
			if (!first || !last) return;
			const have = new Set(cur.map((m) => m.id));
			// Only what lies inside the window: older ones come with the
			// older page, newer ones with the tail.
			const add = w.messages.filter((m) => !have.has(m.id) && m.message_id < last.message_id && (m.message_id > first.message_id || !older));
			if (!add.length) continue;
			const next = [...cur];
			for (const m of add) {
				const i = next.findIndex((x) => x.message_id > m.message_id);
				next.splice(i < 0 ? next.length : i, 0, m);
			}
			keepPlace();
			msgsRef.current = next;
			setMsgs(next);
		}
	}, [scope, chatId, topic, older]);

	// After a resync (events were missed), a window that does not end at
	// the newest is refetched in place rather than from the tail: its own
	// stretch, from its first message, page by page up to its last one.
	// Changed messages replace theirs, missing ones go in by message id,
	// and the reader's view stays where it was. What lies after the
	// window still comes with the newer pages.
	const resyncWindow = useCallback(async () => {
		const sig = win.current?.signal;
		if (!sig) return;
		const start = msgsRef.current;
		const first = start[0], last = start[start.length - 1];
		if (!first || !last) return;
		const got: ChatMessage[] = [];
		let cursor = first.message_id - 1;
		for (let pages = 0; pages < RESYNC_PAGES; pages++) {
			const w = await fetchChatWindow(scope, chatId, { after: cursor }, 100, sig, topic);
			if (sig.aborted) return;
			got.push(...w.messages);
			const end = w.messages[w.messages.length - 1];
			if (!end || !w.has_newer || end.message_id >= last.message_id) break;
			cursor = end.message_id;
		}
		if (!got.length) return;
		const fresh = new Map(got.map((m) => [m.id, m]));
		const next = msgsRef.current.map((m) => fresh.get(m.id) ?? m);
		const have = new Set(next.map((m) => m.id));
		for (const m of got) {
			if (have.has(m.id) || m.message_id <= first.message_id || m.message_id >= last.message_id) continue;
			const i = next.findIndex((x) => x.message_id > m.message_id);
			next.splice(i < 0 ? next.length : i, 0, m);
		}
		keepPlace();
		msgsRef.current = next;
		setMsgs(next);
	}, [scope, chatId, topic]);

	// Signed in with Live on, the chat follows its events wherever the
	// window is; only a window at the newest (following) fetches the
	// tail, counts unseen messages and polls. The topic stays the same
	// when the window reaches the newest, so the stream is not reopened.
	const following = live && !newer && !publicView;
	useLive(live && !publicView ? [`chat:${chatId}`] : null, (e) => {
		const run = (key: string, fn: () => Promise<unknown>) => (refetch ? refetch.request(key, 250, fn) : void fn().catch(() => {}));
		const cur = msgsRef.current;
		const first = cur[0], last = cur[cur.length - 1];
		if (e.kind === 'message.new' && e.message_id !== undefined && first && last && e.message_id < last.message_id) {
			// Inside the window: put in its place, unless already there.
			// Older than the window: the older page brings it.
			if ((e.message_id > first.message_id || !older) && !cur.some((m) => m.message_id === e.message_id)) {
				gaps.current.add(e.message_id);
				run(`chat:${chatId}:gaps`, fillGaps);
			}
		} else if (e.kind === 'message.new' || e.kind === 'resync') {
			// Not at the newest, a new message after the window is left to
			// the newer pages (the jump button already says there are newer
			// messages), and a resync refreshes the window itself.
			if (!newer) run(`chat:${chatId}:tail`, fetchNewer);
			else if (e.kind === 'resync') run(`chat:${chatId}:window`, resyncWindow);
		} else if ((e.kind === 'message.edited' || e.kind === 'message.deleted' || e.kind === 'file.stored') && e.message_id !== undefined && msgsRef.current.some((m) => m.message_id === e.message_id)) {
			const id = e.message_id;
			run(`chat:${chatId}:m${id}`, () => replaceMessage(id));
		}
	});

	const polling = following && (liveState === 'polling' || liveState === 'idle');
	useEffect(() => {
		if (!polling) return;
		const t = setInterval(() => { if (!document.hidden) void fetchNewer().catch(() => {}); }, 1000);
		return () => clearInterval(t);
	}, [polling, fetchNewer]);

	const onReply = (id: number) => {
		const t = document.getElementById('m' + id);
		if (t) { t.scrollIntoView({ block: 'center', behavior: 'smooth' }); setFlash(null); requestAnimationFrame(() => setFlash(id)); }
		else navigate(setQuery(page.url, { at: id, date: undefined, after: undefined, after_ts: undefined }), { replace: true, scroll: false });
	};
	const toLatest = () => {
		if (newer) navigate(setQuery(page.url, { at: undefined, date: undefined, after: undefined, after_ts: undefined }), { replace: true, scroll: false });
		else scroller.current?.scrollTo({ top: scroller.current.scrollHeight, behavior: 'smooth' });
	};

	const title = group ? group.title : p.chat.kind === 'user' ? p.chat.user.name : '';
	const photo = group ? group.photo : p.chat.kind === 'user' ? p.chat.user.photo : null;
	const back = isGroup ? `/groups/${chatId}` : `/users/${chatId}`;
	// Signed out, only an exposed group is shown: its public page.
	const info = publicView ? (isGroup ? back : null) : back;
	// Telegram's member count, when the archive has it; beside it the
	// members the archive has seen.
	const counted = p.chat.kind === 'group' ? group?.member_count ?? p.chat.group.member_count : null;
	const sub = p.chat.kind === 'group'
		? `${TYPE_LABEL[p.chat.group.type]}${counted != null ? ` . ${fmt(counted)} members` : ''}${p.chat.group.members ? ` . ${fmt(p.chat.group.members)} members seen` : ''} . ${fmt(group?.msg_count ?? p.chat.group.msg_count)} messages`
		: `private chat with the logging account . ${fmt(p.chat.count)} messages`;
	// Hashtags search this chat's messages; a signed-out visitor has no
	// search, so there they are only styled.
	const richChat: RichChat | undefined = publicView ? undefined : { scope, id: chatId };
	const body = (
		<div className={`c-chat ${publicView ? 'public' : ''}`}>
			<header className="c-head">
				{!publicView && <Link to={back} className="c-iconbtn" aria-label="Back to details"><ArrowLeft size={20} /></Link>}
				{/* The chat's name opens its details: the group's page, or the
				    profile of the user a private chat is with. */}
				{info ? (
					<Link to={info} className="c-head-link" title={isGroup ? 'Group details' : 'Profile'}>
						<Avatar id={chatId} name={title} photo={photo} size={40} square={isGroup} />
						<div className="c-head-title"><h1>{title}</h1><span className="c-sub">{sub}</span></div>
					</Link>
				) : (
					<>
						<Avatar id={chatId} name={title} photo={photo} size={40} square={isGroup} />
						<div className="c-head-title"><h1>{title}</h1><span className="c-sub">{sub}</span></div>
					</>
				)}
				{!publicView && (
					<button type="button" className={`c-live ${live ? 'on' : ''}`} onClick={() => setLive((x) => !x)} aria-pressed={live} title="Show new messages as they are logged">
						<Radio size={14} />{live ? 'Live' : 'Paused'}
					</button>
				)}
				<button type="button" className="c-iconbtn" onClick={() => setSearching((x) => !x)} aria-label="Search in chat"><Search size={19} /></button>
				<button type="button" className="c-iconbtn" onClick={() => navigate(setQuery(page.url, { after: 0, at: undefined, date: undefined, after_ts: undefined }), { replace: true, scroll: false })} aria-label="Jump to the oldest messages" title="Oldest messages"><ChevronsUp size={19} /></button>
				<label className="c-iconbtn c-datebtn" aria-label="Jump to date and time" title="Jump to a date and time">
					<CalendarDays size={19} />
					{/* The server reads the time in the viewer's timezone. */}
					<input type="datetime-local" onChange={(e) => e.target.value && navigate(setQuery(page.url, { date: e.target.value, at: undefined, after: undefined, after_ts: undefined }), { replace: true, scroll: false })} />
				</label>
				{!publicView && <Link to={back} className="c-iconbtn c-hide-sm" aria-label="Details"><Info size={19} /></Link>}
			</header>
			{p.topics.length > 0 && <TopicStrip topics={p.topics} current={topic} hrefOf={topicHref} />}
			{searching && (
				<form className="c-searchbar" onSubmit={(e) => {
					e.preventDefault();
					const text = new FormData(e.currentTarget).get('q');
					navigate(`/${isGroup ? 'group' : 'private'}_messages?q=${encodeURIComponent(String(text ?? ''))}&f=${encodeURIComponent(JSON.stringify([{ field: 'chat_id', op: '=', value: String(chatId) }]))}`);
				}}>
					<Search size={15} />
					<input name="q" autoFocus placeholder={`Search in ${title}`} />
					<button type="button" className="c-iconbtn" onClick={() => setSearching(false)} aria-label="Close search"><X size={16} /></button>
				</form>
			)}
			<div className="c-scroll" ref={scroller}>
				<div className="c-feed">
					{older ? (prefs(v).autoLoad ? <div className="c-loading">Loading older messages...</div> : <div className="c-loading"><button type="button" className="a-link" onClick={() => void loadOlder()}>Load older messages</button></div>) : <div className="c-start">Beginning of the archived history</div>}
					<Feed msgs={msgs} v={v} isGroup={isGroup} flash={flash} onReply={onReply} tag={tagOf} chat={richChat} topicHref={topicHref} />
					{newer && (prefs(v).autoLoad ? <div className="c-loading">Loading newer messages...</div> : <div className="c-loading"><button type="button" className="a-link" onClick={() => void loadNewer()}>Load newer messages</button></div>)}
				</div>
			</div>
			{(!atBottom || newer) && <button type="button" className="c-tobottom" aria-label={unseen ? `Jump to latest, ${unseen} new` : 'Jump to latest'} onClick={toLatest}><ChevronDown size={22} />{unseen > 0 && <span className="c-unseen">{unseen > 99 ? '99+' : unseen}</span>}</button>}
			<footer className="c-readonly">
				{publicView ? <><Globe size={14} />Public archive of {title}. Read only.</> : <>Archive view, read only. {!live ? 'Live updates paused.' : liveState === 'polling' ? 'Live updates unavailable; checking for new messages every second.' : 'New messages appear as they are logged.'}</>}
			</footer>
		</div>
	);
	if (publicView) {
		return (
			<PublicFrame viewer={v} url={page.url}>{body}</PublicFrame>
		);
	}
	return <Shell flush crumbs={[[isGroup ? 'Groups' : 'Users', isGroup ? '/groups' : '/users'], [title, back], ['Chat', null]]}>{body}</Shell>;
}

export const chatRoute: RouteDef<ChatProps> = {
	Page: Chat,
	head: (p) => {
		const c = p.props.chat;
		const title = c.kind === 'group' ? c.group.title : c.user.name;
		// Only the public pages of exposed groups are for search engines.
		const pub = !p.viewer.signed_in && c.kind === 'group';
		return {
			title: `${title} - tgloggerd`,
			description: pub ? `Public archive of ${title}` : undefined,
			robots: pub ? undefined : 'noindex',
			canonical: pub ? p.url.split('?')[0] : undefined,
			og: pub ? { title, type: 'website' } : undefined,
		};
	},
};
