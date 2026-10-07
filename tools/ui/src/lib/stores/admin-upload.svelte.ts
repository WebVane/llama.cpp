/**
 * adminUploadStore - the GGUF upload queue of the admin panel (stage 3).
 *
 * One job per model (a file, a split set, plus at most one projector), one
 * upload session per job on the server. Jobs run one after another: the
 * next one is created when the previous one has sent its last byte (the
 * server's checks run meanwhile). The queue lives in this store, so closing
 * the dialog does not stop an upload; a reload loses the picked files, and
 * the unfinished sessions of the server are then listed for a resume (pick
 * the same files again).
 *
 * The chunks go through AdminUploadScheduler with the XMLHttpRequest
 * transport; every other call goes through AdminService with the admin
 * key of adminStore (a rejected key goes back to the key form, a 403 hides
 * the admin panel).
 */

import { browser } from '$app/environment';
import { ADMIN_UPLOAD, ADMIN_UPLOAD_STORAGE } from '$lib/constants';
import { AdminErrorType, AdminFeature, AdminNoticeLevel } from '$lib/enums';
import {
	AdminAddModelTab,
	AdminUploadFileRole,
	AdminUploadJobPhase,
	AdminUploadSendState,
	AdminUploadState
} from '$lib/enums/admin.enums';
import { AdminApiError, AdminService } from '$lib/services/admin.service';
import { AdminUploadService } from '$lib/services/admin-upload.service';
// direct imports between stores, not via the barrel, to avoid circular deps
import { adminStore } from '$lib/stores/admin.svelte';
import type {
	AdminUploadJob,
	AdminUploadJobFile,
	AdminUploadRequest,
	AdminUploadResumeReport,
	AdminUploadSendSnapshot,
	ApiAdminUploadCreate,
	ApiAdminUploadCreateResult,
	ApiAdminUploadInfo,
	ApiAdminUploadSession,
	ApiAdminUploadSessionResult,
	ApiAdminUploadSummary
} from '$lib/types/admin-upload';
import { crc32, crc32Hex } from '$lib/utils/admin-crc32';
import {
	describeUploadBlock,
	describeUploadRejection,
	describeUploadSlotWait,
	isJobFinished,
	jobPhaseOf,
	openUploadSessions
} from '$lib/utils/admin-upload';
import {
	AdminUploadScheduler,
	type AdminUploadTransport,
	verifyUploadedStart
} from '$lib/utils/admin-upload-scheduler';
import { toast } from 'svelte-sonner';

function errorText(error: unknown): string {
	return error instanceof Error ? error.message : String(error);
}

function randomId(): string {
	const bytes = new Uint8Array(16);

	crypto.getRandomValues(bytes);

	return Array.from(bytes, (b) => b.toString(16).padStart(2, '0')).join('');
}

/** The X-Upload-Client of this tab: sessionStorage survives a reload and differs between tabs. */
function loadClientId(): string {
	if (!browser) return randomId();

	try {
		const saved = sessionStorage.getItem(ADMIN_UPLOAD_STORAGE.CLIENT_ID);

		if (saved) return saved;

		const id = randomId();

		sessionStorage.setItem(ADMIN_UPLOAD_STORAGE.CLIENT_ID, id);

		return id;
	} catch {
		return randomId();
	}
}

function loadCeiling(): number | null {
	if (!browser) return null;

	try {
		const value = Number(sessionStorage.getItem(ADMIN_UPLOAD_STORAGE.CHUNK_CEILING));

		return Number.isFinite(value) && value > 0 ? value : null;
	} catch {
		return null;
	}
}

/** The local CRC32 of the first MiB, as the server's head_crc32. */
async function headCrc(file: Blob): Promise<string> {
	const length = Math.min(file.size, ADMIN_UPLOAD.HEAD_CRC_BYTES);
	const bytes = new Uint8Array(await file.slice(0, length).arrayBuffer());

	return crc32Hex(crc32(bytes));
}

class AdminUploadStore {
	/** The add-model dialog should open (header indicator, banner, files dropped on the page). */
	dialogRequest = $state<{ tab: AdminAddModelTab; files: File[] | null } | null>(null);
	/** GET /admin/models/upload: availability, limits, the server's sessions. */
	info = $state<ApiAdminUploadInfo | null>(null);
	infoError = $state<string | null>(null);
	jobs = $state<AdminUploadJob[]>([]);

	private ceiling: number | null = null;
	private clientId: string | null = null;
	private infoPromise: Promise<void> | null = null;
	private jobSeq = 0;
	private keyListener: (() => void) | null = null;
	private lastPoll = new Map<string, number>();
	private lastSlotPoll = 0;
	private pollTimer: ReturnType<typeof setTimeout> | null = null;
	private pumping = false;
	private schedulers = new Map<string, AdminUploadScheduler>();
	/**
	 * A create was refused with too_many_uploads: the queue waits until one of
	 * these open sessions of the server ends (their ids from the refusal; an
	 * empty set: until fewer than max_sessions are open). null: not waiting.
	 */
	private slotWait: Set<string> | null = null;
	private transport: AdminUploadTransport | null = null;
	private unloadListening = false;

	/** Jobs that still do something (shown in the header indicator). */
	get activeJobs(): AdminUploadJob[] {
		return this.jobs.filter((job) => !isJobFinished(job.phase));
	}

	/** Uploads are possible on this server (null until known). */
	get available(): boolean | null {
		if (this.info) return this.info.available;

		return adminStore.status?.upload?.available ?? null;
	}

	/** The server has the upload routes. */
	get enabled(): boolean {
		return adminStore.hasFeature(AdminFeature.UPLOAD);
	}

	/**
	 * Unfinished sessions of the server that no job of this page follows:
	 * started before a reload, in another tab or by another admin.
	 */
	get orphanSessions(): ApiAdminUploadSummary[] {
		const followed = new Set(this.jobs.map((job) => job.sessionId).filter((id) => !!id));

		return openUploadSessions(this.info?.sessions ?? []).filter(
			(session) => !followed.has(session.id)
		);
	}

	/** Bytes confirmed / total over the jobs that are sending or waiting to send. */
	get totals(): { confirmed: number; total: number; sending: number } {
		let confirmed = 0;
		let total = 0;
		let sending = 0;

		for (const job of this.jobs) {
			if (job.phase !== AdminUploadJobPhase.UPLOADING && job.phase !== AdminUploadJobPhase.QUEUED) {
				continue;
			}

			const size = job.files.reduce((sum, file) => sum + file.size, 0);

			total += size;
			confirmed += job.send?.confirmed ?? 0;

			if (job.phase === AdminUploadJobPhase.UPLOADING) sending++;
		}

		return { confirmed, sending, total };
	}

	/** Leave a job alone: its scheduler stops and it is removed from the list (the session stays). */
	abandon(key: string): void {
		this.schedulers.get(key)?.stop();
		this.schedulers.delete(key);
		this.jobs = this.jobs.filter((job) => job.key !== key);
		this.updateUnloadGuard();
	}

	/**
	 * Files picked again for a job that needs them (after "not the same file"
	 * or a reload). Matched by name and size; the first MiB is compared with
	 * the server's copy before anything is sent.
	 */
	async attachFiles(key: string, picked: File[]): Promise<AdminUploadResumeReport> {
		const job = this.job(key);

		if (!job || !job.sessionId) {
			return { matched: [], mismatched: [], missing: [], unknown: picked.map((f) => f.name) };
		}

		const report = await this.matchFiles(job, picked);
		const scheduler = this.schedulers.get(key);

		if (scheduler) {
			for (const file of job.files) {
				if (file.file) scheduler.setFileBlob(file.index, file.file);
			}

			if (report.mismatched.length === 0) scheduler.resume();
		} else if (job.phase === AdminUploadJobPhase.UPLOADING) {
			this.startScheduler(job, true);
		}

		return report;
	}

	/** Cancel a job: the server deletes its data. A job without a session is just removed. */
	async cancel(key: string): Promise<void> {
		const job = this.job(key);

		if (!job) return;

		this.schedulers.get(key)?.stop();
		this.schedulers.delete(key);

		if (!job.sessionId) {
			this.jobs = this.jobs.filter((item) => item.key !== key);
			this.updateUnloadGuard();
			void this.pump();

			return;
		}

		try {
			const result = await adminStore.runWithKey((k) =>
				AdminService.cancelUpload(k, job.sessionId as string)
			);

			this.applySession(key, result.upload);
			toast.info(`Upload of ${job.name} cancelled`);
		} catch (error) {
			if (error instanceof AdminApiError && error.status === 410) {
				await this.refreshJob(key);
			} else {
				toast.error(`Could not cancel the upload of ${job.name}: ${errorText(error)}`);

				// it is still on the server: keep following it
				if (job.phase === AdminUploadJobPhase.UPLOADING) this.startScheduler(job, true);
			}
		} finally {
			this.updateUnloadGuard();
			void this.loadInfo();
			void this.pump();
		}
	}

	/** Cancel a session of the server that no job follows (unfinished uploads banner). */
	async cancelSession(id: string): Promise<boolean> {
		try {
			const result = await adminStore.runWithKey((k) => AdminService.cancelUpload(k, id));
			const freed = result.freed_bytes ?? 0;

			toast.info(
				`Upload of ${result.upload?.name ?? id} cancelled${freed > 0 ? `, ${Math.round(freed / 1048576)} MiB freed` : ''}`
			);

			return true;
		} catch (error) {
			toast.error(`Could not cancel the upload: ${errorText(error)}`);

			return false;
		} finally {
			void this.loadInfo();
		}
	}

	/** Remove a finished job from the list. */
	dismiss(key: string): void {
		const job = this.job(key);

		if (job && !isJobFinished(job.phase)) return;

		this.jobs = this.jobs.filter((item) => item.key !== key);
		void this.pump();
	}

	/** Drop one file from a job (a wrong pick the server refused); the rest continues. Throws. */
	async dropFile(key: string, index: number): Promise<void> {
		const job = this.job(key);

		if (!job?.sessionId) return;

		let result: ApiAdminUploadSessionResult;

		try {
			result = await adminStore.runWithKey((k) =>
				AdminService.updateUpload(k, { drop_files: [index], id: job.sessionId as string })
			);
		} catch (error) {
			if (error instanceof AdminApiError && error.status === 409 && error.type === 'upload_busy') {
				// the session moved on meanwhile: show where it is now
				await this.refreshJob(key);

				if (error.details?.staged === true) {
					throw new Error(
						'The files of this upload were already checked and arranged for publishing together, so a file cannot be dropped any more: cancel the upload and upload the model again without that file.'
					);
				}
			}

			throw error;
		}

		this.applySession(key, result.upload);

		const scheduler = this.schedulers.get(key);

		if (scheduler) {
			scheduler.dropFile(index);
			scheduler.resume();
		}
	}

	/**
	 * Check a model before it is queued: name, collisions, locks, disk space
	 * and saved settings of an earlier model with the same id. Throws the
	 * AdminApiError of a refusal (invalid_name, name_taken, field_locked ...).
	 */
	async dryRun(
		request: Omit<ApiAdminUploadCreate, 'dry_run'>
	): Promise<ApiAdminUploadCreateResult> {
		return adminStore.runWithKey((k) =>
			AdminService.createUpload(k, { ...request, dry_run: true })
		);
	}

	/** Queue models for upload; the first one starts at once. */
	enqueue(requests: AdminUploadRequest[]): string[] {
		const keys: string[] = [];

		for (const request of requests) {
			const key = `job-${++this.jobSeq}`;

			keys.push(key);
			this.jobs.push({
				autoPublish: request.autoPublish,
				createdAt: Date.now(),
				error: null,
				errorType: null,
				files: request.files.map((picked, index) => ({
					complete: false,
					expectedSha256: request.expectedSha256[picked.key] ?? '',
					file: picked.file,
					headerEnd: picked.header?.headerEnd ?? null,
					index,
					lastModified: picked.file.lastModified,
					name: picked.name,
					offset: 0,
					role: picked.role,
					size: picked.size
				})),
				fit: null,
				fitError: null,
				fitRunning: false,
				key,
				name: request.name,
				phase: AdminUploadJobPhase.QUEUED,
				placement: Object.keys(request.settings).length > 0,
				send: null,
				session: null,
				sessionId: null,
				settings: request.settings,
				settingsMode: request.settingsMode
			});
		}

		this.updateUnloadGuard();
		void this.pump();

		return keys;
	}

	/** Called when the admin page opens: follow the key and read the server's sessions. */
	enter(): void {
		this.keyListener ??= adminStore.onKeyAccepted(() => {
			for (const job of this.jobs) {
				if (job.send?.state === AdminUploadSendState.KEY_REQUIRED) this.resume(job.key);
			}

			void this.loadInfo();
		});
		void this.loadInfo();
	}

	/**
	 * Follow a session of the server (unfinished uploads banner): checks,
	 * the ready state and its actions. A receiving session needs its files
	 * picked again before it continues (see resumeSession).
	 */
	async follow(id: string): Promise<string | null> {
		const existing = this.jobs.find((job) => job.sessionId === id);

		if (existing) return existing.key;

		let session: ApiAdminUploadSession;

		try {
			session = await adminStore.runWithKey((k) => AdminService.getUpload(k, id));
		} catch (error) {
			toast.error(`The upload could not be read: ${errorText(error)}`);
			void this.loadInfo();

			return null;
		}

		const key = `job-${++this.jobSeq}`;

		this.jobs.push(this.jobFromSession(key, session));
		this.schedulePoll();

		return key;
	}

	/** The scheduler of a job is running (or waiting for a resume). */
	isSending(key: string): boolean {
		return this.schedulers.has(key);
	}

	/** The key of the job that follows a session, null when none. */
	jobOfSession(id: string): AdminUploadJob | null {
		return this.jobs.find((job) => job.sessionId === id) ?? null;
	}

	/** Read availability, limits and the sessions of the server. */
	async loadInfo(): Promise<void> {
		if (!this.enabled) return;

		this.infoPromise ??= (async () => {
			try {
				this.info = await adminStore.runWithKey((k) => AdminService.getUploads(k));
				this.infoError = null;
				this.checkSlotWait();
			} catch (error) {
				this.infoError = errorText(error);
			}
		})().finally(() => {
			this.infoPromise = null;
		});

		return this.infoPromise;
	}

	/**
	 * A receiving job that cannot send: files of the session are not picked
	 * (after a reload, or "not the same file").
	 */
	needsFiles(job: AdminUploadJob): boolean {
		if (job.phase !== AdminUploadJobPhase.UPLOADING) return false;

		const dropped = new Set(
			(job.session?.files ?? []).filter((file) => file.dropped).map((file) => file.index)
		);

		return job.files.some((file) => !file.complete && !file.file && !dropped.has(file.index));
	}

	/** Open the add-model dialog on a tab, optionally with files dropped on the page. */
	openDialog(tab: AdminAddModelTab, files: File[] | null = null): void {
		this.dialogRequest = { files, tab };
	}

	pause(key: string): void {
		this.schedulers.get(key)?.pause();
		this.updateUnloadGuard();
	}

	/**
	 * Publish a ready session, or run its checks again (Retry after a check
	 * that could not run or a SHA-256 mismatch). Throws for the job card.
	 */
	async publish(
		key: string,
		options: { ignoreSha256Mismatch?: boolean; withoutSettings?: boolean } = {}
	): Promise<void> {
		const job = this.job(key);

		if (!job?.sessionId) return;

		const result = await adminStore.runWithKey((k) =>
			AdminService.publishUpload(k, {
				id: job.sessionId as string,
				ignore_sha256_mismatch: options.ignoreSha256Mismatch || undefined,
				without_settings: options.withoutSettings || undefined
			})
		);

		if (options.withoutSettings) job.placement = false;

		this.applySession(key, result.upload);
	}

	/** Rename a ready (or still receiving) session; nothing is uploaded again. Throws. */
	async rename(key: string, name: string): Promise<void> {
		const job = this.job(key);

		if (!job?.sessionId) return;

		const result = await adminStore.runWithKey((k) =>
			AdminService.updateUpload(k, { id: job.sessionId as string, name })
		);

		job.name = name;
		this.applySession(key, result.upload);
	}

	resume(key: string): void {
		const job = this.job(key);
		const scheduler = this.schedulers.get(key);

		if (scheduler) {
			scheduler.resume();
		} else if (job?.phase === AdminUploadJobPhase.UPLOADING) {
			this.startScheduler(job, true);
		}

		this.updateUnloadGuard();
	}

	/**
	 * Pick the files of a session started before a reload. Files are matched
	 * by name and size and their first MiB compared with the server's; the
	 * upload then continues where the server is, with an overlap that the
	 * server compares byte for byte.
	 */
	async resumeSession(id: string, picked: File[]): Promise<AdminUploadResumeReport | null> {
		const existed = this.jobs.some((job) => job.sessionId === id);
		const key = await this.follow(id);

		if (!key) return null;

		const job = this.job(key);

		if (!job) return null;

		const report = await this.matchFiles(job, picked);

		// nothing usable was picked: leave the session in the unfinished list for another try
		if (report.matched.length === 0 && job.phase === AdminUploadJobPhase.UPLOADING && !existed) {
			this.abandon(key);

			return report;
		}

		if (job.phase === AdminUploadJobPhase.UPLOADING && report.mismatched.length === 0) {
			this.startScheduler(job, true);
		}

		return report;
	}

	/** Queue a job again whose session could not be created (after a rename). */
	retryCreate(key: string, name?: string): void {
		const job = this.job(key);

		if (!job || job.sessionId || job.phase !== AdminUploadJobPhase.FAILED) return;

		if (name) job.name = name;

		job.phase = AdminUploadJobPhase.QUEUED;
		job.error = null;
		job.errorType = null;
		// asked by the admin: try now, even while the queue waits for a free upload slot
		this.slotWait = null;
		void this.pump();
	}

	/** Set or clear the expected SHA-256 of a file (Retry then compares again). Throws. */
	async setExpectedSha(key: string, index: number, sha256: string): Promise<void> {
		const job = this.job(key);

		if (!job?.sessionId) return;

		const result = await adminStore.runWithKey((k) =>
			AdminService.updateUpload(k, { files: [{ index, sha256 }], id: job.sessionId as string })
		);
		const file = job.files.find((item) => item.index === index);

		if (file) file.expectedSha256 = sha256;

		this.applySession(key, result.upload);
	}

	/** Another tab sends this upload: take it over (that tab stops at its next chunk). */
	takeover(key: string): void {
		this.schedulers.get(key)?.resume({ takeover: true });
		this.updateUnloadGuard();
	}

	private applySession(key: string, session: ApiAdminUploadSession | null | undefined): void {
		const job = this.job(key);

		if (!job || !session || typeof session !== 'object' || !session.id) return;

		const before = job.phase;

		job.session = session;
		job.name = session.name ?? job.name;

		for (const remote of session.files ?? []) {
			const file = job.files.find((item) => item.index === remote.index);

			if (!file) continue;

			file.offset = remote.offset;
			file.complete = remote.complete;
		}

		const phase = jobPhaseOf(session.state);
		const scheduler = this.schedulers.get(key);

		if (session.state !== AdminUploadState.RECEIVING && scheduler) {
			scheduler.stop();
			this.schedulers.delete(key);
		}

		job.phase = phase;

		if (phase !== before) this.onPhaseChange(job, before);

		this.updateUnloadGuard();
		this.schedulePoll();
	}

	/** Create the session of a queued job and start sending. */
	private async createSession(job: AdminUploadJob): Promise<void> {
		job.phase = AdminUploadJobPhase.CREATING;

		const settings = Object.keys(job.settings).length > 0 ? job.settings : undefined;

		try {
			const result = await adminStore.runWithKey((k) =>
				AdminService.createUpload(k, {
					auto_publish: job.autoPublish,
					files: job.files.map((file) => ({
						last_modified: file.lastModified,
						name: file.name,
						sha256: file.expectedSha256 || undefined,
						size: file.size
					})),
					name: job.name,
					settings,
					settings_mode: job.settingsMode
				})
			);
			const session = result.upload as ApiAdminUploadSession;

			job.sessionId = session.id;
			job.session = session;
			job.phase = AdminUploadJobPhase.UPLOADING;
			job.error = null;
			job.errorType = null;
			this.startScheduler(job, false);
			void this.loadInfo();
		} catch (error) {
			if (error instanceof AdminApiError && error.type === AdminErrorType.TOO_MANY_UPLOADS) {
				// the server's limit of open uploads: the job stays queued until one of them ends
				// (published, cancelled, expired); pump() waits for that, see slotFreed()
				const listed = Array.isArray(error.details?.sessions)
					? (error.details.sessions as { id?: unknown }[])
					: [];
				const ids = listed
					.map((session) => session?.id)
					.filter((id): id is string => typeof id === 'string' && id.length > 0);
				const first = job.errorType !== AdminErrorType.TOO_MANY_UPLOADS;

				job.phase = AdminUploadJobPhase.QUEUED;
				job.errorType = AdminErrorType.TOO_MANY_UPLOADS;
				job.error = describeUploadSlotWait(
					ids.length > 0 ? ids.length : (this.info?.limits.max_sessions ?? null)
				);
				this.slotWait = new Set(ids);
				this.lastSlotPoll = Date.now();

				if (first) toast.info(`${job.name} waits in the queue: ${job.error}`);

				// a session may have ended between the refusal and now
				void this.loadInfo();
				this.schedulePoll();

				return;
			}

			job.phase = AdminUploadJobPhase.FAILED;
			job.error = errorText(error);
			job.errorType = error instanceof AdminApiError ? error.type : null;
			toast.error(`The upload of ${job.name} could not start: ${job.error}`);
		}
	}

	/** The queue waits for a free upload slot: go on when a session of the refusal ended. */
	private checkSlotWait(): void {
		if (!this.slotWait || !this.info) return;

		const open = openUploadSessions(this.info.sessions ?? []);
		const freed =
			this.slotWait.size > 0
				? [...this.slotWait].some((id) => !open.some((session) => session.id === id))
				: open.length < this.info.limits.max_sessions;

		if (freed) this.slotFreed();
	}

	/** An upload session of the server ended: a job waiting for a free slot may start. */
	private slotFreed(): void {
		if (!this.slotWait) return;

		this.slotWait = null;
		void this.pump();
	}

	private job(key: string): AdminUploadJob | undefined {
		return this.jobs.find((job) => job.key === key);
	}

	/** A job built from a server session (files not picked yet). */
	private jobFromSession(key: string, session: ApiAdminUploadSession): AdminUploadJob {
		const files: AdminUploadJobFile[] = (session.files ?? []).map((file) => ({
			complete: file.complete,
			expectedSha256: file.sha256_expected ?? '',
			file: null,
			headerEnd: null,
			index: file.index,
			lastModified: 0,
			name: file.name,
			offset: file.offset,
			role: file.role === 'projector' ? AdminUploadFileRole.PROJECTOR : AdminUploadFileRole.MODEL,
			size: file.size
		}));

		return {
			autoPublish: session.auto_publish,
			createdAt: Date.parse(session.created) || Date.now(),
			error: null,
			errorType: null,
			files,
			fit: null,
			fitError: null,
			fitRunning: false,
			key,
			name: session.name,
			phase: jobPhaseOf(session.state),
			placement: Object.keys(session.settings ?? {}).length > 0,
			send: null,
			session,
			sessionId: session.id,
			settings: { ...(session.settings ?? {}) },
			settingsMode: session.settings_mode ?? 'replace'
		};
	}

	/** Match picked files to the files of a job and check that they are the same bytes. */
	private async matchFiles(job: AdminUploadJob, picked: File[]): Promise<AdminUploadResumeReport> {
		const report: AdminUploadResumeReport = {
			matched: [],
			mismatched: [],
			missing: [],
			unknown: []
		};
		const used: File[] = [];

		for (const file of job.files) {
			const remote = job.session?.files?.find((item) => item.index === file.index);

			if (file.complete || remote?.dropped) continue;

			const candidates = picked.filter(
				(item) => !used.includes(item) && item.size === file.size && item.name === file.name
			);
			const sameSize = picked.filter((item) => !used.includes(item) && item.size === file.size);
			const pick = candidates[0] ?? (sameSize.length === 1 ? sameSize[0] : null);

			if (!pick) {
				if (!file.file) report.missing.push(file.name);

				continue;
			}

			used.push(pick);

			const same = await this.sameBytes(job, file, pick);

			if (same === false) {
				report.mismatched.push(pick.name);

				continue;
			}

			file.file = pick;
			file.lastModified = pick.lastModified;
			report.matched.push(file.name);
		}

		report.unknown = picked.filter((item) => !used.includes(item)).map((item) => item.name);

		return report;
	}

	/** The phase of a job changed: report it and let the queue go on. */
	private onPhaseChange(job: AdminUploadJob, before: AdminUploadJobPhase): void {
		const session = job.session;

		switch (job.phase) {
			case AdminUploadJobPhase.PUBLISHED: {
				const model = session?.result?.model ?? job.name;

				adminStore.notice(AdminNoticeLevel.SUCCESS, `Uploaded and published ${model}`);
				void adminStore.refreshModels();
				void adminStore.loadModelSettings();

				if (session?.result?.reload_error) {
					adminStore.notice(
						AdminNoticeLevel.WARNING,
						`${model} was published, but reloading the model list failed: ${session.result.reload_error}`
					);
				}

				if (job.placement && adminStore.hasFeature(AdminFeature.FIT_CHECK)) void this.runFit(job);

				break;
			}
			case AdminUploadJobPhase.REJECTED:
				adminStore.notice(
					AdminNoticeLevel.ERROR,
					`Upload of ${job.name} rejected: ${describeUploadRejection(session?.error?.code ?? '', session?.error?.message)}`
				);

				break;
			case AdminUploadJobPhase.READY:
				if (before !== AdminUploadJobPhase.READY) {
					toast.info(
						`${job.name}: ${describeUploadBlock(session?.blocked_by ?? null, session?.blocked ?? null).title}`
					);
				}

				break;
			case AdminUploadJobPhase.EXPIRED:
				toast.info(`The upload of ${job.name} expired`);

				break;
		}

		// its session ended: one upload slot of the server is free again
		if (job.sessionId && isJobFinished(job.phase)) this.slotWait = null;

		void this.loadInfo();
		void this.pump();
	}

	private onSendUpdate(key: string, snapshot: AdminUploadSendSnapshot): void {
		const job = this.job(key);

		if (!job) return;

		const before = job.send?.state;

		job.send = snapshot;

		for (const file of job.files) {
			const offset = snapshot.offsets[file.index];

			if (offset !== undefined) file.offset = offset;
		}

		if (before === snapshot.state) return;

		switch (snapshot.state) {
			case AdminUploadSendState.KEY_REQUIRED:
				adminStore.reportKeyRejected();

				break;
			case AdminUploadSendState.FORBIDDEN:
				adminStore.reportForbidden(
					new AdminApiError(snapshot.problem?.message ?? 'Forbidden', 403, 'admin_forbidden')
				);

				break;
			case AdminUploadSendState.DISK_FULL:
				toast.error(`Upload of ${job.name} paused: the disk is full`);

				break;
		}

		this.updateUnloadGuard();
	}

	/** Polls the sessions of the jobs that change on the server. */
	private async pollOnce(): Promise<void> {
		const now = Date.now();

		// waiting for a free upload slot: the sessions that hold them may end in another tab
		if (this.isWaitingForSlot() && now - this.lastSlotPoll >= ADMIN_UPLOAD.POLL_SLOT_MS) {
			this.lastSlotPoll = now;
			await this.loadInfo();
		}

		for (const job of [...this.jobs]) {
			if (!job.sessionId || isJobFinished(job.phase)) continue;

			const every =
				job.phase === AdminUploadJobPhase.UPLOADING
					? ADMIN_UPLOAD.POLL_UPLOADING_MS
					: job.phase === AdminUploadJobPhase.READY
						? null
						: ADMIN_UPLOAD.POLL_CHECKING_MS;

			if (every === null || now - (this.lastPoll.get(job.key) ?? 0) < every) continue;

			this.lastPoll.set(job.key, now);
			await this.refreshJob(job.key);
		}
	}

	/**
	 * Advance the queue: create the next session when nothing is being sent.
	 * A create that fails leaves its job FAILED and the next one is tried at
	 * once; a create refused with too_many_uploads keeps its job queued and the
	 * queue waits for a free slot (slotFreed).
	 */
	private async pump(): Promise<void> {
		if (this.pumping) return;

		this.pumping = true;

		try {
			while (!this.slotWait) {
				const busy = this.jobs.some(
					(job) =>
						job.phase === AdminUploadJobPhase.CREATING ||
						(job.phase === AdminUploadJobPhase.UPLOADING &&
							job.send?.state !== AdminUploadSendState.DONE &&
							job.files.some((file) => file.file))
				);

				if (busy) break;

				const next = this.jobs.find((job) => job.phase === AdminUploadJobPhase.QUEUED);

				if (!next) break;

				await this.createSession(next);
			}
		} finally {
			this.pumping = false;
		}

		this.updateUnloadGuard();
	}

	/** A queued job waits because the server has its maximum of open uploads. */
	private isWaitingForSlot(): boolean {
		return (
			this.slotWait !== null && this.jobs.some((job) => job.phase === AdminUploadJobPhase.QUEUED)
		);
	}

	/** Read one job's session again. */
	private async refreshJob(key: string): Promise<void> {
		const job = this.job(key);

		if (!job?.sessionId) return;

		try {
			const session = await adminStore.runWithKey((k) =>
				AdminService.getUpload(k, job.sessionId as string)
			);

			this.applySession(key, session);
		} catch (error) {
			if (error instanceof AdminApiError && error.status === 410) {
				const state = (error.details?.state as string | undefined) ?? AdminUploadState.CANCELLED;

				job.phase = jobPhaseOf(state);
				this.schedulers.get(key)?.stop();
				this.schedulers.delete(key);
				this.updateUnloadGuard();
				this.slotFreed();
			}
		}
	}

	/** The fit check after a publish with a placement (the model exists now, unloaded). */
	private async runFit(job: AdminUploadJob): Promise<void> {
		const model = job.session?.result?.model ?? job.name;

		job.fitRunning = true;

		try {
			job.fit = await adminStore.checkModelSettings(model, {});
			job.fitError = null;
		} catch (error) {
			job.fitError = errorText(error);
		} finally {
			job.fitRunning = false;
		}
	}

	/** Compare a picked file with the bytes on the server: head CRC, else a verify probe. */
	private async sameBytes(
		job: AdminUploadJob,
		file: AdminUploadJobFile,
		pick: File
	): Promise<boolean | null> {
		const remote = job.session?.files?.find((item) => item.index === file.index);

		if (!remote || remote.offset <= 0) return true;

		try {
			if (remote.head_crc32) return (await headCrc(pick)) === remote.head_crc32;

			return await verifyUploadedStart(
				this.transportFor(),
				job.sessionId as string,
				file.index,
				pick,
				remote.offset,
				new AbortController().signal
			);
		} catch {
			return null;
		}
	}

	private schedulePoll(): void {
		if (this.pollTimer) return;

		const needed =
			this.isWaitingForSlot() ||
			this.jobs.some(
				(job) =>
					job.sessionId &&
					!isJobFinished(job.phase) &&
					job.phase !== AdminUploadJobPhase.READY &&
					job.phase !== AdminUploadJobPhase.QUEUED
			);

		if (!needed) return;

		this.pollTimer = setTimeout(async () => {
			try {
				await this.pollOnce();
			} finally {
				this.pollTimer = null;
				this.schedulePoll();
			}
		}, ADMIN_UPLOAD.POLL_CHECKING_MS);
	}

	/** Start (or restart) the chunk scheduler of a receiving job. */
	private startScheduler(job: AdminUploadJob, overlapFirst: boolean): void {
		if (!job.sessionId) return;

		this.schedulers.get(job.key)?.stop();

		const session = job.session;
		const key = job.key;
		const scheduler = new AdminUploadScheduler({
			ceiling: (this.ceiling ??= loadCeiling()),
			chunkDefault: session?.chunk_default ?? this.info?.limits.chunk_default ?? 8 * 1048576,
			chunkMax: session?.chunk_max ?? this.info?.limits.chunk_max ?? 16 * 1048576,
			chunkMin: this.info?.limits.chunk_min ?? ADMIN_UPLOAD.STEP_LADDER.at(-1) ?? 245760,
			files: job.files.map((file) => {
				const remote = session?.files?.find((item) => item.index === file.index);

				return {
					blob: file.file,
					complete: file.complete,
					dropped: remote?.dropped ?? false,
					headerEnd: file.headerEnd,
					index: file.index,
					offset: file.offset,
					size: file.size
				};
			}),
			// a ceiling found by a proxy's 413 or by connections dropped on a size: kept for the
			// browser session, so a reload (or the next model) does not have to find it again
			onCeiling: (ceiling) => {
				this.ceiling = ceiling;

				if (!browser) return;

				try {
					sessionStorage.setItem(ADMIN_UPLOAD_STORAGE.CHUNK_CEILING, String(ceiling));
				} catch {
					// the ceiling is kept in memory only
				}
			},
			onFinished: () => {
				void this.refreshJob(key).then(() => this.pump());
			},
			onSession: (body) => this.applySession(key, body as unknown as ApiAdminUploadSession),
			onUpdate: (snapshot) => this.onSendUpdate(key, snapshot),
			overlapFirst,
			sessionId: job.sessionId,
			transport: this.transportFor()
		});

		this.schedulers.set(key, scheduler);
		scheduler.start();
		this.schedulePoll();
		this.updateUnloadGuard();
	}

	private transportFor(): AdminUploadTransport {
		this.clientId ??= loadClientId();
		this.transport ??= AdminUploadService.createTransport(
			() => adminStore.currentKey(),
			this.clientId
		);

		return this.transport;
	}

	/** Warn before the page is left while bytes are being sent. */
	private updateUnloadGuard(): void {
		if (!browser || this.unloadListening) return;

		this.unloadListening = true;
		window.addEventListener('beforeunload', (event) => {
			const sending = this.jobs.some(
				(job) =>
					(job.phase === AdminUploadJobPhase.UPLOADING &&
						job.send !== null &&
						job.send.state !== AdminUploadSendState.PAUSED &&
						job.send.state !== AdminUploadSendState.DONE) ||
					job.phase === AdminUploadJobPhase.QUEUED ||
					job.phase === AdminUploadJobPhase.CREATING
			);

			if (!sending) return;

			event.preventDefault();
			// legacy browsers show the dialog only when returnValue is set
			event.returnValue = '';
		});
	}
}

export const adminUploadStore = new AdminUploadStore();
