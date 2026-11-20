// Events of GET /api/events (web-live.md section 3). They name what
// changed, never the content; the page refetches through the API.
import type { S } from '../api/schema.ts';

export type LiveKind =
	| 'message.new' | 'message.edited' | 'message.deleted' | 'file.stored'
	| 'user.updated' | 'group.updated' | 'stats.changed'
	| 'bot.updated'
	| 'resync' | 'session';

export const LIVE_KINDS: LiveKind[] = ['message.new', 'message.edited', 'message.deleted', 'file.stored', 'user.updated', 'group.updated', 'stats.changed', 'bot.updated', 'resync', 'session'];

// The schema's event, with the kinds the client acts on (others, like
// hello, are not listened to). Signed-out streams carry no seq.
export type LiveEvent = Omit<S['LiveEvent'], 'kind'> & { kind: LiveKind };

// The topics an event belongs to (web-live-client.md section 3).
// resync and session reach every subscriber.
export function topicsOf(e: LiveEvent): string[] | 'all' {
	switch (e.kind) {
		case 'message.new':
		case 'message.edited':
		case 'message.deleted':
		case 'file.stored':
			return [`chat:${e.chat_id}`, 'messages'];
		case 'user.updated':
			return [`user:${e.user_id}`];
		case 'group.updated':
			return [`group:${e.chat_id}`];
		case 'stats.changed':
			return ['stats'];
		case 'bot.updated':
			return ['bots'];
		default:
			return 'all';
	}
}
