import { crc32, crc32Hex, crc32Update } from '$lib/utils/admin-crc32';
import { describe, expect, it } from 'vitest';

const text = (value: string) => new TextEncoder().encode(value);

/**
 * CRC32 of the upload (X-Chunk-CRC32 and the head CRC of a resume): the
 * IEEE / zlib one, as the server computes it.
 */
describe('crc32', () => {
	it.each([
		['', '00000000'],
		['a', 'e8b7be43'],
		['abc', '352441c2'],
		['123456789', 'cbf43926'],
		['The quick brown fox jumps over the lazy dog', '414fa339']
	])('CRC32 of %j is %s', (input, expected) => {
		expect(crc32Hex(crc32(text(input)))).toBe(expected);
	});

	it('continues over split input (any split point)', () => {
		const data = new Uint8Array(100_003);

		for (let i = 0; i < data.length; i++) data[i] = (i * 131 + 7) & 0xff;

		const whole = crc32(data);

		for (const split of [0, 1, 7, 8, 9, 4096, 50_001, data.length]) {
			expect(crc32Update(crc32Update(0, data.subarray(0, split)), data.subarray(split))).toBe(
				whole
			);
		}
	});

	it('writes 8 lower-case hex digits', () => {
		expect(crc32Hex(0)).toBe('00000000');
		expect(crc32Hex(0xabc)).toBe('00000abc');
		expect(crc32Hex(0xffffffff)).toBe('ffffffff');
	});

	it('matches the slow bytewise CRC on random input', () => {
		const data = new Uint8Array(1031);

		for (let i = 0; i < data.length; i++) data[i] = Math.floor(Math.random() * 256);

		let c = 0xffffffff;

		for (const byte of data) {
			c ^= byte;

			for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
		}

		expect(crc32(data)).toBe((c ^ 0xffffffff) >>> 0);
	});
});
