// Administration flows that need the real stand-in behind them.
import { test, expect } from './fixtures.ts';
import { featuredUser } from '../../mockup/src/data/users.js';

test('an admin creates a member, granted a federation', async ({ page, as }) => {
	await as('admin');
	await page.goto('/settings/accounts');
	await page.click('button:has-text("New member")');
	const d = page.getByRole('dialog');
	const fields = d.locator('input:not([type=checkbox]):not([type=radio])');
	await fields.nth(0).fill('e2emember');
	await fields.nth(1).fill('E2E member');
	await d.locator('input[type=email]').fill('e2e@example.org');
	await expect(d.getByRole('button', { name: 'Create account' })).toBeDisabled();
	await d.locator('.a-fedgrant input.a-switch').first().check();
	await d.getByRole('button', { name: 'Create account' }).click();
	// The temporary password, shown once.
	await expect(page.getByLabel('Temporary password')).toHaveText(/^[A-Za-z0-9_-]{12,}$/);
	await page.getByRole('button', { name: 'Done' }).click();
	await expect(page.getByLabel('Temporary password')).toHaveCount(0);
	await expect(page.locator('tr:has-text("e2e@example.org")')).toContainText('invited');
});

test('a webhook is added for a chat found by search, and a bad URL is refused', async ({ page }) => {
	await page.goto('/platform-fwd/telegram-discord');
	const rows = await page.locator('tbody tr').count();
	await page.click('button:has-text("Add webhook")');
	const d = page.getByRole('dialog');
	await d.getByRole('button', { name: 'Choose a chat' }).click();
	await d.locator('.a-picker-search input').fill('rust');
	await d.locator('.a-picker-pop').getByRole('button', { name: /Rust Programming/ }).click();
	await d.locator('#webhook-url').fill('https://evil.example/hook');
	await d.getByRole('button', { name: 'Save' }).click();
	await expect(d.getByRole('alert')).toHaveText('That is not a Discord webhook URL.');
	await d.locator('#webhook-url').fill('https://discord.com/api/webhooks/1/abc');
	await d.getByRole('button', { name: 'Save' }).click();
	await expect(page.locator('tbody tr')).toHaveCount(rows + 1);
});

test('a group is pinned to the sidebar from its page', async ({ page }) => {
	await page.goto('/groups/-1001236918171');
	const pinned = page.locator('.a-navgroup:has(.a-navlabel:has-text("Pinned")) .a-navitem');
	const n = await pinned.count();
	await page.click('.a-head-actions button:has-text("Pin")');
	await expect(pinned).toHaveCount(n + 1);
	await page.click('.a-head-actions button:has-text("Unpin")');
	await expect(pinned).toHaveCount(n);
});

test('a bot is added, and its status follows what discordd reports', async ({ page, emit }) => {
	await page.goto('/platform-fwd/bots');
	await expect(page.locator('.a-livestate.is-live')).toBeVisible();
	await page.click('button:has-text("Add bot")');
	const d = page.getByRole('dialog');
	await d.locator('#bot-label').fill('E2E reader');
	// Not a password field (password managers fill those with the site
	// login), but masked all the same.
	const token = d.locator('#bot-token');
	await expect(token).toHaveAttribute('type', 'text');
	expect(await token.evaluate((e) => getComputedStyle(e).getPropertyValue('-webkit-text-security'))).toBe('disc');
	await token.fill('MTIzNDU2Nzg5MDEyMzQ1Njc5.GaBcDe.' + 'w'.repeat(27));
	await d.getByRole('button', { name: 'Add bot' }).click();
	const row = page.locator('tr', { hasText: 'E2E reader' });
	await expect(row).toContainText('connecting');
	expect(await page.content()).not.toContain('w'.repeat(27));
	const id = Number(await page.evaluate(async () => (await (await fetch('/api/bots')).json()).bots.find((b: { name: string }) => b.name === 'E2E reader').id));
	await emit({ kind: 'bot.updated', bot_id: id, status: 'intent_missing' });
	await expect(row).toContainText('intent missing');
	await expect(row).toContainText('Message Content Intent');
});

test('an exposed group\'s note is edited in place', async ({ page }) => {
	await page.goto('/mcp-admin/groups');
	const row = page.locator('tbody tr').first();
	const added = await row.locator('td[title]').textContent();
	await row.getByRole('button', { name: 'Edit note' }).click();
	const d = page.getByRole('dialog');
	await d.getByLabel(/Note/).fill('Reviewed by the e2e run');
	await d.getByRole('button', { name: 'Save' }).click();
	await expect(d).toHaveCount(0);
	await expect(row).toContainText('Reviewed by the e2e run');
	await expect(row.locator('td[title]')).toHaveText(added!);
});

test('an MCP token is minted for a member, renamed and transferred', async ({ page }) => {
	await page.goto('/mcp-admin/tokens');
	await page.getByRole('button', { name: /Mint a token/ }).click();
	let d = page.getByRole('dialog');
	await d.getByRole('textbox', { name: /^Name/ }).fill('e2e agent');
	await d.getByLabel(/Owner/).selectOption({ label: 'Research team (member)' });
	await d.getByRole('button', { name: 'Mint' }).click();
	await expect(page.getByText('Copy this token now')).toBeVisible();
	const row = page.locator('tbody tr', { hasText: 'e2e agent' });
	await expect(row).toContainText('Research team');
	await expect(row).toContainText('minted by Administrator');
	await row.getByRole('button', { name: 'Rename' }).click();
	d = page.getByRole('dialog');
	await d.getByRole('textbox', { name: /^Name/ }).fill('e2e agent, renamed');
	await d.getByRole('button', { name: 'Save' }).click();
	await expect(d).toHaveCount(0);
	const renamed = page.locator('tbody tr', { hasText: 'e2e agent, renamed' });
	await renamed.getByRole('button', { name: 'Transfer' }).click();
	d = page.getByRole('dialog');
	await d.getByLabel(/New owner/).selectOption({ label: 'Security lead (admin)' });
	await d.getByRole('button', { name: 'Transfer' }).click();
	await expect(d).toHaveCount(0);
	await expect(renamed).toContainText('Security lead');
	await expect(renamed).toContainText('minted by Administrator');
});

test('an MCP token is minted with a scope, and its scope edited back to all groups', async ({ page }) => {
	await page.goto('/mcp-admin/tokens');
	await page.getByRole('button', { name: /Mint a token/ }).click();
	let d = page.getByRole('dialog');
	await d.getByRole('textbox', { name: /^Name/ }).fill('e2e scoped');
	await d.getByLabel('Add a federation...').click();
	await d.getByRole('button', { name: 'Add GNU/Weeb network' }).click();
	// A private chat is named by the other person's user id.
	await d.getByPlaceholder('User id').fill(String(featuredUser!.id));
	await d.getByRole('button', { name: 'Add the private chat' }).click();
	await d.getByRole('button', { name: 'Mint' }).click();
	await expect(page.getByText('Copy this token now')).toBeVisible();
	const row = page.locator('tbody tr', { hasText: 'e2e scoped' });
	await expect(row).toContainText('GNU/Weeb network');
	await expect(row).toContainText('Dewi Kusuma');
	await row.getByRole('button', { name: 'Scope' }).click();
	d = page.getByRole('dialog');
	await d.getByRole('button', { name: 'Remove GNU/Weeb network' }).click();
	await d.getByRole('button', { name: 'Remove Dewi Kusuma' }).click();
	await d.getByRole('button', { name: 'Save' }).click();
	await expect(d).toHaveCount(0);
	await expect(row).toContainText('All exposed groups');
});

test('a join that waits for approval holds its filing until it is cancelled', async ({ page, as }) => {
	await as('admin');
	await page.goto('/join-chat');
	const pending = page.locator('section.a-card:has(h2:text-is("Pending filings")) li');
	const held = await pending.count();
	await page.getByRole('textbox', { name: /Invite link or username/ }).fill('https://t.me/+request_e2e');
	await page.getByRole('button', { name: 'Preview' }).click();
	await page.locator('label.a-checkchip').first().click();
	await page.getByRole('button', { name: 'Join', exact: true }).click();
	await expect(page.getByRole('list', { name: 'To be filed in' })).toContainText('GNU/Weeb network');
	await expect(pending).toHaveCount(held + 1);
	page.once('dialog', (d) => void d.accept());
	await page.getByRole('button', { name: 'Cancel filing Approval Club into GNU/Weeb network' }).click();
	await expect(pending).toHaveCount(held);
});
