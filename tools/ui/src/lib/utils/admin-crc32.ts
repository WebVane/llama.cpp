/**
 * CRC32 (IEEE 802.3, the zlib / PNG one) for the GGUF upload: the server
 * checks the X-Chunk-CRC32 header of every chunk, and the CRC32 of the
 * first MiB of a file is the identity a resumed upload is matched by.
 * Plain HTTP origins have no crypto.subtle, and a CRC is enough to catch a
 * chunk damaged on the way.
 */

/** Slicing-by-8 tables: 8 x 256 entries. */
const TABLES: Uint32Array = (() => {
	const tables = new Uint32Array(8 * 256);

	for (let n = 0; n < 256; n++) {
		let c = n;

		for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;

		tables[n] = c >>> 0;
	}

	for (let n = 0; n < 256; n++) {
		let c = tables[n];

		for (let t = 1; t < 8; t++) {
			c = tables[c & 0xff] ^ (c >>> 8);
			tables[t * 256 + n] = c >>> 0;
		}
	}

	return tables;
})();

/**
 * Continue a CRC32 over more bytes: `crc32Update(crc32Update(0, a), b)`
 * equals the CRC32 of `a` followed by `b`.
 *
 * @param crc - The CRC32 of the bytes before (0 to start)
 */
export function crc32Update(crc: number, data: Uint8Array): number {
	const t = TABLES;
	const n = data.length;

	let c = (crc ^ 0xffffffff) >>> 0;
	let i = 0;

	for (; i + 8 <= n; i += 8) {
		const lo = c ^ (data[i] | (data[i + 1] << 8) | (data[i + 2] << 16) | (data[i + 3] << 24));

		c =
			t[1792 + (lo & 0xff)] ^
			t[1536 + ((lo >>> 8) & 0xff)] ^
			t[1280 + ((lo >>> 16) & 0xff)] ^
			t[1024 + (lo >>> 24)] ^
			t[768 + data[i + 4]] ^
			t[512 + data[i + 5]] ^
			t[256 + data[i + 6]] ^
			t[data[i + 7]];
	}

	for (; i < n; i++) c = t[(c ^ data[i]) & 0xff] ^ (c >>> 8);

	return (c ^ 0xffffffff) >>> 0;
}

/** CRC32 of some bytes. */
export function crc32(data: Uint8Array): number {
	return crc32Update(0, data);
}

/** The CRC32 as the server writes it: 8 lower-case hex digits. */
export function crc32Hex(crc: number): string {
	return (crc >>> 0).toString(16).padStart(8, '0');
}
