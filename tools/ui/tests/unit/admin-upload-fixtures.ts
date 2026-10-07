/**
 * Fixtures of the upload tests (admin panel, stage 3): a minimal GGUF writer
 * for the header reader and the grouping, and a fake upload server with a
 * fake clock for the chunk scheduler.
 */

import type { AdminGgufHeader } from '$lib/types/admin-upload';
import { crc32, crc32Hex } from '$lib/utils/admin-crc32';
import type {
	AdminUploadChunkRequest,
	AdminUploadClock,
	AdminUploadTransport,
	AdminUploadTransportResult
} from '$lib/utils/admin-upload-scheduler';

/*
 * GGUF writer
 */

export type GgufValue =
	| { type: 'string'; value: string }
	| { type: 'u8' | 'u16' | 'u32' | 'i32' | 'u64' | 'f32'; value: number }
	| { type: 'bool'; value: boolean }
	| { type: 'strings'; value: string[] }
	| { type: 'u32s'; value: number[] };

const TYPE_IDS: Record<string, number> = {
	bool: 7,
	f32: 6,
	i32: 5,
	string: 8,
	u8: 0,
	u16: 2,
	u32: 4,
	u64: 10
};

class Writer {
	private chunks: number[] = [];

	bytes(): Uint8Array {
		return new Uint8Array(this.chunks);
	}

	raw(data: ArrayLike<number>): void {
		for (let i = 0; i < data.length; i++) this.chunks.push(data[i]);
	}

	str(text: string): void {
		const data = new TextEncoder().encode(text);

		this.u64(data.length);
		this.raw(data);
	}

	u32(value: number): void {
		const b = new Uint8Array(4);

		new DataView(b.buffer).setUint32(0, value, true);
		this.raw(b);
	}

	u64(value: number): void {
		const b = new Uint8Array(8);

		new DataView(b.buffer).setBigUint64(0, BigInt(value), true);
		this.raw(b);
	}

	value(v: GgufValue): void {
		const b = new Uint8Array(8);
		const view = new DataView(b.buffer);

		switch (v.type) {
			case 'string':
				this.u32(8);
				this.str(v.value);

				return;
			case 'strings':
				this.u32(9);
				this.u32(8);
				this.u64(v.value.length);
				v.value.forEach((s) => this.str(s));

				return;
			case 'u32s':
				this.u32(9);
				this.u32(4);
				this.u64(v.value.length);
				v.value.forEach((n) => this.u32(n));

				return;
			case 'bool':
				this.u32(7);
				this.raw([v.value ? 1 : 0]);

				return;
			default:
				this.u32(TYPE_IDS[v.type]);

				switch (v.type) {
					case 'u8':
						this.raw([v.value]);

						return;
					case 'u16':
						view.setUint16(0, v.value, true);
						this.raw(b.subarray(0, 2));

						return;
					case 'u32':
						view.setUint32(0, v.value, true);
						this.raw(b.subarray(0, 4));

						return;
					case 'i32':
						view.setInt32(0, v.value, true);
						this.raw(b.subarray(0, 4));

						return;
					case 'f32':
						view.setFloat32(0, v.value, true);
						this.raw(b.subarray(0, 4));

						return;
					case 'u64':
						this.u64(v.value);

						return;
				}
		}
	}
}

/** A GGUF file: the given keys, `tensors` tensor infos and `dataBytes` bytes of tensor data. */
export function makeGguf(
	kv: Record<string, GgufValue>,
	options: { version?: number; tensors?: number; dataBytes?: number; alignment?: number } = {}
): Uint8Array {
	const w = new Writer();
	const tensors = options.tensors ?? 2;

	w.raw([0x47, 0x47, 0x55, 0x46]);
	w.u32(options.version ?? 3);
	w.u64(tensors);
	w.u64(Object.keys(kv).length);

	for (const [key, value] of Object.entries(kv)) {
		w.str(key);
		w.value(value);
	}

	for (let i = 0; i < tensors; i++) {
		w.str(`blk.${i}.weight`);
		w.u32(2);
		w.u64(16);
		w.u64(4);
		w.u32(0);
		w.u64(i * 256);
	}

	const head = w.bytes();
	const align = options.alignment ?? 32;
	const padded = Math.ceil(head.length / align) * align;
	const out = new Uint8Array(padded + (options.dataBytes ?? 512));

	out.set(head);

	return out;
}

/** The keys of a small llama model. */
export function modelKv(extra: Record<string, GgufValue> = {}): Record<string, GgufValue> {
	return {
		'general.architecture': { type: 'string', value: 'llama' },
		'general.file_type': { type: 'u32', value: 15 },
		'general.name': { type: 'string', value: 'Tiny Llama' },
		'llama.block_count': { type: 'u32', value: 5 },
		'llama.context_length': { type: 'u32', value: 2048 },
		'tokenizer.ggml.tokens': { type: 'strings', value: ['<s>', '</s>', 'a', 'b'] },
		...extra
	};
}

/** The keys of a projector (mmproj). */
export function projectorKv(): Record<string, GgufValue> {
	return {
		'clip.has_audio_encoder': { type: 'bool', value: false },
		'clip.has_vision_encoder': { type: 'bool', value: true },
		'general.architecture': { type: 'string', value: 'clip' },
		'general.file_type': { type: 'u32', value: 1 }
	};
}

/** A File made of bytes (Node 22 has File). */
export function makeFile(name: string, data: Uint8Array): File {
	return new File([data as unknown as BlobPart], name, { lastModified: 1_700_000_000_000 });
}

/** A header as the reader returns it, for the grouping tests. */
export function header(partial: Partial<AdminGgufHeader> = {}): AdminGgufHeader {
	return {
		adapterType: null,
		arch: 'llama',
		blockCount: 5,
		clipAudio: null,
		clipVision: null,
		contextLength: 2048,
		error: null,
		fileType: 15,
		headerEnd: 4096,
		kvEnd: 1024,
		name: null,
		nKv: 6,
		nTensors: 2,
		quant: 'Q4_K_M',
		splitCount: null,
		splitNo: null,
		type: null,
		version: 3,
		...partial
	};
}

/** A File of `size` bytes without content that matters (grouping only reads name and size). */
export function sizedFile(name: string, size = 1000): File {
	return makeFile(name, new Uint8Array(size));
}

/*
 * Fake clock and fake upload server
 */

const FAKE_SLEEP_LIMIT = 10_000;

export class FakeClock implements AdminUploadClock {
	sleeps: number[] = [];
	t = 1_000_000;

	now(): number {
		return this.t;
	}

	async sleep(ms: number, signal: AbortSignal): Promise<void> {
		this.sleeps.push(ms);

		// a scheduler stuck in a retry loop would otherwise spin forever on fake time
		if (this.sleeps.length > FAKE_SLEEP_LIMIT)
			throw new Error(`more than ${FAKE_SLEEP_LIMIT} sleeps`);

		if (!signal.aborted) this.t += ms;

		// a macrotask, not a microtask: a retry loop must not starve settle()'s timers
		await new Promise((resolve) => setTimeout(resolve, 0));
	}
}

export interface FakeFile {
	size: number;
	data: Uint8Array;
	offset: number;
	complete: boolean;
	dropped: boolean;
}

/** Bytes with a pattern that differs at every offset of a file. */
export function pattern(size: number, seed = 1): Uint8Array {
	const data = new Uint8Array(size);

	for (let i = 0; i < size; i++) data[i] = (i * 31 + seed * 7 + (i >> 8)) & 0xff;

	return data;
}

/** A hook returns an answer instead of the server (a proxy, a failure), or null to pass. */
export type FakeHook = (
	request: AdminUploadChunkRequest,
	server: FakeUploadServer
) => AdminUploadTransportResult | null;

/**
 * The server side of an upload session with the chunk rules of
 * /admin/models/upload/chunk: CRC check, offset rules, byte-for-byte
 * overlap compare, verify probes, `checking` after the last byte.
 */
export class FakeUploadServer {
	/** Per chunk: virtual ms the chunk takes */
	chunkMs = 100;
	chunks: { file: number; offset: number; length: number; takeover: boolean; verify: boolean }[] =
		[];
	clock: FakeClock;
	/** Per file: the refusal of its header check, reported in every chunk answer of that file */
	fileErrors: Record<number, { code: string; message: string }> = {};
	files: FakeFile[];
	gets = 0;
	hooks: FakeHook[] = [];
	lastClientAt: number | null = null;
	/** Answers of GET ?id= forced by a test, used once each */
	sessionAnswers: AdminUploadTransportResult[] = [];
	state = 'receiving';

	chunk(request: AdminUploadChunkRequest): AdminUploadTransportResult {
		for (const hook of this.hooks) {
			const answer = hook(request, this);

			if (answer) return answer;
		}

		this.chunks.push({
			file: request.file,
			length: request.data.length,
			offset: request.offset,
			takeover: request.takeover,
			verify: request.verify
		});

		if (this.state !== 'receiving') {
			return error(409, 'upload_busy', { state: this.state });
		}

		if (crc32Hex(crc32(request.data)) !== request.crc) return error(400, 'crc_mismatch');

		const file = this.files[request.file];

		if (request.offset > file.offset) return error(409, 'offset_mismatch', { offset: file.offset });

		const overlapEnd = Math.min(file.offset, request.offset + request.data.length);

		for (let i = request.offset; i < overlapEnd; i++) {
			if (file.data[i] !== request.data[i - request.offset]) {
				return error(409, 'content_mismatch', { offset: i });
			}
		}

		if (request.verify) {
			return {
				body: {
					offset: file.offset,
					state: this.state,
					verified: true,
					verified_bytes: Math.max(0, overlapEnd - request.offset)
				},
				status: 200
			};
		}

		const end = request.offset + request.data.length;

		if (end > file.size) return error(400, 'beyond_declared_size');

		if (end > file.offset) {
			file.data.set(request.data.subarray(file.offset - request.offset), file.offset);
			file.offset = end;
		}

		file.complete = file.offset === file.size;
		this.lastClientAt = this.clock.now();

		// a refused file keeps the session receiving until it is dropped (as the server does)
		const refused = this.files.some((f, index) => !f.dropped && this.fileErrors[index]);

		if (!refused && this.files.every((f) => f.complete || f.dropped)) this.state = 'checking';

		return {
			body: {
				durable_offset: file.offset,
				file: request.file,
				file_complete: file.complete,
				file_error: this.fileErrors[request.file] ?? null,
				offset: file.offset,
				state: this.state
			},
			status: 200
		};
	}

	constructor(clock: FakeClock, sizes: number[]) {
		this.clock = clock;
		this.files = sizes.map((size) => ({
			complete: false,
			data: new Uint8Array(size),
			dropped: false,
			offset: 0,
			size
		}));
	}

	session(): AdminUploadTransportResult {
		this.gets++;

		const forced = this.sessionAnswers.shift();

		if (forced) return forced;

		return {
			body: {
				files: this.files.map((file, index) => ({
					complete: file.complete,
					dropped: file.dropped,
					index,
					offset: file.offset
				})),
				last_client_idle_ms:
					this.lastClientAt === null ? null : this.clock.now() - this.lastClientAt,
				state: this.state
			},
			status: 200
		};
	}
}

export function error(
	status: number,
	type: string,
	extra: Record<string, unknown> = {}
): AdminUploadTransportResult {
	return { body: { error: { code: status, message: type, type, ...extra } }, status };
}

/** The transport of the scheduler, talking to a fake server. */
export function fakeTransport(
	server: FakeUploadServer,
	options: { hang?: () => boolean } = {}
): AdminUploadTransport & { requests: AdminUploadChunkRequest[] } {
	const requests: AdminUploadChunkRequest[] = [];

	return {
		getSession: async () => server.session(),
		readSlice: async (file, start, end) =>
			new Uint8Array(await file.slice(start, end).arrayBuffer()),
		requests,
		sendChunk: async (request) => {
			// keep what the tests look at, not the chunk bytes (up to 16 MiB each)
			requests.push({ ...request, data: { length: request.data.length } as Uint8Array });

			if (options.hang?.()) {
				// a chunk that never answers until it is aborted
				await new Promise<void>((resolve) => {
					if (request.signal.aborted) return resolve();

					request.signal.addEventListener('abort', () => resolve());
				});

				return { aborted: true, body: null, status: 0 };
			}

			server.clock.t += server.chunkMs;
			request.onProgress?.(request.data.length);

			return server.chunk(request);
		}
	};
}

/** Let the scheduler's promises run until `done()` (or give up). */
export async function settle(done: () => boolean, rounds = 20_000): Promise<void> {
	for (let i = 0; i < rounds && !done(); i++) {
		await new Promise((resolve) => setTimeout(resolve, 0));
	}
}
