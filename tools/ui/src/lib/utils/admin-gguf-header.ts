/**
 * GGUF header reader for the upload tab: reads the key-value section (and
 * the tensor infos after it) from the start of a picked file, so the page
 * can show what a file is before any byte is sent - model or projector,
 * architecture, quantization, shard k of n - and flag a set that cannot
 * work (a missing shard, an adapter, an imatrix file).
 *
 * Only a hint: the server checks every file itself. The reader reads the
 * file in growing slices and never more than ADMIN_UPLOAD.HEADER_MAX_BYTES
 * (the server's own bound for the metadata), so a 40 GB file costs a few
 * MiB of memory at most.
 */

import { ADMIN_GGUF_FILE_TYPES, ADMIN_UPLOAD } from '$lib/constants';
import type { AdminGgufHeader } from '$lib/types/admin-upload';

/** GGUF value types (gguf.h). */
const GGUF_TYPE_SIZES: Record<number, number> = {
	0: 1, // uint8
	1: 1, // int8
	2: 2, // uint16
	3: 2, // int16
	4: 4, // uint32
	5: 4, // int32
	6: 4, // float32
	7: 1, // bool
	10: 8, // uint64
	11: 8, // int64
	12: 8 // float64
};
const GGUF_TYPE_STRING = 8;
const GGUF_TYPE_ARRAY = 9;
const GGUF_MAGIC = [0x47, 0x47, 0x55, 0x46]; // "GGUF"
const GGUF_DEFAULT_ALIGNMENT = 32;
const GGML_MAX_DIMS = 4;
/** llama.cpp marks a guessed file type with this bit */
const LLAMA_FTYPE_GUESSED = 1024;
/** Sanity bounds: a count above these is a damaged header, not a real file */
const MAX_KV = 1 << 20;
const MAX_TENSORS = 1 << 24;

/** The parser ran out of bytes: read at least `need` bytes from the start and try again. */
class NeedMore {
	need: number;

	constructor(need: number) {
		this.need = need;
	}
}

/** A header that cannot be a GGUF file. */
class Malformed {
	code: 'not_gguf' | 'unsupported_version' | 'malformed';

	constructor(code: 'not_gguf' | 'unsupported_version' | 'malformed') {
		this.code = code;
	}
}

interface ParseState {
	header: AdminGgufHeader;
	alignment: number;
	/** Numbers read under "<arch>.<name>" keys, resolved once the architecture is known */
	archNumbers: Record<string, number>;
}

class Cursor {
	bytes: Uint8Array;
	pos = 0;
	view: DataView;

	private decoder = new TextDecoder('utf-8', { fatal: false });

	bool(): boolean {
		return this.u8() !== 0;
	}

	constructor(bytes: Uint8Array) {
		this.bytes = bytes;
		this.view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
	}

	f32(): number {
		this.need(4);

		const v = this.view.getFloat32(this.pos, true);

		this.pos += 4;

		return v;
	}

	f64(): number {
		this.need(8);

		const v = this.view.getFloat64(this.pos, true);

		this.pos += 8;

		return v;
	}

	i8(): number {
		this.need(1);

		return this.view.getInt8(this.pos++);
	}

	i16(): number {
		this.need(2);

		const v = this.view.getInt16(this.pos, true);

		this.pos += 2;

		return v;
	}

	i32(): number {
		this.need(4);

		const v = this.view.getInt32(this.pos, true);

		this.pos += 4;

		return v;
	}

	i64(): number {
		this.need(8);

		const v = Number(this.view.getBigInt64(this.pos, true));

		this.pos += 8;

		return v;
	}

	need(n: number): void {
		if (this.pos + n > this.bytes.length) throw new NeedMore(this.pos + n);
	}

	skip(n: number): void {
		this.need(n);
		this.pos += n;
	}

	str(): string {
		const len = this.u64();

		this.need(len);

		const text = this.decoder.decode(this.bytes.subarray(this.pos, this.pos + len));

		this.pos += len;

		return text;
	}

	/** Skip a string without decoding it (token lists can hold 250k of them). */
	strSkip(): void {
		this.skip(this.u64());
	}

	u8(): number {
		this.need(1);

		return this.view.getUint8(this.pos++);
	}

	u16(): number {
		this.need(2);

		const v = this.view.getUint16(this.pos, true);

		this.pos += 2;

		return v;
	}

	u32(): number {
		this.need(4);

		const v = this.view.getUint32(this.pos, true);

		this.pos += 4;

		return v;
	}

	u64(): number {
		this.need(8);

		const v = this.view.getBigUint64(this.pos, true);

		this.pos += 8;

		if (v > BigInt(Number.MAX_SAFE_INTEGER)) throw new Malformed('malformed');

		return Number(v);
	}
}

function emptyHeader(): AdminGgufHeader {
	return {
		adapterType: null,
		arch: null,
		blockCount: null,
		clipAudio: null,
		clipVision: null,
		contextLength: null,
		error: null,
		fileType: null,
		headerEnd: null,
		kvEnd: 0,
		name: null,
		nKv: 0,
		nTensors: 0,
		quant: null,
		splitCount: null,
		splitNo: null,
		type: null,
		version: 0
	};
}

/** A scalar value of the given type, or undefined for a string / array (read separately). */
function readScalar(c: Cursor, type: number): number | boolean {
	switch (type) {
		case 0:
			return c.u8();
		case 1:
			return c.i8();
		case 2:
			return c.u16();
		case 3:
			return c.i16();
		case 4:
			return c.u32();
		case 5:
			return c.i32();
		case 6:
			return c.f32();
		case 7:
			return c.bool();
		case 10:
			return c.u64();
		case 11:
			return c.i64();
		case 12:
			return c.f64();
		default:
			throw new Malformed('malformed');
	}
}

function skipArray(c: Cursor): void {
	const type = c.u32();
	const count = c.u64();

	if (type === GGUF_TYPE_STRING) {
		for (let i = 0; i < count; i++) c.strSkip();

		return;
	}

	const size = GGUF_TYPE_SIZES[type];

	// llama.cpp does not read nested arrays either
	if (size === undefined) throw new Malformed('malformed');

	c.skip(size * count);
}

function takeString(state: ParseState, key: string, value: string): void {
	const h = state.header;

	switch (key) {
		case 'general.architecture':
			h.arch = value;

			break;
		case 'general.type':
			h.type = value;

			break;
		case 'general.name':
			h.name = value;

			break;
		case 'adapter.type':
			h.adapterType = value;

			break;
	}
}

function takeNumber(state: ParseState, key: string, value: number | boolean): void {
	const h = state.header;

	if (typeof value === 'boolean') {
		if (key === 'clip.has_vision_encoder') h.clipVision = value;
		else if (key === 'clip.has_audio_encoder') h.clipAudio = value;

		return;
	}

	switch (key) {
		case 'general.file_type':
			h.fileType = value;

			break;
		case 'general.alignment':
			if (value > 0 && Number.isInteger(value)) state.alignment = value;

			break;
		case 'split.no':
			h.splitNo = value;

			break;
		case 'split.count':
			h.splitCount = value;

			break;
		default:
			if (key.endsWith('.context_length') || key.endsWith('.block_count')) {
				state.archNumbers[key] = value;
			}
	}
}

/** Parse what is in `bytes`; throws NeedMore when the header goes on past them. */
function parseInto(state: ParseState, bytes: Uint8Array, readTensors: boolean): void {
	const c = new Cursor(bytes);
	const h = state.header;

	c.need(4);

	for (let i = 0; i < 4; i++) {
		if (bytes[i] !== GGUF_MAGIC[i]) throw new Malformed('not_gguf');
	}

	c.skip(4);
	h.version = c.u32();

	if (h.version !== 2 && h.version !== 3) throw new Malformed('unsupported_version');

	h.nTensors = c.u64();
	h.nKv = c.u64();

	if (h.nKv > MAX_KV || h.nTensors > MAX_TENSORS) throw new Malformed('malformed');

	for (let i = 0; i < h.nKv; i++) {
		const key = c.str();
		const type = c.u32();

		if (type === GGUF_TYPE_STRING) {
			takeString(state, key, c.str());
		} else if (type === GGUF_TYPE_ARRAY) {
			skipArray(c);
		} else {
			takeNumber(state, key, readScalar(c, type));
		}
	}

	h.kvEnd = c.pos;

	if (h.arch) {
		h.contextLength = state.archNumbers[`${h.arch}.context_length`] ?? null;
		h.blockCount = state.archNumbers[`${h.arch}.block_count`] ?? null;
	}

	if (h.fileType !== null) {
		h.quant = ADMIN_GGUF_FILE_TYPES[h.fileType & ~LLAMA_FTYPE_GUESSED] ?? null;
	}

	if (!readTensors) return;

	for (let i = 0; i < h.nTensors; i++) {
		c.strSkip();

		const dims = c.u32();

		if (dims > GGML_MAX_DIMS) throw new Malformed('malformed');

		c.skip(8 * dims + 4 + 8); // dims, type, offset
	}

	const align = state.alignment;

	h.headerEnd = Math.ceil(c.pos / align) * align;
}

async function readStart(file: Blob, length: number): Promise<Uint8Array> {
	return new Uint8Array(await file.slice(0, length).arrayBuffer());
}

/**
 * Read the GGUF header of a file. Never throws: a file that is not GGUF,
 * cut short or damaged comes back with `error` set.
 *
 * @param options.maxBytes - Read at most this many bytes (default 64 MiB)
 * @param options.firstRead - Bytes of the first read (default 1 MiB)
 */
export async function readGgufHeader(
	file: Blob,
	options: { firstRead?: number; maxBytes?: number } = {}
): Promise<AdminGgufHeader> {
	const maxBytes = options.maxBytes ?? ADMIN_UPLOAD.HEADER_MAX_BYTES;
	const limit = Math.min(file.size, maxBytes);

	let want = Math.min(limit, options.firstRead ?? ADMIN_UPLOAD.HEADER_READ_BYTES);
	let readTensors = true;

	for (;;) {
		const state: ParseState = {
			alignment: GGUF_DEFAULT_ALIGNMENT,
			archNumbers: {},
			header: emptyHeader()
		};

		let bytes: Uint8Array;

		try {
			bytes = await readStart(file, want);
		} catch {
			return { ...state.header, error: 'truncated' };
		}

		try {
			parseInto(state, bytes, readTensors);

			return state.header;
		} catch (error) {
			if (error instanceof Malformed) return { ...state.header, error: error.code };

			if (!(error instanceof NeedMore)) return { ...state.header, error: 'malformed' };

			const metadataDone = state.header.kvEnd > 0;

			if (error.need > limit) {
				// the metadata itself does not fit: cut short, or larger than the server reads
				if (!metadataDone) {
					return { ...state.header, error: error.need > file.size ? 'truncated' : 'too_large' };
				}

				// the tensor infos go on past the limit: the metadata is all the page needs
				readTensors = false;

				continue;
			}

			want = Math.min(limit, Math.max(error.need, want * 2));
		}
	}
}
