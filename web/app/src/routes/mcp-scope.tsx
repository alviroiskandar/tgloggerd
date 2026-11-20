// An MCP token's scope on the tokens page: the federations, groups and
// (superadmins only) private chats it reads; none reads every exposed
// group (decisions/2026-11-20-mcp-token-scope.md).
import { useState } from 'react';
import { Lock, Network, TriangleAlert, Users } from 'lucide-react';
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
