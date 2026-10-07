/**
 * Chunk scheduler of one upload session (admin panel, stage 3).
 *
 * Sends the files of a session as raw-octet chunks, one chunk in flight at
 * a time, each addressed by its offset. The server keeps the offsets: the
 * part of a chunk it already has is compared byte for byte, so a retry is
 * always safe and a resume proves that the same file was picked.
 *
 * - The first bytes of every file go first (up to the end of its GGUF
 *   header), so the server checks the architecture, adapters and split sets
 *   within seconds, then the bulk data file by file.
 * - The chunk size doubles while chunks finish in under 2 s, halves above
 *   15 s, and steps down 8 MiB -> 960 KiB -> 480 KiB -> 240 KiB when a proxy
 *   refuses the size (a 413 that is not the server's JSON) or drops the
 *   connection on the first chunk of a size twice.
 * - After a network error the session is read again (GET ?id=): the chunk
 *   may have landed, another tab may be sending, or the session may have
 *   moved on. Retries back off from 1 s to 30 s; after 30 min of failures it
 *   retries every minute ("server unreachable").
 * - Refusals that need the admin (a disk full, another tab, a refused file,
 *   a rejected key, not the same file) stop the loop until resume().
 *
 * No Svelte, no DOM: the transport and the clock are injected, so the rules
 * are tested with a fake server.
 */

import { ADMIN_UPLOAD } from '$lib/constants';
import { AdminUploadSendState } from '$lib/enums/admin.enums';
import type { AdminUploadSendSnapshot } from '$lib/types/admin-upload';
import { crc32, crc32Hex } from '$lib/utils/admin-crc32';
import { nextChunkSizeDown } from '$lib/utils/admin-upload';

/** One HTTP answer: status 0 = no answer (network error, timeout). */
export interface AdminUploadTransportResult {
	status: number;
	/** Parsed JSON body, null when the body was not JSON (a proxy page) */
	body: Record<string, unknown> | null;
	/** The scheduler aborted the request itself (pause, stop) */
	aborted?: boolean;
}

export interface AdminUploadChunkRequest {
	id: string;
	file: number;
	offset: number;
	data: Uint8Array;
	/** X-Chunk-CRC32 */
	crc: string;
	takeover: boolean;
	verify: boolean;
	signal: AbortSignal;
	/** Bytes of the body handed to the network so far */
	onProgress?: (sent: number) => void;
}

export interface AdminUploadTransport {
	sendChunk(request: AdminUploadChunkRequest): Promise<AdminUploadTransportResult>;
	/** GET /admin/models/upload?id= */
	getSession(id: string, signal: AbortSignal): Promise<AdminUploadTransportResult>;
	/** The bytes [start, end) of a picked file */
	readSlice(file: Blob, start: number, end: number): Promise<Uint8Array>;
}

export interface AdminUploadClock {
	now(): number;
	/** Resolves after `ms`, or early when the signal aborts */
	sleep(ms: number, signal: AbortSignal): Promise<void>;
}

export interface AdminUploadSchedulerFile {
	index: number;
	size: number;
	/** Server-confirmed offset */
	offset: number;
	complete: boolean;
	dropped?: boolean;
	/** The picked file; null when it must be picked again */
	blob: Blob | null;
	/** End of the GGUF header (bytes sent before the bulk data), null when unknown */
	headerEnd: number | null;
}

export interface AdminUploadSchedulerOptions {
	sessionId: string;
	files: AdminUploadSchedulerFile[];
	transport: AdminUploadTransport;
	clock?: AdminUploadClock;
	chunkDefault: number;
	chunkMax: number;
	/** Floor of the step-down ladder */
	chunkMin: number;
	/** A ceiling found earlier in this browser session */
	ceiling?: number | null;
	/** Send an overlap before the server offset first (after a reload: proves the same file) */
	overlapFirst?: boolean;
	onUpdate?: (snapshot: AdminUploadSendSnapshot) => void;
	/** A smaller ceiling was found (proxy: by an HTML 413) */
	onCeiling?: (ceiling: number, proxy: boolean) => void;
	/** The session was read again (after a network error, on resume) */
	onSession?: (session: Record<string, unknown>) => void;
	/** The loop ended for good: every file sent, or the session left `receiving` */
	onFinished?: (state: AdminUploadSendState, serverState: string | null) => void;
}

/** States that wait for resume() (or a new file) before anything is sent. */
const BLOCKING_STATES = new Set<AdminUploadSendState>([
	AdminUploadSendState.DISK_FULL,
	AdminUploadSendState.FAILED,
	AdminUploadSendState.FILE_REJECTED,
	AdminUploadSendState.FORBIDDEN,
	AdminUploadSendState.KEY_REQUIRED,
	AdminUploadSendState.NEEDS_FILE,
	AdminUploadSendState.NOT_SAME_FILE,
	AdminUploadSendState.OTHER_CLIENT,
	AdminUploadSendState.PAUSED
]);
/** States after which the loop does not run again. */
const FINAL_STATES = new Set<AdminUploadSendState>([
	AdminUploadSendState.DONE,
	AdminUploadSendState.GONE,
	AdminUploadSendState.SERVER_BUSY
]);

export const realUploadClock: AdminUploadClock = {
	now: () => Date.now(),
	sleep: (ms, signal) =>
		new Promise<void>((resolve) => {
			if (signal.aborted) return resolve();

			const timer = setTimeout(done, ms);

			function done() {
				clearTimeout(timer);
				signal.removeEventListener('abort', done);
				resolve();
			}

			signal.addEventListener('abort', done);
		})
};

function errorOf(body: Record<string, unknown> | null): Record<string, unknown> {
	const error = body?.error;

	return error && typeof error === 'object' ? (error as Record<string, unknown>) : {};
}

function num(value: unknown): number | null {
	return typeof value === 'number' && Number.isFinite(value) ? value : null;
}

function str(value: unknown): string | null {
	return typeof value === 'string' ? value : null;
}

/**
 * Compare the start of a picked file with the bytes the server has (a
 * verify-only chunk, nothing is written).
 *
 * @returns true when equal, false when they differ, null when the server
 * could not be asked
 */
export async function verifyUploadedStart(
	transport: AdminUploadTransport,
	id: string,
	file: number,
	blob: Blob,
	serverOffset: number,
	signal: AbortSignal
): Promise<boolean | null> {
	const length = Math.min(serverOffset, blob.size, ADMIN_UPLOAD.HEAD_CRC_BYTES);

	if (length <= 0) return true;

	const data = await transport.readSlice(blob, 0, length);
	const result = await transport.sendChunk({
		crc: crc32Hex(crc32(data)),
		data,
		file,
		id,
		offset: 0,
		signal,
		takeover: false,
		verify: true
	});

	if (result.status === 200) return true;

	if (result.status === 409 && errorOf(result.body).type === 'content_mismatch') return false;

	return null;
}

export class AdminUploadScheduler {
	private ceiling: number;
	private chunkSize: number;
	private clock: AdminUploadClock;
	private confirmedSamples: { at: number; bytes: number }[] = [];
	private crcFails = 0;
	private failStart: number | null = null;
	private failures = 0;
	private files: AdminUploadSchedulerFile[];
	private inFlight = 0;
	/** When this client last got a chunk accepted (the server's "last client" rule) */
	private lastAck: number | null = null;
	private lastEmit = 0;
	private loopRunning = false;
	private needResync = false;
	private netFailsAtSize = 0;
	private offsetMismatches = 0;
	private opts: AdminUploadSchedulerOptions;
	/** Files whose next chunk starts an overlap before the server offset */
	private overlapPending = new Set<number>();
	private problem: AdminUploadSendSnapshot['problem'] = null;
	private proxyLimit: number | null = null;
	/** The request in flight (chunk or session read) */
	private requestAbort: AbortController | null = null;
	private retryAt: number | null = null;
	private serverState: string | null = null;
	/** The backoff wait in progress */
	private sleepAbort: AbortController | null = null;
	private state: AdminUploadSendState = AdminUploadSendState.IDLE;
	private stopped = false;
	/** Chunk sizes that went through at least once */
	private successAtSize = new Set<number>();
	private takeoverNext = false;
	private wake: (() => void) | null = null;
	private writeFails = 0;

	/** What the page shows. */
	get snapshot(): AdminUploadSendSnapshot {
		const active = this.files.filter((file) => !file.dropped);
		const total = active.reduce((sum, file) => sum + file.size, 0);
		const confirmed = active.reduce((sum, file) => sum + Math.min(file.offset, file.size), 0);
		const speed = this.speed();
		const offsets: Record<number, number> = {};

		for (const file of this.files) offsets[file.index] = file.offset;

		return {
			chunkSize: this.chunkSize,
			confirmed,
			eta: speed && speed > 0 ? (total - confirmed) / speed : null,
			failures: this.failures,
			inFlight: this.inFlight,
			offsets,
			problem: this.problem,
			proxyLimit: this.proxyLimit,
			retryAt: this.retryAt,
			speed,
			state: this.state,
			total
		};
	}

	constructor(opts: AdminUploadSchedulerOptions) {
		this.opts = opts;
		this.clock = opts.clock ?? realUploadClock;
		this.files = opts.files.map((file) => ({ ...file }));
		this.ceiling = Math.min(opts.chunkMax, opts.ceiling ?? opts.chunkMax);
		this.chunkSize = Math.max(opts.chunkMin, Math.min(this.ceiling, opts.chunkDefault));

		if (opts.overlapFirst) {
			for (const file of this.files) {
				if (file.offset > 0 && !file.complete) this.overlapPending.add(file.index);
			}
		}
	}

	/** Leave this file out (dropped from the session by the admin). */
	dropFile(index: number): void {
		const file = this.files.find((item) => item.index === index);

		if (file) file.dropped = true;

		if (this.problem?.file === index) this.problem = null;
	}

	/** Stop sending; the chunk in flight is aborted (the server keeps whole chunks only). */
	pause(): void {
		if (this.stopped || FINAL_STATES.has(this.state)) return;

		this.setState(AdminUploadSendState.PAUSED);
		this.requestAbort?.abort();
		this.sleepAbort?.abort();
	}

	/**
	 * Continue after a pause or a refusal: the session is read again first.
	 * `takeover` sends the next chunk with takeover=1 (another tab was sending).
	 */
	resume(options: { takeover?: boolean } = {}): void {
		if (this.stopped || FINAL_STATES.has(this.state)) return;

		if (options.takeover) this.takeoverNext = true;

		// already sending: nothing to restart
		if (this.state === AdminUploadSendState.UPLOADING && this.loopRunning) return;

		this.problem = null;
		this.failures = 0;
		this.failStart = null;
		this.retryAt = null;
		this.needResync = true;
		this.setState(AdminUploadSendState.UPLOADING);
		this.sleepAbort?.abort(); // "Retry now" ends a backoff wait at once
		this.wake?.();
		this.start();
	}

	/** A file picked again (after a reload, or after "not the same file"). */
	setFileBlob(index: number, blob: Blob): void {
		const file = this.files.find((item) => item.index === index);

		if (!file) return;

		file.blob = blob;

		if (file.offset > 0) this.overlapPending.add(index);
	}

	/** Start (or restart) the send loop. */
	start(): void {
		if (this.loopRunning || this.stopped || FINAL_STATES.has(this.state)) return;

		this.loopRunning = true;

		void this.loop().finally(() => {
			this.loopRunning = false;
		});
	}

	/** End for good (cancel, the page leaves the job). */
	stop(): void {
		this.stopped = true;
		this.requestAbort?.abort();
		this.sleepAbort?.abort();
		this.wake?.();
	}

	private acknowledged(length: number): void {
		this.lastAck = this.clock.now();
		this.takeoverNext = false;
		this.failures = 0;
		this.failStart = null;
		this.netFailsAtSize = 0;
		this.crcFails = 0;
		this.writeFails = 0;
		this.offsetMismatches = 0;

		if (length >= this.chunkSize) this.successAtSize.add(this.chunkSize);

		this.sample();
	}

	private applySession(session: Record<string, unknown>): void {
		const files = Array.isArray(session.files) ? (session.files as Record<string, unknown>[]) : [];

		for (const remote of files) {
			const file = this.files.find((item) => item.index === remote.index);

			if (!file) continue;

			file.offset = num(remote.offset) ?? file.offset;
			file.complete = remote.complete === true;
			file.dropped = remote.dropped === true;
		}

		this.serverState = str(session.state);
	}

	/** Wait with backoff after a failure; `state` is RETRYING or SERVER_RESTARTING. */
	private async backoff(state: AdminUploadSendState, minDelay = 0): Promise<void> {
		const now = this.clock.now();

		this.failures++;
		this.failStart ??= now;

		let delay = Math.min(
			ADMIN_UPLOAD.BACKOFF_MAX_MS,
			ADMIN_UPLOAD.BACKOFF_MIN_MS * 2 ** Math.min(this.failures - 1, 16)
		);

		if (now - this.failStart >= ADMIN_UPLOAD.UNREACHABLE_AFTER_MS) {
			state = AdminUploadSendState.UNREACHABLE;
			delay = ADMIN_UPLOAD.UNREACHABLE_RETRY_MS;
		}

		delay = Math.max(delay, minDelay);
		this.retryAt = now + delay;
		this.setState(state);
		await this.sleep(delay);
		this.retryAt = null;
	}

	private block(state: AdminUploadSendState, problem: AdminUploadSendSnapshot['problem']): void {
		this.problem = problem;
		this.setState(state);
	}

	private emit(force = true): void {
		const now = this.clock.now();

		if (!force && now - this.lastEmit < 200) return;

		this.lastEmit = now;
		this.opts.onUpdate?.(this.snapshot);
	}

	/** Read the session; applies the offsets. Returns the answer. */
	private async fetchSession(): Promise<AdminUploadTransportResult> {
		const controller = new AbortController();

		this.requestAbort = controller;

		try {
			const result = await this.opts.transport.getSession(this.opts.sessionId, controller.signal);

			if (result.status === 200 && result.body) {
				this.applySession(result.body);
				this.opts.onSession?.(result.body);
			}

			return result;
		} catch {
			return { body: null, status: 0 };
		} finally {
			if (this.requestAbort === controller) this.requestAbort = null;
		}
	}

	private finish(state: AdminUploadSendState, serverState: string | null): void {
		this.serverState = serverState;
		this.setState(state);
		this.opts.onFinished?.(state, serverState);
	}

	private async handleChunkError(
		result: AdminUploadTransportResult,
		file: AdminUploadSchedulerFile,
		length: number
	): Promise<void> {
		const error = errorOf(result.body);
		const type = str(error.type);
		const message = str(error.message) ?? `HTTP ${result.status}`;

		switch (result.status) {
			case 400:
				if (type === 'crc_mismatch' && ++this.crcFails <= ADMIN_UPLOAD.CRC_RETRIES) return;

				this.block(AdminUploadSendState.FAILED, { code: type ?? 'bad_request', message });

				return;
			case 409:
				switch (type) {
					case 'offset_mismatch': {
						const offset = num(error.offset);

						if (offset !== null) file.offset = offset;

						if (++this.offsetMismatches > 5) {
							this.needResync = true;
							await this.backoff(AdminUploadSendState.RETRYING);
						}

						return;
					}
					case 'content_mismatch':
						this.block(AdminUploadSendState.NOT_SAME_FILE, {
							code: 'content_mismatch',
							file: file.index,
							message
						});

						return;
					case 'chunk_in_progress':
						this.needResync = true;
						await this.backoff(AdminUploadSendState.RETRYING, ADMIN_UPLOAD.BUSY_RETRY_MS);

						return;
					case 'other_client_active':
						this.block(AdminUploadSendState.OTHER_CLIENT, {
							code: 'other_client_active',
							idleMs: num(error.idle_ms) ?? undefined,
							message
						});

						return;
					case 'upload_busy':
						this.finish(AdminUploadSendState.SERVER_BUSY, str(error.state));

						return;
					case 'file_dropped':
						file.dropped = true;

						return;
					default:
						await this.backoff(AdminUploadSendState.RETRYING);

						return;
				}
			case 413:
				if (type === 'chunk_too_large') {
					// the server's own limit (an older or newer server): stay below it
					const max = num(error.chunk_max) ?? this.opts.chunkMax;

					this.ceiling = Math.max(this.opts.chunkMin, Math.min(this.ceiling, max));
					this.chunkSize = Math.max(
						this.opts.chunkMin,
						Math.min(this.chunkSize, this.ceiling, Math.floor(length / 2))
					);

					return;
				}

				// not the server's JSON: a proxy in front of it limits the request size
				this.stepDown(length, true);

				return;
			case 422:
				this.block(AdminUploadSendState.FILE_REJECTED, {
					code: str(error.reason) ?? type ?? 'file_rejected',
					file: num(error.file) ?? file.index,
					message
				});

				return;
			case 429: {
				const seconds = num(error.retry_after_s) ?? 2;

				this.setState(AdminUploadSendState.RETRYING);
				this.retryAt = this.clock.now() + seconds * 1000;
				await this.sleep(seconds * 1000);
				this.retryAt = null;

				return;
			}
			case 507:
				file.offset = num(error.offset) ?? file.offset;
				this.block(AdminUploadSendState.DISK_FULL, {
					code: 'disk_full',
					freeBytes: num(error.free_bytes) ?? undefined,
					message
				});

				return;
			case 500:
				if (type === 'write_failed') {
					file.offset = num(error.offset) ?? file.offset;

					if (++this.writeFails > ADMIN_UPLOAD.WRITE_FAILED_RETRIES) {
						this.block(AdminUploadSendState.FAILED, { code: 'write_failed', message });

						return;
					}
				}

				break;
		}

		if (await this.handleCommon(result)) return;

		this.block(AdminUploadSendState.FAILED, { code: type ?? `http_${result.status}`, message });
	}

	/** Handle a refusal that is the same on the chunk route and on GET ?id=. Returns true when handled. */
	private async handleCommon(result: AdminUploadTransportResult): Promise<boolean> {
		const error = errorOf(result.body);
		const message = str(error.message) ?? `HTTP ${result.status}`;

		switch (result.status) {
			case 401:
				this.block(AdminUploadSendState.KEY_REQUIRED, { code: 'admin_key_required', message });

				return true;
			case 403:
				this.block(AdminUploadSendState.FORBIDDEN, { code: 'admin_forbidden', message });

				return true;
			case 410:
				this.finish(AdminUploadSendState.GONE, str(error.state));

				return true;
			case 503:
				await this.backoff(AdminUploadSendState.SERVER_RESTARTING);

				return true;
			default:
				if (result.status >= 500 || result.status === 429 || result.status === 408) {
					await this.backoff(AdminUploadSendState.RETRYING);

					return true;
				}

				return false;
		}
	}

	/** No HTTP answer to a chunk: ask the server what happened. */
	private async handleNetworkError(
		file: AdminUploadSchedulerFile,
		start: number,
		length: number,
		before: Map<number, number>
	): Promise<void> {
		const session = await this.fetchSession();

		if (this.stopped || this.state === AdminUploadSendState.PAUSED) return;

		if (session.status === 0) {
			await this.backoff(AdminUploadSendState.RETRYING);

			return;
		}

		if (session.status !== 200 || !session.body) {
			if (!(await this.handleCommon(session))) {
				const error = errorOf(session.body);

				this.block(AdminUploadSendState.FAILED, {
					code: str(error.type) ?? `http_${session.status}`,
					message: str(error.message) ?? `HTTP ${session.status}`
				});
			}

			return;
		}

		if (this.serverState !== 'receiving') {
			this.finish(AdminUploadSendState.SERVER_BUSY, this.serverState);

			return;
		}

		const landed = file.offset >= start + length;
		const othersMoved = this.files.some(
			(item) => item.index !== file.index && item.offset > (before.get(item.index) ?? 0)
		);
		const idle = num(session.body.last_client_idle_ms);
		const now = this.clock.now();
		const someoneElse =
			othersMoved ||
			(!landed &&
				idle !== null &&
				idle < ADMIN_UPLOAD.OTHER_CLIENT_MS &&
				(this.lastAck === null || now - this.lastAck > idle + 1000));

		if (landed && !othersMoved) {
			// the chunk went through, only its answer was lost
			this.acknowledged(length);

			return;
		}

		if (someoneElse) {
			this.block(AdminUploadSendState.OTHER_CLIENT, {
				code: 'other_client_active',
				idleMs: idle ?? undefined,
				message: 'another browser tab or client is uploading this file right now'
			});

			return;
		}

		if (
			length >= this.chunkSize &&
			!this.successAtSize.has(this.chunkSize) &&
			++this.netFailsAtSize >= ADMIN_UPLOAD.NET_FAILS_BEFORE_STEP_DOWN &&
			this.chunkSize > this.opts.chunkMin
		) {
			// the connection drops on every chunk of this size: something on the way limits it
			this.stepDown(length, false);

			return;
		}

		await this.backoff(AdminUploadSendState.RETRYING);
	}

	private isPaused(): boolean {
		return this.state === AdminUploadSendState.PAUSED;
	}

	private async loop(): Promise<void> {
		while (!this.stopped && !FINAL_STATES.has(this.state)) {
			if (BLOCKING_STATES.has(this.state)) {
				await new Promise<void>((resolve) => (this.wake = resolve));
				this.wake = null;

				continue;
			}

			if (this.needResync) {
				this.needResync = false;

				const result = await this.fetchSession();

				if (this.stopped || BLOCKING_STATES.has(this.state)) continue;

				if (result.status === 0) {
					this.needResync = true;
					await this.backoff(AdminUploadSendState.RETRYING);

					continue;
				}

				if (result.status !== 200) {
					if (!(await this.handleCommon(result))) {
						this.block(AdminUploadSendState.FAILED, {
							code: `http_${result.status}`,
							message: str(errorOf(result.body).message) ?? `HTTP ${result.status}`
						});
					} else if (!FINAL_STATES.has(this.state)) {
						this.needResync = true;
					}

					continue;
				}

				if (this.serverState !== 'receiving') {
					this.finish(
						this.serverState === 'checking'
							? AdminUploadSendState.DONE
							: AdminUploadSendState.SERVER_BUSY,
						this.serverState
					);

					return;
				}
			}

			const next = this.pickNext();

			if (!next) {
				const open = this.files.filter((file) => !file.dropped && !file.complete);

				if (open.length === 0) {
					this.finish(AdminUploadSendState.DONE, this.serverState);

					return;
				}

				this.block(AdminUploadSendState.NEEDS_FILE, {
					code: 'needs_file',
					file: open[0].index,
					message: 'pick the file again'
				});

				continue;
			}

			if (this.state !== AdminUploadSendState.UPLOADING)
				this.setState(AdminUploadSendState.UPLOADING);

			await this.sendOne(next);
		}
	}

	/** Header bytes of every file first, then file by file. */
	private pickNext(): AdminUploadSchedulerFile | null {
		const open = this.files.filter(
			(file) => !file.dropped && !file.complete && file.offset < file.size && file.blob
		);

		if (open.length === 0) return null;

		const headerFirst = open.find((file) => {
			const need = Math.min(file.size, file.headerEnd ?? this.chunkSize);

			return file.offset < need;
		});

		return headerFirst ?? open[0];
	}

	private sample(): void {
		const now = this.clock.now();
		const bytes = this.files.reduce((sum, file) => sum + (file.dropped ? 0 : file.offset), 0);

		this.confirmedSamples.push({ at: now, bytes });
		this.confirmedSamples = this.confirmedSamples.filter(
			(item) => now - item.at <= ADMIN_UPLOAD.SPEED_WINDOW_MS
		);
	}

	private async sendOne(file: AdminUploadSchedulerFile): Promise<void> {
		let start = file.offset;

		if (this.overlapPending.has(file.index)) {
			this.overlapPending.delete(file.index);
			start = Math.max(0, file.offset - ADMIN_UPLOAD.OVERLAP_BYTES);
		}

		const length = Math.min(this.chunkSize, file.size - start);

		let data: Uint8Array;

		try {
			data = await this.opts.transport.readSlice(file.blob as Blob, start, start + length);
		} catch {
			// the file was moved, changed or removed on this computer since it was picked
			file.blob = null;
			this.block(AdminUploadSendState.NEEDS_FILE, {
				code: 'file_unreadable',
				file: file.index,
				message: 'the picked file can no longer be read: pick it again'
			});

			return;
		}

		if (this.stopped || this.state !== AdminUploadSendState.UPLOADING) return;

		const before = new Map(this.files.map((item) => [item.index, item.offset]));
		const controller = new AbortController();
		const started = this.clock.now();
		const takeover = this.takeoverNext;

		this.requestAbort = controller;
		this.inFlight = 0;

		let result: AdminUploadTransportResult;

		try {
			result = await this.opts.transport.sendChunk({
				crc: crc32Hex(crc32(data)),
				data,
				file: file.index,
				id: this.opts.sessionId,
				offset: start,
				onProgress: (sent) => {
					this.inFlight = Math.max(0, start + sent - file.offset);
					this.emit(false);
				},
				signal: controller.signal,
				takeover,
				verify: false
			});
		} catch {
			result = { body: null, status: 0 };
		} finally {
			if (this.requestAbort === controller) this.requestAbort = null;

			this.inFlight = 0;
		}

		// pause() may have run while the chunk was in flight
		if (this.stopped || this.isPaused()) return;

		if (result.status === 200 && result.body) {
			const body = result.body;
			const elapsed = this.clock.now() - started;

			file.offset = num(body.offset) ?? start + length;
			file.complete = body.file_complete === true;
			this.acknowledged(length);

			if (length >= this.chunkSize && elapsed < ADMIN_UPLOAD.CHUNK_GROW_BELOW_MS) {
				this.chunkSize = Math.min(this.ceiling, this.chunkSize * 2);
			} else if (elapsed > ADMIN_UPLOAD.CHUNK_SHRINK_ABOVE_MS) {
				this.chunkSize = Math.max(this.opts.chunkMin, Math.floor(this.chunkSize / 2));
			}

			const state = str(body.state);
			const fileError =
				body.file_error && typeof body.file_error === 'object'
					? (body.file_error as Record<string, unknown>)
					: null;

			if (fileError && (!state || state === 'receiving')) {
				// the header check of this file refused it (an adapter, a damaged projector ...):
				// nothing more of it is sent until it is dropped (or the upload cancelled)
				this.block(AdminUploadSendState.FILE_REJECTED, {
					code: str(fileError.code) ?? 'file_rejected',
					file: num(body.file) ?? file.index,
					message: str(fileError.message) ?? 'the server refused this file'
				});

				return;
			}

			if (state && state !== 'receiving') {
				this.finish(
					state === 'checking' ? AdminUploadSendState.DONE : AdminUploadSendState.SERVER_BUSY,
					state
				);

				return;
			}

			this.emit();

			return;
		}

		if (result.status === 0) {
			await this.handleNetworkError(file, start, length, before);

			return;
		}

		await this.handleChunkError(result, file, length);
	}

	private setState(state: AdminUploadSendState): void {
		this.state = state;
		this.emit();
	}

	/** Sleep, cut short by pause / resume / stop. */
	private async sleep(ms: number): Promise<void> {
		const controller = new AbortController();

		this.sleepAbort = controller;

		try {
			await this.clock.sleep(ms, controller.signal);
		} finally {
			if (this.sleepAbort === controller) this.sleepAbort = null;
		}
	}

	private speed(): number | null {
		const samples = this.confirmedSamples;

		if (samples.length < 2) return null;

		const first = samples[0];
		const last = samples[samples.length - 1];
		const span = last.at - first.at;

		if (span < 1000) return null;

		return ((last.bytes - first.bytes) * 1000) / span;
	}

	/** A smaller chunk size: the next step of the ladder below the size that failed. */
	private stepDown(length: number, proxy: boolean): void {
		const failed = Math.min(length, this.chunkSize);
		const next = nextChunkSizeDown(failed);

		if (next === null || next < this.opts.chunkMin) {
			this.block(AdminUploadSendState.FAILED, {
				code: 'proxy_limit',
				message: `A proxy in front of the server refuses requests of ${Math.round(failed / 1024)} KiB and more; raise its limit for /admin/ (client_max_body_size).`
			});

			return;
		}

		this.ceiling = next;
		this.chunkSize = next;
		this.netFailsAtSize = 0;

		if (proxy) this.proxyLimit = next;

		this.opts.onCeiling?.(next, proxy);
		this.emit();
	}
}
