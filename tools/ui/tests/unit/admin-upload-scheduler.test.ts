import {
	error,
	FakeClock,
	type FakeHook,
	fakeTransport,
	FakeUploadServer,
	pattern,
	settle
} from './admin-upload-fixtures';
import { AdminUploadSendState } from '$lib/enums/admin.enums';
import type { AdminUploadSendSnapshot } from '$lib/types/admin-upload';
import {
	AdminUploadScheduler,
	type AdminUploadSchedulerOptions,
	verifyUploadedStart
} from '$lib/utils/admin-upload-scheduler';
import { afterEach, describe, expect, it } from 'vitest';

const KIB = 1024;
const MIB = 1024 * 1024;

interface Setup {
	ceilings: [number, boolean][];
	clock: FakeClock;
	datas: Uint8Array[];
	finished: { serverState: string | null; state: AdminUploadSendState } | null;
	scheduler: AdminUploadScheduler;
	server: FakeUploadServer;
	states: AdminUploadSendState[];
	transport: ReturnType<typeof fakeTransport>;
}

function setup(
	sizes: number[],
	options: {
		hooks?: FakeHook[];
		hang?: () => boolean;
		files?: Partial<AdminUploadSchedulerOptions['files'][number]>[];
		scheduler?: Partial<AdminUploadSchedulerOptions>;
		/** Bytes already on the server per file (the start of the right data) */
		stored?: number[];
	} = {}
): Setup {
	const clock = new FakeClock();
	const server = new FakeUploadServer(clock, sizes);
	const datas = sizes.map((size, index) => pattern(size, index + 1));

	server.hooks = options.hooks ?? [];
	(options.stored ?? []).forEach((bytes, index) => {
		server.files[index].data.set(datas[index].subarray(0, bytes));
		server.files[index].offset = bytes;
	});

	const transport = fakeTransport(server, { hang: options.hang });
	const result = {
		ceilings: [] as [number, boolean][],
		clock,
		datas,
		finished: null as Setup['finished'],
		server,
		states: [] as AdminUploadSendState[],
		transport
	};
	const scheduler = new AdminUploadScheduler({
		chunkDefault: 8 * MIB,
		chunkMax: 16 * MIB,
		chunkMin: 240 * KIB,
		clock,
		files: sizes.map((size, index) => ({
			blob: new Blob([datas[index] as unknown as BlobPart]),
			complete: false,
			headerEnd: null,
			index,
			offset: options.stored?.[index] ?? 0,
			size,
			...(options.files?.[index] ?? {})
		})),
		onCeiling: (ceiling, proxy) => result.ceilings.push([ceiling, proxy]),
		onFinished: (state, serverState) => (result.finished = { serverState, state }),
		onUpdate: (snapshot: AdminUploadSendSnapshot) => {
			if (result.states.at(-1) !== snapshot.state) result.states.push(snapshot.state);
		},
		sessionId: 'sid',
		transport,
		...(options.scheduler ?? {})
	});

	live.push(scheduler);

	return Object.assign(result, { scheduler });
}

/** Every scheduler of the current test, stopped afterwards so none keeps retrying in the background. */
const live: AdminUploadScheduler[] = [];

afterEach(() => {
	for (const scheduler of live.splice(0)) scheduler.stop();
});

async function run(t: Setup, until: () => boolean = () => t.finished !== null): Promise<void> {
	t.scheduler.start();
	await settle(until);
}

function stateOf(t: Setup): AdminUploadSendState {
	return t.scheduler.snapshot.state;
}

function sameBytes(t: Setup, index: number): boolean {
	const stored = t.server.files[index].data;
	const want = t.datas[index];

	if (stored.length !== want.length) return false;

	for (let i = 0; i < want.length; i++) if (stored[i] !== want[i]) return false;

	return true;
}

function accepted(t: Setup) {
	return t.server.chunks.filter((chunk) => !chunk.verify);
}

describe('AdminUploadScheduler', () => {
	it('sends the header of every file first, then file by file, and the server ends up with the same bytes', async () => {
		const t = setup([20 * MIB, 3 * MIB], { files: [{ headerEnd: MIB }, { headerEnd: MIB }] });

		await run(t);

		expect(t.finished).toEqual({ serverState: 'checking', state: AdminUploadSendState.DONE });
		expect(accepted(t)[0].file).toBe(0);
		expect(accepted(t)[1].file).toBe(1);
		expect(
			accepted(t)
				.slice(2)
				.every((chunk) => chunk.file === 0)
		).toBe(true);
		expect(sameBytes(t, 0)).toBe(true);
		expect(sameBytes(t, 1)).toBe(true);
		expect(t.scheduler.snapshot.confirmed).toBe(23 * MIB);
	});

	it('doubles the chunk size while chunks are fast, up to chunk_max', async () => {
		const t = setup([64 * MIB]);

		await run(t);

		const lengths = accepted(t).map((chunk) => chunk.length);

		expect(lengths[0]).toBe(8 * MIB);
		expect(lengths[1]).toBe(16 * MIB);
		expect(Math.max(...lengths)).toBe(16 * MIB);
		expect(sameBytes(t, 0)).toBe(true);
	});

	it('halves the chunk size when chunks are slow, down to the floor', async () => {
		const t = setup([24 * MIB]);

		t.server.chunkMs = 20_000;
		await run(t);

		const lengths = accepted(t).map((chunk) => chunk.length);

		expect(lengths.slice(0, 4)).toEqual([8 * MIB, 4 * MIB, 2 * MIB, MIB]);
		expect(Math.min(...lengths.slice(0, -1))).toBeGreaterThanOrEqual(240 * KIB);
		expect(sameBytes(t, 0)).toBe(true);
	});

	it('steps down to 960 KiB when a proxy answers an HTML 413, and keeps that ceiling', async () => {
		const t = setup([10 * MIB], {
			hooks: [(request) => (request.data.length > 1_000_000 ? { body: null, status: 413 } : null)]
		});

		await run(t);

		expect(t.finished?.state).toBe(AdminUploadSendState.DONE);
		expect(t.ceilings).toEqual([[960 * KIB, true]]);
		expect(t.scheduler.snapshot.proxyLimit).toBe(960 * KIB);
		expect(accepted(t).every((chunk) => chunk.length <= 960 * KIB)).toBe(true);
		expect(sameBytes(t, 0)).toBe(true);
	});

	it('starts at a ceiling found earlier in the browser session', async () => {
		const t = setup([3 * MIB], { scheduler: { ceiling: 480 * KIB } });

		await run(t);

		expect(accepted(t).every((chunk) => chunk.length <= 480 * KIB)).toBe(true);
	});

	it('keeps under the JSON 413 chunk_too_large limit of the server (no proxy step-down)', async () => {
		const t = setup([12 * MIB], {
			hooks: [
				(request) =>
					request.data.length > 4 * MIB
						? error(413, 'chunk_too_large', { chunk_max: 4 * MIB })
						: null
			]
		});

		await run(t);

		expect(t.finished?.state).toBe(AdminUploadSendState.DONE);
		expect(accepted(t).every((chunk) => chunk.length <= 4 * MIB)).toBe(true);
		expect(t.scheduler.snapshot.proxyLimit).toBeNull();
		expect(t.ceilings).toEqual([]);
	});

	it('reads the session after a dropped connection and steps down after two drops of the first chunk of a size', async () => {
		const t = setup([10 * MIB], {
			hooks: [(request) => (request.data.length > 1_000_000 ? { body: null, status: 0 } : null)]
		});

		await run(t);

		expect(t.finished?.state).toBe(AdminUploadSendState.DONE);
		expect(t.server.gets).toBeGreaterThanOrEqual(2);
		expect(t.ceilings).toEqual([[960 * KIB, false]]);
		expect(t.scheduler.snapshot.proxyLimit).toBeNull();
		expect(sameBytes(t, 0)).toBe(true);
	});

	it('does not send again a chunk that landed when only its answer was lost', async () => {
		let lose = true;

		const t = setup([20 * MIB], {
			hooks: [
				(request, server) => {
					if (!lose) return null;

					lose = false;
					server.chunk(request);

					return { body: null, status: 0 };
				}
			]
		});

		await run(t);

		const offsets = accepted(t).map((chunk) => chunk.offset);

		expect(new Set(offsets).size).toBe(offsets.length);
		expect(t.server.gets).toBe(1);
		expect(sameBytes(t, 0)).toBe(true);
	});

	it('continues at the server offset after 409 offset_mismatch (the server lost the tail in a restart)', async () => {
		const t = setup([6 * MIB], { files: [{ offset: 4 * MIB }], stored: [2 * MIB] });

		t.scheduler = new AdminUploadScheduler({
			chunkDefault: 8 * MIB,
			chunkMax: 16 * MIB,
			chunkMin: 240 * KIB,
			clock: t.clock,
			files: [
				{
					blob: new Blob([t.datas[0] as unknown as BlobPart]),
					complete: false,
					headerEnd: null,
					index: 0,
					offset: 4 * MIB,
					size: 6 * MIB
				}
			],
			onFinished: (state, serverState) => (t.finished = { serverState, state }),
			sessionId: 'sid',
			transport: t.transport
		});
		live.push(t.scheduler);
		await run(t);

		expect(t.transport.requests[0].offset).toBe(4 * MIB);
		expect(t.transport.requests[1].offset).toBe(2 * MIB);
		expect(t.finished?.state).toBe(AdminUploadSendState.DONE);
		expect(sameBytes(t, 0)).toBe(true);
	});

	it('stops at "not the same file" until the right file is picked, then resumes with an overlap', async () => {
		const t = setup([8 * MIB], {
			files: [{ blob: new Blob([pattern(8 * MIB, 9) as unknown as BlobPart]) }],
			scheduler: { overlapFirst: true },
			stored: [4 * MIB]
		});

		await run(t, () => stateOf(t) === AdminUploadSendState.NOT_SAME_FILE);

		expect(t.scheduler.snapshot.problem?.file).toBe(0);
		expect(t.transport.requests[0].offset).toBe(3 * MIB);

		t.scheduler.setFileBlob(0, new Blob([t.datas[0] as unknown as BlobPart]));
		t.scheduler.resume();
		await settle(() => t.finished !== null);

		expect(t.finished?.state).toBe(AdminUploadSendState.DONE);
		expect(t.transport.requests.at(-1)?.offset).toBe(3 * MIB);
		expect(sameBytes(t, 0)).toBe(true);
	});

	it('waits while another tab sends; Take over sends the next chunk with takeover=1 once', async () => {
		let otherTab = true;

		const t = setup([20 * MIB], {
			hooks: [
				(request) =>
					otherTab && !request.takeover
						? error(409, 'other_client_active', { idle_ms: 1200 })
						: null
			]
		});

		await run(t, () => stateOf(t) === AdminUploadSendState.OTHER_CLIENT);

		expect(t.scheduler.snapshot.problem?.idleMs).toBe(1200);

		otherTab = false;
		t.scheduler.resume({ takeover: true });
		await settle(() => t.finished !== null);

		const takeovers = t.transport.requests.map((request) => request.takeover);

		expect(takeovers.filter(Boolean)).toHaveLength(1);
		expect(t.finished?.state).toBe(AdminUploadSendState.DONE);
	});

	it('tells another client from a lost answer after a dropped connection', async () => {
		let first = true;

		const t = setup([20 * MIB], {
			hooks: [
				(_request, server) => {
					if (!first) return null;

					first = false;
					server.lastClientAt = server.clock.now(); // someone else wrote a moment ago

					return { body: null, status: 0 };
				}
			]
		});

		await run(t, () => stateOf(t) === AdminUploadSendState.OTHER_CLIENT);

		expect(stateOf(t)).toBe(AdminUploadSendState.OTHER_CLIENT);
		t.scheduler.stop();
	});

	it('backs off 1 s, 2 s, 4 s ... up to 30 s while the server restarts (503)', async () => {
		let left = 8;

		const t = setup([MIB], {
			hooks: [() => (left-- > 0 ? error(503, 'unavailable_error') : null)]
		});

		await run(t);

		expect(t.clock.sleeps.slice(0, 8)).toEqual([
			1000, 2000, 4000, 8000, 16000, 30000, 30000, 30000
		]);
		expect(t.states).toContain(AdminUploadSendState.SERVER_RESTARTING);
		expect(t.finished?.state).toBe(AdminUploadSendState.DONE);
	});

	it('after 30 min of failures says "unreachable" and retries every minute', async () => {
		const t = setup([MIB], { hooks: [() => error(502, 'bad_gateway')] });

		await run(t, () => t.clock.sleeps.filter((ms) => ms === 60_000).length >= 2);

		expect(t.states).toContain(AdminUploadSendState.RETRYING);
		expect(t.states).toContain(AdminUploadSendState.UNREACHABLE);
		t.scheduler.stop();
	});

	it('waits retry_after_s on 429 upload_slots_busy', async () => {
		let once = true;

		const t = setup([MIB], {
			hooks: [
				() => {
					if (!once) return null;

					once = false;

					return error(429, 'upload_slots_busy', { retry_after_s: 2 });
				}
			]
		});

		await run(t);

		expect(t.clock.sleeps).toContain(2000);
		expect(t.finished?.state).toBe(AdminUploadSendState.DONE);
	});

	it('pauses at a full disk until Resume', async () => {
		let full = true;

		const t = setup([4 * MIB], {
			hooks: [() => (full ? error(507, 'disk_full', { free_bytes: 1234, offset: 0 }) : null)]
		});

		await run(t, () => stateOf(t) === AdminUploadSendState.DISK_FULL);

		expect(t.scheduler.snapshot.problem?.freeBytes).toBe(1234);

		const sent = t.transport.requests.length;

		await settle(() => false, 50);
		expect(t.transport.requests.length).toBe(sent);

		full = false;
		t.scheduler.resume();
		await settle(() => t.finished !== null);
		expect(t.finished?.state).toBe(AdminUploadSendState.DONE);
	});

	it.each([
		[401, 'admin_key_required', AdminUploadSendState.KEY_REQUIRED],
		[403, 'admin_forbidden', AdminUploadSendState.FORBIDDEN]
	])('stops at %i until Resume', async (status, type, expected) => {
		let refuse = true;

		const t = setup([2 * MIB], { hooks: [() => (refuse ? error(status, type) : null)] });

		await run(t, () => stateOf(t) === expected);
		refuse = false;
		t.scheduler.resume();
		await settle(() => t.finished !== null);

		expect(t.finished?.state).toBe(AdminUploadSendState.DONE);
	});

	it('ends for good when the session is gone (410)', async () => {
		const t = setup([2 * MIB], { hooks: [() => error(410, 'upload_gone', { state: 'expired' })] });

		await run(t);

		expect(t.finished).toEqual({ serverState: 'expired', state: AdminUploadSendState.GONE });

		const sent = t.transport.requests.length;

		t.scheduler.resume();
		await settle(() => false, 50);
		expect(t.transport.requests.length).toBe(sent);
	});

	it('ends when the session moved on (409 upload_busy)', async () => {
		const t = setup([2 * MIB], { hooks: [() => error(409, 'upload_busy', { state: 'checking' })] });

		await run(t);

		expect(t.finished).toEqual({
			serverState: 'checking',
			state: AdminUploadSendState.SERVER_BUSY
		});
	});

	it('stops at a refused file; dropping it lets the other files finish', async () => {
		const t = setup([2 * MIB, 2 * MIB], {
			hooks: [
				(request) =>
					request.file === 1 ? error(422, 'file_rejected', { file: 1, reason: 'adapter' }) : null
			]
		});

		await run(t, () => stateOf(t) === AdminUploadSendState.FILE_REJECTED);

		expect(t.scheduler.snapshot.problem).toMatchObject({ code: 'adapter', file: 1 });

		t.server.files[1].dropped = true;
		t.scheduler.dropFile(1);
		t.scheduler.resume();
		await settle(() => t.finished !== null);

		expect(t.finished?.state).toBe(AdminUploadSendState.DONE);
		expect(sameBytes(t, 0)).toBe(true);
	});

	it('stops at a file the header check refused in a 200 answer; dropping it lets the rest finish', async () => {
		const t = setup([2 * MIB, 2 * MIB]);

		t.server.fileErrors[1] = {
			code: 'projector_bad_metadata',
			message: 'clip.has_vision_encoder is not a bool'
		};
		await run(t, () => stateOf(t) === AdminUploadSendState.FILE_REJECTED);

		expect(t.scheduler.snapshot.problem).toMatchObject({ code: 'projector_bad_metadata', file: 1 });

		const sent = t.transport.requests.length;

		await settle(() => false, 50);
		expect(t.transport.requests.length).toBe(sent);

		t.server.files[1].dropped = true;
		t.scheduler.dropFile(1);
		t.scheduler.resume();
		await settle(() => t.finished !== null);

		expect(t.finished?.state).toBe(AdminUploadSendState.DONE);
		expect(sameBytes(t, 0)).toBe(true);
	});

	it('sends a damaged chunk again at most 3 times', async () => {
		const t = setup([MIB], { hooks: [() => error(400, 'crc_mismatch')] });

		await run(t, () => stateOf(t) === AdminUploadSendState.FAILED);

		expect(t.transport.requests).toHaveLength(4);
	});

	it('retries a write_failed from the offset the server names, then stops', async () => {
		const t = setup([MIB], { hooks: [() => error(500, 'write_failed', { offset: 0 })] });

		await run(t, () => stateOf(t) === AdminUploadSendState.FAILED);

		expect(t.transport.requests).toHaveLength(4);
		expect(t.transport.requests.every((request) => request.offset === 0)).toBe(true);
	});

	it('waits and reads the session again after 409 chunk_in_progress', async () => {
		let once = true;

		const t = setup([MIB], {
			hooks: [
				() => {
					if (!once) return null;

					once = false;

					return error(409, 'chunk_in_progress');
				}
			]
		});

		await run(t);

		expect(Math.max(...t.clock.sleeps)).toBeGreaterThanOrEqual(1000);
		expect(t.server.gets).toBeGreaterThanOrEqual(1);
		expect(t.finished?.state).toBe(AdminUploadSendState.DONE);
	});

	it('pause aborts the chunk in flight; resume reads the session and goes on', async () => {
		let hanging = true;

		const t = setup([4 * MIB], { hang: () => hanging });

		await run(t, () => t.transport.requests.length === 1);
		t.scheduler.pause();
		expect(stateOf(t)).toBe(AdminUploadSendState.PAUSED);
		await settle(() => false, 20);
		expect(t.transport.requests).toHaveLength(1);

		hanging = false;
		t.scheduler.resume();
		await settle(() => t.finished !== null);

		expect(t.server.gets).toBeGreaterThanOrEqual(1);
		expect(t.finished?.state).toBe(AdminUploadSendState.DONE);
		expect(sameBytes(t, 0)).toBe(true);
	});

	it('starts a resumed file 1 MiB before the server offset (the server compares the overlap)', async () => {
		const t = setup([8 * MIB], { scheduler: { overlapFirst: true }, stored: [5 * MIB] });

		await run(t);

		expect(t.transport.requests[0].offset).toBe(4 * MIB);
		expect(t.finished?.state).toBe(AdminUploadSendState.DONE);
		expect(sameBytes(t, 0)).toBe(true);
	});

	it('keeps the resume overlap below half a chunk, so a stepped-down chunk still reaches past the offset', async () => {
		// an older server answered a chunk that lies wholly inside the stored bytes with a false 507
		const t = setup([8 * MIB], {
			hooks: [
				(request, server) =>
					!request.verify && request.offset + request.data.length <= server.files[0].offset
						? error(507, 'disk_full', { free_bytes: 10 ** 12 })
						: null
			],
			scheduler: { ceiling: 960 * KIB, overlapFirst: true },
			stored: [5 * MIB]
		});

		await run(t);

		expect(t.transport.requests[0].offset).toBe(5 * MIB - 480 * KIB);
		expect(t.transport.requests[0].data.length).toBe(960 * KIB);
		expect(t.states).not.toContain(AdminUploadSendState.DISK_FULL);
		expect(t.finished?.state).toBe(AdminUploadSendState.DONE);
		expect(sameBytes(t, 0)).toBe(true);
	});

	it('overlaps 120 KiB when a file is picked again while the chunk size is 240 KiB', async () => {
		const t = setup([2 * MIB], { scheduler: { ceiling: 240 * KIB }, stored: [MIB] });

		t.scheduler.setFileBlob(0, new Blob([t.datas[0] as unknown as BlobPart]));
		await run(t);

		expect(t.transport.requests[0].offset).toBe(MIB - 120 * KIB);
		expect(t.finished?.state).toBe(AdminUploadSendState.DONE);
		expect(sameBytes(t, 0)).toBe(true);
	});

	it('steps down when the connection drops on a chunk shorter than the chunk size (the rest of a file)', async () => {
		// 6 MiB left after a resume: less than the 8 MiB chunk, and a proxy resets bodies over 1 MB
		const t = setup([6 * MIB], {
			hooks: [(request) => (request.data.length > 1_000_000 ? { body: null, status: 0 } : null)],
			scheduler: { overlapFirst: true },
			stored: [MIB]
		});

		await run(t);

		expect(t.finished?.state).toBe(AdminUploadSendState.DONE);
		expect(t.ceilings).toEqual([[960 * KIB, false]]);
		expect(accepted(t).every((chunk) => chunk.length <= 960 * KIB)).toBe(true);
		expect(sameBytes(t, 0)).toBe(true);
	});

	it('does not step down on dropped connections below the size that already went through', async () => {
		let drops = 0;

		// the last chunk (2 MiB) drops four times: a flaky network, not a size limit
		const t = setup([10 * MIB], {
			hooks: [
				(request) =>
					request.offset === 8 * MIB && drops++ < 4 ? { body: null, status: 0 } : null
			]
		});

		await run(t);

		expect(drops).toBeGreaterThanOrEqual(4);
		expect(t.ceilings).toEqual([]);
		expect(t.finished?.state).toBe(AdminUploadSendState.DONE);
	});

	it('reads the session after a 5xx page of a proxy (not the JSON of the server)', async () => {
		let once = true;

		const t = setup([4 * MIB], {
			hooks: [
				() => {
					if (!once) return null;

					once = false;

					return { body: null, status: 502 };
				}
			]
		});

		await run(t);

		expect(t.server.gets).toBeGreaterThanOrEqual(1);
		expect(t.finished?.state).toBe(AdminUploadSendState.DONE);
		expect(sameBytes(t, 0)).toBe(true);
	});

	it('learns from the session that another tab sends when a proxy turns the refusal into a 502', async () => {
		let first = true;

		const t = setup([20 * MIB], {
			hooks: [
				(_request, server) => {
					if (!first) return null;

					first = false;
					server.lastClientAt = server.clock.now(); // the other tab wrote a moment ago

					return { body: null, status: 502 };
				}
			]
		});

		await run(t, () => stateOf(t) === AdminUploadSendState.OTHER_CLIENT);

		expect(t.server.gets).toBe(1);
		expect(stateOf(t)).toBe(AdminUploadSendState.OTHER_CLIENT);
		t.scheduler.stop();
	});

	it('asks for the file when it was not picked again, then continues', async () => {
		const t = setup([3 * MIB], { files: [{ blob: null }], stored: [MIB] });

		await run(t, () => stateOf(t) === AdminUploadSendState.NEEDS_FILE);
		t.scheduler.setFileBlob(0, new Blob([t.datas[0] as unknown as BlobPart]));
		t.scheduler.resume();
		await settle(() => t.finished !== null);

		expect(t.finished?.state).toBe(AdminUploadSendState.DONE);
		expect(sameBytes(t, 0)).toBe(true);
	});

	it('reports speed and time left from the confirmed bytes', async () => {
		let sent = 0;

		// the 4th chunk never answers, so the upload is still running when the snapshot is read
		const t = setup([64 * MIB], { hang: () => ++sent > 3 });

		t.server.chunkMs = 1000;
		await run(t, () => accepted(t).length >= 3 && t.transport.requests.length >= 4);

		const snapshot = t.scheduler.snapshot;

		expect(snapshot.speed).toBeGreaterThan(0);
		expect(snapshot.eta).toBeGreaterThan(0);
		t.scheduler.stop();
	});
});

describe('verifyUploadedStart', () => {
	it('compares the start of a picked file with the stored bytes without writing', async () => {
		const clock = new FakeClock();
		const server = new FakeUploadServer(clock, [4 * MIB]);
		const data = pattern(4 * MIB, 1);

		server.files[0].data.set(data.subarray(0, 2 * MIB));
		server.files[0].offset = 2 * MIB;

		const transport = fakeTransport(server);
		const signal = new AbortController().signal;

		expect(
			await verifyUploadedStart(
				transport,
				'sid',
				0,
				new Blob([data as unknown as BlobPart]),
				2 * MIB,
				signal
			)
		).toBe(true);
		expect(
			await verifyUploadedStart(
				transport,
				'sid',
				0,
				new Blob([pattern(4 * MIB, 5) as unknown as BlobPart]),
				2 * MIB,
				signal
			)
		).toBe(false);
		expect(server.files[0].offset).toBe(2 * MIB);
		expect(
			transport.requests.every((request) => request.verify && request.data.length === MIB)
		).toBe(true);

		server.hooks = [() => error(500, 'server_error')];
		expect(
			await verifyUploadedStart(
				transport,
				'sid',
				0,
				new Blob([data as unknown as BlobPart]),
				2 * MIB,
				signal
			)
		).toBeNull();
	});
});
