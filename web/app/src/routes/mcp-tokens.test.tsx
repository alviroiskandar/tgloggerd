// @vitest-environment happy-dom
// The MCP tokens page per role: who owns each token and who minted it,
// minting for another owner, renaming, and (superadmins) transferring.
import { beforeEach, describe, expect, it } from 'vitest';
import { cleanup, fireEvent, screen, waitFor, within } from '@testing-library/react';
import { callsTo, fakeApi, fixture, json, renderPage, setup } from '../test/page.tsx';
import type { McpIndexProps, McpTokensProps } from './mcp.tsx';
import type { AccountsProps } from './accounts.tsx';
import { adminApi } from '../../standin/fixtures.ts';

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
		// A deleted federation, and a private chat on an admin's token.
		expect(tok('').scope.map((e) => [e.kind, e.name === '', e.active])).toEqual([['federation', true, false], ['private', false, false]]);
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
		const [fed, chat] = [...cell.querySelectorAll('.a-tokscope-e')];
		expect(fed!.textContent).toMatch(/^Deleted federation/);
		expect(fed!.getAttribute('title')).toMatch(/deleted/);
		expect(chat!.getAttribute('title')).toMatch(/superadmin owns/);
	});

	it('collapses a long scope behind a toggle', () => {
		const page = fixture<McpTokensProps>('/mcp-admin/tokens');
		const scope = [1, 2, 3, 4, 5].map((i) => ({ kind: 'group' as const, id: -i, name: `g${i}`, active: true }));
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
		expect(within(scopeList()).getByRole('note').textContent).toMatch(/reads nothing/);
		expect(within(dialog()).getByText(/Only a superadmin can give a token a private chat/)).toBeTruthy();
		const go = within(dialog()).getByRole('button', { name: 'Save' }) as HTMLButtonElement;
		expect(go.disabled).toBe(true);
		fireEvent.click(within(dialog()).getByRole('button', { name: 'Remove the private chat' }));
		expect(go.disabled).toBe(false);
		expect(within(dialog()).queryByText(/Only a superadmin/)).toBeNull();
		save();
		await waitFor(() => expect(callsTo(calls, 'PATCH', `/api/mcp/tokens/${t.id}`)).toHaveLength(1));
		expect(callsTo(calls, 'PATCH', `/api/mcp/tokens/${t.id}`)[0]!.body).toEqual({ scope: [{ kind: 'federation', id: 9 }] });
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
