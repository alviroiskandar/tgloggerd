// @vitest-environment happy-dom
// The MCP tokens page per role: who owns each token and who minted it,
// minting for another owner, renaming, and (superadmins) transferring.
import { beforeEach, describe, expect, it } from 'vitest';
import { cleanup, fireEvent, render, screen, waitFor, within } from '@testing-library/react';
import { callsTo, fakeApi, fixture, json, renderPage, setup } from '../test/page.tsx';
import type { McpIndexProps, McpTokensProps } from './mcp.tsx';
import { ScopeChip, readsNothing, type ScopeEntry } from './mcp-scope.tsx';
import type { AccountsProps } from './accounts.tsx';
import { adminApi } from '../../standin/fixtures.ts';
import { accounts, federations, fedGroups } from '../../../mockup/src/data/accounts.js';

beforeEach(setup);
// The table row of a token, by its name.
const rowOf = (name: string) => screen.getByText(name).closest('tr') as HTMLElement;
const dialog = () => screen.getByRole('dialog');
// A second page in one test: drop the first.
const cleanupDialog = () => cleanup();
const names = () => [...document.querySelectorAll('.a-table tbody tr td:first-child')].map((x) => x.textContent);

describe('mcp token owners', () => {
	it('shows each owner as a person, shared tokens as Shared, and who minted one for someone else', () => {
		renderPage('/mcp-admin/tokens');
		expect(within(rowOf('laptop')).getByText('Administrator')).toBeTruthy();
		expect(within(rowOf('old ci job')).getByText('Shared')).toBeTruthy();
		const r = rowOf('research-agent');
		expect(within(r).getByText('Research team')).toBeTruthy();
		expect(within(r).getByText(/minted by GNU\/Weeb admins/)).toBeTruthy();
		// Minted by its owner: no second name.
		expect(within(rowOf('laptop')).queryByText(/minted by/)).toBeNull();
	});

	it('lists every token to a superadmin, newest first', () => {
		const p = fixture<McpTokensProps>('/mcp-admin/tokens').props;
		expect(p.can_transfer).toBe(true);
		renderPage('/mcp-admin/tokens');
		expect(names()).toEqual(['(unnamed)', 'old ci job', 'research-agent', 'laptop']);
	});

	it('lists an admin its own tokens and its members\' only', () => {
		const p = fixture<McpTokensProps>('/mcp-admin/tokens', 'admin').props;
		expect(p.can_transfer).toBe(false);
		expect(p.owners.map((o) => o.name)).toEqual(['GNU/Weeb admins', 'Research team']);
		renderPage('/mcp-admin/tokens', 'admin');
		expect(names()).toEqual(['(unnamed)', 'research-agent']);
	});
});

describe('minting an mcp token', () => {
	const mint = async (persona: 'superadmin' | 'admin', owner?: string) => {
		const calls = fakeApi(persona, { 'POST /api/mcp/tokens': () => json(201, { id: 9, token: 'tgmcp_test_once' }) });
		renderPage('/mcp-admin/tokens', persona);
		fireEvent.click(screen.getByRole('button', { name: /Mint a token/ }));
		fireEvent.change(dialog().querySelector('input')!, { target: { value: 'agent' } });
		if (owner) fireEvent.change(within(dialog()).getByLabelText(/Owner/), { target: { value: owner } });
		fireEvent.click(within(dialog()).getByRole('button', { name: 'Mint' }));
		await waitFor(() => expect(callsTo(calls, 'POST', '/api/mcp/tokens')).toHaveLength(1));
		return callsTo(calls, 'POST', '/api/mcp/tokens')[0]!.body;
	};

	it('mints for the viewer unless another owner is picked', async () => {
		expect(await mint('superadmin')).toEqual({ name: 'agent' });
	});

	it('sends the owner picked from the owners offered', async () => {
		const p = fixture<McpTokensProps>('/mcp-admin/tokens').props;
		const lead = p.owners.find((o) => o.name === 'Security lead')!;
		expect(await mint('superadmin', String(lead.id))).toEqual({ name: 'agent', owner_id: lead.id });
	});

	it('offers no owner pick to a viewer who may mint only for itself', () => {
		expect(fixture<McpTokensProps>('/mcp-admin/tokens', 'member').props.owners).toHaveLength(1);
		renderPage('/mcp-admin/tokens', 'member');
		fireEvent.click(screen.getByRole('button', { name: /Mint a token/ }));
		expect(within(dialog()).queryByLabelText(/Owner/)).toBeNull();
	});

	it('is refused in the stand-in for an owner the viewer may not mint for, or without a name', () => {
		// An admin manages no other admin.
		expect(adminApi('POST', 'mcp/tokens', { name: 'x', owner_id: 4 }, 'admin')[0]).toBe(400);
		expect(adminApi('POST', 'mcp/tokens', { name: '   ' }, 'superadmin')[0]).toBe(400);
		const [st] = adminApi('POST', 'mcp/tokens', { name: 'for the lead', owner_id: 4 }, 'superadmin');
		expect(st).toBe(201);
		const t = fixture<McpTokensProps>('/mcp-admin/tokens').props.tokens.find((x) => x.name === 'for the lead')!;
		expect([t.owner.name, t.minted_by.name]).toEqual(['Security lead', 'Administrator']);
	});
});

describe('renaming an mcp token', () => {
	const open = (name: string, handlers = {}) => {
		const calls = fakeApi('superadmin', handlers);
		renderPage('/mcp-admin/tokens');
		fireEvent.click(within(rowOf(name)).getByRole('button', { name: /Rename/ }));
		return calls;
	};
	const tokenId = (name: string) => fixture<McpTokensProps>('/mcp-admin/tokens').props.tokens.find((t) => t.name === name)!.id;

	it('sends the new name', async () => {
		const calls = open('laptop', { 'PATCH /api/mcp/tokens/:id': () => undefined });
		const i = dialog().querySelector('input')!;
		expect(i.value).toBe('laptop');
		expect(i.getAttribute('autocomplete')).toBe('off');
		fireEvent.change(i, { target: { value: 'work laptop' } });
		fireEvent.click(within(dialog()).getByRole('button', { name: 'Save' }));
		const path = `/api/mcp/tokens/${tokenId('laptop')}`;
		await waitFor(() => expect(callsTo(calls, 'PATCH', path)).toHaveLength(1));
		expect(callsTo(calls, 'PATCH', path)[0]!.body).toEqual({ name: 'work laptop' });
		await waitFor(() => expect(screen.queryByRole('dialog')).toBeNull());
	});

	it('will not save an empty name, and renames a revoked token too', () => {
		open('old ci job');
		fireEvent.change(dialog().querySelector('input')!, { target: { value: '  ' } });
		expect((within(dialog()).getByRole('button', { name: 'Save' }) as HTMLButtonElement).disabled).toBe(true);
	});

	it('shows the server\'s refusal in the dialog', async () => {
		open('laptop', { 'PATCH /api/mcp/tokens/:id': () => json(404, { error: 'not_found', message: 'No such token.' }) });
		fireEvent.click(within(dialog()).getByRole('button', { name: 'Save' }));
		expect((await within(dialog()).findByRole('alert')).textContent).toBe('No such token.');
	});

	it('renames in the stand-in only what the viewer manages', () => {
		const laptop = tokenId('laptop');
		expect(adminApi('PATCH', `mcp/tokens/${laptop}`, { name: 'mine now' }, 'admin')[0]).toBe(404);
		expect(adminApi('PATCH', `mcp/tokens/${laptop}`, { name: ' ' }, 'superadmin')[0]).toBe(400);
		const agent = tokenId('research-agent');
		expect(adminApi('PATCH', `mcp/tokens/${agent}`, { name: 'research-agent ' + 'x'.repeat(80) }, 'admin')).toEqual([204, null]);
		const t = fixture<McpTokensProps>('/mcp-admin/tokens').props.tokens.find((x) => x.id === agent)!;
		expect(t.name).toHaveLength(64);
		expect(adminApi('PATCH', `mcp/tokens/${agent}`, { name: 'research-agent' }, 'admin')).toEqual([204, null]);
	});
});

describe('transferring an mcp token', () => {
	const tokenOf = (name: string) => fixture<McpTokensProps>('/mcp-admin/tokens').props.tokens.find((t) => t.name === name)!;

	it('gives a superadmin a Transfer action that sends the owner picked', async () => {
		const calls = fakeApi('superadmin', { 'POST /api/mcp/tokens/:id/transfer': () => undefined });
		renderPage('/mcp-admin/tokens');
		const t = tokenOf('laptop');
		fireEvent.click(within(rowOf('laptop')).getByRole('button', { name: /Transfer/ }));
		const sel = within(dialog()).getByLabelText(/New owner/) as HTMLSelectElement;
		// Not to the owner it has.
		expect([...sel.options].map((o) => o.textContent)).not.toContain('Administrator (superadmin)');
		const go = within(dialog()).getByRole('button', { name: 'Transfer' }) as HTMLButtonElement;
		expect(go.disabled).toBe(true);
		const lead = fixture<McpTokensProps>('/mcp-admin/tokens').props.owners.find((o) => o.name === 'Security lead')!;
		fireEvent.change(sel, { target: { value: String(lead.id) } });
		fireEvent.click(go);
		const path = `/api/mcp/tokens/${t.id}/transfer`;
		await waitFor(() => expect(callsTo(calls, 'POST', path)).toHaveLength(1));
		expect(callsTo(calls, 'POST', path)[0]!.body).toEqual({ owner_id: lead.id });
		await waitFor(() => expect(screen.queryByRole('dialog')).toBeNull());
	});

	it('offers no transfer to an admin', () => {
		renderPage('/mcp-admin/tokens', 'admin');
		expect(screen.getAllByRole('button', { name: /Rename/ }).length).toBeGreaterThan(0);
		expect(screen.queryByRole('button', { name: /Transfer/ })).toBeNull();
	});

	it('shows the server\'s refusal in the dialog', async () => {
		fakeApi('superadmin', { 'POST /api/mcp/tokens/:id/transfer': () => json(400, { error: 'bad_request', message: 'This account cannot own a token.' }) });
		renderPage('/mcp-admin/tokens');
		fireEvent.click(within(rowOf('laptop')).getByRole('button', { name: /Transfer/ }));
		const sel = within(dialog()).getByLabelText(/New owner/) as HTMLSelectElement;
		fireEvent.change(sel, { target: { value: sel.options[1]!.value } });
		fireEvent.click(within(dialog()).getByRole('button', { name: 'Transfer' }));
		expect((await within(dialog()).findByRole('alert')).textContent).toBe('This account cannot own a token.');
	});

	it('transfers in the stand-in for superadmins only, keeping who minted it', () => {
		const t = tokenOf('research-agent');
		expect(adminApi('POST', `mcp/tokens/${t.id}/transfer`, { owner_id: 3 }, 'admin')[0]).toBe(404);
		// A disabled account cannot own one.
		expect(adminApi('POST', `mcp/tokens/${t.id}/transfer`, { owner_id: 7 }, 'superadmin')[0]).toBe(400);
		expect(adminApi('POST', `mcp/tokens/${t.id}/transfer`, { owner_id: 4 }, 'superadmin')).toEqual([204, null]);
		const after = tokenOf('research-agent');
		expect([after.owner.name, after.minted_by.name]).toEqual(['Security lead', 'GNU/Weeb admins']);
		// The admin no longer manages it.
		expect(fixture<McpTokensProps>('/mcp-admin/tokens', 'admin').props.tokens.map((x) => x.id)).not.toContain(t.id);
		// Back to the member, which holds mcp_tokens.
		expect(adminApi('POST', `mcp/tokens/${t.id}/transfer`, { owner_id: 5 }, 'superadmin')).toEqual([204, null]);
	});
});

describe('a member holding mcp_tokens', () => {
	it('has the MCP menu, but not the exposed groups', () => {
		expect(fixture('/', 'member').viewer.nav).toContain('mcp');
		expect(fixture('/mcp-admin/groups', 'member').route).toBe('not_found');
		expect(adminApi('POST', 'mcp/groups', { group_id: 1 }, 'member')[0]).toBe(404);
		renderPage('/mcp-admin', 'member');
		expect(screen.getByRole('link', { name: /Access tokens/ })).toBeTruthy();
		expect(screen.queryByRole('link', { name: /Exposed groups/ })).toBeNull();
		expect(screen.getByText('Exposed groups')).toBeTruthy();
	});

	it('sees only its own tokens, and counts only those on the index', () => {
		renderPage('/mcp-admin/tokens', 'member');
		expect(names()).toEqual(['research-agent']);
		expect(fixture<McpIndexProps>('/mcp-admin', 'member').props.active_tokens).toBe(1);
		expect(screen.queryByRole('button', { name: /Transfer/ })).toBeNull();
	});

	it('can be minted for by an admin that manages it', async () => {
		const calls = fakeApi('admin', { 'POST /api/mcp/tokens': () => json(201, { id: 9, token: 'tgmcp_test_once' }) });
		renderPage('/mcp-admin/tokens', 'admin');
		fireEvent.click(screen.getByRole('button', { name: /Mint a token/ }));
		fireEvent.change(dialog().querySelector('input')!, { target: { value: 'notebook' } });
		const sel = within(dialog()).getByLabelText(/Owner/) as HTMLSelectElement;
		expect([...sel.options].map((o) => o.textContent)).toEqual(['GNU/Weeb admins (you)', 'Research team (member)']);
		fireEvent.change(sel, { target: { value: '5' } });
		fireEvent.click(within(dialog()).getByRole('button', { name: 'Mint' }));
		await waitFor(() => expect(callsTo(calls, 'POST', '/api/mcp/tokens')).toHaveLength(1));
		expect(callsTo(calls, 'POST', '/api/mcp/tokens')[0]!.body).toEqual({ name: 'notebook', owner_id: 5 });
	});

	it('shows the ability on the accounts page', () => {
		const r = fixture<AccountsProps>('/settings/accounts').props.accounts.find((a) => a.id === 5)!;
		expect(r.abilities).toContain('mcp_tokens');
		renderPage('/settings/accounts');
		expect(screen.getAllByText(/forwarding, fetch files, MCP tokens/).length).toBeGreaterThan(0);
		fireEvent.click(screen.getByRole('button', { name: 'Edit researcher' }));
		fireEvent.click(within(dialog()).getByRole('tab', { name: 'Access' }));
		expect((within(dialog()).getByRole('checkbox', { name: /MCP tokens/ }) as HTMLInputElement).checked).toBe(true);
		expect(adminApi('PATCH', 'accounts/5', { abilities: ['mcp_tokens', 'forward', 'fetch_files'] }, 'superadmin')[0]).toBe(200);
		expect(adminApi('PATCH', 'accounts/5', { abilities: ['mcp'] }, 'superadmin')[0]).toBe(400);
	});
});

describe('mcp token scopes in the stand-in', () => {
	const tokens = (persona: 'superadmin' | 'admin' = 'superadmin') => fixture<McpTokensProps>('/mcp-admin/tokens', persona).props.tokens;
	const tok = (name: string) => tokens().find((t) => t.name === name)!;
	const refs = (name: string) => tok(name).scope.map(({ kind, id }) => ({ kind, id }));
	const opts = (persona: 'superadmin' | 'admin' | 'member') => fixture<McpTokensProps>('/mcp-admin/tokens', persona).props.scope_options;

	it('sends each token\'s scope in order, with its inactive entries', () => {
		expect(tok('old ci job').scope).toEqual([]);
		const s = tok('research-agent').scope;
		expect(s.map((e) => e.kind)).toEqual(['federation', 'group', 'group']);
		expect(s[1]!.id).toBeLessThan(s[2]!.id);
		// One of its groups is not exposed.
		expect(s.filter((e) => e.kind === 'group').map((e) => e.active).sort()).toEqual([false, true]);
		expect(tok('laptop').scope.map((e) => [e.kind, e.active])).toEqual([['private', true]]);
		// A deleted federation, a group hidden from its admin owner, and a
		// private chat on an admin's token.
		expect(tok('').scope.map((e) => [e.kind, e.name === '', e.active])).toEqual([['federation', true, false], ['group', false, false], ['private', false, false]]);
	});

	it('offers the exposed groups the viewer sees, and private chats to a superadmin only', () => {
		expect(opts('superadmin').private).toBe(true);
		expect(opts('admin').private).toBe(false);
		expect(opts('member').private).toBe(false);
		expect(opts('superadmin').federations.map((f) => f.name)).toContain('Default');
		const exposed = fixture<{ groups: { group: { id: number } }[] }>('/mcp-admin/groups').props.groups.map((g) => g.group.id);
		expect(opts('superadmin').groups.map((g) => g.id).sort()).toEqual([...exposed].sort());
		expect(opts('member').groups.length).toBeLessThan(exposed.length);
	});

	it('writes a private chat for a superadmin on a superadmin\'s token only, with the reason', () => {
		const user = tok('laptop').scope[0]!.id;
		const agent = tok('research-agent');
		const before = refs('research-agent');
		const [st, body] = adminApi('PATCH', `mcp/tokens/${agent.id}`, { scope: [{ kind: 'private', id: user }] }, 'admin');
		expect([st, (body as { error: string }).error]).toEqual([403, 'forbidden']);
		expect((body as { message: string }).message).toMatch(/superadmin/);
		// The research team owns it: not even a superadmin may.
		expect(adminApi('PATCH', `mcp/tokens/${agent.id}`, { scope: [{ kind: 'private', id: user }] }, 'superadmin')[0]).toBe(403);
		expect(adminApi('POST', 'mcp/tokens', { name: 'x', scope: [{ kind: 'private', id: user }] }, 'admin')[0]).toBe(403);
		expect(adminApi('PATCH', `mcp/tokens/${agent.id}`, { scope: [{ kind: 'nope', id: 1 }] }, 'superadmin')[0]).toBe(400);
		expect(adminApi('PATCH', `mcp/tokens/${agent.id}`, { scope: Array.from({ length: 201 }, () => ({ kind: 'federation', id: 1 })) }, 'superadmin')[0]).toBe(400);
		expect(refs('research-agent')).toEqual(before);
		const laptop = tok('laptop');
		expect(adminApi('PATCH', `mcp/tokens/${laptop.id}`, { scope: [{ kind: 'private', id: user }, { kind: 'federation', id: 0 }, { kind: 'private', id: user }] }, 'superadmin')).toEqual([204, null]);
		expect(refs('laptop')).toEqual([{ kind: 'federation', id: 0 }, { kind: 'private', id: user }]);
		expect(tok('laptop').name).toBe('laptop');
		// [] reads every exposed group again; then back as it was.
		expect(adminApi('PATCH', `mcp/tokens/${laptop.id}`, { scope: [] }, 'superadmin')).toEqual([204, null]);
		expect(tok('laptop').scope).toEqual([]);
		expect(adminApi('PATCH', `mcp/tokens/${laptop.id}`, { scope: [{ kind: 'private', id: user }] }, 'superadmin')).toEqual([204, null]);
	});
});

describe('mcp token scopes in the list', () => {
	const scopeCell = (name: string) => rowOf(name).querySelectorAll('td')[2] as HTMLElement;
	const unnamed = () => screen.getByText('(unnamed)').closest('tr')!.querySelectorAll('td')[2] as HTMLElement;

	it('says All exposed groups for a token without a scope', () => {
		renderPage('/mcp-admin/tokens');
		expect(scopeCell('old ci job').textContent).toBe('All exposed groups');
		expect(screen.getByRole('columnheader', { name: 'Reads' })).toBeTruthy();
	});

	it('lists federations, groups and private chats, marking the inactive ones and why', () => {
		const p = fixture<McpTokensProps>('/mcp-admin/tokens').props;
		const s = p.tokens.find((t) => t.name === 'research-agent')!.scope;
		renderPage('/mcp-admin/tokens');
		const cell = scopeCell('research-agent');
		const pills = [...cell.querySelectorAll('.a-tokscope-e')];
		expect(pills.map((x) => x.textContent!.replace('inactive', ''))).toEqual(s.map((e) => e.name));
		const off = cell.querySelector('.a-tokscope-e.off')!;
		expect(off.textContent).toMatch(/inactive$/);
		expect(off.getAttribute('title')).toMatch(/not exposed to MCP/);
		expect(within(cell).getByLabelText('Federation')).toBeTruthy();
		expect(within(scopeCell('laptop')).getByLabelText('Private chat')).toBeTruthy();
		expect(cell.textContent).not.toMatch(/reads nothing/);
	});

	it('warns on a token whose scope is all inactive, and says why each entry is', () => {
		renderPage('/mcp-admin/tokens');
		const cell = unnamed();
		expect(within(cell).getByRole('note').textContent).toBe('This token reads nothing: none of its scope is active');
		const [fed, group, chat] = [...cell.querySelectorAll('.a-tokscope-e')];
		expect(fed!.textContent).toMatch(/^Deleted federation/);
		expect(fed!.getAttribute('title')).toMatch(/deleted/);
		expect(group!.getAttribute('title')).toMatch(/outside what the token's owner can see/);
		expect(chat!.getAttribute('title')).toMatch(/superadmin owns/);
	});

	it('collapses a long scope behind a toggle', () => {
		const page = fixture<McpTokensProps>('/mcp-admin/tokens');
		const scope = [1, 2, 3, 4, 5].map((i) => ({ kind: 'group' as const, id: -i, name: `g${i}`, active: true, inactive_reason: null }));
		const props = { ...page.props, tokens: page.props.tokens.map((t) => (t.name === 'laptop' ? { ...t, scope } : t)) };
		renderPage('/mcp-admin/tokens', 'superadmin', { props } as never);
		const cell = scopeCell('laptop');
		expect(cell.querySelectorAll('.a-tokscope-e')).toHaveLength(2);
		const more = within(cell).getByRole('button', { name: '+3 more' });
		fireEvent.click(more);
		expect(cell.querySelectorAll('.a-tokscope-e')).toHaveLength(5);
		fireEvent.click(within(cell).getByRole('button', { name: 'Show less' }));
		expect(cell.querySelectorAll('.a-tokscope-e')).toHaveLength(2);
	});
});

describe('the mcp token scope form', () => {
	const opts = () => fixture<McpTokensProps>('/mcp-admin/tokens').props.scope_options;
	const tokenOf = (name: string, persona: 'superadmin' | 'admin' = 'superadmin') => fixture<McpTokensProps>('/mcp-admin/tokens', persona).props.tokens.find((t) => t.name === name)!;
	const addFrom = (label: RegExp, name: string) => {
		const input = within(dialog()).getByLabelText(label);
		fireEvent.focus(input);
		fireEvent.click(within(dialog()).getByRole('button', { name: `Add ${name}` }));
	};
	const scopeList = () => within(dialog()).getByRole('group', { name: 'Scope' });
	const openScope = (name: string, persona: 'superadmin' | 'admin' = 'superadmin', handlers = {}) => {
		const calls = fakeApi(persona, handlers);
		renderPage('/mcp-admin/tokens', persona);
		const row = name ? rowOf(name) : screen.getByText('(unnamed)').closest('tr') as HTMLElement;
		fireEvent.click(within(row).getByRole('button', { name: /Scope/ }));
		return calls;
	};
	const save = () => fireEvent.click(within(dialog()).getByRole('button', { name: 'Save' }));

	it('mints a token scoped to a federation, a group and a private chat', async () => {
		const calls = fakeApi('superadmin', { 'POST /api/mcp/tokens': () => json(201, { id: 9, token: 'tgmcp_test_once' }) });
		renderPage('/mcp-admin/tokens');
		fireEvent.click(screen.getByRole('button', { name: /Mint a token/ }));
		fireEvent.change(dialog().querySelector('input')!, { target: { value: 'agent' } });
		expect(scopeList().textContent).toMatch(/All exposed groups/);
		const o = opts();
		const g = o.groups[0]!, f = o.federations.find((x) => x.id === 1)!;
		addFrom(/Add a group/, g.name);
		addFrom(/Add a federation/, f.name);
		const uid = within(dialog()).getByPlaceholderText('User id');
		fireEvent.change(uid, { target: { value: 'abc' } });
		expect(within(dialog()).getByText('A user id is a positive number.')).toBeTruthy();
		fireEvent.change(uid, { target: { value: '424242' } });
		fireEvent.click(within(dialog()).getByRole('button', { name: 'Add the private chat' }));
		expect(within(scopeList()).getByText('User 424242')).toBeTruthy();
		// Federations first, then groups, then private chats.
		fireEvent.click(within(dialog()).getByRole('button', { name: 'Mint' }));
		await waitFor(() => expect(callsTo(calls, 'POST', '/api/mcp/tokens')).toHaveLength(1));
		expect(callsTo(calls, 'POST', '/api/mcp/tokens')[0]!.body).toEqual({ name: 'agent', scope: [{ kind: 'federation', id: 1 }, { kind: 'group', id: g.id }, { kind: 'private', id: 424242 }] });
	});

	it('offers private chats to a superadmin only, and only for a superadmin\'s token', () => {
		fakeApi('admin');
		renderPage('/mcp-admin/tokens', 'admin');
		fireEvent.click(screen.getByRole('button', { name: /Mint a token/ }));
		expect(within(dialog()).queryByLabelText(/Add a private chat/)).toBeNull();
		expect(within(dialog()).getByLabelText(/Add a group/)).toBeTruthy();
		cleanupDialog();
		fakeApi('superadmin');
		renderPage('/mcp-admin/tokens');
		fireEvent.click(screen.getByRole('button', { name: /Mint a token/ }));
		expect(within(dialog()).getByPlaceholderText('User id')).toBeTruthy();
		const lead = fixture<McpTokensProps>('/mcp-admin/tokens').props.owners.find((x) => x.name === 'Security lead')!;
		fireEvent.change(within(dialog()).getByLabelText(/Owner/), { target: { value: String(lead.id) } });
		expect(within(dialog()).queryByPlaceholderText('User id')).toBeNull();
	});

	it('removes an entry and sends the rest, or nothing when unchanged', async () => {
		const t = tokenOf('research-agent');
		const path = `/api/mcp/tokens/${t.id}`;
		let calls = openScope('research-agent', 'superadmin', { 'PATCH /api/mcp/tokens/:id': () => undefined });
		save();
		await waitFor(() => expect(screen.queryByRole('dialog')).toBeNull());
		expect(callsTo(calls, 'PATCH', path)).toHaveLength(0);
		cleanupDialog();
		calls = openScope('research-agent', 'superadmin', { 'PATCH /api/mcp/tokens/:id': () => undefined });
		// The inactive group is listed, marked, and removable.
		const off = t.scope.find((e) => !e.active)!;
		expect(within(scopeList()).getByText('inactive')).toBeTruthy();
		fireEvent.click(within(scopeList()).getByRole('button', { name: `Remove ${off.name}` }));
		save();
		await waitFor(() => expect(callsTo(calls, 'PATCH', path)).toHaveLength(1));
		expect(callsTo(calls, 'PATCH', path)[0]!.body).toEqual({ scope: t.scope.filter((e) => e !== off).map(({ kind, id }) => ({ kind, id })) });
	});

	it('sends an empty scope to read every exposed group again', async () => {
		const t = tokenOf('laptop');
		const calls = openScope('laptop', 'superadmin', { 'PATCH /api/mcp/tokens/:id': () => undefined });
		fireEvent.click(within(scopeList()).getByRole('button', { name: /^Remove / }));
		expect(scopeList().textContent).toMatch(/All exposed groups/);
		save();
		await waitFor(() => expect(callsTo(calls, 'PATCH', `/api/mcp/tokens/${t.id}`)).toHaveLength(1));
		expect(callsTo(calls, 'PATCH', `/api/mcp/tokens/${t.id}`)[0]!.body).toEqual({ scope: [] });
	});

	it('tells an admin a private chat blocks the save, and offers to remove it', async () => {
		const t = tokenOf('', 'admin');
		const calls = openScope('', 'admin', { 'PATCH /api/mcp/tokens/:id': () => undefined });
		// Its group is hidden from this admin: the note claims no more.
		expect(within(scopeList()).getByRole('note').textContent).toBe('None of the entries you can see is active; the others are hidden from you');
		expect(within(dialog()).getByText(/Only a superadmin can give a token a private chat/)).toBeTruthy();
		const go = within(dialog()).getByRole('button', { name: 'Save' }) as HTMLButtonElement;
		expect(go.disabled).toBe(true);
		fireEvent.click(within(dialog()).getByRole('button', { name: 'Remove the private chat' }));
		expect(go.disabled).toBe(false);
		expect(within(dialog()).queryByText(/Only a superadmin/)).toBeNull();
		save();
		await waitFor(() => expect(callsTo(calls, 'PATCH', `/api/mcp/tokens/${t.id}`)).toHaveLength(1));
		const hidden = t.scope.find((e) => e.kind === 'group')!;
		expect(callsTo(calls, 'PATCH', `/api/mcp/tokens/${t.id}`)[0]!.body).toEqual({ scope: [{ kind: 'federation', id: 9 }, { kind: 'group', id: hidden.id }] });
	});

	it('shows the server\'s 403 reason in the dialog', async () => {
		openScope('laptop', 'superadmin', { 'PATCH /api/mcp/tokens/:id': () => json(403, { error: 'forbidden', message: 'A private chat can be given only to a token a superadmin owns.' }) });
		addFrom(/Add a federation/, 'Default');
		save();
		expect((await within(dialog()).findByRole('alert')).textContent).toBe('A private chat can be given only to a token a superadmin owns.');
	});
});

// Until the Go service serves /mcp, production reads every exposed
// group whatever a token's scope says; the page says so.
describe('mcp token scope notice', () => {
	const NOTICE = /Scopes take effect once MCP is served by the new web service; until then every token reads all exposed groups\./;
	beforeEach(() => setup());

	it('is shown above the token list and in the scope editor', () => {
		fakeApi('superadmin');
		renderPage('/mcp-admin/tokens');
		expect(screen.getAllByText(NOTICE)).toHaveLength(1);
		fireEvent.click(screen.getByRole('button', { name: /Mint a token/ }));
		const dlg = screen.getByRole('dialog');
		expect(within(dlg).getByText(NOTICE)).toBeTruthy();
	});
});

// Why an entry is inactive comes from the server (inactive_reason),
// one hover text per reason.
describe('mcp token scope: why an entry is inactive', () => {
	beforeEach(() => setup());
	const owner = { id: 1, name: 'Administrator', role: 'superadmin' as const, active: true };
	const off = (kind: 'federation' | 'group' | 'private', id: number, inactive_reason: NonNullable<ScopeEntry['inactive_reason']>): ScopeEntry => ({ kind, id, name: 'x', active: false, inactive_reason });

	it.each([
		[off('group', -1, 'not_exposed'), /not exposed to MCP/],
		[off('group', -2, 'outside_owner_grants'), /outside what the token's owner can see/],
		[off('federation', 9, 'federation_deleted'), /federation was deleted/],
		[off('federation', 1, 'outside_owner_grants'), /outside what the token's owner can see/],
		[off('private', 7, 'private_not_superadmin'), /superadmin owns/],
		[off('private', 8, 'user_unknown'), /knows no such user/],
	])('says %#', (e, text) => {
		render(<ScopeChip e={e} owner={owner} />);
		expect(document.querySelector('.a-tokscope-e.off')!.getAttribute('title')).toMatch(text);
	});

	it('follows the server on a group outside a member owner\'s grants, not the client\'s guess', () => {
		// An exposed group (so not "not exposed") the owner is not granted.
		render(<ScopeChip e={off('group', -3, 'outside_owner_grants')} owner={{ id: 5, name: 'Research team', role: 'member', active: true }} />);
		expect(document.querySelector('.a-tokscope-e.off')!.getAttribute('title')).not.toMatch(/not exposed/);
	});

	it('marks a group hidden from an admin owner as outside what the owner can see', () => {
		const p = fixture<McpTokensProps>('/mcp-admin/tokens').props;
		const t = p.tokens.find((x) => x.owner.id === 3)!;
		const g = t.scope.find((e) => e.kind === 'group')!;
		expect(g.active).toBe(false);
		// Exposed or not decides which reason comes first; never active.
		expect(['not_exposed', 'outside_owner_grants']).toContain(g.inactive_reason);
		render(<ScopeChip e={{ ...g, inactive_reason: 'outside_owner_grants' }} owner={{ id: 3, name: 'GNU/Weeb admins', role: 'admin', active: true }} />);
		expect(document.querySelector('.a-tokscope-e.off')!.getAttribute('title')).toMatch(/outside what the token's owner can see/);
	});

	it('is computed by the stand-in in the server\'s order', () => {
		const p = fixture<McpTokensProps>('/mcp-admin/tokens').props;
		for (const t of p.tokens) for (const e of t.scope) expect(e.active).toBe(e.inactive_reason === null);
		const reasons = p.tokens.flatMap((t) => t.scope.map((e) => e.inactive_reason)).filter(Boolean);
		expect(reasons).toEqual(expect.arrayContaining(['not_exposed', 'federation_deleted', 'private_not_superadmin']));
	});
});

describe('mcp token scope in the stand-in', () => {
	it('writes a scope alone to a token that has no name', () => {
		const t = fixture<McpTokensProps>('/mcp-admin/tokens').props.tokens.find((x) => x.name === '')!;
		// Its writable entries: not the deleted federation, nor the private
		// chat an admin's token may not hold.
		const refs = t.scope.filter((e) => e.kind === 'group').map(({ kind, id }) => ({ kind, id }));
		expect(adminApi('PATCH', `mcp/tokens/${t.id}`, { scope: refs }, 'superadmin')).toEqual([204, null]);
		expect(adminApi('PATCH', `mcp/tokens/${t.id}`, { name: ' ' }, 'superadmin')[0]).toBe(400);
	});
});

// A group hidden from an admin keeps its place in that admin's own
// token: shown without its title, and kept when the scope is saved;
// a hidden group cannot be added.
describe('mcp token scope: a group hidden from the viewer', () => {
	beforeEach(() => setup());
	const mine = () => fixture<McpTokensProps>('/mcp-admin/tokens', 'admin').props.tokens.find((t) => t.owner.id === 3)!;

	it('comes without its title, and reads "A group hidden from you"', () => {
		const g = mine().scope.find((e) => e.kind === 'group')!;
		expect(g.name).toBe('');
		const superView = fixture<McpTokensProps>('/mcp-admin/tokens').props.tokens.find((t) => t.owner.id === 3)!.scope.find((e) => e.kind === 'group')!;
		expect(superView.name).not.toBe('');
		fakeApi('admin');
		renderPage('/mcp-admin/tokens', 'admin');
		expect(screen.getAllByText('A group hidden from you').length).toBeGreaterThan(0);
		expect(document.body.textContent).not.toContain(superView.name);
	});

	it('tells a viewer who cannot see it nothing, whatever its state', () => {
		const id = mine().scope.find((e) => e.kind === 'group')!.id;
		const entry = (who: 'admin' | 'superadmin') => fixture<McpTokensProps>('/mcp-admin/tokens', who).props.tokens.find((t) => t.owner.id === 3)!.scope.find((e) => e.id === id)! as ScopeEntry;
		const blind = { name: '', active: null, inactive_reason: 'hidden_from_viewer' };
		expect(entry('admin')).toMatchObject(blind);
		expect(entry('superadmin')).toMatchObject({ active: false, inactive_reason: 'outside_owner_grants' });
		expect(adminApi('DELETE', `mcp/groups/${id}`, {}, 'superadmin')[0]).toBe(204);
		try {
			expect(entry('admin')).toMatchObject(blind);
			expect(entry('superadmin')).toMatchObject({ active: false, inactive_reason: 'not_exposed' });
		} finally {
			expect(adminApi('POST', 'mcp/groups', { group_id: id }, 'superadmin')[0]).toBe(201);
		}
	});

	it('is drawn neither active nor inactive', () => {
		render(<ScopeChip e={{ kind: 'group', id: -9, name: '', active: null, inactive_reason: 'hidden_from_viewer' }} owner={{ id: 3, name: 'GNU/Weeb admins', role: 'admin', active: true }} />);
		const chip = document.querySelector('.a-tokscope-e')!;
		expect(chip.textContent).toBe('A group hidden from you');
		expect(chip.classList.contains('off')).toBe(false);
		expect(chip.querySelector('.a-tokscope-flag')).toBeNull();
		expect(chip.getAttribute('title')).toMatch(/cannot say whether/);
	});

	it('hides a federation whose groups the viewer sees none of, exposed or not', () => {
		// Security holds the group hidden from this admin and one it sees;
		// hide that one too for the test, so the admin sees none of it,
		// and have a superadmin put it on the member's token.
		const sec = federations.find((f) => f.id === 3)!;
		const restrict = accounts.find((a) => a.id === 3)!.restrict!;
		const before = [...restrict.hide_groups];
		restrict.hide_groups = [...new Set([...before, ...sec.groupIds])];
		const tok = fixture<McpTokensProps>('/mcp-admin/tokens').props.tokens.find((t) => t.name === 'research-agent')!;
		const refs = tok.scope.map(({ kind, id }) => ({ kind, id }));
		expect(adminApi('PATCH', `mcp/tokens/${tok.id}`, { scope: [...refs, { kind: 'federation', id: 3 }] }, 'superadmin')[0]).toBe(204);
		const fed = (who: 'admin' | 'superadmin') => fixture<McpTokensProps>('/mcp-admin/tokens', who).props.tokens.find((t) => t.id === tok.id)!.scope.find((e) => e.kind === 'federation' && e.id === 3)!;
		const blind = { name: '', active: null, inactive_reason: 'hidden_from_viewer' };
		const hidden = mine().scope.find((e) => e.kind === 'group')!.id;
		try {
			expect(fed('admin')).toMatchObject(blind);
			expect(fed('superadmin').name).toBe('Security');
			// Withdrawing one of its groups tells the admin nothing new.
			expect(adminApi('DELETE', `mcp/groups/${hidden}`, {}, 'superadmin')[0]).toBe(204);
			expect(fed('admin')).toMatchObject(blind);
			expect(adminApi('POST', 'mcp/groups', { group_id: hidden }, 'superadmin')[0]).toBe(201);
		} finally {
			restrict.hide_groups = before;
			expect(adminApi('PATCH', `mcp/tokens/${tok.id}`, { scope: refs }, 'superadmin')[0]).toBe(204);
		}
	});

	it('counts as Default\'s groups those in no other federation', () => {
		const tok = fixture<McpTokensProps>('/mcp-admin/tokens').props.tokens.find((t) => t.name === 'research-agent')!;
		const refs = tok.scope.map(({ kind, id }) => ({ kind, id }));
		expect(adminApi('PATCH', `mcp/tokens/${tok.id}`, { scope: [{ kind: 'federation', id: 0 }, ...refs] }, 'superadmin')[0]).toBe(204);
		const dflt = () => fixture<McpTokensProps>('/mcp-admin/tokens', 'admin').props.tokens.find((t) => t.id === tok.id)!.scope.find((e) => e.kind === 'federation' && e.id === 0)!;
		const restrict = accounts.find((a) => a.id === 3)!.restrict!;
		const before = [...restrict.hide_groups];
		try {
			// The admin sees some of the groups in no other federation.
			expect(dflt().inactive_reason).not.toBe('hidden_from_viewer');
			expect(dflt().name).toBe('Default');
			// Hidden all of those, Default tells it nothing; the groups of
			// the other federations do not count for it.
			const own = fedGroups({ id: 0 }).map((g: { id: number }) => g.id);
			restrict.hide_groups = [...new Set([...before, ...own])];
			expect(dflt()).toMatchObject({ name: '', active: null, inactive_reason: 'hidden_from_viewer' });
		} finally {
			restrict.hide_groups = before;
			expect(adminApi('PATCH', `mcp/tokens/${tok.id}`, { scope: refs }, 'superadmin')[0]).toBe(204);
		}
	});

	it('names a hidden federation as such, not as deleted', () => {
		render(<ScopeChip e={{ kind: 'federation', id: 3, name: '', active: null, inactive_reason: 'hidden_from_viewer' }} owner={{ id: 3, name: 'GNU/Weeb admins', role: 'admin', active: true }} />);
		expect(document.querySelector('.a-tokscope-e')!.textContent).toBe('A federation hidden from you');
	});

	it('warns that a token reads nothing only when the page can know it', () => {
		const e = (active: boolean | null): ScopeEntry => ({ kind: 'group', id: -1, name: 'g', active, inactive_reason: active === null ? 'hidden_from_viewer' : active ? null : 'not_exposed' });
		expect(readsNothing([])).toBeNull();
		expect(readsNothing([e(false), e(false)])).toBe('nothing');
		expect(readsNothing([e(false), e(null)])).toBe('unknown');
		expect(readsNothing([e(null)])).toBe('unknown');
		expect(readsNothing([e(null), e(true)])).toBeNull();
	});

	it('is kept by the stand-in when the token already holds it, and refused when new', () => {
		const t = mine();
		const hidden = t.scope.filter((e) => e.kind === 'group').map(({ kind, id }) => ({ kind, id }))[0]!;
		// Last in this file: it rewrites the sample token's scope.
		// The sample token has no name: a scope alone needs none.
		expect(adminApi('PATCH', `mcp/tokens/${t.id}`, { scope: [hidden] }, 'admin')).toEqual([204, null]);
		const other = fixture<McpTokensProps>('/mcp-admin/tokens', 'admin').props.tokens.find((x) => x.id !== t.id && !x.revoked)!;
		const [st, body] = adminApi('PATCH', `mcp/tokens/${other.id}`, { scope: [...other.scope.map(({ kind, id }) => ({ kind, id })), hidden] }, 'admin');
		expect(st).toBe(400);
		expect(body).toMatchObject({ error: 'bad_request' });
	});
});

// A federation hidden from the viewer (it sees none of its groups) is
// not offered, cannot be added, and is kept when a token holds it.
describe('mcp token scope: a federation hidden from the viewer', () => {
	beforeEach(() => setup());
	const sec = () => federations.find((f) => f.id === 3)!;
	const hideSecurity = () => {
		const restrict = accounts.find((a) => a.id === 3)!.restrict!;
		const before = [...restrict.hide_groups];
		restrict.hide_groups = [...new Set([...before, ...sec().groupIds])];
		return () => { restrict.hide_groups = before; };
	};
	const offered = (who: 'admin' | 'superadmin') => fixture<McpTokensProps>('/mcp-admin/tokens', who).props.scope_options.federations.map((f) => f.id);

	it('is not offered to that viewer, and is to one who sees it', () => {
		expect(offered('admin')).toContain(3);
		const undo = hideSecurity();
		try {
			expect(offered('admin')).not.toContain(3);
			expect(offered('superadmin')).toContain(3);
		} finally { undo(); }
	});

	it('cannot be added by that viewer, and is kept when the token holds it', () => {
		const tok = fixture<McpTokensProps>('/mcp-admin/tokens').props.tokens.find((t) => t.name === 'research-agent')!;
		const refs = tok.scope.map(({ kind, id }) => ({ kind, id }));
		const undo = hideSecurity();
		try {
			const add = [...refs, { kind: 'federation', id: 3 }];
			expect(adminApi('PATCH', `mcp/tokens/${tok.id}`, { scope: add }, 'admin')).toEqual([400, expect.objectContaining({ error: 'bad_request' })]);
			// Put there by a superadmin, it goes back with an admin's save.
			expect(adminApi('PATCH', `mcp/tokens/${tok.id}`, { scope: add }, 'superadmin')[0]).toBe(204);
			expect(adminApi('PATCH', `mcp/tokens/${tok.id}`, { scope: add }, 'admin')[0]).toBe(204);
		} finally {
			undo();
			expect(adminApi('PATCH', `mcp/tokens/${tok.id}`, { scope: refs }, 'superadmin')[0]).toBe(204);
		}
	});
});
