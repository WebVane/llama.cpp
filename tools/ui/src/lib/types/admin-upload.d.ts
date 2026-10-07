/**
 * Admin panel, stage 3: GGUF upload into the models directory and deleting
 * models_dir models. Wire shapes of the /admin/models/upload* and
 * /admin/models/delete routes plus the view models of the upload queue.
 * Imported by path ('$lib/types/admin-upload'), not through the types barrel.
 */

import type {
	AdminModelRole,
	AdminUploadFileRole,
	AdminUploadJobPhase,
	AdminUploadSendState
} from '$lib/enums/admin.enums';
import type { ApiAdminFitCheck, ApiAdminSettingLock } from '$lib/types/admin';

/** `limits` of GET /admin/models/upload. */
export interface ApiAdminUploadLimits {
	chunk_max: number;
	chunk_default: number;
	/** The client's floor when a proxy limits the request size */
	chunk_min: number;
	max_sessions: number;
	max_writers: number;
	max_files: number;
	ttl_h: number;
	ttl_s: number;
	max_header_bytes: number;
}

/** `upload` of GET /admin/status. */
export interface ApiAdminUploadAvailability {
	available: boolean;
	/** no_models_dir | models_dir_missing | not_writable | staging_unusable | staging_other_filesystem */
	reason: string | null;
	message: string | null;
}

/** `{code, message, ...}` of a session error or block. */
export interface ApiAdminUploadProblem {
	code: string;
	message: string;
	[extra: string]: unknown;
}

/** One session in the list of GET /admin/models/upload (finished ones stay 24 h). */
export interface ApiAdminUploadSummary {
	id: string;
	name: string;
	/** receiving | checking | publishing | ready | published | rejected | cancelled | expired */
	state: string;
	/** queued | hashing | classifying | load_check while checking */
	substate: string | null;
	/** manual | name_taken | publish_error | sha256_mismatch | check_failed | settings_not_saved | settings_invalid */
	blocked_by: string | null;
	total_bytes: number;
	received_bytes: number;
	n_files: number;
	auto_publish: boolean;
	created: string;
	last_activity: string;
	error: ApiAdminUploadProblem | null;
	blocked: ApiAdminUploadProblem | null;
	/** Only while receiving or ready */
	expires_at: string | null;
}

/** `header` of a session file: the early check of its first bytes. */
export interface ApiAdminUploadFileHeader {
	/** none | scanning | queued | ok | warning | failed | error */
	state: string;
	arch?: string | null;
	type?: string | null;
	split?: { no: number; count: number };
	message?: string;
}

export interface ApiAdminUploadFile {
	index: number;
	name: string;
	size: number;
	/** Bytes accepted so far: the next chunk starts here */
	offset: number;
	/** Bytes known to be on the disk (survive a restart) */
	durable_offset: number;
	complete: boolean;
	dropped: boolean;
	role: 'model' | 'projector' | null;
	sha256: string | null;
	sha256_expected: string | null;
	/** '%08x' CRC32 of the first min(1 MiB, size) bytes, once they are stored */
	head_crc32: string | null;
	final_name: string | null;
	header: ApiAdminUploadFileHeader;
	error: ApiAdminUploadProblem | null;
}

export interface ApiAdminUploadPlanFile {
	index: number;
	final_name: string;
	role: string;
	split_no?: number;
}

export interface ApiAdminUploadPlan {
	layout: 'file' | 'folder';
	target: string;
	id: string;
	files: ApiAdminUploadPlanFile[];
}

/** What the checks read from the model. */
export interface ApiAdminUploadFacts {
	arch?: string;
	name?: string;
	n_params?: number;
	n_ctx_train?: number;
	n_layer?: number;
	file_type?: string | number;
	n_tensors?: number;
	split?: number;
	n_vocab?: number;
	desc?: string;
	vision?: boolean;
	audio?: boolean;
}

export interface ApiAdminUploadResultFile {
	/** Relative to the models directory */
	name: string;
	role: string;
	size: number;
	sha256: string;
}

/** `result` of a published session. */
export interface ApiAdminUploadResult {
	model: string;
	layout: string;
	target: string;
	rename?: string;
	reload_error: string | null;
	settings_saved: boolean;
	files: ApiAdminUploadResultFile[];
}

/** GET /admin/models/upload?id= and the `upload` of the session routes. */
export interface ApiAdminUploadSession extends ApiAdminUploadSummary {
	files: ApiAdminUploadFile[];
	settings: Record<string, string>;
	settings_mode: 'replace' | 'keep';
	plan: ApiAdminUploadPlan | null;
	facts: ApiAdminUploadFacts | null;
	result: ApiAdminUploadResult | null;
	warnings: string[];
	idle_ms: number;
	/** ms since the last chunk from any client; null before the first one */
	last_client_idle_ms: number | null;
	recovered: boolean;
	recovered_without_record: boolean;
	record_persisted: boolean;
	chunk_max: number;
	chunk_default: number;
	/** While hashing */
	progress: { done_bytes: number; total_bytes: number } | null;
	checks: { step: string; file: number; status: string; message: string }[];
}

/** GET /admin/models/upload. */
export interface ApiAdminUploadInfo {
	available: boolean;
	reason: string | null;
	message: string | null;
	limits: ApiAdminUploadLimits;
	sessions: ApiAdminUploadSummary[];
	staging_bytes: number;
	models_dir: string | null;
	settings_writable: boolean;
	settings_error: string | null;
}

export interface ApiAdminUploadDisk {
	free_bytes: number;
	total_bytes: number;
	needed_bytes: number;
	other_uploads_bytes: number;
	shares_disk_with: string[];
}

/** POST /admin/models/upload body. */
export interface ApiAdminUploadCreate {
	name: string;
	files: { name: string; size: number; last_modified?: number; sha256?: string }[];
	settings?: Record<string, string | null>;
	settings_mode?: 'replace' | 'keep';
	auto_publish?: boolean;
	dry_run?: boolean;
}

/** POST /admin/models/upload answer (201; 200 with `upload.id` null for a dry run). */
export interface ApiAdminUploadCreateResult {
	upload: ApiAdminUploadSession | (Partial<ApiAdminUploadSession> & { id: null; name: string });
	disk: ApiAdminUploadDisk;
	warnings: string[];
	locks: Record<string, ApiAdminSettingLock>;
	/** Saved settings of an earlier model with this id (an orphaned entry) */
	existing_settings?: Record<string, string>;
}

/** POST /admin/models/upload/update body. */
export interface ApiAdminUploadUpdate {
	id: string;
	name?: string;
	settings?: Record<string, string | null> | null;
	settings_mode?: 'replace' | 'keep';
	auto_publish?: boolean;
	files?: { index: number; sha256: string }[];
	drop_files?: number[];
}

/** Answer of update / publish / cancel. */
export interface ApiAdminUploadSessionResult {
	upload: ApiAdminUploadSession;
	warnings?: string[];
	locks?: Record<string, ApiAdminSettingLock>;
	existing_settings?: Record<string, string>;
	cancelled?: boolean;
	freed_bytes?: number;
}

/** 200 answer of POST /admin/models/upload/chunk. */
export interface ApiAdminUploadChunkResult {
	offset: number;
	durable_offset: number;
	file_complete: boolean;
	state: string;
	file?: number;
	received_bytes?: number;
	total_bytes?: number;
	file_error?: ApiAdminUploadProblem | null;
	/** verify=1 probe */
	verified?: boolean;
	verified_bytes?: number;
}

/** One file a delete removes. */
export interface ApiAdminDeleteFile {
	/** Relative to the models directory */
	path: string;
	bytes: number;
	role: 'model' | 'shard' | 'projector' | 'draft';
}

/**
 * An entry that stays: in the model's folder but not part of the model, or
 * (`shared`) a projector or draft every model uses (router command line,
 * [*] of models.ini).
 */
export interface ApiAdminDeleteKept {
	path: string;
	bytes: number | null;
	kind: 'file' | 'dir' | 'symlink';
	shared?: boolean;
}

/** POST /admin/models/delete with dry_run. */
export interface ApiAdminDeletePreview {
	model: string;
	source: string;
	projector_only: boolean;
	total_bytes: number;
	n_files: number;
	n_kept: number;
	kept_bytes: number;
	running: boolean;
	status: string;
	pinned: boolean;
	has_settings: boolean;
	folder: string | null;
	folder_removed: boolean;
	files: ApiAdminDeleteFile[];
	/** At most 100 listed; n_kept has the full count */
	kept: ApiAdminDeleteKept[];
	/** `kept` lists fewer entries than n_kept (absent on an older server) */
	kept_truncated?: boolean;
	/** folder_keeps_other_model | running */
	warnings: string[];
	dry_run: true;
}

/** POST /admin/models/delete. */
export interface ApiAdminDeleteResult {
	model: string;
	freed_bytes: number;
	folder_removed: boolean;
	settings_removed: boolean;
	pin_removed: boolean;
	trash_left: boolean;
	reload_error: string | null;
	deleted?: boolean;
	projector_removed?: boolean;
	files: ApiAdminDeleteFile[];
	kept: ApiAdminDeleteKept[];
}

/** POST /admin/models/delete body. */
export interface ApiAdminDeleteRequest {
	model: string;
	dry_run?: boolean;
	stop?: boolean;
	remove_settings?: boolean;
	projector_only?: boolean;
}

/*
 * Browser side of an upload: what the GGUF header reader found, how picked
 * files were grouped into models, and the jobs of the upload queue.
 */

/** What the browser read from the start of a GGUF file (a hint; the server decides). */
export interface AdminGgufHeader {
	/** null when the header could be read */
	error: 'not_gguf' | 'unsupported_version' | 'truncated' | 'malformed' | 'too_large' | null;
	version: number;
	nTensors: number;
	nKv: number;
	arch: string | null;
	/** general.type: model | adapter | imatrix | mmproj ... */
	type: string | null;
	name: string | null;
	fileType: number | null;
	/** Quantization label from general.file_type, e.g. Q4_K_M */
	quant: string | null;
	/** split.no (0-based) and split.count */
	splitNo: number | null;
	splitCount: number | null;
	contextLength: number | null;
	blockCount: number | null;
	adapterType: string | null;
	clipVision: boolean | null;
	clipAudio: boolean | null;
	/** Bytes of the metadata (key-value) section, from the start of the file */
	kvEnd: number;
	/** End of the tensor infos (aligned to the data start); null when not reached */
	headerEnd: number | null;
}

/** The placement fields of an upload (empty = the model inherits the value). */
export interface AdminUploadPlacementValue {
	device: string;
	ctxSize: string;
	parallel: string;
	role: AdminModelRole;
}

/** One picked file after grouping. */
export interface AdminUploadPickedFile {
	/** Index in the pick */
	key: number;
	file: File;
	name: string;
	size: number;
	header: AdminGgufHeader | null;
	role: AdminUploadFileRole;
	/** Shard number (1-based) and count from the file name */
	shard: { no: number; count: number } | null;
	/** Problem codes of this file (see describeUploadFileProblem) */
	problems: string[];
}

/** A model made of picked files: one upload session. */
export interface AdminUploadGroup {
	key: string;
	files: AdminUploadPickedFile[];
	/** The projector paired with this model, if any */
	projector: AdminUploadPickedFile | null;
	/** Problem codes of the set (missing shards, split mismatch ...) */
	problems: string[];
	/** Shard numbers that are missing from a split set */
	missingShards: number[];
	proposedName: string;
}

/** Result of grouping a pick. */
export interface AdminUploadGrouping {
	groups: AdminUploadGroup[];
	/** Projectors that could not be paired by themselves: the user picks the model */
	unpairedProjectors: AdminUploadPickedFile[];
	/** Files that cannot be uploaded, with the reason code */
	ignored: { name: string; reason: string }[];
}

/** One file of a job: the picked File (null after a reload until picked again). */
export interface AdminUploadJobFile {
	index: number;
	name: string;
	size: number;
	lastModified: number;
	file: File | null;
	role: AdminUploadFileRole;
	headerEnd: number | null;
	expectedSha256: string;
	/** Server-confirmed offset */
	offset: number;
	complete: boolean;
}

/** What the chunk scheduler reports. */
export interface AdminUploadSendSnapshot {
	state: AdminUploadSendState;
	/** Server-confirmed bytes over all files */
	confirmed: number;
	/** Bytes of the chunk in flight already handed to the network */
	inFlight: number;
	total: number;
	/** Bytes per second over the last seconds, null while unknown */
	speed: number | null;
	/** Seconds left at that speed, null while unknown */
	eta: number | null;
	chunkSize: number;
	/** A proxy refused larger requests: the size it allows */
	proxyLimit: number | null;
	/** When the next retry is due (ms since the epoch) */
	retryAt: number | null;
	/** Failed attempts in a row */
	failures: number;
	/** The file and reason of a refused file, a gone session, a disk full ... */
	problem: {
		code: string;
		message: string;
		file?: number;
		freeBytes?: number;
		idleMs?: number;
	} | null;
	/** Per file offsets, by index */
	offsets: Record<number, number>;
}

/** One entry of the upload queue (a model being or to be uploaded). */
export interface AdminUploadJob {
	key: string;
	name: string;
	phase: AdminUploadJobPhase;
	sessionId: string | null;
	files: AdminUploadJobFile[];
	settings: Record<string, string>;
	settingsMode: 'replace' | 'keep';
	autoPublish: boolean;
	/** The last session answer of the server */
	session: ApiAdminUploadSession | null;
	send: AdminUploadSendSnapshot | null;
	/** Why the job stopped (create refused, network ...) */
	error: string | null;
	errorType: string | null;
	createdAt: number;
	/** A placement was chosen: the fit is checked after the publish */
	placement: boolean;
	/** The fit check run after the publish (null until it answered) */
	fit: ApiAdminFitCheck | null;
	fitError: string | null;
	fitRunning: boolean;
}

/** A model the upload tab hands to the queue. */
export interface AdminUploadRequest {
	name: string;
	/** Model files first, then the projector */
	files: AdminUploadPickedFile[];
	/** Expected SHA-256 per file (by pick key), empty = none */
	expectedSha256: Record<number, string>;
	settings: Record<string, string>;
	settingsMode: 'replace' | 'keep';
	autoPublish: boolean;
}

/** What the upload tab needs to know about one model card. */
export interface AdminUploadGroupState {
	/** The request to queue, null while it cannot be queued */
	request: AdminUploadRequest | null;
	/** Why it cannot be queued (a name problem, a missing part ...) */
	blocking: string | null;
	/** Warnings of the server's dry run: "Upload anyway" */
	warnings: string[];
	checking: boolean;
}

/** Outcome of picking files for a session that was started before a reload. */
export interface AdminUploadResumeReport {
	matched: string[];
	/** Picked files with the right name and size but other bytes */
	mismatched: string[];
	/** Files of the session that still need a pick */
	missing: string[];
	/** Picked files that belong to no file of the session */
	unknown: string[];
}
