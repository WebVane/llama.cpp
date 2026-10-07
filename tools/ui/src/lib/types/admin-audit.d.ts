/**
 * Admin panel, stage 3 milestone M3: the audit log of admin actions. Wire
 * shapes of GET /admin/audit and of the `audit` objects of /admin/status,
 * /admin/storage, plus the view models of the Activity tab.
 * Imported by path ('$lib/types/admin-audit'), not through the types barrel.
 */

import type {
	AdminAuditActionGroup,
	AdminAuditKind,
	AdminAuditResultGroup
} from '$lib/enums/admin.enums';

/** `audit` of GET /admin/status and of GET /admin/audit. */
export interface ApiAdminAuditStatus {
	/** False: at least one of the files cannot be written, its records are in memory only */
	persisted: boolean;
	/** Why writing failed (the first store that failed), null when persisted */
	error: string | null;
	/** ISO time of the first write error of this episode */
	since: string | null;
	/** Records held in the memory rings now */
	in_memory: number;
	/** Records dropped from a full memory ring in this episode */
	lost: number;
	/** Rotation size of each file */
	max_bytes: number;
	/** Records each memory ring keeps */
	ring_max: number;
}

/** One file of `audit.files` (GET /admin/storage, the diagnostics item). */
export interface ApiAdminAuditFile {
	name: string;
	bytes: number;
	/**
	 * changes: admin-audit.jsonl, refusals: admin-audit-refusals.jsonl,
	 * observed: admin-audit-observed.jsonl (a newer server may add others)
	 */
	store: string;
}

/** `audit` of GET /admin/storage. */
export interface ApiAdminAuditStorage {
	persisted: boolean;
	files: ApiAdminAuditFile[];
	total_bytes: number;
	max_bytes: number;
}

/**
 * `actor` of a record. A client carries its addresses; the other types
 * only a label (see AdminAuditActorType).
 */
export interface ApiAdminAuditActor {
	/** client | server | public_api | various | unknown */
	type: string;
	label?: string;
	/** TCP peer, "unix socket" when there is none */
	peer?: string;
	/** The client address the allow-list checks (peer, or the hop from a trusted proxy); "" when unknown */
	client?: string;
	/** The client address came through a trusted proxy */
	via_proxy?: boolean;
	/** Raw X-Forwarded-For (at most 128 B) */
	forwarded_for?: string;
	/** False: the peer is not a trusted proxy, the header is unverified */
	forwarded_trusted?: boolean;
	/** Why a trusted proxy's X-Forwarded-For was not used */
	note?: string;
	/** At most 128 B */
	user_agent?: string;
}

/** One record of GET /admin/audit (newest first). */
export interface ApiAdminAuditEntry {
	seq: number;
	/** The cursor: `before` of the next page */
	id: string;
	kind: string;
	action: string;
	result: string;
	/** Wall time in ms (merged records: the start of the window) */
	ts?: number;
	/** `YYYY-MM-DDTHH:MM:SSZ` */
	time?: string;
	/** Model id, request path, settings, gpus ... ("" when none) */
	target?: string;
	actor?: ApiAdminAuditActor;
	details?: Record<string, unknown>;
	/** False: held in memory only (lost on restart) */
	persisted: boolean;
	/** A merge window still counting (refusals, observed.more) */
	open: boolean;
	/** Merged records: how many times it happened */
	count?: number;
	/** Merged records: the seq of the latest update line */
	updated_seq?: number;
	/** The line could not be parsed: only seq, kind, action and result are known */
	unreadable?: boolean;
	/** The line was longer than 4 KiB: everything but the head was dropped */
	truncated?: boolean;
}

/** GET /admin/audit (JSON). */
export interface ApiAdminAuditList {
	entries: ApiAdminAuditEntry[];
	/** `before` of the next page, null on the last one */
	next_before: string | null;
	/** Every record matching the filters before `before`, this page included */
	matched: number;
	persisted: boolean;
	audit: ApiAdminAuditStatus;
	/** [] or ['audit_not_persisted'] */
	warnings: string[];
}

/** Filters of the Activity tab. */
export interface AdminAuditFilters {
	/** Empty: every kind */
	kinds: AdminAuditKind[];
	action: AdminAuditActionGroup;
	result: AdminAuditResultGroup;
}

/** Paging and format of one GET /admin/audit request. */
export interface AdminAuditRequest {
	before?: string | null;
	limit?: number;
	format?: 'json' | 'jsonl';
}

/** The actor column of a record. */
export interface AdminAuditActorView {
	/** `primary` is an address (shown in a monospace font) */
	address: boolean;
	/** The address, "Server", "Public API" ... */
	primary: string;
	/** "via trusted proxy 127.0.0.1", "X-Forwarded-For 10.0.0.7" */
	secondary: string | null;
	/** The X-Forwarded-For header did not come from a trusted proxy */
	unverified: boolean;
	/** Full description for the tooltip */
	title: string;
}

/** One line of the expanded details of a record. */
export interface AdminAuditDetailRow {
	label: string;
	value: string;
	/** Paths, hashes, codes: shown in a monospace font */
	mono?: boolean;
	/** Several values (paths, hashes, a sample of clients) */
	list?: string[];
}

/** A file to save (GET /admin/audit?format=jsonl). */
export interface AdminAuditDownload {
	blob: Blob;
	filename: string;
}
