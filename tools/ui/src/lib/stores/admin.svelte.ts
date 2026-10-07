/**
 * adminStore - Admin panel state (provision-server, router mode)
 *
 * Owns the GET /admin/status probe that decides whether the admin panel
 * exists at this address, the admin key, the protected settings / storage
 * snapshots and the bookkeeping of the Models tab (downloads, exit codes,
 * pending row actions, result lines).
 *
 * The model table itself is fed by modelsStore: loading, unloading,
 * downloading and deleting go through the server's public model routes, and
 * their outcome is read from the /models/sse feed. Only the settings, the
 * storage figures and the download pre-flight need the admin key, which is
 * sent by AdminService and nowhere else.
 *
 * The key lives in memory and sessionStorage; localStorage is used only when
 * the user ticked "Remember on this device".
 */

import { browser } from '$app/environment';
import {
	ADMIN_AUDIT,
	ADMIN_FEED_REOPEN_REFRESH_MIN_MS,
	ADMIN_GPU_REFRESH,
	ADMIN_KEY_STORAGE_KEY,
	ADMIN_NOTICES_MAX,
	ADMIN_REFRESH_DEBOUNCE_MS,
	ADMIN_TIMEOUTS
} from '$lib/constants';
import {
	AdminDownloadOutcome,
	AdminErrorType,
	AdminFeature,
	AdminModelOperation,
	AdminNoticeLevel,
	AdminProbeState,
	AdminStorageDirKind,
	ServerModelsSseEventType,
	ServerModelStatus
} from '$lib/enums';
import { AdminApiError, AdminService } from '$lib/services/admin.service';
import { ModelsService } from '$lib/services/models.service';
// direct imports between stores, not via the barrel, to avoid circular deps
import { modelsStore } from '$lib/stores/models/index.svelte';
import type {
	AdminDownload,
	AdminDownloadResult,
	AdminNotice,
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
	ApiAdminStorage,
	ApiAdminStorageDir,
	ApiModelsSseDownloadData,
	ApiModelsSseEvent
} from '$lib/types';
import type { ApiAdminAuditStatus } from '$lib/types/admin-audit';
import { ApiError, cacheModelIds, resolveDownloadResult } from '$lib/utils';
import { SvelteMap } from 'svelte/reactivity';
import { toast } from 'svelte-sonner';

/** Grace period for a final download event that is still on its way. */
const DOWNLOAD_RECONCILE_DELAY_MS = 1500;

function errorMessage(error: unknown): string {
	return error instanceof Error ? error.message : String(error);
}

/** Why the probe failed, in words an operator can act on. */
function describeProbeFailure(error: unknown): string {
	if (error instanceof AdminApiError) {
		if (error.status === 404) {
			return 'The server answered 404 for /admin/status: the admin panel is not enabled on this server, or a proxy in front of it hides /admin.';
		}

		if (error.status === 403) {
			// admin_forbidden: the allow-list of the server or of nginx; the server's message names
			// the address it saw, or says that its allow-list is invalid and locks /admin for everybody
			return error.type === AdminErrorType.FORBIDDEN && error.message
				? `${error.message} (HTTP 403).`
				: 'Access to /admin is denied for this address (HTTP 403).';
		}

		if (error.status === 401) {
			return 'The server asked for its API key before it would answer (HTTP 401).';
		}

		if (error.status > 0) return `HTTP ${error.status}: ${error.message}`;
	}

	return errorMessage(error);
}

class AdminStore {
	/** A key typed into the key form is being verified. */
	checkingKey = $state(false);
	/** GET /admin/diagnostics, read when the Settings tab is opened. */
	diagnostics = $state<ApiAdminDiagnostics | null>(null);
	diagnosticsError = $state<string | null>(null);
	diagnosticsLoading = $state(false);
	/** Downloads followed by this page, keyed by the name the download row lives under. */
	downloads = new SvelteMap<string, AdminDownload>();
	/** Exit code of the last unload seen on the feed, per model id. */
	exitCodes = new SvelteMap<string, number>();
	/**
	 * The admin API answered 403 for this address (the server's allow-list or
	 * the one of a proxy), at the probe or on any later admin request: the
	 * sidebar item is hidden and a direct visit goes back to the chat.
	 */
	forbidden = $state(false);
	/** GET /admin/gpus: GPU cards; null until read (or on a server without the route). */
	gpus = $state<ApiAdminGpus | null>(null);
	gpusError = $state<string | null>(null);
	/** When the last GPU answer arrived (ms since the epoch), for "updated N s ago". */
	gpusLoadedAt = $state<number | null>(null);
	gpusLoading = $state(false);
	/** "Re-detect GPUs" is running. */
	gpusRescanning = $state(false);
	keyError = $state<string | null>(null);
	/** GET /admin/models/settings: one row per model, unattached entries, file state. */
	modelSettings = $state<ApiAdminModelSettingsList | null>(null);
	modelSettingsError = $state<string | null>(null);
	/** Result lines shown above the model table until dismissed. */
	notices = $state<AdminNotice[]>([]);
	/** Row actions waiting for the server, per model id. */
	operations = new SvelteMap<string, AdminModelOperation>();
	probeError = $state<string | null>(null);
	probeState = $state<AdminProbeState>(AdminProbeState.IDLE);
	rescanning = $state(false);
	saveError = $state<string | null>(null);
	savingSettings = $state(false);
	settings = $state<ApiAdminSettings | null>(null);
	settingsError = $state<string | null>(null);
	settingsLoading = $state(false);
	/**
	 * A download started from this page finished with a new model: the Models
	 * tab opens its settings dialog (placement right after the download).
	 */
	settingsRequest = $state<string | null>(null);
	status = $state<ApiAdminStatus | null>(null);
	storage = $state<ApiAdminStorage | null>(null);
	storageError = $state<string | null>(null);

	private feedOpenStop: (() => void) | null = null;
	private feedRefreshedAt = 0;
	private feedRefreshTimer: ReturnType<typeof setTimeout> | null = null;
	private feedStop: (() => void) | null = null;
	private gpuInterval: ReturnType<typeof setInterval> | null = null;
	private gpuPromise: Promise<void> | null = null;
	private gpuQueued: 'none' | 'cached' | 'fresh' = 'none';
	private gpuStatusTimer: ReturnType<typeof setTimeout> | null = null;
	/** The admin key, in memory. Never rendered and never logged. */
	private key: string | null = null;
	private modelSettingsPromise: Promise<void> | null = null;
	private modelSettingsQueued = false;
	private modelSettingsTimer: ReturnType<typeof setTimeout> | null = null;
	/** The Models tab is the one shown: only then the GPU cards refresh on a timer. */
	private modelsTabVisible = false;
	private noticeSeq = 0;
	/** Called after the key was accepted (the key form, or a probe with a saved key). */
	private onUnlocked = new Set<() => void>();
	/** The admin page is mounted (enter / leave): the stage-2 refreshes run only then. */
	private pageOpen = false;
	private probePromise: Promise<void> | null = null;
	private refreshPromise: Promise<boolean> | null = null;
	private refreshQueued = false;
	private refreshTimer: ReturnType<typeof setTimeout> | null = null;
	/** Last status seen on the feed per model: a progress update repeats it and refreshes nothing. */
	private seenStatus = new Map<string, string>();
	/** Loads stopped from this page: their next unload is not reported as a failure. */
	private stoppedByUser = new Set<string>();
	private visibilityListening = false;

	/**
	 * GPU devices this server detected (`build_devices`), empty on a CPU-only
	 * build and on a server without stage 2.
	 */
	get buildDevices(): string[] {
		return (
			this.modelSettings?.build_devices ??
			this.gpus?.build_devices ??
			this.settings?.build_devices ??
			[]
		);
	}

	/** Directory downloads go to, with its free space; null until storage is loaded. */
	get cacheDir(): ApiAdminStorageDir | null {
		return this.storage?.dirs.find((dir) => dir.kind === AdminStorageDirKind.CACHE) ?? null;
	}

	/**
	 * The server accepted the admin key held by this page. `status.authorized`
	 * is only ever true while the key is in memory (see runProbe / submitKey).
	 */
	get isAuthorized(): boolean {
		return this.isAvailable && this.status?.authorized === true;
	}

	/** The probe succeeded: the admin panel exists at this address. */
	get isAvailable(): boolean {
		return this.probeState === AdminProbeState.AVAILABLE;
	}

	/** False when the server could not obtain any admin key (protected routes answer 503). */
	get isKeyConfigured(): boolean {
		return this.status?.key_configured !== false;
	}

	/** Pinned model ids in effect, from the last settings snapshot. */
	get pinnedIds(): string[] {
		return this.settings?.pinned ?? [];
	}

	/**
	 * A fresher `audit` object than the one of the last status probe (every
	 * GET /admin/audit answer carries one): keeps the audit_not_persisted
	 * warning of the page in step without another probe.
	 */
	applyAuditStatus(audit: ApiAdminAuditStatus): void {
		const status = this.status;

		if (!status?.authorized || !audit || typeof audit.persisted !== 'boolean') return;

		if (JSON.stringify(status.audit) === JSON.stringify(audit)) return;

		const warnings = (status.warnings ?? []).filter(
			(code) => code !== ADMIN_AUDIT.NOT_PERSISTED_WARNING
		);

		if (!audit.persisted) warnings.push(ADMIN_AUDIT.NOT_PERSISTED_WARNING);

		this.status = { ...status, audit, warnings };
	}

	/**
	 * Cancel a running download: POST /models/unload on the downloading entry,
	 * straight through the service (ModelStatusManager.unload() returns early
	 * for a model that is not loaded). The result is the download_failed that
	 * follows on the feed, reported as "Cancelled".
	 */
	async cancelDownload(name: string): Promise<void> {
		if (this.operations.has(name)) return;

		const download = this.trackDownload(name, true);

		this.downloads.set(name, { ...download, cancelRequested: true });
		this.operations.set(name, AdminModelOperation.CANCELLING);

		try {
			await ModelsService.unload(name, { timeoutMs: ADMIN_TIMEOUTS.CANCEL_MS });
		} catch (error) {
			const current = this.downloads.get(name);

			if (current) this.downloads.set(name, { ...current, cancelRequested: false });

			this.operations.delete(name);
			this.notify(AdminNoticeLevel.ERROR, `Could not cancel ${name}: ${errorMessage(error)}`);

			return;
		}

		// covers a feed that missed the final event
		await this.refreshModels();
	}

	/**
	 * Advisory pre-flight for a download. Throws on failure so the dialog can
	 * show the reason next to the input.
	 */
	async checkDownload(model: string): Promise<ApiAdminDownloadCheck> {
		return this.guarded((key) => AdminService.checkDownload(key, model));
	}

	/**
	 * Exact fit of draft settings by llama.cpp on the server. Throws; a
	 * rejected dry run is an answer with `ok: false`, not an error.
	 */
	async checkModelSettings(
		model: string,
		settings: ApiAdminModelSettingsUpdate['settings'],
		signal?: AbortSignal
	): Promise<ApiAdminFitCheck> {
		return this.guarded((key) => AdminService.checkModelSettings(key, model, settings, signal));
	}

	/**
	 * The admin key held by this page, for the upload transport (the chunk
	 * requests are sent with XMLHttpRequest, not through AdminService). Never
	 * rendered and never logged.
	 */
	currentKey(): string | null {
		return this.key;
	}

	/**
	 * Delete a cache model from disk through the public DELETE /models route.
	 * @param removeSettings - afterwards also remove its saved per-model settings
	 * (otherwise a later download of the same model gets them back)
	 * @returns true when the server removed it
	 */
	async deleteModel(id: string, removeSettings = false): Promise<boolean> {
		if (this.operations.has(id)) return false;

		this.operations.set(id, AdminModelOperation.DELETING);

		try {
			await ModelsService.remove(id, { timeoutMs: ADMIN_TIMEOUTS.REMOVE_MS });

			const settingsRemoved =
				removeSettings && this.hasFeature(AdminFeature.MODEL_SETTINGS)
					? await this.removeModelSettings(id)
					: false;

			this.notify(
				AdminNoticeLevel.SUCCESS,
				settingsRemoved ? `Deleted ${id} and its saved settings` : `Deleted ${id}`
			);

			return true;
		} catch (error) {
			this.notify(AdminNoticeLevel.ERROR, `Could not delete ${id}: ${errorMessage(error)}`);

			return false;
		} finally {
			this.operations.delete(id);
			await this.refreshModels();
			this.scheduleProtectedRefresh();
		}
	}

	dismissNotice(id: number): void {
		this.notices = this.notices.filter((notice) => notice.id !== id);
	}

	/**
	 * Called when the admin page opens: makes sure the probe ran (a direct
	 * visit in a mode where the startup probe did not), starts following the
	 * model feed and loads what the page shows. A failed probe is not retried
	 * here - the page offers a Retry button instead.
	 */
	async enter(): Promise<void> {
		this.pageOpen = true;

		if (this.probeState === AdminProbeState.IDLE || this.probeState === AdminProbeState.CHECKING) {
			await this.probe();
		} else if (this.probeState === AdminProbeState.AVAILABLE) {
			await this.runProbe(true);
		}

		await this.openPage();
	}

	/** Drop the admin key from memory and from both storages; back to the key form. */
	forgetKey(): void {
		this.requireKey(null);
	}

	/** Busy slots of a running model (public GET /slots), null when unknown. */
	async getSlotsBusy(model: string): Promise<AdminSlotsBusy | null> {
		return AdminService.getSlotsBusy(model);
	}

	/**
	 * The server lists a stage-2 part in GET /admin/status `features`. A newer
	 * cached page against an older server shows only what that server has.
	 */
	hasFeature(feature: AdminFeature): boolean {
		return this.isAuthorized && (this.status?.features ?? []).includes(feature);
	}

	/**
	 * This browser holds an admin key (in memory, sessionStorage or
	 * localStorage). A refused address sees the explanation of the 403 only
	 * then; anybody else is sent back to the chat.
	 */
	hasSavedKey(): boolean {
		return this.key !== null || this.readStoredKey() !== null;
	}

	/** True when a key is kept in localStorage ("Remember on this device"). */
	isKeyRemembered(): boolean {
		if (!browser) return false;

		try {
			return localStorage.getItem(ADMIN_KEY_STORAGE_KEY) !== null;
		} catch {
			return false;
		}
	}

	/**
	 * Called when the admin page closes. The feed stays followed (download
	 * results are still reported on other pages), but the per-model settings
	 * and GPU cards are no longer re-read on status changes; enter() loads
	 * them again.
	 */
	leave(): void {
		this.pageOpen = false;
		this.setModelsTabVisible(false);

		if (this.modelSettingsTimer) clearTimeout(this.modelSettingsTimer);

		this.modelSettingsTimer = null;

		if (this.gpuStatusTimer) clearTimeout(this.gpuStatusTimer);

		this.gpuStatusTimer = null;
	}

	async loadDiagnostics(): Promise<void> {
		if (!this.hasFeature(AdminFeature.DIAGNOSTICS) || this.diagnosticsLoading) return;

		this.diagnosticsLoading = true;

		try {
			this.diagnostics = await this.guarded((key) => AdminService.getDiagnostics(key));
			this.diagnosticsError = null;
		} catch (error) {
			this.diagnosticsError = errorMessage(error);
		} finally {
			this.diagnosticsLoading = false;
		}
	}

	/**
	 * Read the GPU cards. Calls made while one runs are folded into one more
	 * run after it; `refresh` asks the server for a new vendor-tool reading.
	 */
	async loadGpus(refresh = false): Promise<void> {
		if (!this.hasFeature(AdminFeature.GPUS)) return;

		if (this.gpuPromise) {
			if (refresh || this.gpuQueued === 'none') this.gpuQueued = refresh ? 'fresh' : 'cached';

			return this.gpuPromise;
		}

		this.gpuPromise = (async () => {
			let fresh = refresh;
			let again = true;

			while (again) {
				this.gpuQueued = 'none';
				this.gpusLoading = true;

				try {
					const wantFresh = fresh;

					this.gpus = await this.guarded((key) => AdminService.getGpus(key, wantFresh));
					this.gpusLoadedAt = Date.now();
					this.gpusError = null;
				} catch (error) {
					this.gpusError = errorMessage(error);
				} finally {
					this.gpusLoading = false;
				}

				// set again by a call that came in while this one ran
				const queued: string = this.gpuQueued;

				fresh = queued === 'fresh';
				again = queued !== 'none';
			}
		})().finally(() => {
			this.gpuPromise = null;
		});

		return this.gpuPromise;
	}

	/**
	 * Load a model through the status manager (same path as the chat
	 * selector), with the admin page's timeouts.
	 */
	async loadModel(id: string): Promise<void> {
		try {
			await modelsStore.status.load(id, {
				requestTimeoutMs: ADMIN_TIMEOUTS.LOAD_REQUEST_MS,
				statusTimeoutMs: ADMIN_TIMEOUTS.LOAD_STATUS_MS
			});
		} catch (error) {
			// the manager already raised a toast; a failed start is on the row itself
			// (Failed + exit code), but a refused request has a reason worth keeping
			if (error instanceof ApiError) {
				this.addNotice(AdminNoticeLevel.ERROR, `Could not load ${id}: ${error.message}`);
			}
		}
	}

	/** Output tail, failure reason and last failure of a model. Throws. */
	async loadModelLog(model: string): Promise<ApiAdminModelLog> {
		return this.guarded((key) => AdminService.getModelLog(key, model));
	}

	/**
	 * Read the per-model settings of every model; calls made while one runs
	 * are folded into one more run after it.
	 */
	async loadModelSettings(): Promise<void> {
		if (!this.hasFeature(AdminFeature.MODEL_SETTINGS)) return;

		if (this.modelSettingsPromise) {
			this.modelSettingsQueued = true;

			return this.modelSettingsPromise;
		}

		this.modelSettingsPromise = (async () => {
			do {
				this.modelSettingsQueued = false;

				try {
					this.modelSettings = await this.guarded((key) => AdminService.getModelSettings(key));
					this.modelSettingsError = null;
				} catch (error) {
					this.modelSettingsError = errorMessage(error);
				}
			} while (this.modelSettingsQueued);
		})().finally(() => {
			this.modelSettingsPromise = null;
		});

		return this.modelSettingsPromise;
	}

	/**
	 * One model with its facts read on demand (the list carries cached facts
	 * only). The row also replaces the one in the list. Throws.
	 */
	async loadModelSettingsRow(id: string): Promise<ApiAdminModelSettingsRow> {
		const row = await this.guarded((key) => AdminService.getModelSettingsRow(key, id));

		if (row?.exists) this.replaceModelSettingsRow(row);

		return row;
	}

	async loadSettings(): Promise<void> {
		if (!this.isAuthorized) return;

		this.settingsLoading = true;

		try {
			this.settings = await this.guarded((key) => AdminService.getSettings(key));
			this.settingsError = null;
		} catch (error) {
			this.settingsError = errorMessage(error);
		} finally {
			this.settingsLoading = false;
		}
	}

	async loadStorage(): Promise<void> {
		if (!this.isAuthorized) return;

		try {
			this.storage = await this.guarded((key) => AdminService.getStorage(key));
			this.storageError = null;
		} catch (error) {
			this.storageError = errorMessage(error);
		}
	}

	/** One row of the per-model settings list, null when not known. */
	modelSettingsRow(id: string): ApiAdminModelSettingsRow | null {
		return this.modelSettings?.models.find((row) => row.id === id) ?? null;
	}

	/** A result line above the model table (and a toast), e.g. from the settings dialog. */
	notice(level: AdminNoticeLevel, text: string): void {
		this.notify(level, text);
	}

	/**
	 * Run `listener` each time a key is accepted; returns the function that
	 * removes it. The upload queue continues a job that stopped at a 401.
	 */
	onKeyAccepted(listener: () => void): () => void {
		this.onUnlocked.add(listener);

		return () => this.onUnlocked.delete(listener);
	}

	/**
	 * One GET /admin/status. Runs once at app start in router mode and again
	 * only when the user presses Retry; there is no retry loop.
	 */
	async probe(): Promise<void> {
		this.probePromise ??= this.runProbe(false).finally(() => {
			this.probePromise = null;
		});

		return this.probePromise;
	}

	/** Start the one-shot startup probe unless it already ran. */
	probeOnce(): void {
		if (this.probeState === AdminProbeState.IDLE) void this.probe();
	}

	/**
	 * Re-read the model list. Uses modelsStore.fetch(true) so the chat model
	 * selector sees the result of an admin action as well. Calls made while a
	 * refresh is running are folded into one more run after it.
	 * @returns false when the list could not be fetched
	 */
	async refreshModels(): Promise<boolean> {
		if (this.refreshPromise) {
			this.refreshQueued = true;

			return this.refreshPromise;
		}

		this.refreshPromise = (async () => {
			let ok = true;

			do {
				this.refreshQueued = false;
				ok = await this.runModelsRefresh();
			} while (this.refreshQueued);

			return ok;
		})().finally(() => {
			this.refreshPromise = null;
		});

		return this.refreshPromise;
	}

	/**
	 * Remove every saved per-model value of a model (or of an id that is no
	 * longer in the list). A running model is not restarted.
	 * @returns true when the server removed them
	 */
	async removeModelSettings(id: string): Promise<boolean> {
		try {
			await this.guarded((key) => AdminService.saveModelSettings(key, { model: id, reset: true }));
			void this.loadModelSettings();

			return true;
		} catch (error) {
			this.notify(
				AdminNoticeLevel.ERROR,
				`Could not remove the saved settings of ${id}: ${errorMessage(error)}`
			);

			return false;
		}
	}

	/**
	 * A download the server refused after its dialog was closed: the dialog
	 * can no longer show the reason, so it becomes a result line and a toast.
	 */
	reportDownloadNotStarted(model: string, message: string): void {
		this.notify(
			AdminNoticeLevel.ERROR,
			`The server did not start the download of ${model}: ${message}`
		);
	}

	/**
	 * An admin request answered 403 (allow-list of the server, or a proxy that
	 * locks /admin): the admin API is not available from this address. Hides
	 * the sidebar item without a reload.
	 */
	reportForbidden(error: unknown): void {
		this.forbidden = true;
		this.status = null;
		this.probeError = describeProbeFailure(error);
		this.probeState = AdminProbeState.UNAVAILABLE;
		this.updateGpuTimer();
	}

	/** The server rejected the key of a request made outside AdminService (the upload). */
	reportKeyRejected(): void {
		this.requireKey('The server rejected the admin key. Enter it again.');
	}

	/** Re-scan the model sources on the server (GET /v1/models?reload=1). */
	async rescan(): Promise<void> {
		if (this.rescanning) return;

		this.rescanning = true;

		try {
			await ModelsService.reload({ timeoutMs: ADMIN_TIMEOUTS.RESCAN_MS });
			await this.refreshModels();
			toast.success('Model list rescanned');
		} catch (error) {
			this.notify(AdminNoticeLevel.ERROR, `Rescan failed: ${errorMessage(error)}`);
		} finally {
			this.rescanning = false;
			this.scheduleProtectedRefresh();
		}
	}

	/**
	 * "Re-detect GPUs": the server measures its devices again (it briefly
	 * needs a little memory on every GPU).
	 */
	async rescanGpus(): Promise<void> {
		if (this.gpusRescanning) return;

		this.gpusRescanning = true;

		try {
			this.gpus = await this.guarded((key) => AdminService.rescanGpus(key));
			this.gpusLoadedAt = Date.now();
			this.gpusError = null;
			toast.success('GPUs detected again');
			void this.loadModelSettings();
		} catch (error) {
			const busy = error instanceof AdminApiError && error.type === AdminErrorType.RESCAN_BUSY;

			this.notify(
				busy ? AdminNoticeLevel.INFO : AdminNoticeLevel.ERROR,
				busy
					? 'A GPU re-detection is already running; the cards update when it ends.'
					: `Re-detecting the GPUs failed: ${errorMessage(error)}`
			);
		} finally {
			this.gpusRescanning = false;
		}
	}

	/**
	 * The Retry button of the admin page: one more probe and, when the admin
	 * API answers now, everything enter() does after its probe.
	 */
	async retry(): Promise<void> {
		await this.probe();
		await this.openPage();
	}

	/**
	 * Run a call that needs the admin key, with the same handling of a
	 * rejected key (back to the key form) and of a 403 as every other admin
	 * call. Throws the AdminApiError.
	 */
	async runWithKey<T>(run: (key: string) => Promise<T>): Promise<T> {
		return this.guarded(run);
	}

	/**
	 * Save per-model settings (POST /admin/models/settings). Updates the row,
	 * the model list and the GPU cards afterwards. Throws the AdminApiError of
	 * a refusal (400 with `param`, 409 restart_required / field_locked /
	 * read-only, 504 stop_timeout) for the dialog to show.
	 */
	async saveModelSettings(
		update: ApiAdminModelSettingsUpdate
	): Promise<ApiAdminModelSettingsSaveResult> {
		try {
			const result = await this.guarded((key) => AdminService.saveModelSettings(key, update));

			if (result?.model?.exists) this.replaceModelSettingsRow(result.model);

			return result;
		} finally {
			void this.refreshModels();
			void this.loadModelSettings();
			this.scheduleGpuRefresh();
		}
	}

	/**
	 * Save a subset of the settings. The server validates, persists and
	 * applies; its message is kept in `saveError` for the form.
	 * @returns true when the settings were saved
	 */
	async saveSettings(update: ApiAdminSettingsUpdate): Promise<boolean> {
		if (this.savingSettings) return false;

		this.savingSettings = true;
		this.saveError = null;

		try {
			this.settings = await this.guarded((key) => AdminService.saveSettings(key, update));
			this.settingsError = null;

			// a changed device makes the server rebuild the presets and reload its list
			if (update.default_device !== undefined) void this.refreshModels();

			return true;
		} catch (error) {
			this.saveError = errorMessage(error);

			// refused values: the settings in effect may differ from the ones the form
			// was filled from (changed from another browser), so read them again
			if (error instanceof AdminApiError && (error.status === 400 || error.status === 409)) {
				void this.loadSettings();
			}

			return false;
		} finally {
			this.savingSettings = false;
		}
	}

	/**
	 * The Models tab is shown or hidden. While it is shown and the browser
	 * page is visible, the GPU cards refresh on a timer.
	 */
	setModelsTabVisible(visible: boolean): void {
		this.modelsTabVisible = visible;
		this.listenToVisibility();
		this.updateGpuTimer();

		if (visible) void this.loadGpus();
	}

	/**
	 * Start a Hugging Face download through the public POST /models route.
	 * Throws when the server refused it (already exists, repository not
	 * found, ...); once started, progress and the result come from the feed.
	 */
	async startDownload(model: string): Promise<void> {
		modelsStore.status.subscribe();
		this.attachFeed();

		// a list that could not be fetched reads as empty, and every cached model of
		// the repository would then count as downloaded by this download
		if (modelsStore.routerModels.length === 0) await this.refreshModels();

		// the result is judged by what appears in the list after this point
		this.downloads.set(model, {
			cacheIdsBefore: cacheModelIds(modelsStore.routerModels),
			cancelRequested: false,
			confirmed: false,
			files: {},
			name: model
		});

		try {
			await ModelsService.download(model, { timeoutMs: ADMIN_TIMEOUTS.DOWNLOAD_START_MS });
		} catch (error) {
			if (!this.downloads.get(model)?.confirmed) this.downloads.delete(model);

			throw error;
		}

		const download = this.downloads.get(model);

		if (download) this.downloads.set(model, { ...download, confirmed: true });

		await this.refreshModels();
	}

	/**
	 * Stop a model that is still loading: POST /models/unload straight through
	 * the service, for the same reason as cancelDownload.
	 */
	async stopLoading(id: string): Promise<void> {
		if (this.operations.has(id)) return;

		this.operations.set(id, AdminModelOperation.STOPPING);
		// before the request: the server kills the child at once, and the feed may
		// report the unload before the request returns
		this.stoppedByUser.add(id);

		try {
			await ModelsService.unload(id, { timeoutMs: ADMIN_TIMEOUTS.CANCEL_MS });
		} catch (error) {
			this.stoppedByUser.delete(id);
			this.operations.delete(id);
			this.notify(AdminNoticeLevel.ERROR, `Could not stop ${id}: ${errorMessage(error)}`);

			return;
		}

		await this.refreshModels();

		// the feed normally clears this when the model leaves the loading state
		if (modelsStore.getModelStatus(id) !== ServerModelStatus.LOADING) this.operations.delete(id);
	}

	/**
	 * Verify a key typed into the key form and keep it when the server
	 * accepts it.
	 * @param remember - also keep the key in localStorage
	 */
	async submitKey(input: string, remember: boolean): Promise<boolean> {
		const key = input.trim();

		if (!key || this.checkingKey) return false;

		this.checkingKey = true;
		this.keyError = null;

		try {
			const status = await AdminService.getStatus(key);

			this.probeState = AdminProbeState.AVAILABLE;
			this.probeError = null;

			if (!status.authorized) {
				this.status = status;
				this.keyError = status.key_configured
					? 'The admin key is not correct.'
					: 'The server has no admin key configured.';

				return false;
			}

			// the key first: an authorized status must never be visible without it
			this.storeKey(key, remember);
			this.status = status;
			this.forbidden = false;
			void this.loadProtected();
			this.notifyUnlocked();

			return true;
		} catch (error) {
			this.keyError = describeProbeFailure(error);

			if (error instanceof AdminApiError && error.status === 403) this.reportForbidden(error);

			return false;
		} finally {
			this.checkingKey = false;
		}
	}

	/** The model whose settings dialog should open now (taken once), null when none. */
	takeSettingsRequest(): string | null {
		const id = this.settingsRequest;

		this.settingsRequest = null;

		return id;
	}

	async unloadModel(id: string): Promise<void> {
		try {
			await modelsStore.status.unload(id, {
				requestTimeoutMs: ADMIN_TIMEOUTS.UNLOAD_REQUEST_MS,
				statusTimeoutMs: ADMIN_TIMEOUTS.UNLOAD_STATUS_MS
			});
		} catch (error) {
			if (error instanceof ApiError) {
				this.addNotice(AdminNoticeLevel.ERROR, `Could not unload ${id}: ${error.message}`);
			}
		}
	}

	private addNotice(level: AdminNoticeLevel, text: string): void {
		this.notices = [{ id: ++this.noticeSeq, level, text }, ...this.notices].slice(
			0,
			ADMIN_NOTICES_MAX
		);
	}

	/** Follow the /models/sse feed through the status manager; idempotent. */
	private attachFeed(): void {
		this.feedStop ??= modelsStore.status.onEvent((event) => this.handleFeedEvent(event));
		this.feedOpenStop ??= modelsStore.status.onFeedOpen(() => this.handleFeedOpen());
	}

	/**
	 * Models a finished download reported as its result are not new for the
	 * downloads still followed: a later Cancel or failure of another download
	 * of the same repository must not be reported as a success with them.
	 */
	private claimNewModels(ids: string[]): void {
		if (ids.length === 0) return;

		for (const [name, download] of [...this.downloads]) {
			this.downloads.set(name, {
				...download,
				cacheIdsBefore: [...download.cacheIdsBefore, ...ids]
			});
		}
	}

	private clearKey(): void {
		this.key = null;

		if (!browser) return;

		try {
			sessionStorage.removeItem(ADMIN_KEY_STORAGE_KEY);
			localStorage.removeItem(ADMIN_KEY_STORAGE_KEY);
		} catch {
			// storage unavailable: nothing was kept there either
		}
	}

	/**
	 * Close the books on a download after its final feed event: re-read the
	 * list, then report what really happened (see resolveDownloadResult).
	 */
	private async finishDownload(name: string, failed: boolean): Promise<void> {
		const download = this.downloads.get(name);

		// taken out first, so the list reconciliation cannot report it a second time
		this.downloads.delete(name);
		this.operations.delete(name);

		const refreshed = await this.refreshModels();

		this.scheduleProtectedRefresh();

		// not followed by this page (it ended before the page saw it start)
		if (!download) return;

		if (!refreshed) {
			this.notify(
				AdminNoticeLevel.WARNING,
				`The download of ${name} ended, but the model list could not be refreshed. Press Rescan to see the result.`
			);

			return;
		}

		this.reportDownload(name, resolveDownloadResult(download, modelsStore.routerModels, failed));
	}

	/**
	 * Run a call that needs the admin key. A 401 `admin_key_required` means
	 * the key is wrong or was changed: forget it and return to the key form.
	 */
	private async guarded<T>(run: (key: string) => Promise<T>): Promise<T> {
		const key = this.key;

		if (!key) {
			this.requireKey(null);

			throw new AdminApiError('The admin key is required', 401, AdminErrorType.KEY_REQUIRED);
		}

		try {
			return await run(key);
		} catch (error) {
			if (error instanceof AdminApiError && error.isKeyRejected) {
				this.requireKey('The server rejected the admin key. Enter it again.');
			}

			// mid-session refusal of this address: the item goes away without a reload
			if (error instanceof AdminApiError && error.status === 403) this.reportForbidden(error);

			throw error;
		}
	}

	private handleDownloadProgress(event: ApiModelsSseEvent): void {
		if (!event.model) return;

		const data = event.data as unknown as ApiModelsSseDownloadData | undefined;
		const download = this.trackDownload(event.model, true);

		if (data?.progress) {
			this.downloads.set(event.model, { ...download, files: data.progress });
		}

		this.refreshIfUnknown(event.model);
	}

	/** One record of the /models/sse feed, after the status manager applied it. */
	private handleFeedEvent(event: ApiModelsSseEvent): void {
		switch (event.event) {
			case ServerModelsSseEventType.STATUS_CHANGE:
			case ServerModelsSseEventType.MODEL_STATUS:
			case ServerModelsSseEventType.STATUS_UPDATE:
				this.handleStatusEvent(event);

				break;
			case ServerModelsSseEventType.MODELS_RELOAD:
				// the rows are re-read and carry their own failed flag again
				this.exitCodes.clear();
				this.seenStatus.clear();
				void this.refreshModels();
				this.scheduleProtectedRefresh();

				break;
			case ServerModelsSseEventType.MODEL_REMOVE:
				this.exitCodes.delete(event.model);
				this.seenStatus.delete(event.model);
				this.scheduleProtectedRefresh();

				break;
			case ServerModelsSseEventType.DOWNLOAD_PROGRESS:
				this.handleDownloadProgress(event);

				break;
			case ServerModelsSseEventType.DOWNLOAD_FAILED:
				void this.finishDownload(event.model, true);

				break;
			case ServerModelsSseEventType.DOWNLOAD_FINISHED:
				void this.finishDownload(event.model, false);

				break;
		}
	}

	/**
	 * The feed (re)connected, e.g. after a server restart. Whatever happened
	 * while it was down was not reported, so the list is the truth again.
	 */
	private handleFeedOpen(): void {
		// one refresh is already waiting for its turn
		if (this.feedRefreshTimer) return;

		const wait = this.feedRefreshedAt + ADMIN_FEED_REOPEN_REFRESH_MIN_MS - Date.now();

		this.feedRefreshTimer = setTimeout(
			() => {
				this.feedRefreshTimer = null;
				this.feedRefreshedAt = Date.now();
				this.exitCodes.clear();
				this.seenStatus.clear(); // statuses were missed while the feed was down
				void this.refreshModels();
				this.scheduleProtectedRefresh();
			},
			Math.max(0, wait)
		);
	}

	private handleStatusEvent(event: ApiModelsSseEvent): void {
		const status = event.data?.status;

		if (!event.model || !status) return;

		if (status === ServerModelStatus.UNLOADED) {
			// a load the user stopped is not a failure, whatever the killed child returned
			const stopped = this.stoppedByUser.delete(event.model);

			this.exitCodes.set(event.model, stopped ? 0 : (event.data.exit_code ?? 0));
		} else {
			this.exitCodes.delete(event.model);
		}

		if (
			status !== ServerModelStatus.LOADING &&
			this.operations.get(event.model) === AdminModelOperation.STOPPING
		) {
			this.operations.delete(event.model);
		}

		if (status === ServerModelStatus.DOWNLOADING) this.trackDownload(event.model, true);

		this.refreshIfUnknown(event.model);

		// stage 2: GPU memory, phase / last failure / revert of the row follow the status; a load
		// sends its progress as status_change events with the same status (every ~200 ms): those
		// change neither, and nothing is re-read while the admin page is closed
		const changed = this.seenStatus.get(event.model) !== status;

		this.seenStatus.set(event.model, status);

		if (event.event === ServerModelsSseEventType.STATUS_CHANGE && changed && this.pageOpen) {
			this.scheduleGpuRefresh();
			this.scheduleModelSettingsRefresh();
		}
	}

	/**
	 * Start watching the visibility of the browser page (once): the GPU timer
	 * runs only while the page is visible.
	 */
	private listenToVisibility(): void {
		if (!browser || this.visibilityListening) return;

		this.visibilityListening = true;
		document.addEventListener('visibilitychange', () => {
			this.updateGpuTimer();

			if (document.visibilityState === 'visible' && this.modelsTabVisible) void this.loadGpus();
		});
	}

	private async loadProtected(): Promise<void> {
		await Promise.all([
			this.loadSettings(),
			this.loadStorage(),
			this.loadModelSettings(),
			this.loadGpus()
		]);
	}

	/** A result line on the page plus a toast that is also seen on other pages. */
	private notify(level: AdminNoticeLevel, text: string): void {
		this.addNotice(level, text);
		toast[level](text);
	}

	private notifyUnlocked(): void {
		for (const listener of [...this.onUnlocked]) {
			try {
				listener();
			} catch {
				// a listener's failure is its own business
			}
		}
	}

	/** After a successful probe: follow the model feed and load what the page shows. */
	private async openPage(): Promise<void> {
		if (!this.isAvailable) return;

		modelsStore.status.subscribe();
		this.attachFeed();
		void this.refreshModels();

		if (this.isAuthorized) await this.loadProtected();
	}

	private readStoredKey(): string | null {
		if (!browser) return null;

		try {
			return (
				sessionStorage.getItem(ADMIN_KEY_STORAGE_KEY) ?? localStorage.getItem(ADMIN_KEY_STORAGE_KEY)
			);
		} catch {
			return null;
		}
	}

	/**
	 * After a list refresh: a followed download whose row is no longer
	 * `downloading` has ended. Its final event normally arrives first; give it
	 * a moment, then settle from the list so a missed event leaves nothing
	 * hanging.
	 */
	private reconcileDownloads(): void {
		for (const [name, download] of this.downloads) {
			if (!download.confirmed) continue;

			if (modelsStore.getModelStatus(name) === ServerModelStatus.DOWNLOADING) continue;

			setTimeout(() => {
				const current = this.downloads.get(name);

				if (!current || modelsStore.getModelStatus(name) === ServerModelStatus.DOWNLOADING) return;

				this.downloads.delete(name);
				this.operations.delete(name);
				this.reportDownload(name, resolveDownloadResult(current, modelsStore.routerModels, false));
				this.scheduleProtectedRefresh();
			}, DOWNLOAD_RECONCILE_DELAY_MS);
		}
	}

	/** The feed named a model the list does not have yet (e.g. a download started elsewhere). */
	private refreshIfUnknown(modelId: string): void {
		if (!modelsStore.routerModels.some((model) => model.id === modelId)) {
			void this.refreshModels();
		}
	}

	/** Put a fresh row into the list (the list may not be loaded yet). */
	private replaceModelSettingsRow(row: ApiAdminModelSettingsRow): void {
		const list = this.modelSettings;

		if (!list) return;

		const index = list.models.findIndex((current) => current.id === row.id);
		const models = [...list.models];

		if (index === -1) models.push(row);
		else models[index] = row;

		this.modelSettings = { ...list, models };
	}

	private reportDownload(name: string, result: AdminDownloadResult): void {
		switch (result.outcome) {
			case AdminDownloadOutcome.SUCCESS:
				this.claimNewModels(result.newModelIds);
				this.notify(AdminNoticeLevel.SUCCESS, `Downloaded ${result.newModelIds.join(', ')}`);

				// placement right after the download: the Models tab opens the settings dialog
				if (this.pageOpen && this.hasFeature(AdminFeature.MODEL_SETTINGS)) {
					this.settingsRequest = result.newModelIds[0] ?? null;
				}

				break;
			case AdminDownloadOutcome.CANCELLED:
				this.notify(AdminNoticeLevel.INFO, `Cancelled: download of ${name}`);

				break;
			case AdminDownloadOutcome.FAILED:
				this.notify(
					AdminNoticeLevel.ERROR,
					`Download of ${name} failed. The server log has the reason.`
				);

				break;
			case AdminDownloadOutcome.NOTHING:
				this.notify(
					AdminNoticeLevel.WARNING,
					`Nothing was downloaded for ${name}: no new model appeared in the cache.`
				);

				break;
		}
	}

	/** Forget the key and show the key form, optionally with a reason. */
	private requireKey(message: string | null): void {
		this.clearKey();
		this.settings = null;
		this.storage = null;
		this.saveError = null;
		this.settingsError = null;
		this.storageError = null;
		this.modelSettings = null;
		this.modelSettingsError = null;
		this.gpus = null;
		this.gpusError = null;
		this.gpusLoadedAt = null;
		this.diagnostics = null;
		this.diagnosticsError = null;
		this.updateGpuTimer();
		this.keyError = message;

		if (this.status) {
			this.status = {
				admin: true,
				authorized: false,
				key_configured: this.status.key_configured
			};
		}
	}

	private async runModelsRefresh(): Promise<boolean> {
		try {
			// fetch() hands out a request that is already running; it may predate
			// the change this refresh is for, so let it finish and ask again
			if (modelsStore.loading) await modelsStore.fetch(true).catch(() => {});

			await modelsStore.fetch(true);
		} catch {
			return false;
		}

		// a deleted model must not stay selected for the chat
		if (modelsStore.selectedModelId && !modelsStore.findModelById(modelsStore.selectedModelId)) {
			modelsStore.clearSelection();
		}

		// a download that was already running when the page opened is followed too
		for (const model of modelsStore.routerModels) {
			if (model.status?.value === ServerModelStatus.DOWNLOADING) {
				this.trackDownload(model.id, true);
			}
		}

		this.reconcileDownloads();

		return true;
	}

	/**
	 * @param silent - keep the current state while the request runs (a re-check
	 * of a panel that is already shown), instead of flipping to "checking"
	 */
	private async runProbe(silent: boolean): Promise<void> {
		if (!silent) this.probeState = AdminProbeState.CHECKING;

		const key = this.key ?? this.readStoredKey();

		try {
			const status = await AdminService.getStatus(key);

			// the key first: an authorized status must never be visible without it
			if (key && status.authorized) this.key = key;

			this.status = status;
			this.probeError = null;
			this.probeState = AdminProbeState.AVAILABLE;
			this.forbidden = false;

			if (key && !status.authorized) {
				this.requireKey('The saved admin key is no longer accepted. Enter the current key.');
			} else if (key) {
				this.notifyUnlocked();
			}
		} catch (error) {
			this.status = null;
			this.probeError = describeProbeFailure(error);
			this.probeState = AdminProbeState.UNAVAILABLE;
			this.forbidden = error instanceof AdminApiError && error.status === 403;
		}
	}

	/** Memory settles after a load or unload: read the GPU cards a moment later. */
	private scheduleGpuRefresh(): void {
		if (!this.hasFeature(AdminFeature.GPUS)) return;

		if (this.gpuStatusTimer) clearTimeout(this.gpuStatusTimer);

		this.gpuStatusTimer = setTimeout(() => {
			this.gpuStatusTimer = null;
			void this.loadGpus(true);
		}, ADMIN_GPU_REFRESH.AFTER_STATUS_MS);
	}

	/** Phase, last failure and revert of a model change with its status: read them again, folded. */
	private scheduleModelSettingsRefresh(): void {
		if (!this.hasFeature(AdminFeature.MODEL_SETTINGS) || this.modelSettingsTimer) return;

		this.modelSettingsTimer = setTimeout(() => {
			this.modelSettingsTimer = null;
			void this.loadModelSettings();
		}, ADMIN_GPU_REFRESH.AFTER_STATUS_MS);
	}

	/** Settings (model list, pins) and storage figures follow the model list; fold bursts. */
	private scheduleProtectedRefresh(): void {
		if (!this.isAuthorized || this.refreshTimer) return;

		this.refreshTimer = setTimeout(() => {
			this.refreshTimer = null;
			void this.loadProtected();
		}, ADMIN_REFRESH_DEBOUNCE_MS);
	}

	private storeKey(key: string, remember: boolean): void {
		this.key = key;

		if (!browser) return;

		try {
			sessionStorage.setItem(ADMIN_KEY_STORAGE_KEY, key);

			if (remember) {
				localStorage.setItem(ADMIN_KEY_STORAGE_KEY, key);
			} else {
				localStorage.removeItem(ADMIN_KEY_STORAGE_KEY);
			}
		} catch {
			// storage unavailable (private mode): the key stays in memory only
		}
	}

	/** Start following a download, remembering which cache models existed before it. */
	private trackDownload(name: string, confirmed: boolean): AdminDownload {
		const existing = this.downloads.get(name);

		if (existing && (existing.confirmed || !confirmed)) return existing;

		const download: AdminDownload = existing
			? { ...existing, confirmed: true }
			: {
					cacheIdsBefore: cacheModelIds(modelsStore.routerModels),
					cancelRequested: false,
					confirmed,
					files: {},
					name
				};

		this.downloads.set(name, download);

		return download;
	}

	/** Run the GPU timer while the Models tab and the page are visible, stop it otherwise. */
	private updateGpuTimer(): void {
		const active =
			browser &&
			this.modelsTabVisible &&
			document.visibilityState === 'visible' &&
			this.hasFeature(AdminFeature.GPUS);

		if (active && !this.gpuInterval) {
			this.gpuInterval = setInterval(() => {
				if (this.modelsTabVisible && document.visibilityState === 'visible') void this.loadGpus();
			}, ADMIN_GPU_REFRESH.INTERVAL_MS);
		} else if (!active && this.gpuInterval) {
			clearInterval(this.gpuInterval);
			this.gpuInterval = null;
		}
	}
}

export const adminStore = new AdminStore();
