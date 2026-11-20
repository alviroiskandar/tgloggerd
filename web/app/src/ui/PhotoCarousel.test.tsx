// @vitest-environment happy-dom
// The photo carousel: from a user's or a group's photo, at the current
// one; from a photo change in the history, at that photo; and moving
// through the photos by key, button, thumbnail and swipe.
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { fireEvent, screen, waitFor, within } from '@testing-library/react';
import { callsTo, fakeApi, fixture, json, renderPage, setup } from '../test/page.tsx';
import { groupPhotos, sampleContacts, userPhotos } from '../../standin/fixtures.ts';
import type { UserProps } from '../routes/user.tsx';
import type { GroupsProps } from '../routes/groups.tsx';

beforeEach(setup);
type Photos = { items: { id: number; at: string; url: string | null; video_url: string | null }[]; current_id: number | null };
const serve = {
	'GET /api/users/:id/photos': (c: { path: string }) => { const [st, b] = userPhotos(c.path.split('/')[3]!, 'superadmin'); return json(st, b); },
	'GET /api/groups/:id/photos': (c: { path: string }) => { const [st, b] = groupPhotos(c.path.split('/')[3]!, 'superadmin'); return json(st, b); },
};
const box = () => screen.getByRole('dialog', { name: /^Photos of / });
const count = () => box().querySelector('.a-carousel-count')!.textContent;

// A current photo the history lacks: the stand-in lists it first, marks
// it current and gives it the row 99.
const missing = (p: Photos) => p.current_id !== null && p.items[0]!.id === p.current_id && p.items[0]!.id % 100 === 99;

// A user with a photo and more than one recorded; with stills, one
// whose first two photos have no video.
function userWithPhotos(stills = false) {
	for (const id of [...sampleContacts(), ...fixture<{ rows: { id: number }[] }>('/users').props.rows.map((r) => r.id)]) {
		const [st, p] = userPhotos(String(id), 'superadmin') as [number, Photos];
		if (st === 200 && p.items.length > 2 && (!stills || p.items.slice(0, 2).every((x) => !x.video_url))) return { id, p };
	}
	throw new Error('no user with photos in the sample');
}

describe('photo carousel', () => {
	it('opens from the profile photo at the current one, with its date', async () => {
		const { id, p } = userWithPhotos();
		const calls = fakeApi('superadmin', serve);
		const page = fixture<Extract<UserProps, { limited?: false }>>(`/users/${id}`).props;
		renderPage(`/users/${id}`, 'superadmin', { props: { ...page, user: { ...page.user, photo: '/files/now' } } });
		fireEvent.click(screen.getByRole('button', { name: /^Show the photos of / }));
		await waitFor(() => expect(count()).toContain(`of ${p.items.length}`));
		expect(callsTo(calls, 'GET', `/api/users/${id}/photos`)).toHaveLength(1);
		expect(count()).toContain('1 of');
		expect(within(box()).getByText(/^Set /)).toBeTruthy();
	});

	it('opens from the profile photo at the first, even when current_id names an older one', async () => {
		const { id, p } = userWithPhotos();
		// As an older answer could: the current file was also an older photo.
		fakeApi('superadmin', { 'GET /api/users/:id/photos': () => ({ ...p, current_id: p.items[2]!.id }) });
		const page = fixture<Extract<UserProps, { limited?: false }>>(`/users/${id}`).props;
		renderPage(`/users/${id}`, 'superadmin', { props: { ...page, user: { ...page.user, photo: '/files/now' } } });
		fireEvent.click(screen.getByRole('button', { name: /^Show the photos of / }));
		await waitFor(() => expect(count()).toContain(`1 of ${p.items.length}`));
	});

	it('opens at a user\'s current photo the history lacks', async () => {
		const uid = [...sampleContacts(), ...fixture<{ rows: { id: number }[] }>('/users').props.rows.map((r) => r.id)]
			.find((x) => { const [st, p] = userPhotos(String(x), 'superadmin') as [number, Photos]; return st === 200 && p.items.length > 2 && missing(p); })!;
		const up = userPhotos(String(uid), 'superadmin')[1] as Photos;
		fakeApi('superadmin', serve);
		const page = fixture<Extract<UserProps, { limited?: false }>>(`/users/${uid}`).props;
		expect(page.history.some((e) => e.kind === 'photo' && e.photo_id === up.current_id)).toBe(false);
		renderPage(`/users/${uid}`, 'superadmin', { props: { ...page, user: { ...page.user, photo: '/files/now' } } });
		fireEvent.click(screen.getByRole('button', { name: /^Show the photos of / }));
		await waitFor(() => expect(count()).toContain(`1 of ${up.items.length}`));
		expect(within(box()).getByText('current photo')).toBeTruthy();
	});

	it('opens at a group\'s current photo the history lacks', async () => {
		const g = fixture<GroupsProps>('/groups').props.rows.find((r) => missing(groupPhotos(String(r.id), 'superadmin')[1] as Photos))!;
		const gp = groupPhotos(String(g.id), 'superadmin')[1] as Photos;
		fakeApi('superadmin', serve);
		const gpage = fixture<{ group: object }>(`/groups/${g.id}`).props;
		renderPage(`/groups/${g.id}`, 'superadmin', { props: { ...gpage, group: { ...gpage.group, photo: '/files/g' } } });
		fireEvent.click(screen.getByRole('button', { name: /^Show the photos of / }));
		await waitFor(() => expect(count()).toContain(`1 of ${gp.items.length}`));
		expect(within(box()).getByText('current photo')).toBeTruthy();
	});

	it('moves by arrow keys, Home and End, buttons and thumbnails; Esc closes', async () => {
		const { id, p } = userWithPhotos();
		fakeApi('superadmin', serve);
		const page = fixture<Extract<UserProps, { limited?: false }>>(`/users/${id}`).props;
		renderPage(`/users/${id}`, 'superadmin', { props: { ...page, user: { ...page.user, photo: '/files/now' } } });
		fireEvent.click(screen.getByRole('button', { name: /^Show the photos of / }));
		await waitFor(() => expect(count()).toContain(`of ${p.items.length}`));
		fireEvent.keyDown(window, { key: 'Home' });
		expect(count()).toContain('1 of');
		fireEvent.keyDown(window, { key: 'ArrowRight' });
		expect(count()).toContain('2 of');
		fireEvent.click(within(box()).getByRole('button', { name: 'Newer photo' }));
		expect(count()).toContain('1 of');
		fireEvent.keyDown(window, { key: 'End' });
		expect(count()).toContain(`${p.items.length} of`);
		expect((within(box()).getByRole('button', { name: 'Older photo' }) as HTMLButtonElement).disabled).toBe(true);
		fireEvent.click(within(box()).getAllByRole('button', { name: /^Photo set / })[1]!);
		expect(count()).toContain('2 of');
		fireEvent.keyDown(window, { key: 'Escape' });
		expect(screen.queryByRole('dialog', { name: /^Photos of / })).toBeNull();
	});

	it('moves by swiping', async () => {
		const { id, p } = userWithPhotos();
		fakeApi('superadmin', serve);
		const page = fixture<Extract<UserProps, { limited?: false }>>(`/users/${id}`).props;
		renderPage(`/users/${id}`, 'superadmin', { props: { ...page, user: { ...page.user, photo: '/files/now' } } });
		fireEvent.click(screen.getByRole('button', { name: /^Show the photos of / }));
		await waitFor(() => expect(count()).toContain(`of ${p.items.length}`));
		fireEvent.keyDown(window, { key: 'Home' });
		const stage = box().querySelector('.a-carousel-stage')!;
		fireEvent.pointerDown(stage, { clientX: 300 });
		fireEvent.pointerUp(stage, { clientX: 200 });
		expect(count()).toContain('2 of');
		fireEvent.pointerDown(stage, { clientX: 200 });
		fireEvent.pointerUp(stage, { clientX: 300 });
		expect(count()).toContain('1 of');
	});

	it('opens from a photo change in the history at that photo', async () => {
		const { id, p } = userWithPhotos();
		fakeApi('superadmin', serve);
		const page = fixture<Extract<UserProps, { limited?: false }>>(`/users/${id}`).props;
		const target = p.items[2]!;
		renderPage(`/users/${id}/history`, 'superadmin', { props: { ...page, history: [{ kind: 'photo', at: target.at, photo: '/files/x', photo_id: target.id }], history_next: null } });
		fireEvent.click(screen.getByRole('button', { name: /^Show this photo/ }));
		await waitFor(() => expect(count()).toContain(`3 of ${p.items.length}`));
	});

	it('says when a photo\'s file is not stored', async () => {
		const { id } = userWithPhotos();
		fakeApi('superadmin', { 'GET /api/users/:id/photos': () => ({ items: [{ id: 1, at: '2026-11-01T10:00:00Z', url: null, file_id: null, video_url: null, video_file_id: null }], current_id: null }) });
		const page = fixture<Extract<UserProps, { limited?: false }>>(`/users/${id}`).props;
		renderPage(`/users/${id}`, 'superadmin', { props: { ...page, user: { ...page.user, photo: '/files/now' } } });
		fireEvent.click(screen.getByRole('button', { name: /^Show the photos of / }));
		expect(await within(box()).findByText(/file is not stored/)).toBeTruthy();
	});

	it('opens from a group\'s photo and from its photo changes', async () => {
		const g = fixture<GroupsProps>('/groups').props.rows.find((r) => (groupPhotos(String(r.id), 'superadmin')[1] as Photos).items.length > 1)!;
		const p = groupPhotos(String(g.id), 'superadmin')[1] as Photos;
		const calls = fakeApi('superadmin', serve);
		const page = fixture<{ group: object }>(`/groups/${g.id}/history`).props;
		const target = p.items[1]!;
		renderPage(`/groups/${g.id}/history`, 'superadmin', { props: { ...page, group: { ...page.group, photo: '/files/g' }, history: [{ kind: 'photo', at: target.at, photo: '/files/x', photo_id: target.id }], history_next: null } });
		fireEvent.click(screen.getByRole('button', { name: /^Show this photo/ }));
		await waitFor(() => expect(count()).toContain(`2 of ${p.items.length}`));
		fireEvent.keyDown(window, { key: 'Escape' });
		fireEvent.click(screen.getByRole('button', { name: /^Show the photos of / }));
		await waitFor(() => expect(callsTo(calls, 'GET', `/api/groups/${g.id}/photos`)).toHaveLength(2));
	});

	it('offers no carousel without a photo', () => {
		const { id } = userWithPhotos();
		const page = fixture<Extract<UserProps, { limited?: false }>>(`/users/${id}`).props;
		renderPage(`/users/${id}`, 'superadmin', { props: { ...page, user: { ...page.user, photo: null } } });
		expect(screen.queryByRole('button', { name: /^Show the photos of / })).toBeNull();
	});

	it('shows a neutral box while a photo loads, and a placeholder when it fails, in the viewer and its strip', async () => {
		const { id, p } = userWithPhotos(true);
		fakeApi('superadmin', serve);
		const page = fixture<Extract<UserProps, { limited?: false }>>(`/users/${id}`).props;
		renderPage(`/users/${id}`, 'superadmin', { props: { ...page, user: { ...page.user, photo: '/files/now' } } });
		fireEvent.click(screen.getByRole('button', { name: /^Show the photos of / }));
		await waitFor(() => expect(count()).toContain(`of ${p.items.length}`));
		// The stage: a spinner and the picture hidden until it loads.
		const stage = box().querySelector('.a-carousel-stage')!;
		const img = stage.querySelector('img.a-carousel-img')!;
		expect(img.classList.contains('loading')).toBe(true);
		expect(within(box()).getByRole('status', { name: 'Loading the photo' })).toBeTruthy();
		fireEvent.load(img);
		expect(img.classList.contains('loading')).toBe(false);
		expect(within(box()).queryByRole('status', { name: 'Loading the photo' })).toBeNull();
		// It fails: a note, no picture left to show broken.
		fireEvent.error(img);
		expect(stage.querySelector('img')).toBeNull();
		expect(within(stage as HTMLElement).getByText('Photo not available')).toBeTruthy();
		// The strip's thumbnails likewise: a box while loading, then the
		// placeholder.
		const strip = within(box()).getByRole('group', { name: 'All photos' });
		const thumb = strip.querySelectorAll('img')[1]!;
		expect(thumb.parentElement!.classList.contains('loading')).toBe(true);
		fireEvent.error(thumb);
		const buttons = within(strip).getAllByRole('button');
		expect(buttons[1]!.querySelector('img')).toBeNull();
		expect(within(buttons[1]!).getByRole('img', { name: 'Photo not available' }).getAttribute('title')).toBe('Photo not available');
		// Moving on to another photo starts its own load.
		fireEvent.keyDown(window, { key: 'ArrowRight' });
		expect(box().querySelector('.a-carousel-stage img.a-carousel-img.loading')).toBeTruthy();
	});

	it('shows a placeholder for a failed history photo of a user', async () => {
		const { id, p } = userWithPhotos();
		const page = fixture<Extract<UserProps, { limited?: false }>>(`/users/${id}`).props;
		renderPage(`/users/${id}/history`, 'superadmin', { props: { ...page, history: [{ kind: 'photo', at: p.items[1]!.at, photo: '/files/standin-gone-1', photo_id: p.items[1]!.id }], history_next: null } });
		const btn = screen.getByRole('button', { name: /^Show this photo/ });
		const img = btn.querySelector('img')!;
		expect(img.parentElement!.classList.contains('loading')).toBe(true);
		fireEvent.error(img);
		expect(btn.querySelector('img')).toBeNull();
		expect(within(btn).getByRole('img', { name: 'Photo not available' }).classList.contains('a-tl-photo')).toBe(true);
	});

	it('shows a placeholder for a failed history photo of a group', async () => {
		const g = fixture<GroupsProps>('/groups').props.rows.find((r) => (groupPhotos(String(r.id), 'superadmin')[1] as Photos).items.length > 1)!;
		const p = groupPhotos(String(g.id), 'superadmin')[1] as Photos;
		const page = fixture<{ group: object }>(`/groups/${g.id}/history`).props;
		renderPage(`/groups/${g.id}/history`, 'superadmin', { props: { ...page, history: [{ kind: 'photo', at: p.items[0]!.at, photo: '/files/standin-gone-2', photo_id: p.items[0]!.id }], history_next: null } });
		const btn = screen.getByRole('button', { name: /^Show this photo/ });
		const img = btn.querySelector('img')!;
		fireEvent.load(img);
		expect(img.parentElement!.classList.contains('shown')).toBe(true);
		fireEvent.error(img);
		expect(btn.querySelector('img')).toBeNull();
		expect(within(btn).getByRole('img', { name: 'Photo not available' })).toBeTruthy();
	});
});

// Animated photos: the video plays muted and looped over its still, not
// for reduced motion, and stops when the carousel moves on.
describe('photo carousel with a video', () => {
	const items = [
		{ id: 3, at: '2026-11-03T10:00:00Z', url: '/files/still-3', file_id: 3, video_url: '/files/video-3', video_file_id: 30 },
		{ id: 2, at: '2026-11-02T10:00:00Z', url: '/files/still-2', file_id: 2, video_url: null, video_file_id: 20 },
		{ id: 1, at: '2026-11-01T10:00:00Z', url: '/files/still-1', file_id: 1, video_url: '/files/video-1', video_file_id: 10 },
	];
	let play: ReturnType<typeof vi.fn>;
	let pause: ReturnType<typeof vi.fn<(this: HTMLMediaElement) => void>>;
	beforeEach(() => {
		play = vi.spyOn(HTMLMediaElement.prototype, 'play').mockResolvedValue(undefined);
		pause = vi.spyOn(HTMLMediaElement.prototype, 'pause').mockImplementation(() => {});
	});
	afterEach(() => vi.restoreAllMocks());
	const reduced = (on: boolean) => vi.stubGlobal('matchMedia', (q: string) => ({ matches: on && q.includes('reduce'), media: q, addEventListener() {}, removeEventListener() {} }));
	async function open() {
		const { id } = userWithPhotos();
		fakeApi('superadmin', { 'GET /api/users/:id/photos': () => ({ items, current_id: 3 }) });
		const page = fixture<Extract<UserProps, { limited?: false }>>(`/users/${id}`).props;
		renderPage(`/users/${id}`, 'superadmin', { props: { ...page, user: { ...page.user, photo: '/files/now' } } });
		fireEvent.click(screen.getByRole('button', { name: /^Show the photos of / }));
		await waitFor(() => expect(count()).toContain('1 of 3'));
		return box().querySelector('.a-carousel-stage')!;
	}

	it('plays an animated photo muted and looped, with its still as poster, in the still\'s box', async () => {
		reduced(false);
		const stage = await open();
		const v = stage.querySelector('video')!;
		expect(v).toBeTruthy();
		expect(v.getAttribute('src')).toBe('/files/video-3');
		expect(v.getAttribute('poster')).toBe('/files/still-3');
		expect(v.muted).toBe(true);
		expect(v.loop).toBe(true);
		expect(v.hasAttribute('playsinline')).toBe(true);
		expect(v.autoplay).toBe(true);
		expect(v.controls).toBe(false);
		expect(v.getAttribute('preload')).toBe('metadata');
		expect(v.classList.contains('a-carousel-img')).toBe(true);
		expect(v.getAttribute('aria-label')).toMatch(/^Photo of .*, set /);
		expect(stage.querySelector('img')).toBeNull();
		expect(play).toHaveBeenCalled();
	});

	it('shows a photo without a video, or with one that cannot be served, as a still', async () => {
		reduced(false);
		const stage = await open();
		fireEvent.keyDown(window, { key: 'ArrowRight' });
		expect(stage.querySelector('video')).toBeNull();
		expect(stage.querySelector('img.a-carousel-img')!.getAttribute('src')).toBe('/files/still-2');
	});

	it('stops the old video when moving to another photo', async () => {
		reduced(false);
		const stage = await open();
		const v = stage.querySelector('video')!;
		const stopped = pause.mock.contexts.filter((c: unknown) => c === v).length;
		fireEvent.keyDown(window, { key: 'End' });
		expect(pause.mock.contexts.filter((c: unknown) => c === v).length).toBeGreaterThan(stopped);
		expect(stage.querySelector('video')!.getAttribute('src')).toBe('/files/video-1');
	});

	it('does not autoplay for reduced motion: the poster with a play control', async () => {
		reduced(true);
		const stage = await open();
		const v = stage.querySelector('video')!;
		expect(v.getAttribute('poster')).toBe('/files/still-3');
		expect(v.autoplay).toBe(false);
		expect(v.controls).toBe(true);
		expect(play).not.toHaveBeenCalled();
	});

	it('falls back to the still when the video cannot play', async () => {
		reduced(false);
		const stage = await open();
		fireEvent.error(stage.querySelector('video')!);
		expect(stage.querySelector('video')).toBeNull();
		expect(stage.querySelector('img.a-carousel-img')!.getAttribute('src')).toBe('/files/still-3');
	});
});
