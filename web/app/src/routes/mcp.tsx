// Integrations > MCP: the exposure allowlist and the access tokens
// (web/docs/mcp.md). Exposing a group also publishes its chat on the web.
import { useState } from 'react';
import { ArrowRight, ArrowRightLeft, Check, Copy, Globe, KeyRound, ListFilter, Pencil, Plus, ShieldAlert, Trash2, TriangleAlert } from 'lucide-react';
import Shell from '../ui/Shell.tsx';
import { Link, useNav } from '../nav.tsx';
import { Avatar, Btn, Card, PageHead, Pill } from '../ui/primitives.tsx';
import { ChatPicker, Field, NO_FILL, type PickItem } from '../ui/forms.tsx';
import { GroupCell } from '../ui/bits.tsx';
import { Modal } from '../ui/Modal.tsx';
import { ApiError, api, ok } from '../api/client.ts';
import { ago, compact, fmt } from '../format.ts';
import type { PageData } from '../page.ts';
import type { GroupSummary } from '../types.ts';
import type { RouteDef } from './index.tsx';
import { ScopeEditor, ScopeNotice, ScopeSummary, blockedPrivate, sameScope, scopeRefs, type ScopeEntry } from './mcp-scope.tsx';
import type { S } from '../api/schema.ts';

export type McpIndexProps = S['McpIndexProps'];
export type McpGroupsProps = S['McpGroupsProps'];
export type McpTokensProps = S['McpTokensProps'];

type Token = McpTokensProps['tokens'][number];

function errText(x: unknown) { return x instanceof ApiError ? x.message : 'The server could not be reached.'; }

// The owner, and who minted the token when that is someone else: its
// requests are attributed to the minter, and disabling the minter stops
// the token whoever owns it.
function OwnerCell({ t }: { t: Token }) {
	const o = t.owner;
	return (
		<span className="a-ownercell">
			<Avatar id={o.id} name={o.name} size={22} />
			<span>
				<span className={o.active === false ? 'a-muted' : ''}>{o.name}</span>
				{o.active === false && <> <Pill tone="danger"><TriangleAlert size={11} />owner disabled</Pill></>}
				{t.minted_by.id !== o.id && <span className="a-muted sm a-block">minted by {t.minted_by.name}{t.minted_by.active === false ? ' (disabled)' : ''}</span>}
			</span>
		</span>
	);
}

function McpIndex({ page }: { page: PageData<McpIndexProps> }) {
	const p = page.props;
	// A member reaches only its own tokens: the allowlist is an admin's,
	// so its card is shown without a link.
	const allowlist = page.viewer.account?.role !== 'member';
	const groups = <>
		<div className="a-dircard-top"><Globe size={18} /><h2>Exposed groups</h2>{allowlist && <ArrowRight size={16} className="a-muted" />}</div>
		<p className="a-muted">The allowlist. Nothing is readable through MCP until a group is added here. Private groups and direct messages are never exposed.</p>
		<div className="a-dircard-foot"><span><b className="num">{p.exposed_groups}</b> groups</span><span className="a-muted">{compact(p.exposed_messages)} messages readable</span></div>
	</>;
	return (
		<Shell crumbs={[['MCP', null]]}>
			<PageHead title="MCP" sub="The Model Context Protocol endpoint lets a language model read this archive. Two things gate it: which groups are exposed, and who holds a token." />
			<div className="a-dircards">
				{allowlist ? <Link to="/mcp-admin/groups" className="a-card a-dircard">{groups}</Link> : <div className="a-card a-dircard">{groups}</div>}
				<Link to="/mcp-admin/tokens" className="a-card a-dircard">
					<div className="a-dircard-top"><KeyRound size={18} /><h2>Access tokens</h2><ArrowRight size={16} className="a-muted" /></div>
					<p className="a-muted">Bearer tokens for MCP clients. Shown once when minted and stored only as a hash.</p>
					<div className="a-dircard-foot"><span><b className="num">{p.active_tokens}</b> active tokens</span><span className="a-muted">{p.last_used ? `last used ${ago(p.last_used, page.viewer)}` : 'never used'}</span></div>
				</Link>
			</div>
			<Card title="Connecting a client" className="a-mt">
				<p className="a-muted">Point an MCP client at this endpoint and pass a token as a bearer credential. The endpoint is read only.</p>
				<div className="a-code"><pre>{`URL            ${p.endpoint}\nAuthorization  Bearer tgmcp_...`}</pre></div>
			</Card>
		</Shell>
	);
}

// An empty note clears it. A 404 means someone withdrew the group
// meanwhile.
const saveNote = (id: number, note: string) => ok(api.PATCH('/api/mcp/groups/{group_id}', { params: { path: { group_id: id } }, body: { note } })).catch((x: unknown) => {
	throw x instanceof ApiError && x.status === 404 ? new ApiError(404, x.code, 'This group is no longer exposed. Reload the page to see the current list.') : x;
});

function McpGroups({ page }: { page: PageData<McpGroupsProps> }) {
	const { navigate } = useNav();
	const v = page.viewer;
	const [adding, setAdding] = useState(false);
	const [pick, setPick] = useState<PickItem | null>(null);
	const [note, setNote] = useState('');
	const [err, setErr] = useState('');
	// The row whose note is being edited, and the draft.
	const [editing, setEditing] = useState<McpGroupsProps['groups'][number] | null>(null);
	const [draft, setDraft] = useState('');
	const exposed = new Set(page.props.groups.map((g) => g.group.id));
	const refresh = () => navigate(page.url, { replace: true, scroll: false });
	const act = async (fn: () => Promise<unknown>) => { setErr(''); try { await fn(); setAdding(false); setEditing(null); refresh(); } catch (x) { setErr(errText(x)); } };
	return (
		<Shell crumbs={[['MCP', '/mcp-admin'], ['Exposed groups', null]]}>
			<PageHead title="Exposed groups" sub="Only these groups can be read through MCP. Everything else in the archive stays private."
				actions={<Btn primary icon={Plus} onClick={() => setAdding(true)}>Expose a group</Btn>} />
			<div className="a-banner warn"><ShieldAlert size={15} /><span><b>Exposing a group also publishes its chat on the web.</b> Anyone can read its history and files at /groups/&lt;id&gt;/chat without signing in.</span></div>
			{err && !adding && !editing && <div className="a-banner warn" role="alert"><TriangleAlert size={15} /><span>{err}</span></div>}
			<div className="a-card a-tablecard">
				<div className="a-tablescroll">
					<table className="a-table">
						<thead><tr><th>Group</th><th className="r">Messages</th><th>Public on Telegram</th><th>Note</th><th>Added</th><th /></tr></thead>
						<tbody>{page.props.groups.map((e) => (
							<tr key={e.group.id}>
								<td><GroupCell g={e.group} /></td>
								<td className="r num">{fmt(e.group.msg_count)}</td>
								<td>{e.public_now ? <Pill tone="ok">public</Pill> : <Pill tone="danger"><TriangleAlert size={11} />no longer public</Pill>}</td>
								<td className="a-muted a-clip">{e.note}</td>
								<td title={e.added}>{ago(e.added, v)}</td>
								<td className="a-rowactions"><Btn ghost icon={Pencil} onClick={() => { setErr(''); setDraft(e.note); setEditing(e); }}>Edit note</Btn><Btn ghost danger icon={Trash2} onClick={() => { if (confirm(`Stop exposing ${e.group.title}?`)) void act(() => ok(api.DELETE('/api/mcp/groups/{group_id}', { params: { path: { group_id: e.group.id } } }))); }}>Remove</Btn></td>
							</tr>
						))}</tbody>
					</table>
				</div>
				{!page.props.groups.length && <div className="a-empty">Nothing is exposed.</div>}
			</div>
			<p className="a-muted a-foot"><b>Public on Telegram</b> says whether the group still has a public username. A group that went private stays exposed until it is removed here.</p>
			{adding && (
				<Modal title="Expose a group" onClose={() => setAdding(false)}
					footer={<><span className="a-grow" /><Btn ghost onClick={() => setAdding(false)}>Cancel</Btn><Btn primary disabled={!pick} onClick={() => void act(() => ok(api.POST('/api/mcp/groups', { body: { group_id: pick!.id, note } })))}>Expose</Btn></>}>
					{err && <div className="a-banner warn" role="alert"><TriangleAlert size={15} /><span>{err}</span></div>}
					<Field label="Group" hint="Busiest groups first. Groups already exposed are not listed."><ChatPicker value={pick} onChange={setPick} search={{ kind: 'group' }} exclude={exposed} placeholder="Search groups and channels" /></Field>
					<Field label="Note" hint="Why it is exposed, for whoever reviews this later."><input value={note} onChange={(e) => setNote(e.target.value)} maxLength={255} /></Field>
					<div className="a-banner warn"><ShieldAlert size={15} /><span>Its whole history becomes readable on the web without signing in.</span></div>
				</Modal>
			)}
			{editing && (
				<Modal title={`Note for ${editing.group.title}`} onClose={() => setEditing(null)}
					footer={<><span className="a-grow" /><Btn ghost onClick={() => setEditing(null)}>Cancel</Btn><Btn primary onClick={() => void act(() => saveNote(editing.group.id, draft))}>Save</Btn></>}>
					{err && <div className="a-banner warn" role="alert"><TriangleAlert size={15} /><span>{err}</span></div>}
					<Field label="Note" hint="Why it is exposed, for whoever reviews this later. Leave it empty to clear it."><input value={draft} onChange={(e) => setDraft(e.target.value)} maxLength={255} {...NO_FILL} /></Field>
				</Modal>
			)}
		</Shell>
	);
}

function McpTokens({ page }: { page: PageData<McpTokensProps> }) {
	const { navigate } = useNav();
	const v = page.viewer;
	const [minting, setMinting] = useState(false);
	const [name, setName] = useState('');
	// Who the new token is for: the viewer unless another owner is picked.
	const me = v.account?.id ?? 0;
	const owners = page.props.owners;
	const [owner, setOwner] = useState(owners.some((o) => o.id === me) ? me : owners[0]?.id ?? me);
	const [minted, setMinted] = useState<string | null>(null);
	const [copied, setCopied] = useState(false);
	const [err, setErr] = useState('');
	// The token being renamed, and the draft name.
	const [renaming, setRenaming] = useState<Token | null>(null);
	const [draft, setDraft] = useState('');
	// The token being given to another owner (superadmins), and to whom.
	const [moving, setMoving] = useState<Token | null>(null);
	const [to, setTo] = useState('');
	// The new token's scope, and the token whose scope is being edited
	// with its draft.
	const [scope, setScope] = useState<ScopeEntry[]>([]);
	const [scoping, setScoping] = useState<Token | null>(null);
	const [scopeDraft, setScopeDraft] = useState<ScopeEntry[]>([]);
	const opts = page.props.scope_options;
	// The new token's owner as a person: whether it may hold private chats.
	const ownerOf = (id: number) => owners.find((o) => o.id === id) ?? { id, name: '', role: v.account?.role };
	const refresh = () => navigate(page.url, { replace: true, scroll: false });
	const transfer = async () => {
		setErr('');
		try { await ok(api.POST('/api/mcp/tokens/{id}/transfer', { params: { path: { id: moving!.id } }, body: { owner_id: Number(to) } })); setMoving(null); refresh(); } catch (x) { setErr(errText(x)); }
	};
	const rename = async () => {
		setErr('');
		try { await ok(api.PATCH('/api/mcp/tokens/{id}', { params: { path: { id: renaming!.id } }, body: { name: draft } })); setRenaming(null); refresh(); } catch (x) { setErr(errText(x)); }
	};
	// Sent only when it changed; [] reads every exposed group again.
	const saveScope = async () => {
		setErr('');
		if (sameScope(scopeDraft, scoping!.scope)) { setScoping(null); return; }
		try { await ok(api.PATCH('/api/mcp/tokens/{id}', { params: { path: { id: scoping!.id } }, body: { scope: scopeRefs(scopeDraft) } })); setScoping(null); refresh(); } catch (x) { setErr(errText(x)); }
	};
	const mint = async () => {
		setErr('');
		try {
			const body = { name, ...(owner === me ? {} : { owner_id: owner }), ...(scope.length ? { scope: scopeRefs(scope) } : {}) };
			const r = await ok(api.POST('/api/mcp/tokens', { body }));
			setMinting(false); setCopied(false); setMinted(r.token); setName(''); setOwner(me); setScope([]); refresh();
		} catch (x) { setErr(errText(x)); }
	};
	const revoke = async (id: number) => { try { await ok(api.POST('/api/mcp/tokens/{id}/revoke', { params: { path: { id } } })); refresh(); } catch (x) { setErr(errText(x)); } };
	return (
		<Shell crumbs={[['MCP', '/mcp-admin'], ['Access tokens', null]]}>
			<PageHead title="Access tokens" sub="A token reads every exposed group unless its scope narrows it, so treat one like a password."
				actions={<Btn primary icon={Plus} onClick={() => setMinting(true)}>Mint a token</Btn>} />
			{err && !minting && !renaming && !moving && !scoping && <div className="a-banner warn" role="alert"><TriangleAlert size={15} /><span>{err}</span></div>}
			{minted && (
				<div className="a-card a-minted">
					<div className="a-cardhead"><h2><KeyRound size={15} /> Copy this token now</h2></div>
					<p className="a-muted">This is the only time it is shown. Only a hash is stored; if it is lost, revoke it and mint another.</p>
					<div className="a-code"><pre>{minted}</pre>
						<button type="button" className="a-btn" onClick={() => { void navigator.clipboard?.writeText(minted); setCopied(true); }}>{copied ? <><Check size={14} />Copied</> : <><Copy size={14} />Copy</>}</button></div>
					<Btn ghost onClick={() => setMinted(null)}>Done</Btn>
				</div>
			)}
			<ScopeNotice />
			<div className="a-card a-tablecard">
				<div className="a-tablescroll">
					<table className="a-table">
						<thead><tr><th>Name</th><th>Owner</th><th>Reads</th><th>Created</th><th>Last used</th><th>Status</th><th /></tr></thead>
						<tbody>{page.props.tokens.map((t) => (
							<tr key={t.id} className={t.revoked ? 'a-off' : ''}>
								<td>{t.name ? <b>{t.name}</b> : <span className="a-muted">(unnamed)</span>}</td>
								<td><OwnerCell t={t} /></td>
								<td className="a-tokscope-cell"><ScopeSummary scope={t.scope} owner={t.owner} /></td>
								<td title={t.created}>{ago(t.created, v)}</td>
								<td>{t.last_used ? ago(t.last_used, v) : <span className="a-muted">never</span>}</td>
								<td>{t.revoked ? <span className="a-muted">revoked {ago(t.revoked, v)}</span> : <Pill tone="ok">active</Pill>}</td>
								<td className="a-rowactions"><Btn ghost icon={Pencil} onClick={() => { setErr(''); setDraft(t.name); setRenaming(t); }}>Rename</Btn>{!t.revoked && <Btn ghost icon={ListFilter} onClick={() => { setErr(''); setScopeDraft(t.scope); setScoping(t); }}>Scope</Btn>}{page.props.can_transfer && <Btn ghost icon={ArrowRightLeft} onClick={() => { setErr(''); setTo(''); setMoving(t); }}>Transfer</Btn>}{!t.revoked && <Btn ghost danger onClick={() => { if (confirm(`Revoke ${t.name || 'this token'}? Clients using it stop working at once.`)) void revoke(t.id); }}>Revoke</Btn>}</td>
							</tr>
						))}</tbody>
					</table>
				</div>
			</div>
			<p className="a-muted a-foot">Revoking keeps the row for the audit trail; the token stops working at once.</p>
			{minting && (
				<Modal title="Mint a token" onClose={() => setMinting(false)}
					footer={<><span className="a-grow" /><Btn ghost onClick={() => setMinting(false)}>Cancel</Btn><Btn primary disabled={blockedPrivate(scope, opts, ownerOf(owner)).length > 0} onClick={() => void mint()}>Mint</Btn></>}>
					{err && <div className="a-banner warn" role="alert"><TriangleAlert size={15} /><span>{err}</span></div>}
					<Field label="Name" hint="A label, so this one can be revoked later without guessing which is which."><input id="mcp-token-label" name="mcp-token-label" value={name} onChange={(e) => setName(e.target.value)} maxLength={64} {...NO_FILL} /></Field>
					{owners.length > 1 && (
						<Field label="Owner" hint="Who manages the token. Its requests are still made in your name, so disabling your account stops it.">
							<select className="a-select" value={owner} onChange={(e) => setOwner(Number(e.target.value))}>
								{owners.map((o) => <option key={o.id} value={o.id}>{o.id === me ? `${o.name} (you)` : o.role ? `${o.name} (${o.role})` : o.name}</option>)}
							</select>
						</Field>
					)}
					<ScopeEditor value={scope} onChange={setScope} options={opts} owner={ownerOf(owner)} />
				</Modal>
			)}
			{scoping && (
				<Modal title={`Scope of ${scoping.name || 'this token'}`} onClose={() => setScoping(null)}
					footer={<><span className="a-grow" /><Btn ghost onClick={() => setScoping(null)}>Cancel</Btn><Btn primary disabled={blockedPrivate(scopeDraft, opts, scoping.owner).length > 0} onClick={() => void saveScope()}>Save</Btn></>}>
					{err && <div className="a-banner warn" role="alert"><TriangleAlert size={15} /><span>{err}</span></div>}
					<p className="a-muted sm">What the token reads, from its next request on. Clients keep their token.</p>
					<ScopeEditor value={scopeDraft} onChange={setScopeDraft} options={opts} owner={scoping.owner} />
				</Modal>
			)}
			{renaming && (
				<Modal title={`Rename ${renaming.name || 'this token'}`} onClose={() => setRenaming(null)}
					footer={<><span className="a-grow" /><Btn ghost onClick={() => setRenaming(null)}>Cancel</Btn><Btn primary disabled={!draft.trim()} onClick={() => void rename()}>Save</Btn></>}>
					{err && <div className="a-banner warn" role="alert"><TriangleAlert size={15} /><span>{err}</span></div>}
					<Field label="Name" hint="Clients keep working; only the label changes."><input id="mcp-token-rename" name="mcp-token-rename" value={draft} onChange={(e) => setDraft(e.target.value)} maxLength={64} {...NO_FILL} /></Field>
				</Modal>
			)}
			{moving && (
				<Modal title={`Transfer ${moving.name || 'this token'}`} onClose={() => setMoving(null)}
					footer={<><span className="a-grow" /><Btn ghost onClick={() => setMoving(null)}>Cancel</Btn><Btn primary disabled={!to} onClick={() => void transfer()}>Transfer</Btn></>}>
					{err && <div className="a-banner warn" role="alert"><TriangleAlert size={15} /><span>{err}</span></div>}
					<p className="a-muted sm">Owned by <b>{moving.owner.name}</b>.</p>
					<Field label="New owner" hint={`The token keeps working, and its requests stay attributed to ${moving.minted_by.name}, who minted it.`}>
						<select className="a-select" value={to} onChange={(e) => setTo(e.target.value)}>
							<option value="">Choose an account...</option>
							{owners.filter((o) => o.id !== moving.owner.id).map((o) => <option key={o.id} value={o.id}>{o.role ? `${o.name} (${o.role})` : o.name}</option>)}
						</select>
					</Field>
				</Modal>
			)}
		</Shell>
	);
}

export const mcpIndexRoute: RouteDef<McpIndexProps> = { Page: McpIndex, head: () => ({ title: 'MCP - tgloggerd', robots: 'noindex' }) };
export const mcpGroupsRoute: RouteDef<McpGroupsProps> = { Page: McpGroups, head: () => ({ title: 'Exposed groups - tgloggerd', robots: 'noindex' }) };
export const mcpTokensRoute: RouteDef<McpTokensProps> = { Page: McpTokens, head: () => ({ title: 'MCP tokens - tgloggerd', robots: 'noindex' }) };
