/**
 * Pure helpers of the Activity tab (admin panel, stage 3 milestone M3): the
 * query of GET /admin/audit from the filters, paging, and the texts of a
 * record (action, actor, one-line summary, expanded details). No state and
 * no I/O, covered by unit tests. Imported by path ('$lib/utils/admin-audit').
 */

import {
	ADMIN_AUDIT,
	ADMIN_AUDIT_ACTION_GROUPS,
	ADMIN_AUDIT_ACTION_LABELS,
	ADMIN_AUDIT_ATTEMPT_LABELS,
	ADMIN_AUDIT_DETAIL_LABELS,
	ADMIN_AUDIT_KIND_LABELS,
	ADMIN_AUDIT_KIND_ORDER,
	ADMIN_AUDIT_REFUSAL_REASONS,
	ADMIN_AUDIT_RESULT_GROUPS,
	ADMIN_AUDIT_RESULT_LABELS,
	ADMIN_AUDIT_REVERT_TARGETS,
	ADMIN_FAILURE_LABELS,
	ADMIN_SETTING_LABELS
} from '$lib/constants';
import { AdminFailureCode, AdminSettingKey } from '$lib/enums';
import {
	AdminAuditActionGroup,
	AdminAuditActorType,
	AdminAuditKind,
	AdminAuditResult,
	AdminAuditResultGroup
} from '$lib/enums/admin.enums';
import type {
	AdminAuditActorView,
	AdminAuditDetailRow,
	AdminAuditFilters,
	AdminAuditRequest,
	ApiAdminAuditActor,
	ApiAdminAuditEntry,
	ApiAdminAuditList,
	ApiAdminAuditStatus
} from '$lib/types/admin-audit';
import { formatBytes } from '$lib/utils/admin';

/** How a result is shown: the colour of its badge. */
export type AdminAuditTone = 'error' | 'success' | 'warning' | 'neutral';

/** Router settings keys of settings.update, as the Settings tab names them. */
const ROUTER_SETTING_LABELS: Record<string, string> = {
	default_device: 'GPU for new models',
	models_max: 'Max loaded models',
	pinned: 'Pinned models'
};
/** Detail fields shown with a monospace font. */
const MONO_KEYS = new Set([
	'code',
	'folder',
	'id',
	'paths',
	'sha256',
	'version',
	'items',
	'sample'
]);
/** Detail fields holding a byte count. */
const BYTE_KEYS = new Set(['bytes', 'max_bytes']);
/** Detail fields holding an ISO time. */
const TIME_KEYS = new Set(['first', 'last', 'not_persisted_since', 'since']);
/** Detail fields that are shown as part of another row, or in a section of their own. */
const SKIPPED_KEYS = new Set(['changes', 'clients_capped', 'items_capped', 'settings']);

function isRecord(value: unknown): value is Record<string, unknown> {
	return typeof value === 'object' && value !== null && !Array.isArray(value);
}

function num(value: unknown): number | null {
	return typeof value === 'number' && Number.isFinite(value) ? value : null;
}

function str(value: unknown): string {
	return typeof value === 'string' ? value : '';
}

function pad2(value: number): string {
	return String(value).padStart(2, '0');
}

function plural(count: number, word: string): string {
	return `${count} ${word}${count === 1 ? '' : 's'}`;
}

/** "unsupported_architecture" -> "unsupported architecture". */
function humanizeCode(code: string): string {
	return code.replace(/[_.]+/g, ' ').trim();
}

/** The default filters: everything. */
export function defaultAuditFilters(): AdminAuditFilters {
	return {
		action: AdminAuditActionGroup.ALL,
		kinds: [],
		result: AdminAuditResultGroup.ALL
	};
}

/**
 * The kinds of a filter in the order of the filter; every kind (or none)
 * means no filter, so both are returned as [].
 */
export function normalizeAuditKinds(kinds: readonly string[]): AdminAuditKind[] {
	const picked = ADMIN_AUDIT_KIND_ORDER.filter((kind) => kinds.includes(kind));

	return picked.length === ADMIN_AUDIT_KIND_ORDER.length ? [] : picked;
}

/** Toggle one kind of the kind filter. */
export function toggleAuditKind(kinds: readonly AdminAuditKind[], kind: AdminAuditKind) {
	return normalizeAuditKinds(
		kinds.includes(kind) ? kinds.filter((item) => item !== kind) : [...kinds, kind]
	);
}

/** Any filter differs from "everything". */
export function isAuditFiltered(filters: AdminAuditFilters): boolean {
	return (
		normalizeAuditKinds(filters.kinds).length > 0 ||
		filters.action !== AdminAuditActionGroup.ALL ||
		filters.result !== AdminAuditResultGroup.ALL
	);
}

/**
 * The query string of GET /admin/audit ("" or "?..."): the filters as the
 * server's comma lists, plus paging and format.
 */
export function buildAuditQuery(filters: AdminAuditFilters, request: AdminAuditRequest = {}) {
	const params: string[] = [];
	const list = (values: readonly string[]) => values.map(encodeURIComponent).join(',');
	const kinds = normalizeAuditKinds(filters.kinds);
	const actions = ADMIN_AUDIT_ACTION_GROUPS[filters.action]?.actions ?? [];
	const results = ADMIN_AUDIT_RESULT_GROUPS[filters.result]?.results ?? [];

	if (request.format === 'jsonl') params.push('format=jsonl');

	if (request.before) params.push(`before=${encodeURIComponent(request.before)}`);

	if (request.limit !== undefined && request.format !== 'jsonl') {
		const limit = Math.min(ADMIN_AUDIT.LIMIT_MAX, Math.max(1, Math.round(request.limit)));

		params.push(`limit=${limit}`);
	}

	if (kinds.length > 0) params.push(`kind=${list(kinds)}`);

	if (actions.length > 0) params.push(`action=${list(actions)}`);

	if (results.length > 0) params.push(`result=${list(results)}`);

	return params.length > 0 ? `?${params.join('&')}` : '';
}

/**
 * Add a page to the records already shown: newest first, one row per id (a
 * merged record that was still counting is replaced by its newer version).
 */
export function mergeAuditEntries(
	current: readonly ApiAdminAuditEntry[],
	page: readonly ApiAdminAuditEntry[]
): ApiAdminAuditEntry[] {
	const byId = new Map<string, ApiAdminAuditEntry>();

	for (const entry of current) byId.set(entry.id, entry);

	for (const entry of page) byId.set(entry.id, entry);

	return [...byId.values()].sort((a, b) => b.seq - a.seq);
}

/** Matching records older than the page that are not loaded yet. */
export function auditRemaining(page: Pick<ApiAdminAuditList, 'entries' | 'matched'>): number {
	return Math.max(0, (num(page.matched) ?? 0) - page.entries.length);
}

/** Local time `YYYY-MM-DD HH:MM:SS` of a wall time in ms, '' when unknown. */
export function formatAuditTime(ms: number | null | undefined): string {
	if (typeof ms !== 'number' || !Number.isFinite(ms)) return '';

	const d = new Date(ms);

	return `${d.getFullYear()}-${pad2(d.getMonth() + 1)}-${pad2(d.getDate())} ${pad2(d.getHours())}:${pad2(d.getMinutes())}:${pad2(d.getSeconds())}`;
}

/** Local time of an ISO time of the server, the text itself when it does not parse. */
export function formatAuditIsoTime(iso: unknown): string {
	if (typeof iso !== 'string' || !iso) return '';

	const ms = Date.parse(iso);

	return Number.isFinite(ms) ? formatAuditTime(ms) : iso;
}

/** Local time of a record: `ts`, else its ISO `time`. */
export function auditEntryTime(entry: Pick<ApiAdminAuditEntry, 'time' | 'ts'>): string {
	return formatAuditTime(entry.ts) || formatAuditIsoTime(entry.time);
}

/**
 * What an action is, in words. A change route that answered with an error
 * did not happen: "Settings change failed" instead of "Settings changed".
 */
export function describeAuditAction(action: string, result?: string): string {
	if (result === AdminAuditResult.ERROR && ADMIN_AUDIT_ATTEMPT_LABELS[action]) {
		return `${ADMIN_AUDIT_ATTEMPT_LABELS[action]} failed`;
	}

	return ADMIN_AUDIT_ACTION_LABELS[action] ?? action;
}

/** `YYYY-MM-DD HH:MM:SS UTC` of an ISO time of the server, '' when there is none. */
export function formatAuditUtc(iso: string | null | undefined): string {
	if (!iso) return '';

	return `${iso.replace('T', ' ').replace(/Z$/, '')} UTC`;
}

export function describeAuditResult(result: string): string {
	return ADMIN_AUDIT_RESULT_LABELS[result as AdminAuditResult] ?? result;
}

export function auditResultTone(result: string): AdminAuditTone {
	switch (result) {
		case AdminAuditResult.OK:
			return 'success';
		case AdminAuditResult.REFUSED:
			return 'warning';
		case AdminAuditResult.ERROR:
		case AdminAuditResult.FAILED:
		case AdminAuditResult.REJECTED:
			return 'error';
		default:
			return 'neutral';
	}
}

export function describeAuditKind(kind: string): string {
	return ADMIN_AUDIT_KIND_LABELS[kind as AdminAuditKind] ?? kind;
}

/**
 * The actor column: the address that presented the key (or Server, Public
 * API), how it came in, and whether a forwarded address is only a claim.
 */
export function describeAuditActor(
	actor: ApiAdminAuditActor | null | undefined,
	details?: Record<string, unknown> | null
): AdminAuditActorView {
	if (!actor || typeof actor !== 'object') {
		return {
			address: false,
			primary: 'unknown',
			secondary: null,
			title: 'Unknown',
			unverified: false
		};
	}

	switch (actor.type) {
		case AdminAuditActorType.CLIENT: {
			const peer = str(actor.peer);
			const address = str(actor.client) || peer || str(actor.label) || 'unknown';
			const forwarded = str(actor.forwarded_for);
			const unverified = forwarded !== '' && actor.forwarded_trusted !== true;

			let secondary: string | null = null;

			if (actor.via_proxy && peer) secondary = `via trusted proxy ${peer}`;
			else if (unverified) secondary = `X-Forwarded-For ${forwarded}`;

			const title = [
				`Address: ${address}`,
				peer && peer !== address ? `TCP peer: ${peer}` : null,
				actor.via_proxy ? 'came through a trusted proxy' : null,
				forwarded
					? `X-Forwarded-For: ${forwarded}${unverified ? ' (unverified: the peer is not a trusted proxy)' : ''}`
					: null,
				str(actor.note) ? `Note: ${str(actor.note)}` : null,
				str(actor.user_agent) ? `User-Agent: ${str(actor.user_agent)}` : null
			]
				.filter(Boolean)
				.join('\n');

			return { address: true, primary: address, secondary, title, unverified };
		}
		case AdminAuditActorType.SERVER:
			return {
				address: false,
				primary: 'Server',
				secondary: null,
				title: 'The server did this by itself',
				unverified: false
			};
		case AdminAuditActorType.PUBLIC_API:
			return {
				address: false,
				primary: 'Public API',
				secondary: 'caller unknown',
				title:
					'Seen on the model event feed. The public model routes take no admin key, so who called them is not known.',
				unverified: false
			};
		case AdminAuditActorType.VARIOUS: {
			const label = str(actor.label) || 'several clients';
			const clients = isRecord(details) ? num(details.clients) : null;
			const capped = isRecord(details) && details.clients_capped === true;

			return {
				address: false,
				primary: label.charAt(0).toUpperCase() + label.slice(1),
				secondary:
					clients !== null ? `${plural(clients, 'client')}${capped ? ' or more' : ''}` : null,
				title:
					'A merged record of requests from several addresses (some of them are in the details)',
				unverified: false
			};
		}
		default: {
			const label = str(actor.label) || 'unknown';

			return { address: false, primary: label, secondary: null, title: label, unverified: false };
		}
	}
}

/** A stored value of a change, in words: null is "unset", a list is joined. */
export function formatAuditValue(value: unknown): string {
	if (value === null || value === undefined) return 'unset';

	if (value === '') return 'empty';

	if (Array.isArray(value)) {
		return value.length === 0 ? 'none' : value.map((item) => formatAuditValue(item)).join(', ');
	}

	if (typeof value === 'boolean') return value ? 'true' : 'false';

	if (typeof value === 'object') return JSON.stringify(value);

	return String(value);
}

/** One change `{from, to}` as "from → to". */
export function formatAuditChange(change: unknown): string {
	if (!isRecord(change)) return formatAuditValue(change);

	return `${formatAuditValue(change.from)} → ${formatAuditValue(change.to)}`;
}

/** Name of a changed key: router settings and per-model settings as the panel names them. */
export function describeAuditSettingKey(key: string): string {
	return ROUTER_SETTING_LABELS[key] ?? ADMIN_SETTING_LABELS[key as AdminSettingKey] ?? key;
}

/** "models_max 2 → 3, pinned none → a +1 more" from `details.changes`, '' when none. */
export function summarizeAuditChanges(changes: unknown): string {
	if (!isRecord(changes)) return '';

	const keys = Object.keys(changes);

	if (keys.length === 0) return '';

	const shown = keys
		.slice(0, ADMIN_AUDIT.SUMMARY_CHANGES_MAX)
		.map((key) => `${key} ${formatAuditChange(changes[key])}`);
	const more = keys.length - shown.length;

	return shown.join(', ') + (more > 0 ? ` +${more} more` : '');
}

/** "401 · wrong admin key" of a refused request. */
function describeRefusal(details: Record<string, unknown>): string {
	const status = num(details.status);
	const reason = str(details.reason);
	const words = ADMIN_AUDIT_REFUSAL_REASONS[reason] ?? (reason ? humanizeCode(reason) : '');

	return [status !== null ? String(status) : null, words || null].filter(Boolean).join(' · ');
}

/** The times a merged record counted: `count` of the record, else of its details. */
export function auditEntryCount(entry: ApiAdminAuditEntry): number | null {
	return num(entry.count) ?? (isRecord(entry.details) ? num(entry.details.count) : null);
}

/**
 * One line under the action: what changed or what happened ("models_max
 * 2 → 3", "2 files, 1.20 MB", "401 · wrong admin key · 3 times"); '' when
 * there is nothing to add.
 */
export function summarizeAuditEntry(entry: ApiAdminAuditEntry): string {
	if (entry.unreadable) return 'This line of the log could not be read';

	const d = isRecord(entry.details) ? entry.details : {};

	if (entry.truncated) return 'The record was longer than 4 KiB and was cut';

	if (entry.result === AdminAuditResult.ERROR) {
		const status = num(d.status);
		const code = str(d.code);

		return [status !== null ? `HTTP ${status}` : null, code || null].filter(Boolean).join(' · ');
	}

	const bytes = num(d.bytes);
	const files = num(d.files);
	const count = auditEntryCount(entry);

	switch (entry.action) {
		case 'settings.update':
		case 'model_settings.update':
		case 'model_settings.reset': {
			if (d.changed === false) return 'No change';

			const changes = summarizeAuditChanges(d.changes);

			if (changes) return changes;

			return entry.action === 'model_settings.reset' ? 'Saved settings removed' : '';
		}
		case 'gpus.rescan': {
			if (str(d.error)) return `failed: ${str(d.error)}`;

			const devices = num(d.devices);

			return devices === null ? '' : `${plural(devices, 'device')} found`;
		}
		case 'upload.create':
			return [
				files !== null ? plural(files, 'file') : null,
				bytes !== null ? formatBytes(bytes) : null
			]
				.filter(Boolean)
				.join(', ');
		case 'upload.cancel':
			return bytes !== null ? `${formatBytes(bytes)} received` : '';
		case 'upload.published':
			return [
				bytes !== null ? formatBytes(bytes) : null,
				str(d.arch) || null,
				d.layout === 'folder' ? 'folder' : d.layout === 'file' ? 'single file' : null
			]
				.filter(Boolean)
				.join(' · ');
		case 'upload.rejected':
		case 'upload.expired': {
			const code = str(d.code);
			const file = num(d.file);

			return [code ? humanizeCode(code) : null, file !== null ? `file ${file + 1}` : null]
				.filter(Boolean)
				.join(' · ');
		}
		case 'upload.resumed_after_restart':
			return str(d.state) ? `state: ${str(d.state)}` : '';
		case 'upload.swept':
			return [str(d.name) || str(d.folder) || null, d.foreign === true ? 'of another server' : null]
				.filter(Boolean)
				.join(' · ');
		case 'model.delete':
			return [
				files !== null ? plural(files, 'file') : null,
				bytes !== null ? formatBytes(bytes) : null,
				d.stopped === true ? 'stopped first' : null
			]
				.filter(Boolean)
				.join(', ');
		case 'model.remove_projector':
			return bytes !== null ? formatBytes(bytes) : '';
		case 'model.start_failed': {
			const reason = str(d.reason);
			const exit = num(d.exit_code);

			return [
				reason ? (ADMIN_FAILURE_LABELS[reason as AdminFailureCode] ?? humanizeCode(reason)) : null,
				exit !== null ? `exit code ${exit}` : null,
				d.settings_reverted === true ? 'settings reverted' : null
			]
				.filter(Boolean)
				.join(' · ');
		}
		case 'model_settings.reverted': {
			const failures = num(d.failures);
			const to = ADMIN_AUDIT_REVERT_TARGETS[str(d.to)] ?? str(d.to);

			return [
				failures !== null ? `after ${plural(failures, 'failed start')}` : null,
				to ? `back to ${to}` : null
			]
				.filter(Boolean)
				.join(', ');
		}
		case 'model_settings.reverted_at_startup': {
			const to = ADMIN_AUDIT_REVERT_TARGETS[str(d.to)] ?? str(d.to);

			return to ? `back to ${to}` : '';
		}
		case 'audit.started':
			return [str(d.version) || null, d.persisted === false ? 'in memory only' : null]
				.filter(Boolean)
				.join(' · ');
		case 'audit.persisted_again': {
			const lost = num(d.lost) ?? 0;

			return lost > 0 ? `${plural(lost, 'record')} lost meanwhile` : 'nothing lost';
		}
		case 'access.refused': {
			const clients = num(d.clients);

			return [
				describeRefusal(d) || null,
				count !== null && count > 1 ? `${count} times` : null,
				clients !== null && clients > 1 ? `${clients} clients` : null
			]
				.filter(Boolean)
				.join(' · ');
		}
		case 'access.refused_more': {
			const clients = num(d.clients);

			return [
				count !== null ? `${count} more` : null,
				clients !== null
					? `from ${plural(clients, 'client')}${d.clients_capped === true ? ' or more' : ''}`
					: null
			]
				.filter(Boolean)
				.join(' ');
		}
		case 'observed.more':
			return count !== null ? `${plural(count, 'more event')}` : '';
		default:
			return '';
	}
}

/** One detail value in words, by the meaning of its key. */
function formatDetailValue(key: string, value: unknown): string {
	if (value === null || value === undefined) return '—';

	if (BYTE_KEYS.has(key) && num(value) !== null) return formatBytes(value);

	if (TIME_KEYS.has(key)) return formatAuditIsoTime(value) || String(value);

	if (typeof value === 'boolean') return value ? 'yes' : 'no';

	if (key === 'reason' && typeof value === 'string') {
		return (
			ADMIN_AUDIT_REFUSAL_REASONS[value] ?? ADMIN_FAILURE_LABELS[value as AdminFailureCode] ?? value
		);
	}

	if (key === 'to' && typeof value === 'string') return ADMIN_AUDIT_REVERT_TARGETS[value] ?? value;

	if (key === 'file' && num(value) !== null) return `${(value as number) + 1}`;

	if (typeof value === 'object') return JSON.stringify(value);

	return String(value);
}

/**
 * The expanded details of a record: the changed keys first ("from → to"),
 * the placement of an upload, then every other field of `details` in the
 * server's order. Unknown fields are shown with their key, so a newer
 * server's records stay readable.
 */
export function auditDetailRows(entry: ApiAdminAuditEntry): AdminAuditDetailRow[] {
	const rows: AdminAuditDetailRow[] = [];
	const d = isRecord(entry.details) ? entry.details : {};
	const changeCount = isRecord(d.changes) ? Object.keys(d.changes).length : 0;

	if (isRecord(d.changes)) {
		for (const [key, change] of Object.entries(d.changes)) {
			rows.push({
				label: describeAuditSettingKey(key),
				mono: true,
				value: formatAuditChange(change)
			});
		}
	}

	if (isRecord(d.settings)) {
		const list = Object.entries(d.settings).map(
			([key, value]) => `${key} = ${formatAuditValue(value)}`
		);

		if (list.length > 0) {
			rows.push({
				label: ADMIN_AUDIT_DETAIL_LABELS.settings,
				list,
				mono: true,
				value: list.join(', ')
			});
		}
	}

	for (const [key, value] of Object.entries(d)) {
		if (SKIPPED_KEYS.has(key)) continue;

		// "changed: yes" says nothing the change rows above do not
		if (key === 'changed' && value === true && changeCount > 0) continue;

		const label = ADMIN_AUDIT_DETAIL_LABELS[key] ?? key;
		const mono = MONO_KEYS.has(key);

		if (Array.isArray(value)) {
			const list = value.map((item) => (typeof item === 'string' ? item : JSON.stringify(item)));
			const capped =
				(key === 'items' && d.items_capped === true) ||
				(key === 'sample' && d.clients_capped === true);

			rows.push({
				label,
				list: capped ? [...list, '(more not listed)'] : list,
				mono,
				value: list.length === 0 ? 'none' : list.join(', ')
			});

			continue;
		}

		let text = formatDetailValue(key, value);

		if (key === 'clients' && d.clients_capped === true) text += ' or more';

		rows.push({ label, mono, value: text });
	}

	return rows;
}

/** The actor in full: address, TCP peer, the forwarded header and whether it is verified, User-Agent. */
export function auditActorRows(
	actor: ApiAdminAuditActor | null | undefined
): AdminAuditDetailRow[] {
	if (!actor || typeof actor !== 'object') return [{ label: 'Actor', value: 'unknown' }];

	if (actor.type !== AdminAuditActorType.CLIENT) {
		const view = describeAuditActor(actor);

		return [{ label: 'Actor', value: str(actor.label) || view.primary }];
	}

	const rows: AdminAuditDetailRow[] = [
		{ label: 'Client address', mono: true, value: str(actor.client) || 'unknown' },
		{ label: 'TCP peer', mono: true, value: str(actor.peer) || 'unknown' }
	];

	if (actor.via_proxy) rows.push({ label: 'Came through', value: 'a trusted proxy' });

	if (str(actor.forwarded_for)) {
		rows.push({
			label: 'X-Forwarded-For',
			mono: true,
			value: `${str(actor.forwarded_for)}${actor.forwarded_trusted === true ? '' : ' (unverified)'}`
		});
	}

	if (str(actor.note)) rows.push({ label: 'Note', value: str(actor.note) });

	if (str(actor.user_agent)) rows.push({ label: 'User-Agent', value: str(actor.user_agent) });

	return rows;
}

/** Facts of the record itself: number, kind, action name, times, where it is kept. */
export function auditRecordRows(entry: ApiAdminAuditEntry): AdminAuditDetailRow[] {
	const rows: AdminAuditDetailRow[] = [
		{ label: 'Number', mono: true, value: `#${entry.seq}` },
		{ label: 'Kind', value: describeAuditKind(entry.kind) },
		{ label: 'Action', mono: true, value: entry.action }
	];
	const time = auditEntryTime(entry);

	if (time) {
		rows.push({
			label: 'Time',
			value: entry.time ? `${time} (${formatAuditUtc(entry.time)})` : time
		});
	}

	if (entry.open) {
		rows.push({ label: 'Merge window', value: 'still counting (refresh for the final count)' });
	} else if (entry.updated_seq !== undefined && entry.updated_seq !== entry.seq) {
		rows.push({
			label: 'Final count',
			mono: true,
			value: `written as record #${entry.updated_seq}`
		});
	}

	rows.push({
		label: 'Kept',
		value: entry.persisted
			? 'in the audit file of the state directory'
			: 'in memory only: lost when the server restarts'
	});

	return rows;
}

/** The record as JSON for "Raw record" (the cursor fields of the answer left out). */
export function formatAuditRaw(entry: ApiAdminAuditEntry): string {
	const raw: Record<string, unknown> = { ...entry };

	delete raw.id;

	return JSON.stringify(raw, null, 2);
}

/** The file name of `Content-Disposition`, else the default name. */
export function auditDownloadName(contentDisposition: string | null | undefined): string {
	const header = contentDisposition ?? '';
	const star = /filename\*\s*=\s*(?:UTF-8'')?([^;]+)/i.exec(header);
	const plain = /filename\s*=\s*"([^"]*)"|filename\s*=\s*([^;]+)/i.exec(header);

	let name = '';

	if (star) {
		try {
			name = decodeURIComponent(star[1].trim().replace(/^"|"$/g, ''));
		} catch {
			name = '';
		}
	}

	if (!name && plain) name = (plain[1] ?? plain[2] ?? '').trim();

	// only a plain file name: no directories, no control characters
	name = Array.from(name.replace(/[\\/]/g, '_'))
		.filter((ch) => ch.charCodeAt(0) >= 0x20)
		.join('');

	return name || ADMIN_AUDIT.DOWNLOAD_NAME;
}

/** Banner of an audit log that is held in memory only. */
export function describeAuditNotPersisted(status: ApiAdminAuditStatus | null | undefined): string {
	const parts: string[] = [];
	const since = status?.since ? formatAuditIsoTime(status.since) : '';
	const ring = num(status?.ring_max) ?? 1000;

	parts.push(
		`The server cannot write the audit log to its state directory${since ? ` since ${since}` : ''}${
			status?.error ? ` (${status.error})` : ''
		}.`
	);
	parts.push(
		`New records are kept in memory only (the last ${ring} of each file) and are lost when the server restarts.`
	);

	const lost = num(status?.lost) ?? 0;

	if (lost > 0) parts.push(`${plural(lost, 'older record')} already dropped out of memory.`);

	parts.push(
		'The server tries to write again every minute and saves the records from memory once it can; make the state directory writable.'
	);

	return parts.join(' ');
}
