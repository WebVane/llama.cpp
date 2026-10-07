// the real layout: the column widths of the table are Tailwind classes
import '../../src/app.css';
import AdminActivityTab from '$lib/components/app/admin/AdminActivityTab.svelte';
import { AdminFeature } from '$lib/enums';
import { AdminApiError, AdminService } from '$lib/services/admin.service';
import { adminStore } from '$lib/stores/admin.svelte';
import { adminAuditStore } from '$lib/stores/admin-audit.svelte';
import type {
	ApiAdminAuditEntry,
	ApiAdminAuditList,
	ApiAdminAuditStatus
} from '$lib/types/admin-audit';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { page, userEvent } from 'vitest/browser';
import { render } from 'vitest-browser-svelte';

/**
 * Client-side tests of the Activity tab (admin panel, stage 3 M3): the
 * table of GET /admin/audit, the filters as they reach the server, "Load
 * more" with the cursor, the expanded details, the in-memory banner and
 * the download. The server is a fake of AdminService.getAudit that filters
 * and pages like server-admin-audit.cpp.
 */

const PERSISTED: ApiAdminAuditStatus = {
	error: null,
	in_memory: 0,
	lost: 0,
	max_bytes: 4194304,
	persisted: true,
	ring_max: 1000,
	since: null
};

function record(seq: number, partial: Partial<ApiAdminAuditEntry> = {}): ApiAdminAuditEntry {
	return {
		action: 'model_settings.update',
		actor: {
			client: '127.0.0.1',
			label: '127.0.0.1',
			peer: '127.0.0.1',
			type: 'client',
			user_agent: 'test',
			via_proxy: false
		},
		details: {
			changed: true,
			changes: { 'ctx-size': { from: String(seq), to: String(seq + 1) } },
			restart: false,
			restarted: false,
			was_running: false
		},
		id: String(seq),
		kind: 'change',
		open: false,
		persisted: true,
		result: 'ok',
		seq,
		target: 'tiny-chat',
		time: '2026-10-07T15:42:55Z',
		ts: Date.UTC(2026, 9, 7, 15, 42, 55) + seq * 1000,
		...partial
	};
}

/** 130 records: 120 changes, 6 refusals (one from an unverified proxy), 4 observed. */
function seed(): ApiAdminAuditEntry[] {
	const out: ApiAdminAuditEntry[] = [];

	for (let seq = 1; seq <= 130; seq++) {
		if (seq % 20 === 0) {
			out.push(
				record(seq, {
					action: 'access.refused',
					actor: {
						client: '172.18.0.3',
						forwarded_for: '10.64.0.7',
						forwarded_trusted: false,
						label: '172.18.0.3 (X-Forwarded-For 10.64.0.7, unverified)',
						peer: '172.18.0.3',
						type: 'client',
						via_proxy: false
					},
					count: 3,
					details: {
						code: 'admin_key_required',
						count: 3,
						reason: 'wrong_key',
						status: 401
					},
					kind: 'refusal',
					result: 'refused',
					target: '/admin/settings'
				})
			);
		} else if (seq % 33 === 0) {
			out.push(
				record(seq, {
					action: 'download.failed',
					actor: { label: 'public API (unknown)', type: 'public_api' },
					details: {},
					kind: 'observed',
					result: 'failed',
					target: 'user/repo:Q4_K_M'
				})
			);
		} else {
			out.push(record(seq));
		}
	}

	return out;
}

/** GET /admin/audit of the fake server: filters, newest first, `before` and `limit`. */
function answer(
	records: ApiAdminAuditEntry[],
	query: string,
	audit: ApiAdminAuditStatus
): ApiAdminAuditList {
	const params = new URLSearchParams(query.replace(/^\?/, ''));
	const list = (name: string) => (params.get(name) ?? '').split(',').filter(Boolean);
	const kinds = list('kind');
	const actions = list('action');
	const results = list('result');
	const before = Number(params.get('before') ?? 0);
	const limit = Number(params.get('limit') ?? 100);
	const match = records
		.filter(
			(r) =>
				(!before || r.seq < before) &&
				(kinds.length === 0 || kinds.includes(r.kind)) &&
				(actions.length === 0 ||
					actions.some((a) => r.action === a || r.action.startsWith(`${a}.`))) &&
				(results.length === 0 || results.includes(r.result))
		)
		.sort((a, b) => b.seq - a.seq);
	const entries = match.slice(0, limit);
	const more = entries.length < match.length;

	return {
		audit,
		entries,
		matched: match.length,
		next_before: more ? entries[entries.length - 1].id : null,
		persisted: audit.persisted,
		warnings: audit.persisted ? [] : ['audit_not_persisted']
	};
}

describe('AdminActivityTab', () => {
	let records: ApiAdminAuditEntry[];
	let queries: string[];
	let audit: ApiAdminAuditStatus;
	let fail: AdminApiError | null;

	beforeEach(() => {
		records = seed();
		queries = [];
		audit = PERSISTED;
		fail = null;
		vi.spyOn(adminStore, 'hasFeature').mockImplementation(
			(feature) => feature === AdminFeature.AUDIT
		);
		vi.spyOn(adminStore, 'runWithKey').mockImplementation((run) => run('test-key'));
		vi.spyOn(adminStore, 'applyAuditStatus').mockImplementation(() => {});
		vi.spyOn(AdminService, 'getAudit').mockImplementation(async (_key, query) => {
			queries.push(query);

			if (fail) throw fail;

			return answer(records, query, audit);
		});
		adminAuditStore.clear();
		adminAuditStore.resetFilters();
		adminAuditStore.clear();
		queries = [];
	});

	afterEach(() => {
		vi.restoreAllMocks();
		document.body.innerHTML = '';
	});

	/** Shown: the tab reads the newest page by itself. */
	async function open() {
		const screen = render(AdminActivityTab, { visible: true });

		await vi.waitFor(() =>
			expect(adminAuditStore.loaded || adminAuditStore.error !== null).toBe(true)
		);
		await expect.element(page.getByTestId('admin-activity')).toBeVisible();

		return screen;
	}

	function rows() {
		return document.querySelectorAll('[data-audit-id]');
	}

	it('shows the newest page, then loads the older records with the cursor', async () => {
		await open();

		await expect.element(page.getByTestId('audit-table')).toBeVisible();
		expect(rows()).toHaveLength(100);
		expect(rows()[0].getAttribute('data-audit-id')).toBe('130');
		await expect
			.element(page.getByTestId('audit-count'))
			.toHaveTextContent('Showing 100 of 130 records');
		expect(queries).toEqual(['?limit=100']);

		await page.getByTestId('audit-load-more').click();

		await vi.waitFor(() => expect(rows()).toHaveLength(130));
		expect(queries[1]).toBe('?before=31&limit=100');
		expect(rows()[129].getAttribute('data-audit-id')).toBe('1');
		await expect.element(page.getByTestId('audit-count')).toHaveTextContent('130 records');
		expect(document.querySelector('[data-testid="audit-load-more"]')).toBeNull();
	});

	it('sends the filters to the server and starts again from the newest record', async () => {
		await open();

		await page.getByRole('button', { exact: true, name: 'Refusals' }).click();

		await vi.waitFor(() => expect(queries.at(-1)).toBe('?limit=100&kind=refusal'));
		await vi.waitFor(() => expect(rows()).toHaveLength(6));
		expect([...rows()].every((row) => row.getAttribute('data-kind') === 'refusal')).toBe(true);
		await expect
			.element(page.getByTestId('audit-count'))
			.toHaveTextContent('6 records matching the filters');

		await page.getByRole('button', { exact: true, name: 'Public API' }).click();
		await vi.waitFor(() => expect(queries.at(-1)).toBe('?limit=100&kind=observed,refusal'));
		await vi.waitFor(() => expect(rows()).toHaveLength(9));

		await page.getByTestId('audit-reset-filters').click();
		await vi.waitFor(() => expect(queries.at(-1)).toBe('?limit=100'));
		await vi.waitFor(() => expect(rows()).toHaveLength(100));
	});

	it('filters by action group and result group', async () => {
		await open();

		await page.getByTestId('audit-result-filter').click();
		await page.getByRole('option', { name: 'Errors and failures' }).click();

		await vi.waitFor(() => expect(queries.at(-1)).toBe('?limit=100&result=error,failed,rejected'));
		await vi.waitFor(() => expect(rows()).toHaveLength(3));

		await page.getByTestId('audit-action-filter').click();
		await page.getByRole('option', { name: 'Uploads' }).click();

		await vi.waitFor(() =>
			expect(queries.at(-1)).toBe('?limit=100&action=upload&result=error,failed,rejected')
		);
		await expect.element(page.getByTestId('audit-empty')).toHaveTextContent('No records match');
	});

	it('shows the actor, the summary and the result of a refusal', async () => {
		await open();

		const row = page.getByTestId('audit-table').getByText('Request refused').first();

		await expect.element(row).toBeVisible();

		const refused = document.querySelector('[data-audit-id="120"]') as HTMLElement;

		expect(refused.textContent).toContain('172.18.0.3');
		expect(refused.textContent).toContain('X-Forwarded-For 10.64.0.7');
		expect(refused.querySelector('[data-testid="audit-unverified"]')).not.toBeNull();
		expect(refused.textContent).toContain('401 · wrong admin key · 3 times');
		expect(refused.querySelector('[data-result="refused"]')?.textContent).toContain('Refused');
	});

	it('expands the details of a record and hides them again', async () => {
		await open();

		const toggle = page.getByRole('button', { name: 'details of record 130' });

		await expect.element(toggle).toHaveAttribute('aria-expanded', 'false');
		await toggle.click();

		const details = page.getByTestId('audit-details');

		await expect.element(details).toBeVisible();
		await expect.element(details).toHaveTextContent('Context size');
		await expect.element(details).toHaveTextContent('130 → 131');
		await expect.element(details).toHaveTextContent('User-Agent');
		await expect.element(details).toHaveTextContent('#130');
		await expect.element(toggle).toHaveAttribute('aria-expanded', 'true');

		await page.getByTestId('audit-raw-toggle').click();
		await expect.element(page.getByTestId('audit-raw')).toHaveTextContent('"seq": 130');

		await toggle.click();
		expect(document.querySelector('[data-testid="audit-details"]')).toBeNull();
		await expect.element(toggle).toHaveAttribute('aria-expanded', 'false');
	});

	it('names the details toggle by its visible time (label in name)', async () => {
		await open();

		const button = document.querySelector(
			'[data-audit-id="130"] [data-testid="audit-toggle"]'
		) as HTMLElement;
		const visible = (button.querySelector('span[title]') as HTMLElement).textContent?.trim();

		expect(visible).toMatch(/^2026-10-07 \d\d:\d\d:\d\d$/);
		expect(button.getAttribute('aria-label')?.startsWith(`${visible},`)).toBe(true);
		await expect
			.element(page.getByRole('button', { exact: true, name: `${visible}, details of record 130` }))
			.toBeVisible();
	});

	it('shows a banner when the log is held in memory only', async () => {
		audit = {
			...PERSISTED,
			error: 'the admin state directory does not exist',
			in_memory: 2,
			lost: 5,
			persisted: false,
			since: '2026-10-07T15:59:23Z'
		};
		records = [record(2, { persisted: false }), record(1, { persisted: false })];
		await open();

		const banner = page.getByTestId('audit-memory-banner');

		await expect.element(banner).toHaveTextContent('kept in memory only');
		await expect.element(banner).toHaveTextContent('the admin state directory does not exist');
		await expect.element(banner).toHaveTextContent('5 older records already dropped');
		expect(document.querySelectorAll('[data-testid="audit-memory-only"]')).toHaveLength(2);
		expect(adminStore.applyAuditStatus).toHaveBeenCalledWith(audit);
	});

	it('has no banner and says so when nothing was recorded', async () => {
		records = [];
		await open();

		await expect
			.element(page.getByTestId('audit-empty'))
			.toHaveTextContent('Nothing has been recorded');
		expect(document.querySelector('[data-testid="audit-memory-banner"]')).toBeNull();
	});

	it('explains a failed read and tries again', async () => {
		fail = new AdminApiError('Request timed out', 0);
		await open();

		await expect.element(page.getByTestId('audit-error')).toHaveTextContent('Request timed out');

		fail = null;
		await page.getByRole('button', { name: 'Try again' }).click();
		await vi.waitFor(() => expect(rows()).toHaveLength(100));
		expect(document.querySelector('[data-testid="audit-error"]')).toBeNull();
	});

	it('drops the records of the old filters when the new ones cannot be read', async () => {
		await open();
		expect(rows()).toHaveLength(100);

		fail = new AdminApiError('Internal server error', 500);
		await page.getByRole('button', { exact: true, name: 'Refusals' }).click();

		await expect
			.element(page.getByTestId('audit-error'))
			.toHaveTextContent('Internal server error');
		await expect
			.element(page.getByRole('button', { exact: true, name: 'Refusals' }))
			.toHaveAttribute('aria-pressed', 'true');
		// nothing of kind=change under the Refusals chip, and no cursor of the old filters
		expect(rows()).toHaveLength(0);
		expect(document.querySelector('[data-testid="audit-count"]')).toBeNull();
		expect(document.querySelector('[data-testid="audit-load-more"]')).toBeNull();
		expect(document.querySelector('[data-testid="audit-empty"]')).toBeNull();
		expect(adminAuditStore.nextBefore).toBeNull();
		expect(adminAuditStore.remaining).toBe(0);

		await adminAuditStore.loadMore();
		expect(queries.some((query) => query.includes('before='))).toBe(false);

		fail = null;
		await page.getByRole('button', { name: 'Try again' }).click();
		await vi.waitFor(() => expect(rows()).toHaveLength(6));
		expect([...rows()].every((row) => row.getAttribute('data-kind') === 'refusal')).toBe(true);
		expect(queries.at(-1)).toBe('?limit=100&kind=refusal');

		// a failed refresh of the same filters keeps its records: they still match
		fail = new AdminApiError('Internal server error', 500);
		await page.getByTestId('audit-refresh').click();
		await expect.element(page.getByTestId('audit-error')).toBeVisible();
		expect(rows()).toHaveLength(6);
		await expect
			.element(page.getByTestId('audit-count'))
			.toHaveTextContent('6 records matching the filters');
	});

	it('never mixes a cursor into a page of other filters', async () => {
		await open();
		expect(adminAuditStore.nextBefore).toBe('31');

		// the filters change, their first page has not arrived yet
		let release: () => void = () => {};

		const slow = new Promise<void>((resolve) => (release = resolve));

		vi.mocked(AdminService.getAudit).mockImplementation(async (_key, query) => {
			queries.push(query);
			await slow;

			throw new AdminApiError('Internal server error', 500);
		});
		adminAuditStore.setFilters({ kinds: ['refusal' as never] });
		await adminAuditStore.loadMore();
		release();
		await vi.waitFor(() => expect(adminAuditStore.loading).toBe(false));

		expect(queries).toEqual(['?limit=100', '?limit=100&kind=refusal']);
		expect(adminAuditStore.entries).toHaveLength(0);
	});

	it('lays the records out by the width of the panel, not of the window', async () => {
		await page.viewport(1400, 900);

		try {
			// the window is wide; the panel next to an open sidebar is not
			const panel = document.createElement('div');

			panel.style.width = '700px';
			document.body.appendChild(panel);
			render(AdminActivityTab, { props: { visible: true }, target: panel });
			await vi.waitFor(() => expect(rows()).toHaveLength(100));

			const head = () =>
				document.querySelector('[data-testid="audit-action-head"]') as HTMLElement | null;

			// cards: one cell per record, no header
			await vi.waitFor(() => expect(head()).toBeNull());
			expect(rows()[0].querySelectorAll('td')).toHaveLength(1);

			// an iPad in portrait without the sidebar, or a 1280 px laptop with it
			panel.style.width = '940px';
			await vi.waitFor(() => expect(head()).not.toBeNull());
			expect(rows()[0].querySelectorAll('td')).toHaveLength(5);
			expect(head()!.getBoundingClientRect().width).toBeGreaterThanOrEqual(300);

			const summary = rows()[0].querySelector('[data-testid="audit-summary"]') as HTMLElement;

			// the summary ("Context size 130 → 131") stays on one line
			expect(summary.getBoundingClientRect().height).toBeLessThan(24);

			// the full width of the admin page
			panel.style.width = '1120px';
			await vi.waitFor(() =>
				expect(head()!.getBoundingClientRect().width).toBeGreaterThanOrEqual(400)
			);
		} finally {
			await page.viewport(414, 896);
		}
	});

	it('keeps the keyboard focus in the panel', async () => {
		await open();

		const refresh = document.querySelector('[data-testid="audit-refresh"]') as HTMLElement;

		refresh.focus();
		await userEvent.keyboard('{Enter}');
		await vi.waitFor(() => expect(queries).toHaveLength(2));
		expect(document.activeElement).toBe(refresh);
		await vi.waitFor(() => expect(adminAuditStore.loading).toBe(false));
		expect(document.activeElement).toBe(refresh);

		// the last page takes "Load more" away: on to the first record it added
		(document.querySelector('[data-testid="audit-load-more"]') as HTMLElement).focus();
		await userEvent.keyboard('{Enter}');
		await vi.waitFor(() => expect(rows()).toHaveLength(130));
		await vi.waitFor(() =>
			expect(document.activeElement).toBe(
				document.querySelector('[data-audit-id="30"] [data-testid="audit-toggle"]')
			)
		);

		// "Clear filters" goes away with its press: on to "All"
		await page.getByRole('button', { exact: true, name: 'Refusals' }).click();
		await vi.waitFor(() => expect(rows()).toHaveLength(6));
		(document.querySelector('[data-testid="audit-reset-filters"]') as HTMLElement).focus();
		await userEvent.keyboard('{Enter}');
		await vi.waitFor(() => expect(rows()).toHaveLength(100));
		await vi.waitFor(() =>
			expect(document.activeElement).toBe(document.querySelector('button[data-kind="all"]'))
		);

		// "Try again" goes away when the read works: on to Refresh
		fail = new AdminApiError('Request timed out', 0);
		refresh.click();
		await expect.element(page.getByTestId('audit-error')).toBeVisible();
		fail = null;
		(document.querySelector('[data-testid="audit-error"] button') as HTMLElement).focus();
		await userEvent.keyboard('{Enter}');
		await vi.waitFor(() =>
			expect(document.querySelector('[data-testid="audit-error"]')).toBeNull()
		);
		await vi.waitFor(() => expect(document.activeElement).toBe(refresh));
	});

	it('drops the answer of a request that a newer filter replaced', async () => {
		let release: () => void = () => {};

		const slow = new Promise<void>((resolve) => (release = resolve));

		vi.mocked(AdminService.getAudit).mockImplementation(async (_key, query) => {
			queries.push(query);

			if (query === '?limit=100') await slow;

			return answer(records, query, audit);
		});
		// shown: the first (slow) request starts by itself
		render(AdminActivityTab, { visible: true });
		await vi.waitFor(() => expect(queries).toEqual(['?limit=100']));

		adminAuditStore.setFilters({ kinds: ['observed' as never] });
		await vi.waitFor(() => expect(rows()).toHaveLength(3));
		release();
		await new Promise((resolve) => setTimeout(resolve, 50));
		expect(rows()).toHaveLength(3);
		expect(adminAuditStore.loading).toBe(false);
	});

	it('reads nothing while hidden, and the newest records each time it is shown', async () => {
		const screen = render(AdminActivityTab, { visible: false });

		await new Promise((resolve) => setTimeout(resolve, 50));
		expect(queries).toEqual([]);

		await screen.rerender({ visible: true });
		await vi.waitFor(() => expect(rows()).toHaveLength(100));
		await screen.rerender({ visible: false });
		records.push(record(131));
		await screen.rerender({ visible: true });
		await vi.waitFor(() => expect(queries).toEqual(['?limit=100', '?limit=100']));
		await vi.waitFor(() => expect(rows()[0].getAttribute('data-audit-id')).toBe('131'));
	});

	it('downloads the raw log with the filters as a file', async () => {
		const download = vi.spyOn(AdminService, 'downloadAudit').mockResolvedValue({
			blob: new Blob(['{"seq":1}\n'], { type: 'application/x-ndjson' }),
			filename: 'provision-admin-audit.jsonl'
		});
		const click = vi.spyOn(HTMLAnchorElement.prototype, 'click').mockImplementation(() => {});

		await open();
		await page.getByRole('button', { exact: true, name: 'Server' }).click();
		await vi.waitFor(() => expect(queries.at(-1)).toBe('?limit=100&kind=server'));
		await page.getByTestId('audit-download').click();

		await vi.waitFor(() => expect(click).toHaveBeenCalledTimes(1));
		expect(download).toHaveBeenCalledWith('test-key', '?format=jsonl&kind=server');
		expect((click.mock.contexts[0] as HTMLAnchorElement).download).toBe(
			'provision-admin-audit.jsonl'
		);
	});

	it('forgets the records when the tab goes away', async () => {
		const screen = await open();

		expect(adminAuditStore.entries.length).toBe(100);
		screen.unmount();
		expect(adminAuditStore.entries).toHaveLength(0);
		expect(adminAuditStore.loaded).toBe(false);
	});
});
