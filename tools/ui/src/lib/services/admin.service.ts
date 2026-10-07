/**
 * AdminService - Stateless admin API layer (provision-server, router mode)
 *
 * Wraps the /admin routes: the unauthenticated status probe and the routes
 * protected by the admin key (settings, storage, download pre-flight, and in
 * stage 2 the per-model settings, fit check, model log, GPUs and
 * diagnostics). This is the only place that sends the X-Admin-Key header.
 * Model operations (load, unload, download, delete) are NOT here: they use
 * the public model routes through ModelsService.
 *
 * Every call has a client-side timeout and fails with an {@link AdminApiError}
 * that keeps the server's `error.type`, so the store can tell a rejected
 * admin key from any other failure.
 */

import { base } from '$app/paths';
import {
	ADMIN_AUDIT_TIMEOUTS,
	ADMIN_KEY_HEADER,
	ADMIN_TIMEOUTS,
	ADMIN_UPLOAD_TIMEOUTS,
	API_ADMIN,
	API_ADMIN_AUDIT,
	API_ADMIN_UPLOAD,
	API_SLOTS,
	ERROR_MESSAGES
} from '$lib/constants';
import { AdminErrorType } from '$lib/enums';
import type {
	AdminSlotsBusy,
	ApiAdminDiagnostics,
	ApiAdminDownloadCheck,
	ApiAdminFitCheck,
	ApiAdminGpus,
	ApiAdminModelLog,
	ApiAdminModelSettingsList,
	ApiAdminModelSettingsRow,
	ApiAdminModelSettingsSaveResult,
	ApiAdminModelSettingsUpdate,
	ApiAdminSettings,
	ApiAdminSettingsUpdate,
	ApiAdminStatus,
	ApiAdminStorage
} from '$lib/types';
import type { AdminAuditDownload, ApiAdminAuditList } from '$lib/types/admin-audit';
import type {
	ApiAdminDeletePreview,
	ApiAdminDeleteRequest,
	ApiAdminDeleteResult,
	ApiAdminUploadCreate,
	ApiAdminUploadCreateResult,
	ApiAdminUploadInfo,
	ApiAdminUploadSession,
	ApiAdminUploadSessionResult,
	ApiAdminUploadUpdate
} from '$lib/types/admin-upload';
// direct module imports, not the utils barrel: api-headers pulls in the settings
// store, and going through the barrel makes the bundler warn about a chunk cycle
import { auditDownloadName } from '$lib/utils/admin-audit';
import { ApiError } from '$lib/utils/api-fetch';
import { getAuthHeaders, getJsonHeaders } from '$lib/utils/api-headers';

/** `type` of an AdminApiError for a request the caller aborted itself. */
const ADMIN_REQUEST_CANCELLED = 'cancelled';

/**
 * Failed admin request. `status` 0 means the request never got an HTTP
 * answer (network error, timeout, or aborted by the caller).
 */
export class AdminApiError extends ApiError {
	/**
	 * The whole `error` object of the answer: the extra fields some refusals
	 * carry (`reason`, `by`, `other`, `state`, `chunk_max` ...), null when the
	 * body was not the server's JSON
	 */
	details: Record<string, unknown> | null = null;
	/** The `param` of a 400 / 409 answer: the setting it is about */
	param: string | null = null;
	type: string | null;

	/** The caller aborted the request (a newer request replaced it). */
	get isCancelled(): boolean {
		return this.status === 0 && this.type === ADMIN_REQUEST_CANCELLED;
	}

	/** The server rejected (or did not get) the admin key. */
	get isKeyRejected(): boolean {
		return this.status === 401 && this.type === AdminErrorType.KEY_REQUIRED;
	}

	constructor(message: string, status: number, type: string | null = null) {
		super(message, status);
		this.name = 'AdminApiError';
		this.type = type;
	}
}

interface AdminRequestInit {
	body?: unknown;
	key?: string | null;
	method?: 'GET' | 'POST';
	/** Read a successful answer another way than as JSON (a download) */
	read?: (response: Response) => Promise<unknown>;
	/** Abort the request early (a newer request replaced it) */
	signal?: AbortSignal;
	timeoutMs?: number;
}

export class AdminService {
	/** Cancel an upload session: its data is deleted. */
	static async cancelUpload(key: string, id: string): Promise<ApiAdminUploadSessionResult> {
		return AdminService.request<ApiAdminUploadSessionResult>(API_ADMIN_UPLOAD.CANCEL, {
			body: { id },
			key,
			method: 'POST',
			timeoutMs: ADMIN_UPLOAD_TIMEOUTS.CANCEL_MS
		});
	}

	/**
	 * Advisory pre-flight for a Hugging Face download: files, sizes, free
	 * space and warnings. Never blocks a download.
	 *
	 * @param key - Admin key
	 * @param model - `user/repo[:QUANT]`
	 */
	static async checkDownload(key: string, model: string): Promise<ApiAdminDownloadCheck> {
		return AdminService.request<ApiAdminDownloadCheck>(API_ADMIN.DOWNLOAD_CHECK, {
			body: { model },
			key,
			method: 'POST',
			timeoutMs: ADMIN_TIMEOUTS.DOWNLOAD_CHECK_MS
		});
	}

	/**
	 * Exact fit of draft settings: the server runs llama.cpp on them in a
	 * helper process (a dry run without allocating weights). A rejected dry
	 * run is still a 200 answer with `ok: false`; 409 `check_busy` while
	 * another check runs.
	 *
	 * @param settings - Changes over the saved settings, the POST shape
	 */
	static async checkModelSettings(
		key: string,
		model: string,
		settings: ApiAdminModelSettingsUpdate['settings'],
		signal?: AbortSignal
	): Promise<ApiAdminFitCheck> {
		return AdminService.request<ApiAdminFitCheck>(API_ADMIN.MODEL_SETTINGS_CHECK, {
			body: { model, settings: settings ?? {} },
			key,
			method: 'POST',
			signal,
			timeoutMs: ADMIN_TIMEOUTS.FIT_CHECK_MS
		});
	}

	/**
	 * Create an upload session (201), or check name, settings and disk with
	 * `dry_run` (200, `upload.id` null). Refusals: 400 invalid_name, 409
	 * name_taken / field_locked / too_many_uploads / upload_unavailable.
	 */
	static async createUpload(
		key: string,
		body: ApiAdminUploadCreate
	): Promise<ApiAdminUploadCreateResult> {
		return AdminService.request<ApiAdminUploadCreateResult>(API_ADMIN_UPLOAD.UPLOAD, {
			body,
			key,
			method: 'POST',
			timeoutMs: ADMIN_UPLOAD_TIMEOUTS.CREATE_MS
		});
	}

	/**
	 * Delete a models_dir model (only the files its preset uses) or, with
	 * `projector_only`, its projector. `dry_run` lists what would go and what
	 * stays.
	 */
	static async deleteModelFiles(
		key: string,
		body: ApiAdminDeleteRequest
	): Promise<ApiAdminDeletePreview | ApiAdminDeleteResult> {
		return AdminService.request<ApiAdminDeletePreview | ApiAdminDeleteResult>(
			API_ADMIN_UPLOAD.DELETE,
			{
				body,
				key,
				method: 'POST',
				timeoutMs: body.dry_run
					? ADMIN_UPLOAD_TIMEOUTS.DELETE_PREVIEW_MS
					: ADMIN_UPLOAD_TIMEOUTS.DELETE_MS
			}
		);
	}

	/**
	 * The raw audit log (GET /admin/audit?format=jsonl): JSON lines of both
	 * files and the memory rings, oldest first, with the filters of `query`
	 * (`limit` does not apply).
	 *
	 * @param query - From buildAuditQuery(filters, { format: 'jsonl' })
	 */
	static async downloadAudit(key: string, query: string): Promise<AdminAuditDownload> {
		return AdminService.request<AdminAuditDownload>(`${API_ADMIN_AUDIT}${query}`, {
			key,
			read: async (response) => ({
				blob: await response.blob(),
				filename: auditDownloadName(response.headers.get('Content-Disposition'))
			}),
			timeoutMs: ADMIN_AUDIT_TIMEOUTS.DOWNLOAD_MS
		});
	}

	/**
	 * One page of the audit log, newest first (stage 3 M3). 400 with `param`
	 * before / limit / kind / format for a bad query.
	 *
	 * @param query - From buildAuditQuery(filters, { before, limit })
	 */
	static async getAudit(
		key: string,
		query: string,
		signal?: AbortSignal
	): Promise<ApiAdminAuditList> {
		return AdminService.request<ApiAdminAuditList>(`${API_ADMIN_AUDIT}${query}`, {
			key,
			signal,
			timeoutMs: ADMIN_AUDIT_TIMEOUTS.LIST_MS
		});
	}

	/** Deployment, GPU backend, vendor tool, state dir... as ok / warning / error items. */
	static async getDiagnostics(key: string): Promise<ApiAdminDiagnostics> {
		return AdminService.request<ApiAdminDiagnostics>(API_ADMIN.DIAGNOSTICS, { key });
	}

	/**
	 * GPU cards with memory and the models placed on them.
	 *
	 * @param refresh - Ask for a new vendor-tool reading instead of the cached one
	 */
	static async getGpus(key: string, refresh = false): Promise<ApiAdminGpus> {
		return AdminService.request<ApiAdminGpus>(
			refresh ? `${API_ADMIN.GPUS}?refresh=1` : API_ADMIN.GPUS,
			{ key, timeoutMs: ADMIN_TIMEOUTS.GPUS_MS }
		);
	}

	/** Output tail of the last instance of a model, its failure reason and last failure. */
	static async getModelLog(key: string, model: string): Promise<ApiAdminModelLog> {
		return AdminService.request<ApiAdminModelLog>(
			`${API_ADMIN.MODEL_LOG}?model=${encodeURIComponent(model)}`,
			{ key }
		);
	}

	/** Per-model settings of every model (cached facts only), unattached entries, file state. */
	static async getModelSettings(key: string): Promise<ApiAdminModelSettingsList> {
		return AdminService.request<ApiAdminModelSettingsList>(API_ADMIN.MODEL_SETTINGS, { key });
	}

	/** One model row with its facts read on demand. */
	static async getModelSettingsRow(key: string, model: string): Promise<ApiAdminModelSettingsRow> {
		return AdminService.request<ApiAdminModelSettingsRow>(
			`${API_ADMIN.MODEL_SETTINGS}?model=${encodeURIComponent(model)}`,
			{ key }
		);
	}

	static async getSettings(key: string): Promise<ApiAdminSettings> {
		return AdminService.request<ApiAdminSettings>(API_ADMIN.SETTINGS, { key });
	}

	/**
	 * Busy slots of a running model through the public GET /slots route
	 * (no admin key). null when the model is not running or the route is off.
	 */
	static async getSlotsBusy(model: string): Promise<AdminSlotsBusy | null> {
		try {
			const slots = await AdminService.request<{ is_processing?: boolean }[]>(
				`${API_SLOTS.LIST.replace(/^\./, '')}?model=${encodeURIComponent(model)}&autoload=false`,
				{ timeoutMs: ADMIN_TIMEOUTS.SLOTS_MS }
			);

			if (!Array.isArray(slots)) return null;

			return { busy: slots.filter((slot) => slot?.is_processing).length, total: slots.length };
		} catch {
			return null;
		}
	}

	/**
	 * Probe / status. Answers 200 without a key (three fields, `authorized:
	 * false`); with a valid key the body carries the full status.
	 *
	 * @param key - Admin key to verify, or null for the anonymous probe
	 */
	static async getStatus(key: string | null): Promise<ApiAdminStatus> {
		const status = await AdminService.request<ApiAdminStatus>(API_ADMIN.STATUS, {
			key,
			timeoutMs: ADMIN_TIMEOUTS.PROBE_MS
		});

		// a static host or proxy may answer any path with 200: require the real shape
		if (!status || status.admin !== true || typeof status.authorized !== 'boolean') {
			throw new AdminApiError('Unexpected answer from the admin API', 0);
		}

		return status;
	}

	static async getStorage(key: string): Promise<ApiAdminStorage> {
		return AdminService.request<ApiAdminStorage>(API_ADMIN.STORAGE, { key });
	}

	/** One upload session with its files, checks and result (410 when gone). */
	static async getUpload(key: string, id: string): Promise<ApiAdminUploadSession> {
		return AdminService.request<ApiAdminUploadSession>(
			`${API_ADMIN_UPLOAD.UPLOAD}?id=${encodeURIComponent(id)}`,
			{ key, timeoutMs: ADMIN_UPLOAD_TIMEOUTS.GET_MS }
		);
	}

	/** Whether uploads are possible, the limits and the sessions (finished ones stay 24 h). */
	static async getUploads(key: string): Promise<ApiAdminUploadInfo> {
		return AdminService.request<ApiAdminUploadInfo>(API_ADMIN_UPLOAD.UPLOAD, {
			key,
			timeoutMs: ADMIN_UPLOAD_TIMEOUTS.GET_MS
		});
	}

	/**
	 * Publish a ready session (202), or run its checks again when they could
	 * not run or the SHA-256 did not match ("Retry").
	 */
	static async publishUpload(
		key: string,
		body: { id: string; ignore_sha256_mismatch?: boolean; without_settings?: boolean }
	): Promise<ApiAdminUploadSessionResult> {
		return AdminService.request<ApiAdminUploadSessionResult>(API_ADMIN_UPLOAD.PUBLISH, {
			body,
			key,
			method: 'POST',
			timeoutMs: ADMIN_UPLOAD_TIMEOUTS.PUBLISH_MS
		});
	}

	/**
	 * Run the device inventory again ("Re-detect GPUs"): it briefly creates a
	 * context on every GPU. Answers the same body as GET /admin/gpus; 409
	 * `rescan_busy` while one runs.
	 */
	static async rescanGpus(key: string): Promise<ApiAdminGpus> {
		return AdminService.request<ApiAdminGpus>(API_ADMIN.GPUS_RESCAN, {
			body: {},
			key,
			method: 'POST',
			timeoutMs: ADMIN_TIMEOUTS.GPUS_RESCAN_MS
		});
	}

	/**
	 * Save, remove or reset the per-model settings of a model; `restart`
	 * restarts a running model (or loads a stopped one). A running model
	 * without `restart` is refused with 409 `restart_required`.
	 */
	static async saveModelSettings(
		key: string,
		update: ApiAdminModelSettingsUpdate
	): Promise<ApiAdminModelSettingsSaveResult> {
		return AdminService.request<ApiAdminModelSettingsSaveResult>(API_ADMIN.MODEL_SETTINGS, {
			body: update,
			key,
			method: 'POST',
			timeoutMs: ADMIN_TIMEOUTS.MODEL_SETTINGS_SAVE_MS
		});
	}

	/**
	 * Save any subset of the settings; the server validates, persists and
	 * applies them, then returns the settings now in effect.
	 */
	static async saveSettings(
		key: string,
		update: ApiAdminSettingsUpdate
	): Promise<ApiAdminSettings> {
		return AdminService.request<ApiAdminSettings>(API_ADMIN.SETTINGS, {
			body: update,
			key,
			method: 'POST',
			timeoutMs: ADMIN_TIMEOUTS.SETTINGS_SAVE_MS
		});
	}

	/** Change the name, the settings, an expected SHA-256 of a session, or drop one of its files. */
	static async updateUpload(
		key: string,
		body: ApiAdminUploadUpdate
	): Promise<ApiAdminUploadSessionResult> {
		return AdminService.request<ApiAdminUploadSessionResult>(API_ADMIN_UPLOAD.UPDATE, {
			body,
			key,
			method: 'POST',
			timeoutMs: ADMIN_UPLOAD_TIMEOUTS.UPDATE_MS
		});
	}

	private static async request<T>(path: string, init: AdminRequestInit = {}): Promise<T> {
		const { body, key, method = 'GET', read, signal, timeoutMs = ADMIN_TIMEOUTS.REQUEST_MS } = init;
		const headers: Record<string, string> =
			body === undefined ? getAuthHeaders() : getJsonHeaders();

		if (key) headers[ADMIN_KEY_HEADER] = key;

		const controller = new AbortController();
		const timer = setTimeout(() => controller.abort(), timeoutMs);
		const cancel = () => controller.abort();

		signal?.addEventListener('abort', cancel);

		try {
			let response: Response;

			try {
				response = await fetch(`${base}${path}`, {
					body: body === undefined ? undefined : JSON.stringify(body),
					cache: 'no-store',
					headers,
					method,
					signal: controller.signal
				});
			} catch {
				if (signal?.aborted) throw new AdminApiError('Cancelled', 0, ADMIN_REQUEST_CANCELLED);

				throw new AdminApiError(
					controller.signal.aborted
						? ERROR_MESSAGES.NETWORK.TIMEOUT
						: ERROR_MESSAGES.NETWORK.UNREACHABLE,
					0
				);
			}

			if (!response.ok) {
				throw await AdminService.toError(response);
			}

			try {
				return (read ? await read(response) : await response.json()) as T;
			} catch {
				if (signal?.aborted) throw new AdminApiError('Cancelled', 0, ADMIN_REQUEST_CANCELLED);

				throw new AdminApiError(
					controller.signal.aborted
						? ERROR_MESSAGES.NETWORK.TIMEOUT
						: 'Unexpected answer from the admin API',
					0
				);
			}
		} finally {
			clearTimeout(timer);
			signal?.removeEventListener('abort', cancel);
		}
	}

	/** Build the error for a non-2xx answer, keeping `error.type` when the body has one. */
	private static async toError(response: Response): Promise<AdminApiError> {
		let message =
			`${ERROR_MESSAGES.HTTP.GENERIC}: ${response.status} ${response.statusText}`.trim();
		let type: string | null = null;
		let param: string | null = null;
		let details: Record<string, unknown> | null = null;

		try {
			const data = await response.json();

			if (typeof data?.error?.message === 'string') message = data.error.message;

			if (typeof data?.error?.type === 'string') type = data.error.type;

			if (typeof data?.error?.param === 'string') param = data.error.param;

			if (data?.error && typeof data.error === 'object') details = data.error;
		} catch {
			// not JSON (a proxy error page): keep the status line
		}

		const error = new AdminApiError(message, response.status, type);

		error.param = param;
		error.details = details;

		return error;
	}
}
