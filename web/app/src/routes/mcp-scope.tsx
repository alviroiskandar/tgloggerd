// An MCP token's scope on the tokens page: the federations, groups and
// (superadmins only) private chats it reads; none reads every exposed
// group (decisions/2026-11-20-mcp-token-scope.md). The list's summary
// and the form's editor.
import { useState } from 'react';
import { Info, Lock, Network, Plus, TriangleAlert, Users, X } from 'lucide-react';
import { Btn } from '../ui/primitives.tsx';
import { Field, MultiPicker, NO_FILL, type PickItem } from '../ui/forms.tsx';
import type { S } from '../api/schema.ts';

export type ScopeEntry = S['McpScopeEntry'];
export type ScopeKind = S['McpScopeKind'];
type Person = S['FwdPerson'];

// Whether private chats on a token of this owner work: a superadmin's
// token, or one without an owner record (id 0, the superadmins').
export const superOwned = (o: Person) => o.id === 0 || (o.role === 'superadmin' && o.active !== false);

const KINDS: Record<ScopeKind, { icon: typeof Lock; label: string }> = {
	federation: { icon: Network, label: 'Federation' },
	group: { icon: Users, label: 'Group' },
	private: { icon: Lock, label: 'Private chat' },
};

// The entry's name, or what it was when it no longer exists.
const nameOf = (e: ScopeEntry) => e.name || (e.kind === 'federation' ? 'Deleted federation' : e.kind === 'group' ? `Group ${e.id}` : `User ${e.id}`);

// Production's /mcp is still served by the old web service, which
// ignores token scopes; only the Go service applies them, once it
// serves /mcp. Until then the page says so. After the switch, set this
// to false, or remove it with ScopeNotice and its two uses.
export const SCOPES_PENDING = true;

export function ScopeNotice() {
	if (!SCOPES_PENDING) return null;
	return <p className="a-tokscope-pending" role="note"><Info size={14} aria-hidden="true" />Scopes take effect once MCP is served by the new web service; until then every token reads all exposed groups.</p>;
}

// Why an inactive entry reads nothing, as far as the page can tell.
function whyInactive(e: ScopeEntry, owner: Person) {
	if (e.kind === 'federation') return 'Inactive: the federation was deleted, so this entry reads nothing.';
	if (e.kind === 'group') return e.name ? 'Inactive: the group is not exposed to MCP, so this entry reads nothing until it is.' : 'Inactive: the group is no longer in the archive.';
	if (!superOwned(owner)) return 'Inactive: a private chat works only on a token a superadmin owns, so this entry reads nothing.';
	return 'Inactive: this private chat reads nothing.';
}

// A scope with entries none of which is active reads nothing: it never
// falls back to every exposed group.
export const readsNothing = (scope: ScopeEntry[]) => scope.length > 0 && scope.every((e) => !e.active);

export function NothingWarning() {
	return <span className="a-tokscope-none" role="note"><TriangleAlert size={12} />This token reads nothing: none of its scope is active</span>;
}

export function ScopeChip({ e, owner }: { e: ScopeEntry; owner: Person }) {
	const k = KINDS[e.kind];
	return (
		<span className={`a-pill a-tokscope-e${e.active ? '' : ' off'}`} title={e.active ? `${k.label}: ${nameOf(e)}` : whyInactive(e, owner)}>
			<k.icon size={11} aria-label={k.label} />{nameOf(e)}{!e.active && <span className="a-tokscope-flag">inactive</span>}
		</span>
	);
}

// More entries than this collapse behind a toggle.
const SHORT = 3;

// The list's short form of a scope, in the server's order (federations,
// groups, private chats).
export function ScopeSummary({ scope, owner }: { scope: ScopeEntry[]; owner: Person }) {
	const [open, setOpen] = useState(false);
	if (!scope.length) return <span className="a-muted">All exposed groups</span>;
	const long = scope.length > SHORT;
	const shown = long && !open ? scope.slice(0, SHORT - 1) : scope;
	return (
		<span className="a-tokscope">
			{shown.map((e) => <ScopeChip key={e.kind + e.id} e={e} owner={owner} />)}
			{long && <button type="button" className="a-tokscope-more" aria-expanded={open} onClick={() => setOpen((o) => !o)}>{open ? 'Show less' : `+${scope.length - shown.length} more`}</button>}
			{readsNothing(scope) && <NothingWarning />}
		</span>
	);
}

export type ScopeOptions = S['McpTokensProps']['scope_options'];
export type ScopeWrite = S['McpScopeWrite'];

const ORDER: ScopeKind[] = ['federation', 'group', 'private'];
const same = (a: { kind: ScopeKind; id: number }, b: { kind: ScopeKind; id: number }) => a.kind === b.kind && a.id === b.id;
// As the server orders it: federations, groups, private chats, by id.
const sorted = (v: ScopeEntry[]) => [...v].sort((a, b) => ORDER.indexOf(a.kind) - ORDER.indexOf(b.kind) || a.id - b.id);
export const scopeRefs = (v: ScopeEntry[]): ScopeWrite => v.map(({ kind, id }) => ({ kind, id }));
export const sameScope = (a: ScopeEntry[], b: ScopeEntry[]) => a.length === b.length && a.every((e) => b.some((x) => same(e, x)));

// Whether the viewer may give this token private chats: a superadmin,
// on a token a superadmin owns.
export const mayPrivate = (o: ScopeOptions, owner: Person) => o.private && superOwned(owner);
// The private entries the server would refuse to save (403): any, when
// the viewer may not give them. The form says so and offers to drop them.
export const blockedPrivate = (v: ScopeEntry[], o: ScopeOptions, owner: Person) => (mayPrivate(o, owner) ? [] : v.filter((e) => e.kind === 'private'));

const asPick = (f: S['FedRef'], kind: 'group'): PickItem => ({ id: f.id, title: f.name, kind, photo: null });

export function ScopeEditor({ value, onChange, options, owner }: { value: ScopeEntry[]; onChange: (v: ScopeEntry[]) => void; options: ScopeOptions; owner: Person }) {
	const [uid, setUid] = useState('');
	const has = (kind: ScopeKind, id: number) => value.some((e) => same(e, { kind, id }));
	const add = (e: ScopeEntry) => { if (!has(e.kind, e.id)) onChange(sorted([...value, e])); };
	const remove = (e: ScopeEntry) => onChange(value.filter((x) => !same(x, e)));
	const pick = (kind: 'federation' | 'group') => (v: PickItem[]) => { const p = v[v.length - 1]; if (p) add({ kind, id: p.id, name: p.title, active: true }); };
	const feds = options.federations.filter((f) => !has('federation', f.id)).map((f) => asPick(f, 'group'));
	const groups = options.groups.filter((g) => !has('group', g.id)).map((g) => asPick(g, 'group'));
	const blocked = blockedPrivate(value, options, owner);
	// A private chat is the other person's user id.
	const id = /^\d{1,19}$/.test(uid.trim()) ? Number(uid.trim()) : 0;
	const uidErr = !uid.trim() ? '' : !id ? 'A user id is a positive number.' : has('private', id) ? 'Already in the scope.' : '';
	const addPrivate = () => { if (id && !uidErr) { add({ kind: 'private', id, name: '', active: true }); setUid(''); } };
	return (
		<div className="a-tokscope-edit">
			<ScopeNotice />
			<div className="a-field" role="group" aria-label="Scope">
				<span className="a-field-label">Scope</span>
				{value.length
					? <ul className="a-tokscope-list">{value.map((e) => (
						<li key={e.kind + e.id}><ScopeChip e={e} owner={owner} /><button type="button" className="a-icon-btn sm" aria-label={`Remove ${nameOf(e)}`} title="Remove" onClick={() => remove(e)}><X size={12} /></button></li>
					))}</ul>
					: <p className="a-muted sm">All exposed groups. Add federations or groups to narrow it.</p>}
				{readsNothing(value) && <NothingWarning />}
			</div>
			{blocked.length > 0 && (
				<div className="a-banner warn"><TriangleAlert size={15} /><span>
					{options.private ? 'A private chat works only on a token a superadmin owns' : 'Only a superadmin can give a token a private chat'}, so this scope cannot be saved with {blocked.length === 1 ? 'its private chat' : `its ${blocked.length} private chats`}.{' '}
					<Btn onClick={() => onChange(value.filter((e) => e.kind !== 'private'))}>{blocked.length === 1 ? 'Remove the private chat' : 'Remove the private chats'}</Btn>
				</span></div>
			)}
			<Field label="Add a federation" hint="Its exposed groups, following the groups as they join or leave it.">
				<MultiPicker value={[]} onChange={pick('federation')} options={feds} placeholder="Add a federation..." />
			</Field>
			<Field label="Add a group" hint="Only exposed groups are offered.">
				<MultiPicker value={[]} onChange={pick('group')} options={groups} placeholder="Add a group..." />
			</Field>
			{mayPrivate(options, owner) && (
				<Field label="Add a private chat" hint={uidErr || 'The other person\'s user id. A private chat is never exposed; only tokens scoped to it read it.'}>
					<span className="a-tokscope-uid">
						<input id="mcp-scope-uid" name="mcp-scope-uid" inputMode="numeric" value={uid} aria-invalid={!!uidErr} onChange={(e) => setUid(e.target.value)} onKeyDown={(e) => { if (e.key === 'Enter') { e.preventDefault(); addPrivate(); } }} placeholder="User id" {...NO_FILL} />
						<Btn icon={Plus} aria-label="Add the private chat" disabled={!id || !!uidErr} onClick={addPrivate}>Add</Btn>
					</span>
				</Field>
			)}
		</div>
	);
}
