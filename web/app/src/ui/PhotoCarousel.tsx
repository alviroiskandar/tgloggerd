// A user's or a group's recorded photos in a carousel over the page
// (GET /api/users/{id}/photos, /api/groups/{id}/photos): the current
// photo first, then the others newest first; opened at the photo asked
// for or else the current one. The arrow keys,
// Home and End, the buttons and a swipe move through them; Esc, the close
// button or a click beside the photo closes it. An animated photo plays
// its video, muted and looped, over its still.
import { useCallback, useEffect, useRef, useState, type PointerEvent } from 'react';
import { track } from './busy.ts';
import { useScrollLock } from './scrollLock.ts';
import { createPortal } from 'react-dom';
import { ChevronLeft, ChevronRight, CloudOff, ExternalLink, ImageOff, LoaderCircle, X } from 'lucide-react';
import { api, ApiError, ok } from '../api/client.ts';
import { dateTime } from '../format.ts';
import type { Viewer } from '../page.ts';
import type { S } from '../api/schema.ts';
import { NOT_AVAILABLE, PhotoImg, usePhotoState } from './PhotoImg.tsx';

type Photos = S['UserPhotos'];

const SWIPE = 50;

// The photo on the stage: a spinner while it loads (the picture, hidden,
// still takes no room), and a note if it cannot be loaded.
function StageImg({ src, alt }: { src: string; alt: string }) {
	const { state, ref, onLoad, onError } = usePhotoState(src);
	if (state === 'failed') return <div className="a-carousel-note" title={NOT_AVAILABLE}><ImageOff size={28} aria-hidden="true" /><span>{NOT_AVAILABLE}</span></div>;
	return (
		<>
			<img ref={ref} className={`a-carousel-img${state === 'loading' ? ' loading' : ''}`} src={src} alt={alt} draggable={false} onLoad={onLoad} onError={onError} />
			{state === 'loading' && <div className="a-carousel-note a-carousel-wait" role="status" aria-label="Loading the photo"><LoaderCircle size={22} className="a-spin" /></div>}
		</>
	);
}

const reducedMotion = () => typeof matchMedia === 'function' && matchMedia('(prefers-reduced-motion: reduce)').matches;

// An animated photo on the stage: its video, muted and looped, in the
// still's place with the still as poster. For reduced motion it waits on
// the poster with the player's controls; it stops when the stage moves
// on (the element goes with its item); if it cannot play, the still.
function StageVideo({ src, poster, alt }: { src: string; poster: string | null; alt: string }) {
	const ref = useRef<HTMLVideoElement>(null);
	const [still] = useState(reducedMotion);
	const [failed, setFailed] = useState(false);
	useEffect(() => {
		const v = ref.current;
		if (!v) return;
		// React sets muted as a property only; autoplay needs it set first.
		v.muted = true;
		if (!still) v.play()?.catch(() => {});
		return () => v.pause();
	}, [src, still]);
	if (failed) return poster ? <StageImg src={poster} alt={alt} /> : <div className="a-carousel-note" title={NOT_AVAILABLE}><ImageOff size={28} aria-hidden="true" /><span>{NOT_AVAILABLE}</span></div>;
	return (
		<video ref={ref} className="a-carousel-img a-carousel-video" src={src} poster={poster ?? undefined} aria-label={alt}
			muted loop playsInline autoPlay={!still} controls={still} preload="metadata" onError={() => setFailed(true)} />
	);
}

export function PhotoCarousel({ kind, id, start, title, viewer, onClose }: { kind: 'users' | 'groups'; id: number; start: number | null; title: string; viewer: Viewer; onClose: () => void }) {
	const [photos, setPhotos] = useState<Photos | null>(null);
	const [err, setErr] = useState('');
	const [at, setAt] = useState(0);
	const close = useRef<HTMLButtonElement>(null);
	const down = useRef<number | null>(null);
	useEffect(() => {
		let live = true;
		const call = kind === 'users'
			? ok(api.GET('/api/users/{id}/photos', { params: { path: { id } } }))
			: ok(api.GET('/api/groups/{id}/photos', { params: { path: { id } } }));
		track(call).then((p) => {
			if (!live) return;
			setPhotos(p);
			// The photo asked for, else the first: the list starts with the
			// current photo, also one the recorded history lacks, so
			// current_id (a history row) may name an older item.
			const i = start === null ? 0 : p.items.findIndex((x) => x.id === start);
			setAt(Math.max(0, i));
		}, (x) => { if (live) setErr(x instanceof ApiError ? x.message : 'The server could not be reached.'); });
		return () => { live = false; };
	}, [kind, id, start]);
	const n = photos?.items.length ?? 0;
	const go = useCallback((i: number) => setAt((cur) => (n ? Math.min(n - 1, Math.max(0, i)) : cur)), [n]);
	useScrollLock();
	const closing = useRef(onClose);
	closing.current = onClose;
	useEffect(() => {
		const on = (e: KeyboardEvent) => {
			if (e.key === 'Escape') closing.current();
			// Newest first: left is newer, right is older, as a timeline reads.
			else if (e.key === 'ArrowLeft') setAt((i) => Math.max(0, i - 1));
			else if (e.key === 'ArrowRight') setAt((i) => Math.min(n - 1, i + 1));
			else if (e.key === 'Home') setAt(0);
			else if (e.key === 'End') setAt(n - 1);
			else return;
			e.preventDefault();
		};
		addEventListener('keydown', on);
		close.current?.focus();
		return () => removeEventListener('keydown', on);
	}, [n]);
	const onDown = (e: PointerEvent) => { down.current = e.clientX; };
	const onUp = (e: PointerEvent) => {
		if (down.current === null) return;
		const dx = e.clientX - down.current;
		down.current = null;
		if (dx > SWIPE) go(at - 1);
		else if (dx < -SWIPE) go(at + 1);
	};
	const p = photos?.items[at];
	const box = (
		<div className="a-carousel" role="dialog" aria-modal="true" aria-label={`Photos of ${title}`} onMouseDown={(e) => e.target === e.currentTarget && onClose()}>
			<div className="a-carousel-bar">
				<span className="a-grow a-ellipsis"><b>{title}</b>{n > 0 && <span className="a-carousel-count"> . {at + 1} of {n}</span>}</span>
				{p?.url && <a className="a-carousel-btn" href={p.url} target="_blank" rel="noopener" aria-label="Open the file"><ExternalLink size={18} /></a>}
				<button ref={close} type="button" className="a-carousel-btn" onClick={onClose} aria-label="Close"><X size={20} /></button>
			</div>
			<div className="a-carousel-stage" onPointerDown={onDown} onPointerUp={onUp} onMouseDown={(e) => e.target === e.currentTarget && onClose()}>
				{err ? <div className="a-carousel-note" role="alert">{err}</div>
					: !photos ? <div className="a-carousel-note"><LoaderCircle size={22} className="a-spin" /></div>
						: !p ? <div className="a-carousel-note">No photo is recorded.</div>
							: p.video_url ? <StageVideo key={p.id} src={p.video_url} poster={p.url} alt={`Photo of ${title}, set ${dateTime(p.at, viewer)}`} />
								: p.url ? <StageImg key={p.id} src={p.url} alt={`Photo of ${title}, set ${dateTime(p.at, viewer)}`} />
									: <div className="a-carousel-note"><CloudOff size={28} /><span>This photo's file is not stored.</span></div>}
				{n > 1 && <button type="button" className="a-carousel-nav prev" onClick={() => go(at - 1)} disabled={at === 0} aria-label="Newer photo"><ChevronLeft size={26} /></button>}
				{n > 1 && <button type="button" className="a-carousel-nav next" onClick={() => go(at + 1)} disabled={at === n - 1} aria-label="Older photo"><ChevronRight size={26} /></button>}
			</div>
			{p && (
				<div className="a-carousel-caption" aria-live="polite">
					Set {dateTime(p.at, viewer)}{p.id === photos?.current_id ? <span className="a-carousel-current">current photo</span> : null}
				</div>
			)}
			{n > 1 && (
				<div className="a-carousel-strip" role="group" aria-label="All photos">
					{photos!.items.map((x, i) => (
						<button key={x.id} type="button" className={i === at ? 'on' : ''} aria-current={i === at ? 'true' : undefined} aria-label={`Photo set ${dateTime(x.at, viewer)}`} onClick={() => go(i)}>
							{x.url ? <PhotoImg className="a-photo-fill" src={x.url} alt="" lazy iconSize={14} /> : <CloudOff size={14} />}
						</button>
					))}
				</div>
			)}
		</div>
	);
	return createPortal(box, document.querySelector('.app') ?? document.body);
}
