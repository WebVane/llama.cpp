/**
 * AdminUploadService - the browser transport of the GGUF upload (admin
 * panel, stage 3).
 *
 * Chunks go through XMLHttpRequest, the only browser API with upload
 * progress: raw bytes with Content-Type application/octet-stream, the admin
 * key, the API key (`Authorization`, for a server started with --api-key),
 * the tab's X-Upload-Client id and the chunk's X-Chunk-CRC32. A chunk that
 * stops making progress for a minute (or gets no answer two minutes after
 * its last byte) is aborted and reported as a network error, which the
 * scheduler answers by reading the session again.
 *
 * Results are plain {status, body} values, never exceptions: the scheduler
 * decides what each answer means.
 */

import { base } from '$app/paths';
import {
	ADMIN_KEY_HEADER,
	ADMIN_UPLOAD,
	ADMIN_UPLOAD_HEADERS,
	ADMIN_UPLOAD_TIMEOUTS,
	API_ADMIN_UPLOAD
} from '$lib/constants';
import type {
	AdminUploadChunkRequest,
	AdminUploadTransport,
	AdminUploadTransportResult
} from '$lib/utils/admin-upload-scheduler';
// direct module import, not the utils barrel (see admin.service.ts)
import { getAuthHeaders } from '$lib/utils/api-headers';

function parseJson(text: string, contentType: string | null): Record<string, unknown> | null {
	if (!contentType || !contentType.toLowerCase().includes('json')) return null;

	try {
		const data = JSON.parse(text);

		return data && typeof data === 'object' ? (data as Record<string, unknown>) : null;
	} catch {
		return null;
	}
}

function sendChunk(
	request: AdminUploadChunkRequest,
	key: string | null,
	clientId: string
): Promise<AdminUploadTransportResult> {
	return new Promise((resolve) => {
		const params = new URLSearchParams({
			file: String(request.file),
			id: request.id,
			offset: String(request.offset)
		});

		if (request.takeover) params.set('takeover', '1');

		if (request.verify) params.set('verify', '1');

		const xhr = new XMLHttpRequest();

		let settled = false;
		let lastProgress = Date.now();
		let sentAll = false;
		let sentAllAt = 0;
		let watchdog: ReturnType<typeof setInterval> | null = null;

		const finish = (result: AdminUploadTransportResult) => {
			if (settled) return;

			settled = true;

			if (watchdog) clearInterval(watchdog);

			request.signal.removeEventListener('abort', onAbort);
			resolve(result);
		};
		const onAbort = () => {
			xhr.abort();
			finish({ aborted: true, body: null, status: 0 });
		};

		xhr.open('POST', `${base}${API_ADMIN_UPLOAD.CHUNK}?${params.toString()}`);

		for (const [name, value] of Object.entries(getAuthHeaders())) {
			xhr.setRequestHeader(name, value);
		}

		xhr.setRequestHeader('Content-Type', 'application/octet-stream');

		if (key) xhr.setRequestHeader(ADMIN_KEY_HEADER, key);

		xhr.setRequestHeader(ADMIN_UPLOAD_HEADERS.CLIENT, clientId);
		xhr.setRequestHeader(ADMIN_UPLOAD_HEADERS.CRC32, request.crc);

		xhr.upload.onprogress = (event) => {
			lastProgress = Date.now();
			request.onProgress?.(event.loaded);
		};
		xhr.upload.onload = () => {
			sentAll = true;
			sentAllAt = Date.now();
		};
		xhr.onload = () =>
			finish({
				body: parseJson(xhr.responseText, xhr.getResponseHeader('Content-Type')),
				status: xhr.status
			});
		xhr.onerror = () => finish({ body: null, status: 0 });
		xhr.ontimeout = () => finish({ body: null, status: 0 });
		xhr.onabort = () => finish({ aborted: request.signal.aborted, body: null, status: 0 });

		watchdog = setInterval(() => {
			const now = Date.now();
			const stalled = !sentAll && now - lastProgress > ADMIN_UPLOAD.STALL_TIMEOUT_MS;
			const noAnswer = sentAll && now - sentAllAt > ADMIN_UPLOAD.RESPONSE_TIMEOUT_MS;

			if (stalled || noAnswer) {
				xhr.abort();
				finish({ body: null, status: 0 });
			}
		}, 5_000);

		if (request.signal.aborted) {
			onAbort();

			return;
		}

		request.signal.addEventListener('abort', onAbort);
		xhr.send(request.data as unknown as XMLHttpRequestBodyInit);
	});
}

async function getSession(
	id: string,
	key: string | null,
	signal: AbortSignal
): Promise<AdminUploadTransportResult> {
	const headers: Record<string, string> = getAuthHeaders();

	if (key) headers[ADMIN_KEY_HEADER] = key;

	const controller = new AbortController();
	const timer = setTimeout(() => controller.abort(), ADMIN_UPLOAD_TIMEOUTS.GET_MS);
	const onAbort = () => controller.abort();

	signal.addEventListener('abort', onAbort);

	try {
		const response = await fetch(`${base}${API_ADMIN_UPLOAD.UPLOAD}?id=${encodeURIComponent(id)}`, {
			cache: 'no-store',
			headers,
			signal: controller.signal
		});
		const text = await response.text();

		return {
			body: parseJson(text, response.headers.get('Content-Type')),
			status: response.status
		};
	} catch {
		return { aborted: signal.aborted, body: null, status: 0 };
	} finally {
		clearTimeout(timer);
		signal.removeEventListener('abort', onAbort);
	}
}

export class AdminUploadService {
	/**
	 * The transport of the chunk scheduler.
	 *
	 * @param getKey - The admin key at the time of each request (it can change mid-upload)
	 * @param clientId - X-Upload-Client of this tab
	 */
	static createTransport(getKey: () => string | null, clientId: string): AdminUploadTransport {
		return {
			getSession: (id, signal) => getSession(id, getKey(), signal),
			readSlice: async (file, start, end) =>
				new Uint8Array(await file.slice(start, end).arrayBuffer()),
			sendChunk: (request) => sendChunk(request, getKey(), clientId)
		};
	}
}
