/**
 * adminAuditStore - the Activity tab of the admin panel (stage 3 milestone
 * M3): the records of GET /admin/audit, newest first, page by page.
 *
 * The filters are sent to the server (kind, action, result); "Load more"
 * asks for the records before the oldest one shown (`next_before`). A
 * change of the filters or a refresh starts again from the newest record,
 * and an answer to an older request is dropped; when the first page of new
 * filters fails, the records of the old ones go too. Every call goes through
 * adminStore with its admin key (a rejected key goes back to the key form,
 * a 403 hides the admin panel). The records hold client addresses, so they
 * are dropped when the tab goes away (clear()).
 */

import { ADMIN_AUDIT } from '$lib/constants';
import { AdminFeature } from '$lib/enums';
import { AdminApiError, AdminService } from '$lib/services/admin.service';
// direct imports between stores, not via the barrel, to avoid circular deps
import { adminStore } from '$lib/stores/admin.svelte';
import type {
	AdminAuditDownload,
	AdminAuditFilters,
	ApiAdminAuditEntry,
	ApiAdminAuditList,
	ApiAdminAuditStatus
} from '$lib/types/admin-audit';
import {
	auditRemaining,
	buildAuditQuery,
	defaultAuditFilters,
	isAuditFiltered,
	mergeAuditEntries,
	normalizeAuditKinds
} from '$lib/utils/admin-audit';

function errorText(error: unknown): string {
	if (error instanceof AdminApiError && error.status === 400 && error.param) {
		return `The server refused the ${error.param} of the request: ${error.message}`;
	}

	return error instanceof Error ? error.message : String(error);
}

class AdminAuditStore {
	/** A download of the raw log is running */
	downloading = $state(false);
	entries = $state<ApiAdminAuditEntry[]>([]);
	/** Why the first page could not be read */
	error = $state<string | null>(null);
	filters = $state<AdminAuditFilters>(defaultAuditFilters());
	/**
	 * A first page was read since the tab opened (false again when a first
	 * page of other filters failed: the old records were dropped)
	 */
	loaded = $state(false);
	/** Wall time of the last answer */
	loadedAt = $state<number | null>(null);
	/** The first page is being read */
	loading = $state(false);
	loadingMore = $state(false);
	/** Why "Load more" failed (the records shown stay) */
	moreError = $state<string | null>(null);
	/** `before` of the next page, null when every matching record is shown */
	nextBefore = $state<string | null>(null);
	/** Matching records older than the ones shown */
	remaining = $state(0);
	/** `audit` of the last answer: persisted, error, since, in memory, lost */
	status = $state<ApiAdminAuditStatus | null>(null);

	/** Bumped by every new first page and by clear(): older answers are dropped */
	private generation = 0;
	/**
	 * The filters the records shown were read with (their query without
	 * cursor and limit), null when none are shown. A first page of other
	 * filters that fails must not leave them under the new filters.
	 */
	private shownFilters: string | null = null;

	/** The server has the audit log (GET /admin/status `features`). */
	get isAvailable(): boolean {
		return adminStore.hasFeature(AdminFeature.AUDIT);
	}

	get isFiltered(): boolean {
		return isAuditFiltered(this.filters);
	}

	/** False when the server holds the log in memory only (audit_not_persisted). */
	get persisted(): boolean {
		return this.statusInfo?.persisted ?? true;
	}

	/**
	 * Where the log is kept: the last answer of the audit route, else the
	 * status probe. null when neither is known yet.
	 */
	get statusInfo(): ApiAdminAuditStatus | null {
		return this.status ?? adminStore.status?.audit ?? null;
	}

	/** Every matching record: the ones shown plus the older ones not loaded yet. */
	get total(): number {
		return this.entries.length + this.remaining;
	}

	/** Forget the records (the tab went away, or the key was dropped). */
	clear(): void {
		this.generation++;
		this.dropEntries();
		this.status = null;
		this.error = null;
		this.moreError = null;
		this.loading = false;
		this.loadingMore = false;
		this.loadedAt = null;
	}

	/**
	 * The raw log as JSON lines (oldest first) with the current filters, as
	 * a file to save; null when it failed (the error is thrown to the caller).
	 */
	async download(): Promise<AdminAuditDownload | null> {
		if (!this.isAvailable || this.downloading) return null;

		this.downloading = true;

		try {
			const query = buildAuditQuery(this.filters, { format: 'jsonl' });

			return await adminStore.runWithKey((key) => AdminService.downloadAudit(key, query));
		} finally {
			this.downloading = false;
		}
	}

	/** Read the newest page with the current filters (also "Refresh"). */
	async load(): Promise<void> {
		if (!this.isAvailable) return;

		const generation = ++this.generation;
		const filters = buildAuditQuery(this.filters);
		const query = buildAuditQuery(this.filters, { limit: ADMIN_AUDIT.PAGE_SIZE });

		this.loading = true;
		this.loadingMore = false;
		this.moreError = null;

		try {
			const page = await adminStore.runWithKey((key) => AdminService.getAudit(key, query));

			if (generation !== this.generation) return;

			this.entries = mergeAuditEntries([], page.entries ?? []);
			this.shownFilters = filters;
			this.applyPage(page);
			this.error = null;
			this.loaded = true;
		} catch (error) {
			if (generation !== this.generation) return;

			this.error = errorText(error);

			// a failed refresh keeps its records; those of other filters do not match the ones selected
			if (this.shownFilters !== filters) this.dropEntries();
		} finally {
			if (generation === this.generation) this.loading = false;
		}
	}

	/** Read the records before the oldest one shown. */
	async loadMore(): Promise<void> {
		const before = this.nextBefore;

		if (!this.isAvailable || !before || this.loading || this.loadingMore) return;

		// the cursor belongs to the filters the records shown were read with
		if (this.shownFilters !== buildAuditQuery(this.filters)) return;

		const generation = this.generation;
		const query = buildAuditQuery(this.filters, { before, limit: ADMIN_AUDIT.PAGE_SIZE });

		this.loadingMore = true;
		this.moreError = null;

		try {
			const page = await adminStore.runWithKey((key) => AdminService.getAudit(key, query));

			if (generation !== this.generation) return;

			this.entries = mergeAuditEntries(this.entries, page.entries ?? []);
			this.applyPage(page);
		} catch (error) {
			if (generation !== this.generation) return;

			this.moreError = errorText(error);
		} finally {
			if (generation === this.generation) this.loadingMore = false;
		}
	}

	/** Back to every record. */
	resetFilters(): void {
		this.setFilters(defaultAuditFilters());
	}

	/** Change some filters and read the newest matching page. */
	setFilters(filters: Partial<AdminAuditFilters>): void {
		const next = { ...this.filters, ...filters };

		next.kinds = normalizeAuditKinds(next.kinds);

		if (
			next.action === this.filters.action &&
			next.result === this.filters.result &&
			next.kinds.join(',') === this.filters.kinds.join(',')
		) {
			return;
		}

		this.filters = next;
		void this.load();
	}

	private applyPage(page: ApiAdminAuditList): void {
		this.nextBefore = page.next_before ?? null;
		this.remaining = this.nextBefore ? auditRemaining(page) : 0;
		this.loadedAt = Date.now();

		if (page.audit && typeof page.audit.persisted === 'boolean') {
			this.status = page.audit;
			adminStore.applyAuditStatus(page.audit);
		}
	}

	/** No records shown: the first page has to be read again. */
	private dropEntries(): void {
		this.entries = [];
		this.nextBefore = null;
		this.remaining = 0;
		this.loaded = false;
		this.shownFilters = null;
	}
}

export const adminAuditStore = new AdminAuditStore();
