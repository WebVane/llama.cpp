import { ADMIN_AUDIT } from '$lib/constants';
import {
	AdminAuditActionGroup,
	AdminAuditKind,
	AdminAuditResultGroup
} from '$lib/enums/admin.enums';
import type { ApiAdminDiagnosticItem } from '$lib/types';
import type { ApiAdminAuditEntry, ApiAdminAuditStatus } from '$lib/types/admin-audit';
import {
	auditActorRows,
	auditDetailRows,
	auditDownloadName,
	auditEntryCount,
	auditEntryTime,
	auditRecordRows,
	auditRemaining,
	auditResultTone,
	buildAuditQuery,
	defaultAuditFilters,
	describeAuditAction,
	describeAuditActor,
	describeAuditNotPersisted,
	describeAuditResult,
	describeAuditSettingKey,
	formatAuditChange,
	formatAuditIsoTime,
	formatAuditRaw,
	formatAuditTime,
	formatAuditUtc,
	formatAuditValue,
	isAuditFiltered,
	mergeAuditEntries,
	normalizeAuditKinds,
	summarizeAuditChanges,
	summarizeAuditEntry,
	toggleAuditKind
} from '$lib/utils/admin-audit';
import { summarizeDiagnosticValue } from '$lib/utils/admin-gpus';
import { describeModelWarning } from '$lib/utils/admin-model-settings';
import { describe, expect, it } from 'vitest';

/**
 * Pure helpers of the Activity tab (admin panel, stage 3 M3): the query of
 * GET /admin/audit, paging, and the texts of a record. The shapes follow the
 * server's answers (tools/server/server-admin-audit.cpp).
 */

const CLIENT = {
	client: '127.0.0.1',
	label: '127.0.0.1',
	peer: '127.0.0.1',
	type: 'client',
	user_agent: 'curl/8.7.1',
	via_proxy: false
};

function entry(partial: Partial<ApiAdminAuditEntry> = {}): ApiAdminAuditEntry {
	const seq = partial.seq ?? 1;

	return {
		action: 'settings.update',
		actor: CLIENT,
		details: {},
		id: String(seq),
		kind: 'change',
		open: false,
		persisted: true,
		result: 'ok',
		seq,
		target: 'settings',
		time: '2026-10-07T15:42:55Z',
		ts: Date.UTC(2026, 9, 7, 15, 42, 55),
		...partial
	};
}

function localTime(ms: number): string {
	const d = new Date(ms);
	const p = (n: number) => String(n).padStart(2, '0');

	return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())} ${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`;
}

describe('buildAuditQuery', () => {
	it('sends nothing for the default filters', () => {
		expect(buildAuditQuery(defaultAuditFilters())).toBe('');
	});

	it('adds paging, kinds, actions and results as comma lists', () => {
		const query = buildAuditQuery(
			{
				action: AdminAuditActionGroup.DELETES,
				kinds: [AdminAuditKind.REFUSAL, AdminAuditKind.CHANGE],
				result: AdminAuditResultGroup.PROBLEMS
			},
			{ before: '42', limit: 100 }
		);

		expect(query).toBe(
			'?before=42&limit=100&kind=change,refusal&action=model.delete,model.remove_projector,model.removed&result=error,failed,rejected'
		);
	});

	it('maps every action and result group to the server names', () => {
		const action = (group: AdminAuditActionGroup) =>
			buildAuditQuery({ ...defaultAuditFilters(), action: group });
		const result = (group: AdminAuditResultGroup) =>
			buildAuditQuery({ ...defaultAuditFilters(), result: group });

		expect(action(AdminAuditActionGroup.SETTINGS)).toBe('?action=settings,model_settings');
		expect(action(AdminAuditActionGroup.UPLOADS)).toBe('?action=upload');
		expect(action(AdminAuditActionGroup.DOWNLOADS)).toBe('?action=download');
		expect(action(AdminAuditActionGroup.STARTS)).toBe('?action=model.start_failed');
		expect(action(AdminAuditActionGroup.GPUS)).toBe('?action=gpus');
		expect(action(AdminAuditActionGroup.ACCESS)).toBe('?action=access');
		expect(action(AdminAuditActionGroup.AUDIT_LOG)).toBe('?action=audit');
		expect(action(AdminAuditActionGroup.ALL)).toBe('');
		expect(result(AdminAuditResultGroup.OK)).toBe('?result=ok');
		expect(result(AdminAuditResultGroup.REFUSED)).toBe('?result=refused');
		expect(result(AdminAuditResultGroup.ALL)).toBe('');
	});

	it('treats every kind as no kind filter', () => {
		expect(
			buildAuditQuery({
				...defaultAuditFilters(),
				kinds: [
					AdminAuditKind.SERVER,
					AdminAuditKind.CHANGE,
					AdminAuditKind.OBSERVED,
					AdminAuditKind.REFUSAL
				]
			})
		).toBe('');
	});

	it('keeps the limit inside what the server accepts', () => {
		expect(buildAuditQuery(defaultAuditFilters(), { limit: 0 })).toBe('?limit=1');
		expect(buildAuditQuery(defaultAuditFilters(), { limit: 10_000 })).toBe(
			`?limit=${ADMIN_AUDIT.LIMIT_MAX}`
		);
	});

	it('downloads with the filters but without a limit', () => {
		expect(
			buildAuditQuery(
				{ ...defaultAuditFilters(), kinds: [AdminAuditKind.OBSERVED] },
				{ format: 'jsonl', limit: 100 }
			)
		).toBe('?format=jsonl&kind=observed');
	});

	it('encodes the cursor', () => {
		expect(buildAuditQuery(defaultAuditFilters(), { before: '1&x=2' })).toBe('?before=1%26x%3D2');
	});
});

describe('filters', () => {
	it('normalizes kinds to the filter order without duplicates', () => {
		expect(normalizeAuditKinds(['refusal', 'change', 'change', 'bogus'])).toEqual([
			AdminAuditKind.CHANGE,
			AdminAuditKind.REFUSAL
		]);
		expect(normalizeAuditKinds([])).toEqual([]);
	});

	it('toggles a kind on and off, and every kind on is "all"', () => {
		let kinds = toggleAuditKind([], AdminAuditKind.REFUSAL);

		expect(kinds).toEqual([AdminAuditKind.REFUSAL]);

		kinds = toggleAuditKind(kinds, AdminAuditKind.CHANGE);

		expect(kinds).toEqual([AdminAuditKind.CHANGE, AdminAuditKind.REFUSAL]);
		expect(toggleAuditKind(kinds, AdminAuditKind.REFUSAL)).toEqual([AdminAuditKind.CHANGE]);

		kinds = toggleAuditKind(kinds, AdminAuditKind.SERVER);

		expect(toggleAuditKind(kinds, AdminAuditKind.OBSERVED)).toEqual([]);
	});

	it('tells whether anything is filtered', () => {
		expect(isAuditFiltered(defaultAuditFilters())).toBe(false);
		expect(isAuditFiltered({ ...defaultAuditFilters(), kinds: [AdminAuditKind.SERVER] })).toBe(
			true
		);
		expect(
			isAuditFiltered({ ...defaultAuditFilters(), action: AdminAuditActionGroup.UPLOADS })
		).toBe(true);
		expect(
			isAuditFiltered({ ...defaultAuditFilters(), result: AdminAuditResultGroup.REFUSED })
		).toBe(true);
	});
});

describe('paging', () => {
	it('merges pages newest first, one row per id', () => {
		const first = [entry({ seq: 9 }), entry({ count: 1, open: true, seq: 7 })];
		const page = [entry({ count: 4, seq: 7 }), entry({ seq: 3 }), entry({ seq: 5 })];
		const merged = mergeAuditEntries(first, page);

		expect(merged.map((e) => e.seq)).toEqual([9, 7, 5, 3]);
		expect(merged[1].count).toBe(4);
	});

	it('counts the matching records not loaded yet', () => {
		expect(auditRemaining({ entries: [entry(), entry({ seq: 2 })], matched: 250 })).toBe(248);
		expect(auditRemaining({ entries: [entry()], matched: 1 })).toBe(0);
		expect(auditRemaining({ entries: [entry()], matched: undefined as unknown as number })).toBe(0);
	});
});

describe('times', () => {
	it('formats a wall time in local time', () => {
		const ms = Date.UTC(2026, 0, 2, 3, 4, 5);

		expect(formatAuditTime(ms)).toBe(localTime(ms));
		expect(formatAuditTime(ms)).toMatch(/^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}$/);
		expect(formatAuditTime(undefined)).toBe('');
		expect(formatAuditTime(Number.NaN)).toBe('');
	});

	it('formats an ISO time of the server, and keeps a text that does not parse', () => {
		expect(formatAuditIsoTime('2026-10-07T15:42:55Z')).toBe(
			localTime(Date.UTC(2026, 9, 7, 15, 42, 55))
		);
		expect(formatAuditIsoTime('yesterday-ish')).toBe('yesterday-ish');
		expect(formatAuditIsoTime(null)).toBe('');
	});

	it('takes ts first, then time', () => {
		expect(auditEntryTime(entry())).toBe(localTime(Date.UTC(2026, 9, 7, 15, 42, 55)));
		expect(auditEntryTime({ time: '2026-10-07T15:42:55Z', ts: undefined })).toBe(
			localTime(Date.UTC(2026, 9, 7, 15, 42, 55))
		);
		expect(auditEntryTime({ time: undefined, ts: undefined })).toBe('');
	});
});

describe('describeAuditActor', () => {
	it('shows the client address', () => {
		const view = describeAuditActor(CLIENT);

		expect(view).toMatchObject({ primary: '127.0.0.1', secondary: null, unverified: false });
		expect(view.title).toContain('User-Agent: curl/8.7.1');
	});

	it('names the trusted proxy a verified address came through', () => {
		const view = describeAuditActor({
			client: '10.0.0.5',
			forwarded_for: '10.0.0.5',
			forwarded_trusted: true,
			label: '10.0.0.5 via trusted proxy 127.0.0.1',
			peer: '127.0.0.1',
			type: 'client',
			via_proxy: true
		});

		expect(view).toMatchObject({
			primary: '10.0.0.5',
			secondary: 'via trusted proxy 127.0.0.1',
			unverified: false
		});
		expect(view.title).toContain('TCP peer: 127.0.0.1');
	});

	it('marks an X-Forwarded-For of an untrusted peer as unverified', () => {
		const view = describeAuditActor({
			client: '172.18.0.3',
			forwarded_for: '10.64.0.7',
			forwarded_trusted: false,
			label: '172.18.0.3 (X-Forwarded-For 10.64.0.7, unverified)',
			peer: '172.18.0.3',
			type: 'client',
			via_proxy: false
		});

		expect(view).toMatchObject({
			primary: '172.18.0.3',
			secondary: 'X-Forwarded-For 10.64.0.7',
			unverified: true
		});
		expect(view.title).toContain('unverified');
	});

	it('falls back to the peer when the client is unknown', () => {
		expect(
			describeAuditActor({ client: '', peer: 'unix socket', type: 'client', via_proxy: false })
				.primary
		).toBe('unix socket');
	});

	it('counts the clients of a merged record, and shows only addresses as addresses', () => {
		expect(
			describeAuditActor(
				{ label: 'browsers of refused addresses', type: 'various' },
				{ clients: 300, clients_capped: true }
			)
		).toMatchObject({ address: false, secondary: '300 clients or more' });
		expect(
			describeAuditActor({ label: 'several clients', type: 'various' }, { clients: 1 }).secondary
		).toBe('1 client');
		expect(describeAuditActor(CLIENT).address).toBe(true);
		expect(describeAuditActor({ label: 'server', type: 'server' }).address).toBe(false);
	});

	it('names the server, the public API and merged clients', () => {
		expect(describeAuditActor({ label: 'server', type: 'server' }).primary).toBe('Server');
		expect(describeAuditActor({ label: 'public API (unknown)', type: 'public_api' })).toMatchObject(
			{ primary: 'Public API', secondary: 'caller unknown' }
		);
		expect(
			describeAuditActor({ label: 'browsers of refused addresses', type: 'various' }).primary
		).toBe('Browsers of refused addresses');
		expect(describeAuditActor({ label: 'unknown', type: 'unknown' }).primary).toBe('unknown');
		expect(describeAuditActor(undefined).primary).toBe('unknown');
		expect(describeAuditActor({ label: 'robot', type: 'future_kind' }).primary).toBe('robot');
	});
});

describe('labels', () => {
	it('names actions, falling back to the action name', () => {
		expect(describeAuditAction('model.delete')).toBe('Model files deleted');
		expect(describeAuditAction('access.refused')).toBe('Request refused');
		expect(describeAuditAction('something.new')).toBe('something.new');
	});

	it('names a change the server refused as a failed attempt', () => {
		expect(describeAuditAction('settings.update', 'error')).toBe('Settings change failed');
		expect(describeAuditAction('model.delete', 'error')).toBe('Delete of the model files failed');
		expect(describeAuditAction('settings.update', 'ok')).toBe('Settings changed');
		expect(describeAuditAction('gpus.rescan', 'failed')).toBe('GPUs re-detected');
		expect(describeAuditAction('something.new', 'error')).toBe('something.new');
	});

	it('writes the UTC time of the server', () => {
		expect(formatAuditUtc('2026-10-07T15:42:55Z')).toBe('2026-10-07 15:42:55 UTC');
		expect(formatAuditUtc(undefined)).toBe('');
	});

	it('names results and gives them a tone', () => {
		expect(describeAuditResult('ok')).toBe('OK');
		expect(describeAuditResult('refused')).toBe('Refused');
		expect(describeAuditResult('weird')).toBe('weird');
		expect(auditResultTone('ok')).toBe('success');
		expect(auditResultTone('refused')).toBe('warning');
		expect(auditResultTone('error')).toBe('error');
		expect(auditResultTone('failed')).toBe('error');
		expect(auditResultTone('rejected')).toBe('error');
		expect(auditResultTone('weird')).toBe('neutral');
	});

	it('names settings keys as the panel does', () => {
		expect(describeAuditSettingKey('models_max')).toBe('Max loaded models');
		expect(describeAuditSettingKey('default_device')).toBe('GPU for new models');
		expect(describeAuditSettingKey('ctx-size')).toBe('Context size');
		expect(describeAuditSettingKey('n-cpu-moe')).toBe('n-cpu-moe');
	});

	it('formats stored values and changes', () => {
		expect(formatAuditValue(null)).toBe('unset');
		expect(formatAuditValue('')).toBe('empty');
		expect(formatAuditValue([])).toBe('none');
		expect(formatAuditValue(['a', 'b'])).toBe('a, b');
		expect(formatAuditValue(false)).toBe('false');
		expect(formatAuditValue({ a: 1 })).toBe('{"a":1}');
		expect(formatAuditChange({ from: 2, to: 3 })).toBe('2 → 3');
		expect(formatAuditChange({ from: null, to: '1024' })).toBe('unset → 1024');
		expect(formatAuditChange({ from: ['x'], to: [] })).toBe('x → none');
	});
});

describe('summarizeAuditEntry', () => {
	it('lists what changed, at most three keys', () => {
		expect(
			summarizeAuditEntry(
				entry({ details: { changed: true, changes: { models_max: { from: 2, to: 3 } } } })
			)
		).toBe('models_max 2 → 3');
		expect(
			summarizeAuditChanges({
				a: { from: 1, to: 2 },
				b: { from: 1, to: 2 },
				c: { from: 1, to: 2 },
				d: { from: 1, to: 2 },
				e: { from: 1, to: 2 }
			})
		).toBe('a 1 → 2, b 1 → 2, c 1 → 2 +2 more');
		expect(summarizeAuditChanges({})).toBe('');
		expect(summarizeAuditChanges(null)).toBe('');
	});

	it('says when nothing changed, and what a reset removed', () => {
		expect(
			summarizeAuditEntry(
				entry({
					action: 'model_settings.update',
					details: { changed: false, changes: {}, stored: false }
				})
			)
		).toBe('No change');
		expect(
			summarizeAuditEntry(
				entry({
					action: 'model_settings.reset',
					details: { changed: true, changes: { 'ctx-size': { from: '1024', to: null } } }
				})
			)
		).toBe('ctx-size 1024 → unset');
		expect(
			summarizeAuditEntry(
				entry({ action: 'model_settings.reset', details: { changed: true, changes: {} } })
			)
		).toBe('Saved settings removed');
	});

	it('summarizes uploads', () => {
		expect(
			summarizeAuditEntry(
				entry({ action: 'upload.create', details: { bytes: 1_048_576, files: 1, id: 'u1' } })
			)
		).toBe('1 file, 1.00 MB');
		expect(
			summarizeAuditEntry(
				entry({
					action: 'upload.published',
					details: { arch: 'llama', bytes: 2_097_152, layout: 'folder', sha256: ['ab'] }
				})
			)
		).toBe('2.00 MB · llama · folder');
		expect(
			summarizeAuditEntry(
				entry({
					action: 'upload.rejected',
					details: { code: 'unsupported_architecture', file: 1, id: 'u1' },
					result: 'rejected'
				})
			)
		).toBe('unsupported architecture · file 2');
		expect(
			summarizeAuditEntry(entry({ action: 'upload.cancel', details: { bytes: 512, id: 'u1' } }))
		).toBe('512 B received');
		expect(
			summarizeAuditEntry(
				entry({
					action: 'upload.swept',
					details: { folder: '.provision-upload/abc', foreign: true, name: null }
				})
			)
		).toBe('.provision-upload/abc · of another server');
		expect(
			summarizeAuditEntry(
				entry({ action: 'upload.resumed_after_restart', details: { id: 'u', state: 'receiving' } })
			)
		).toBe('state: receiving');
	});

	it('summarizes deletes, GPU re-detection and failed starts', () => {
		expect(
			summarizeAuditEntry(
				entry({ action: 'model.delete', details: { bytes: 4096, files: 2, stopped: true } })
			)
		).toBe('2 files, 4.00 KB, stopped first');
		expect(
			summarizeAuditEntry(entry({ action: 'model.remove_projector', details: { bytes: 2048 } }))
		).toBe('2.00 KB');
		expect(
			summarizeAuditEntry(entry({ action: 'gpus.rescan', details: { devices: 2, error: null } }))
		).toBe('2 devices found');
		expect(
			summarizeAuditEntry(
				entry({
					action: 'gpus.rescan',
					details: { devices: 0, error: 'timeout' },
					result: 'failed'
				})
			)
		).toBe('failed: timeout');
		expect(
			summarizeAuditEntry(
				entry({
					action: 'model.start_failed',
					details: { exit_code: 1, failures: 2, reason: 'out_of_memory', settings_reverted: true },
					kind: 'observed',
					result: 'failed'
				})
			)
		).toBe('Out of memory · exit code 1 · settings reverted');
	});

	it('summarizes what the server did by itself', () => {
		expect(
			summarizeAuditEntry(
				entry({
					action: 'model_settings.reverted',
					details: { failures: 2, reason: 'out_of_memory', to: 'last_good' }
				})
			)
		).toBe('after 2 failed starts, back to the settings that last worked');
		expect(
			summarizeAuditEntry(
				entry({
					action: 'model_settings.reverted_at_startup',
					details: { phase: 'trial', to: 'models_ini' }
				})
			)
		).toBe('back to the models.ini values');
		expect(
			summarizeAuditEntry(
				entry({
					action: 'audit.started',
					details: { max_bytes: 4194304, persisted: false, version: 'b1-abc' }
				})
			)
		).toBe('b1-abc · in memory only');
		expect(
			summarizeAuditEntry(
				entry({
					action: 'audit.persisted_again',
					details: { lost: 3, not_persisted_since: '', store: 'changes' }
				})
			)
		).toBe('3 records lost meanwhile');
		expect(
			summarizeAuditEntry(entry({ action: 'audit.persisted_again', details: { lost: 0 } }))
		).toBe('nothing lost');
	});

	it('summarizes refused requests with their count', () => {
		const refused = entry({
			action: 'access.refused',
			count: 3,
			details: { code: 'admin_key_required', count: 3, reason: 'wrong_key', status: 401 },
			kind: 'refusal',
			result: 'refused',
			target: '/admin/settings'
		});

		expect(summarizeAuditEntry(refused)).toBe('401 · wrong admin key · 3 times');
		expect(auditEntryCount(refused)).toBe(3);
		expect(
			summarizeAuditEntry(
				entry({
					action: 'access.refused',
					details: { clients: 12, count: 40, probe: true, reason: 'not_allowed', status: 403 },
					kind: 'refusal',
					result: 'refused'
				})
			)
		).toBe('403 · address not allowed · 40 times · 12 clients');
		expect(
			summarizeAuditEntry(
				entry({
					action: 'access.refused_more',
					count: 900,
					details: { clients: 256, clients_capped: true, count: 900 },
					kind: 'refusal',
					result: 'refused'
				})
			)
		).toBe('900 more from 256 clients or more');
		expect(
			summarizeAuditEntry(
				entry({ action: 'observed.more', count: 7, details: { count: 7 }, kind: 'observed' })
			)
		).toBe('7 more events');
	});

	it('shows the answer of a change that failed', () => {
		expect(
			summarizeAuditEntry(
				entry({
					action: 'model_settings.update',
					details: { code: 'restart_required', status: 409 },
					result: 'error'
				})
			)
		).toBe('HTTP 409 · restart_required');
	});

	it('says when a line could not be read or was cut', () => {
		expect(summarizeAuditEntry(entry({ details: undefined, unreadable: true }))).toBe(
			'This line of the log could not be read'
		);
		expect(summarizeAuditEntry(entry({ details: undefined, truncated: true }))).toContain('4 KiB');
		expect(summarizeAuditEntry(entry({ action: 'brand.new', details: { x: 1 } }))).toBe('');
	});
});

describe('auditDetailRows', () => {
	it('puts the changes first, by their names in the panel', () => {
		const rows = auditDetailRows(
			entry({
				action: 'model_settings.update',
				details: {
					changed: true,
					changes: { 'ctx-size': { from: null, to: '1024' } },
					restart: false,
					restarted: false,
					was_running: false
				}
			})
		);

		expect(rows[0]).toEqual({ label: 'Context size', mono: true, value: 'unset → 1024' });
		expect(rows.find((row) => row.label === 'Changed anything')).toBeUndefined();
		expect(rows.find((row) => row.label === 'Was running')?.value).toBe('no');
	});

	it('keeps "changed: no" of a request that changed nothing', () => {
		const rows = auditDetailRows(
			entry({ details: { changed: false, changes: {}, keys: ['models_max'] } })
		);

		expect(rows.find((row) => row.label === 'Changed anything')?.value).toBe('no');
		expect(rows.find((row) => row.label === 'Fields sent')?.list).toEqual(['models_max']);
	});

	it('formats sizes, lists, placements and indexes', () => {
		const rows = auditDetailRows(
			entry({
				action: 'upload.published',
				details: {
					arch: 'llama',
					bytes: 1_048_576,
					id: 'abc',
					layout: 'file',
					settings: { 'ctx-size': '2048', device: 'CUDA0' },
					sha256: ['aa11', 'bb22']
				}
			})
		);

		expect(rows[0]).toMatchObject({
			label: 'Placement',
			list: ['ctx-size = 2048', 'device = CUDA0']
		});
		expect(rows.find((row) => row.label === 'Size')?.value).toBe('1.00 MB');
		expect(rows.find((row) => row.label === 'SHA-256')).toMatchObject({
			list: ['aa11', 'bb22'],
			mono: true
		});
		expect(rows.find((row) => row.label === 'Upload session')).toMatchObject({
			mono: true,
			value: 'abc'
		});
		expect(
			auditDetailRows(entry({ action: 'upload.rejected', details: { file: 0 } }))[0].value
		).toBe('1');
	});

	it('shows times locally, reasons in words and capped lists', () => {
		const rows = auditDetailRows(
			entry({
				action: 'access.refused_more',
				details: {
					clients: 256,
					clients_capped: true,
					count: 900,
					first: '2026-10-07T15:42:55Z',
					items: ['/admin/a', '/admin/b'],
					items_capped: true,
					note: 'more refusals than the log keeps',
					reason: 'no_key',
					sample: ['10.0.0.1']
				},
				kind: 'refusal'
			})
		);

		expect(rows.find((row) => row.label === 'First')?.value).toBe(
			localTime(Date.UTC(2026, 9, 7, 15, 42, 55))
		);
		expect(rows.find((row) => row.label === 'Clients')?.value).toBe('256 or more');
		expect(rows.find((row) => row.label === 'Items')?.list).toEqual([
			'/admin/a',
			'/admin/b',
			'(more not listed)'
		]);
		expect(rows.find((row) => row.label === 'Some of the clients')?.list).toEqual([
			'10.0.0.1',
			'(more not listed)'
		]);
		expect(rows.find((row) => row.label === 'Reason')?.value).toBe('no admin key');
		expect(rows.some((row) => row.label === 'clients_capped' || row.label === 'items_capped')).toBe(
			false
		);
	});

	it('shows unknown fields with their key, and a cut record as text', () => {
		const rows = auditDetailRows(
			entry({ details: { future_field: { a: 1 }, text: '{"x"', truncated: true } })
		);

		expect(rows.find((row) => row.label === 'future_field')?.value).toBe('{"a":1}');
		expect(rows.find((row) => row.label === 'Cut to fit')?.value).toBe('yes');
		expect(rows.find((row) => row.label === 'Text')?.value).toBe('{"x"');
		expect(auditDetailRows(entry({ details: undefined }))).toEqual([]);
	});

	it('names the revert target and the failure reason', () => {
		const rows = auditDetailRows(
			entry({
				action: 'model.start_failed',
				details: { exit_code: 1, reason: 'invalid_device', to: 'models_ini' }
			})
		);

		expect(rows.find((row) => row.label === 'Reason')?.value).toBe('Device not found');
		expect(rows.find((row) => row.label === 'Reverted to')?.value).toBe('the models.ini values');
	});
});

describe('actor and record rows', () => {
	it('lists the addresses of a client', () => {
		expect(
			auditActorRows({
				...CLIENT,
				forwarded_for: '10.64.0.7',
				forwarded_trusted: false,
				note: 'X-Forwarded-For has more hops than trusted proxies'
			})
		).toEqual([
			{ label: 'Client address', mono: true, value: '127.0.0.1' },
			{ label: 'TCP peer', mono: true, value: '127.0.0.1' },
			{ label: 'X-Forwarded-For', mono: true, value: '10.64.0.7 (unverified)' },
			{ label: 'Note', value: 'X-Forwarded-For has more hops than trusted proxies' },
			{ label: 'User-Agent', value: 'curl/8.7.1' }
		]);
		expect(
			auditActorRows({
				...CLIENT,
				forwarded_for: '10.0.0.5',
				forwarded_trusted: true,
				via_proxy: true
			})
		).toContainEqual({ label: 'X-Forwarded-For', mono: true, value: '10.0.0.5' });
		expect(auditActorRows({ label: 'server', type: 'server' })).toEqual([
			{ label: 'Actor', value: 'server' }
		]);
		expect(auditActorRows(null)).toEqual([{ label: 'Actor', value: 'unknown' }]);
	});

	it('describes the record: number, kind, time, merge window, where it is kept', () => {
		const rows = auditRecordRows(
			entry({ action: 'access.refused', kind: 'refusal', open: true, persisted: false, seq: 12 })
		);

		expect(rows[0]).toEqual({ label: 'Number', mono: true, value: '#12' });
		expect(rows.find((row) => row.label === 'Kind')?.value).toBe('Refusals');
		expect(rows.find((row) => row.label === 'Time')?.value).toContain('(2026-10-07 15:42:55 UTC)');
		expect(rows.find((row) => row.label === 'Merge window')?.value).toContain('still counting');
		expect(rows.find((row) => row.label === 'Kept')?.value).toContain('memory only');

		const closed = auditRecordRows(entry({ seq: 4, updated_seq: 9 }));

		expect(closed.find((row) => row.label === 'Final count')?.value).toBe('written as record #9');
		expect(closed.find((row) => row.label === 'Kept')?.value).toContain('audit file');
	});

	it('shows the raw record without the cursor', () => {
		const raw = JSON.parse(formatAuditRaw(entry({ seq: 5 })));

		expect(raw.seq).toBe(5);
		expect(raw.id).toBeUndefined();
	});
});

describe('auditDownloadName', () => {
	it('takes the name of Content-Disposition', () => {
		expect(auditDownloadName('attachment; filename="provision-admin-audit.jsonl"')).toBe(
			'provision-admin-audit.jsonl'
		);
		expect(auditDownloadName("attachment; filename*=UTF-8''audit%20log.jsonl")).toBe(
			'audit log.jsonl'
		);
		expect(auditDownloadName('attachment; filename=plain.jsonl')).toBe('plain.jsonl');
	});

	it('never returns a path, and falls back to the default name', () => {
		expect(auditDownloadName('attachment; filename="../../etc/passwd"')).toBe('.._.._etc_passwd');
		expect(auditDownloadName(null)).toBe(ADMIN_AUDIT.DOWNLOAD_NAME);
		expect(auditDownloadName('attachment')).toBe(ADMIN_AUDIT.DOWNLOAD_NAME);
		expect(auditDownloadName('attachment; filename=""')).toBe(ADMIN_AUDIT.DOWNLOAD_NAME);
	});
});

describe('the log held in memory', () => {
	const status: ApiAdminAuditStatus = {
		error: 'the admin state directory does not exist',
		in_memory: 3,
		lost: 0,
		max_bytes: 4194304,
		persisted: false,
		ring_max: 1000,
		since: '2026-10-07T15:59:23Z'
	};

	it('explains the banner', () => {
		const text = describeAuditNotPersisted(status);

		expect(text).toContain(localTime(Date.UTC(2026, 9, 7, 15, 59, 23)));
		expect(text).toContain('the admin state directory does not exist');
		expect(text).toContain('the last 1000 of each file');
		expect(text).toContain('lost when the server restarts');
		expect(text).not.toContain('dropped out of memory');
		expect(describeAuditNotPersisted({ ...status, lost: 2 })).toContain(
			'2 older records already dropped out of memory'
		);
		expect(describeAuditNotPersisted(null)).toContain('cannot write the audit log');
	});

	it('words the status warning and the diagnostics item', () => {
		expect(describeModelWarning('audit_not_persisted')).toContain('kept in memory only');

		const item = (value: unknown): ApiAdminDiagnosticItem => ({
			hint: '',
			id: 'audit',
			label: 'Audit log',
			status: 'ok',
			value
		});

		expect(
			summarizeDiagnosticValue(
				item({
					...status,
					files: [
						{ bytes: 1024, name: 'admin-audit.jsonl', store: 'changes' },
						{ bytes: 1024, name: 'admin-audit-refusals.jsonl', store: 'refusals' }
					],
					persisted: true
				})
			)
		).toBe('written to the state directory (2 files, 2.00 KB)');
		expect(summarizeDiagnosticValue(item({ ...status, files: [] }))).toBe(
			'in memory only (3 records)'
		);
	});
});
