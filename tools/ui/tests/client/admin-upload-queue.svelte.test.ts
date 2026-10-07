import { AdminFeature } from '$lib/enums';
import { AdminUploadFileRole, AdminUploadJobPhase } from '$lib/enums/admin.enums';
import { AdminApiError, AdminService } from '$lib/services/admin.service';
import { AdminUploadService } from '$lib/services/admin-upload.service';
import { adminStore } from '$lib/stores/admin.svelte';
import { adminUploadStore } from '$lib/stores/admin-upload.svelte';
import type {
	ApiAdminUploadCreate,
	ApiAdminUploadCreateResult,
	ApiAdminUploadInfo,
	ApiAdminUploadSession,
	ApiAdminUploadSummary
} from '$lib/types/admin-upload';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

/**
 * Client-side tests of the upload queue (admin panel, stage 3): a model whose
 * session cannot be created must not stop the models queued after it, and a
 * create refused with too_many_uploads waits in the queue until one of the
 * server's open uploads ends. The server is replaced by stubs of AdminService
 * and of the chunk transport (every chunk lands, the checks pass at once).
 */

const SIZE = 4096;

function summary(id: string, state = 'receiving'): ApiAdminUploadSummary {
	return {
		auto_publish: false,
		blocked: null,
		blocked_by: null,
		created: new Date().toISOString(),
		error: null,
		expires_at: null,
		id,
		last_activity: new Date().toISOString(),
		n_files: 1,
		name: id,
		received_bytes: 0,
		state,
		substate: null,
		total_bytes: SIZE
	};
}

function session(id: string, name: string, state: string): ApiAdminUploadSession {
	return {
		...summary(id, state),
		blocked_by: state === 'ready' ? 'manual' : null,
		checks: [],
		chunk_default: 8 * 1048576,
		chunk_max: 16 * 1048576,
		facts: null,
		files: [
			{
				complete: state !== 'receiving',
				index: 0,
				name: `${name}.gguf`,
				offset: state === 'receiving' ? 0 : SIZE,
				role: 'model',
				size: SIZE
			}
		] as unknown as ApiAdminUploadSession['files'],
		idle_ms: 0,
		last_client_idle_ms: null,
		name,
		plan: null,
		progress: null,
		record_persisted: true,
		recovered: false,
		recovered_without_record: false,
		result: null,
		settings: {},
		settings_mode: 'replace',
		warnings: []
	};
}

function info(sessions: ApiAdminUploadSummary[]): ApiAdminUploadInfo {
	return {
		available: true,
		limits: {
			chunk_default: 8 * 1048576,
			chunk_max: 16 * 1048576,
			chunk_min: 245760,
			max_sessions: 4
		},
		message: null,
		models_dir: 'models',
		reason: null,
		sessions,
		settings_error: null,
		settings_writable: true,
		staging_bytes: 0
	} as unknown as ApiAdminUploadInfo;
}

function request(name: string) {
	const file = new File([new Uint8Array(SIZE)], `${name}.gguf`, { lastModified: 1 });

	return {
		autoPublish: false,
		expectedSha256: {},
		files: [
			{
				file,
				header: null,
				key: 0,
				name: file.name,
				problems: [],
				role: AdminUploadFileRole.MODEL,
				shard: null,
				size: SIZE
			}
		],
		name,
		settings: {},
		settingsMode: 'replace' as const
	};
}

function phaseOf(name: string): AdminUploadJobPhase | undefined {
	return adminUploadStore.jobs.find((job) => job.name === name)?.phase;
}

describe('upload queue', () => {
	let creates: ApiAdminUploadCreate[];
	let createAnswer: (body: ApiAdminUploadCreate) => ApiAdminUploadCreateResult;
	let serverSessions: ApiAdminUploadSummary[];

	const created = new Map<string, string>();

	beforeEach(() => {
		creates = [];
		serverSessions = [];
		created.clear();
		createAnswer = (body) => {
			const id = `id-${body.name}`;

			created.set(id, body.name);

			return {
				disk: {
					free_bytes: 0,
					needed_bytes: 0,
					other_uploads_bytes: 0,
					shares_disk_with: [],
					total_bytes: 0
				},
				locks: {},
				upload: session(id, body.name, 'receiving'),
				warnings: []
			};
		};

		vi.spyOn(adminStore, 'runWithKey').mockImplementation((run) => run('test-key'));
		vi.spyOn(adminStore, 'hasFeature').mockImplementation(
			(feature) => feature === AdminFeature.UPLOAD
		);
		vi.spyOn(adminStore, 'notice').mockImplementation(() => {});
		vi.spyOn(AdminService, 'createUpload').mockImplementation(async (_key, body) => {
			creates.push(body);

			return createAnswer(body);
		});
		vi.spyOn(AdminService, 'getUploads').mockImplementation(async () => info(serverSessions));
		// every session that was sent completely passed its checks and waits for Publish
		vi.spyOn(AdminService, 'getUpload').mockImplementation(async (_key, id) =>
			session(id, created.get(id) ?? id, 'ready')
		);
		vi.spyOn(AdminUploadService, 'createTransport').mockReturnValue({
			getSession: async (id) => ({
				body: session(id, created.get(id) ?? id, 'checking') as unknown as Record<string, unknown>,
				status: 200
			}),
			readSlice: async (file, start, end) =>
				new Uint8Array(await file.slice(start, end).arrayBuffer()),
			sendChunk: async (chunk) => ({
				body: {
					file_complete: chunk.offset + chunk.data.length >= SIZE,
					offset: chunk.offset + chunk.data.length,
					state: chunk.offset + chunk.data.length >= SIZE ? 'checking' : 'receiving'
				},
				status: 200
			})
		});
	});

	afterEach(() => {
		for (const job of [...adminUploadStore.jobs]) adminUploadStore.abandon(job.key);

		vi.restoreAllMocks();
	});

	it('goes on with the next model when a session cannot be created', async () => {
		const refused = createAnswer;

		createAnswer = (body) => {
			if (body.name === 'first') {
				const error = new AdminApiError("the name 'first' is already used", 409, 'name_taken');

				error.details = { by: 'model', other: 'first' };

				throw error;
			}

			return refused(body);
		};

		adminUploadStore.enqueue([request('first'), request('second'), request('third')]);

		await vi.waitFor(() => expect(phaseOf('third')).toBe(AdminUploadJobPhase.READY), {
			timeout: 10_000
		});

		expect(phaseOf('first')).toBe(AdminUploadJobPhase.FAILED);
		expect(phaseOf('second')).toBe(AdminUploadJobPhase.READY);
		expect(creates.map((body) => body.name)).toEqual(['first', 'second', 'third']);
	});

	it('keeps a model queued while the server has its maximum of uploads, then starts it', async () => {
		const open = ['s1', 's2', 's3', 's4'].map((id) => summary(id));

		let full = true;

		const normal = createAnswer;

		serverSessions = open;
		createAnswer = (body) => {
			if (full) {
				const error = new AdminApiError(
					'at most 4 uploads can be open at once',
					409,
					'too_many_uploads'
				);

				error.details = { sessions: open };

				throw error;
			}

			return normal(body);
		};

		adminUploadStore.enqueue([request('waits'), request('after')]);

		await vi.waitFor(() => expect(creates).toHaveLength(1));
		await adminUploadStore.loadInfo();

		const waiting = adminUploadStore.jobs.find((job) => job.name === 'waits');

		expect(waiting?.phase).toBe(AdminUploadJobPhase.QUEUED);
		expect(waiting?.errorType).toBe('too_many_uploads');
		expect(waiting?.error).toMatch(/already has 4 uploads open/);
		expect(phaseOf('after')).toBe(AdminUploadJobPhase.QUEUED);
		// still full: nothing is tried again
		expect(creates).toHaveLength(1);

		// one of the open uploads was cancelled (in another tab, or in the banner)
		full = false;
		serverSessions = [summary('s1', 'cancelled'), ...open.slice(1)];
		await adminUploadStore.loadInfo();

		await vi.waitFor(() => expect(phaseOf('after')).toBe(AdminUploadJobPhase.READY), {
			timeout: 10_000
		});

		expect(phaseOf('waits')).toBe(AdminUploadJobPhase.READY);
		expect(adminUploadStore.jobs.find((job) => job.name === 'waits')?.error).toBeNull();
		expect(creates.map((body) => body.name)).toEqual(['waits', 'waits', 'after']);
	});
});
