import { makeFile, makeGguf, modelKv, projectorKv } from './admin-upload-fixtures';
import { readGgufHeader } from '$lib/utils/admin-gguf-header';
import { describe, expect, it } from 'vitest';

/**
 * The GGUF key-value reader of the upload tab: what a file is before any
 * byte is sent. Only a hint; the server checks every file itself.
 */
describe('readGgufHeader', () => {
	it('reads architecture, quantization, name, context and layers of a model', async () => {
		const header = await readGgufHeader(makeFile('m.gguf', makeGguf(modelKv())));

		expect(header.error).toBeNull();
		expect(header.version).toBe(3);
		expect(header.arch).toBe('llama');
		expect(header.quant).toBe('Q4_K_M');
		expect(header.fileType).toBe(15);
		expect(header.name).toBe('Tiny Llama');
		expect(header.contextLength).toBe(2048);
		expect(header.blockCount).toBe(5);
		expect(header.nTensors).toBe(2);
		expect(header.splitCount).toBeNull();
	});

	it('finds the end of the header (tensor infos, aligned to the data start)', async () => {
		const bytes = makeGguf(modelKv(), { alignment: 64, tensors: 3 });
		const header = await readGgufHeader(makeFile('m.gguf', bytes));

		expect(header.kvEnd).toBeGreaterThan(24);
		expect(header.headerEnd).not.toBeNull();
		expect(header.headerEnd! % 32).toBe(0);
		expect(header.headerEnd!).toBeGreaterThan(header.kvEnd);
	});

	it('honours general.alignment', async () => {
		const bytes = makeGguf(modelKv({ 'general.alignment': { type: 'u32', value: 4096 } }), {
			alignment: 4096
		});
		const header = await readGgufHeader(makeFile('m.gguf', bytes));

		expect(header.headerEnd).toBe(4096);
	});

	it('reads the split keys of a shard (split.no is 0-based)', async () => {
		const bytes = makeGguf(
			modelKv({
				'split.count': { type: 'u16', value: 3 },
				'split.no': { type: 'u16', value: 1 }
			})
		);
		const header = await readGgufHeader(makeFile('m-00002-of-00003.gguf', bytes));

		expect(header.splitNo).toBe(1);
		expect(header.splitCount).toBe(3);
	});

	it('tells a projector by its clip architecture and encoders', async () => {
		const header = await readGgufHeader(makeFile('mmproj.gguf', makeGguf(projectorKv())));

		expect(header.arch).toBe('clip');
		expect(header.clipVision).toBe(true);
		expect(header.clipAudio).toBe(false);
		expect(header.quant).toBe('F16');
	});

	it('ignores a wrongly typed encoder flag (the server rejects such a projector)', async () => {
		const header = await readGgufHeader(
			makeFile(
				'mmproj.gguf',
				makeGguf({
					...projectorKv(),
					'clip.has_vision_encoder': { type: 'u8', value: 1 }
				})
			)
		);

		expect(header.error).toBeNull();
		expect(header.clipVision).toBeNull();
	});

	it('reads general.type and adapter.type of an adapter', async () => {
		const header = await readGgufHeader(
			makeFile(
				'lora.gguf',
				makeGguf(
					modelKv({
						'adapter.type': { type: 'string', value: 'lora' },
						'general.type': { type: 'string', value: 'adapter' }
					})
				)
			)
		);

		expect(header.type).toBe('adapter');
		expect(header.adapterType).toBe('lora');
	});

	it('marks a guessed file type by its plain quantization', async () => {
		const header = await readGgufHeader(
			makeFile(
				'm.gguf',
				makeGguf(modelKv({ 'general.file_type': { type: 'u32', value: 7 | 1024 } }))
			)
		);

		expect(header.quant).toBe('Q8_0');
	});

	it('refuses a file that is not GGUF', async () => {
		const header = await readGgufHeader(
			makeFile('x.gguf', new TextEncoder().encode('not a gguf file, at all'))
		);

		expect(header.error).toBe('not_gguf');
	});

	it('refuses GGUF version 1', async () => {
		const header = await readGgufHeader(makeFile('v1.gguf', makeGguf(modelKv(), { version: 1 })));

		expect(header.error).toBe('unsupported_version');
	});

	it('reports a file cut inside its metadata as truncated', async () => {
		const bytes = makeGguf(modelKv());
		const header = await readGgufHeader(makeFile('cut.gguf', bytes.subarray(0, 60)));

		expect(header.error).toBe('truncated');
	});

	it('reports metadata above the read limit as too large', async () => {
		const tokens = Array.from({ length: 2000 }, (_, i) => `token-${i}-with-some-length`);
		const bytes = makeGguf(
			modelKv({ 'tokenizer.ggml.tokens': { type: 'strings', value: tokens } })
		);
		const header = await readGgufHeader(makeFile('big.gguf', bytes), { maxBytes: 4096 });

		expect(header.error).toBe('too_large');
	});

	it('reads more of the file when the metadata is longer than the first read', async () => {
		const tokens = Array.from({ length: 5000 }, (_, i) => `t${i}`);
		const bytes = makeGguf(
			modelKv({ 'tokenizer.ggml.tokens': { type: 'strings', value: tokens } })
		);
		const header = await readGgufHeader(makeFile('m.gguf', bytes), { firstRead: 256 });

		expect(header.error).toBeNull();
		expect(header.arch).toBe('llama');
		expect(header.headerEnd).not.toBeNull();
	});

	it('keeps the metadata when the tensor infos go on past the limit', async () => {
		const bytes = makeGguf(modelKv(), { tensors: 400 });
		const kvOnly = await readGgufHeader(makeFile('m.gguf', bytes));
		const header = await readGgufHeader(makeFile('m.gguf', bytes), {
			firstRead: 256,
			maxBytes: kvOnly.kvEnd + 100
		});

		expect(header.error).toBeNull();
		expect(header.arch).toBe('llama');
		expect(header.headerEnd).toBeNull();
	});

	it('refuses a damaged count instead of reading for ever', async () => {
		const bytes = makeGguf(modelKv());

		new DataView(bytes.buffer).setBigUint64(16, 1n << 40n, true); // n_kv

		const header = await readGgufHeader(makeFile('bad.gguf', bytes));

		expect(header.error).toBe('malformed');
	});

	it('reads at most the first slices of a large file', async () => {
		const head = makeGguf(modelKv());
		const big = new Blob([head as unknown as BlobPart, new Uint8Array(32 * 1024 * 1024)]);
		const sliced: number[] = [];
		const spy = {
			size: big.size,
			slice: (start: number, end: number) => {
				sliced.push(end - start);

				return big.slice(start, end);
			}
		};
		const header = await readGgufHeader(spy as unknown as Blob);

		expect(header.arch).toBe('llama');
		expect(Math.max(...sliced)).toBeLessThanOrEqual(1024 * 1024);
	});
});
