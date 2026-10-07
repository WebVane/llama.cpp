/**
 * Admin panel (provision-server, router mode with PROVISION_ADMIN=1) — wire
 * shapes of the /admin routes plus the view models of the admin page.
 * Every /admin route except the status probe needs the admin key; the model
 * table itself is fed by the public model routes.
 */

import type {
	AdminDownloadOutcome,
	AdminFitSource,
	AdminFitVerdict,
	AdminImportIssue,
	AdminNoticeLevel,
	AdminStorageDirKind,
	ServerModelSource,
	ServerModelStatus
} from '$lib/enums';
import type { ApiAdminAuditStatus, ApiAdminAuditStorage } from '$lib/types/admin-audit';
import type { ApiModelsSseDownloadFile } from '$lib/types/api';

/**
 * GET /admin/status. Without a valid key only the first three fields are
 * returned; the rest is present when `authorized` is true.
 */
export interface ApiAdminStatus {
	admin: boolean;
	authorized: boolean;
	/** False when the server could not obtain a key: protected routes answer 503 */
	key_configured: boolean;
	version?: number;
	key_source?: 'env' | 'file' | 'generated';
	models?: { max: number; running: number; total: number };
	/** `error` carries the reason a state file could not be read, empty when fine */
	state?: {
		dir: string;
		writable: boolean;
		error: string;
		model_settings?: ApiAdminModelSettingsFileState;
	};
	/** `available` = HTTPS downloads compiled in and the router is not offline */
	download?: { available: boolean; offline: boolean };
	dirs?: { cache?: string; models_dir?: string; preset?: string };
	license_build?: boolean;
	/** Stage-2 parts this server has (see AdminFeature); absent on an older server */
	features?: string[];
	/** e.g. `default_device_not_present` */
	warnings?: string[];
	/** Stage 3: whether GGUF files can be uploaded into the models directory */
	upload?: { available: boolean; reason: string | null; message: string | null };
	/** Stage 3 M3: whether the audit log is written to the state directory (warning audit_not_persisted) */
	audit?: ApiAdminAuditStatus;
}

/** `state.model_settings` of GET /admin/status: the per-model settings file. */
export interface ApiAdminModelSettingsFileState {
	file: string;
	/** Why the file could not be read, empty when fine */
	error: string;
	/** Nothing can be saved: the file is corrupt or has a newer version */
	read_only: boolean;
	/** The main file was unreadable and its .bak copy was used */
	backup_loaded: boolean;
	entries?: number;
	/** Entries or values that were skipped while reading */
	problems?: string[];
}

export interface ApiAdminSettingsModel {
	id: string;
	source: ServerModelSource;
	status: ServerModelStatus;
	pinned: boolean;
}

/** GET / POST /admin/settings response. */
export interface ApiAdminSettings {
	models_max: number;
	default_device: string;
	/** MODEL_DEVICES has a `*=` entry: the value is fixed by the deployment */
	default_device_locked: boolean;
	/** Pinned ids actually in effect (exist in `models`, cut to models_max - 1) */
	pinned: string[];
	limits: { models_max_min: number; models_max_max: number };
	/** Hint list collected from the model presets, the router never probes GPUs */
	known_devices: string[];
	models: ApiAdminSettingsModel[];
	/** Configured pins dropped because they exceed models_max - 1 */
	pinned_ignored?: string[];
	/** Configured pins that name no model in the list */
	pinned_unknown?: string[];
	/** Settings were saved, but reloading the model list afterwards failed */
	reload_error?: string;
	/** GPU devices this server detected (stage 2); absent on an older server */
	build_devices?: string[];
	/** e.g. `default_device_not_present` */
	warnings?: string[];
}

/** POST /admin/settings body: any subset. */
export interface ApiAdminSettingsUpdate {
	models_max?: number;
	default_device?: string;
	pinned?: string[];
}

export interface ApiAdminStorageDir {
	kind: AdminStorageDirKind;
	path: string;
	exists: boolean;
	writable: boolean;
	total_bytes: number;
	free_bytes: number;
}

export interface ApiAdminPartialDownload {
	path: string;
	bytes: number;
}

/** GET /admin/storage. */
export interface ApiAdminStorage {
	dirs: ApiAdminStorageDir[];
	partial_downloads: ApiAdminPartialDownload[];
	/** Stage 3 M3: the audit files in the state directory */
	audit?: ApiAdminAuditStorage;
}

export interface ApiAdminDownloadCheckFile {
	/** Path inside the repository, may contain a sub-directory */
	name: string;
	bytes: number;
}

/** POST /admin/models/download/check — advisory pre-flight. */
export interface ApiAdminDownloadCheck {
	model: string;
	repo: string;
	quant: string;
	hf_reachable: boolean;
	exists: boolean;
	already_present: boolean;
	files: ApiAdminDownloadCheckFile[];
	total_bytes: number;
	free_bytes: number;
	enough_space: boolean;
	/** Codes, see AdminDownloadWarning; unknown codes are shown as is */
	warnings: string[];
	/** Quantization the downloader picks when none was given */
	resolved_quant?: string;
}

/** One pre-flight warning turned into a sentence for the dialog. */
export interface AdminDownloadWarningInfo {
	code: string;
	level: AdminNoticeLevel;
	text: string;
}

/** Summed byte counters of a running download. */
export interface AdminDownloadTotals {
	done: number;
	total: number;
	/** 0.0 -> 1.0, null while the total size is unknown */
	fraction: number | null;
}

/**
 * A download followed by the admin page, keyed by the name the download row
 * lives under (the name as typed).
 */
export interface AdminDownload {
	name: string;
	/** Per-file counters from the last download_progress record, keyed by URL */
	files: Record<string, ApiModelsSseDownloadFile>;
	/** The user pressed Cancel: a following download_failed means "Cancelled" */
	cancelRequested: boolean;
	/** The row was seen as `downloading` in the list or on the feed */
	confirmed: boolean;
	/** Cache model ids known when the download was first seen */
	cacheIdsBefore: string[];
}

/** Result line shown above the model table until dismissed. */
export interface AdminNotice {
	id: number;
	level: AdminNoticeLevel;
	text: string;
}

/** What decided the outcome of a finished download. */
export interface AdminDownloadResult {
	outcome: AdminDownloadOutcome;
	/** Cache models of the download's repository that appeared since it started */
	newModelIds: string[];
}

/** One row of the admin model table. */
export interface AdminModelRow {
	id: string;
	aliases: string[];
	tags: string[];
	source: ServerModelSource | null;
	/** Server status, with FAILED derived from "unloaded with a non-zero exit code" */
	status: ServerModelStatus;
	exitCode: number | null;
	pinned: boolean;
	/** Cache model that is not being downloaded */
	canDelete: boolean;
	download: AdminDownloadTotals | null;
}

/** Editable copy of the settings shown in the Settings tab. */
export interface AdminSettingsDraft {
	/** Raw text of the number input */
	modelsMax: string;
	defaultDevice: string;
	pinned: string[];
}

/** Per-field messages of the settings form, null when the field is fine. */
export interface AdminSettingsValidation {
	modelsMax: string | null;
	defaultDevice: string | null;
	pinned: string | null;
	valid: boolean;
}

/*
 * Stage 2: per-model settings, GPU memory, fit check, failure log, diagnostics.
 * Every value of a setting travels as its canonical string ("1024", "auto",
 * "q8_0", "true", "CUDA1"); null means "not set".
 */

/** One whitelisted key of GET /admin/models/settings `schema`. */
export interface ApiAdminSettingSchema {
	key: string;
	group: 'placement' | 'advanced' | 'role';
	/** Built-in default in canonical spelling; null = the option is simply absent */
	default: string | null;
	type: 'device' | 'int' | 'ngl' | 'enum' | 'bool';
	min?: number;
	max?: number;
	/** enum values, or the words an ngl value may also be ("auto", "all") */
	values?: string[];
	/** The number that means "automatic" (parallel: -1) */
	auto?: number;
	/** The key that must hold the same value (cache-type-k / cache-type-v) */
	pair?: string;
}

export interface ApiAdminSettingLock {
	by: string;
	value: string | null;
}

/** One key of a model row: what the model gets now and why. */
export interface ApiAdminSettingField {
	/** What the model process gets now; null = the option is absent (built-in default) */
	effective: string | null;
	default: string | null;
	/** The value without a per-model setting (models.ini, GPU for new models, ...) */
	inherited: string | null;
	inherited_origin: string;
	/** Fixed by MODEL_DEVICES or the router command line */
	locked: ApiAdminSettingLock | null;
	/** The saved per-model value, null when none */
	override: string | null;
	/** The saved value is what the model gets */
	active: boolean;
	/** Why a saved value is not applied */
	inactive_reason: string | null;
	origin: string;
}

/** Classified reason of a failed start. */
export interface ApiAdminFailureReason {
	code: string;
	message: string;
	hint: string;
	/** The output line the reason was taken from */
	line: string;
	/** Extra hints, e.g. `memory_tight` */
	hints: string[];
}

/** Snapshot of the last failed start, replaced only by a later failure. */
export interface ApiAdminFailure {
	time: string;
	time_ms: number;
	reason: ApiAdminFailureReason;
	port: number;
	exit_code: number;
	before_loaded: boolean;
	lines: string[];
	truncated: boolean;
	failures: number;
	settings_reverted: boolean;
}

export interface ApiAdminLastRun {
	status: string;
	port: number;
	running: boolean;
	failed: boolean;
	exit_code: number;
	reason: ApiAdminFailureReason | null;
}

/** Settings put back after failed starts (circuit breaker). */
export interface ApiAdminReverted {
	reason: string;
	message: string;
	time: string;
	/** The settings that failed */
	settings: Record<string, string>;
	failures: number;
}

/** A saved device name now points at another physical GPU. */
export interface ApiAdminDeviceMoved {
	device: string;
	saved_pci: string;
	current_pci: string | null;
	/** The device name the saved GPU has now, if it is still there */
	now_named: string | null;
}

export interface ApiAdminDeviceMissing {
	devices: string[];
	message: string;
}

/** What the context settings mean for one request. */
export interface ApiAdminContextInfo {
	slots: number;
	unified: boolean;
	/** KV pool in tokens (ctx-size rounded up to 256); null when ctx-size is not set */
	pool: number | null;
	per_slot_cap: number | null;
	n_ctx_train: number | null;
	/** min(pool, per-slot cap, n_ctx_train) */
	max_per_request: number | null;
	/** pool / slots with a shared pool */
	per_request_all_busy: number | null;
}

export interface ApiAdminModelFile {
	name: string;
	bytes: number;
}

/** GGUF facts of a model, read from its file header by the server. */
export interface ApiAdminModelFacts {
	arch: string;
	n_layer: number;
	n_ctx_train: number;
	n_embd: number;
	n_head: number;
	n_head_kv: number;
	/** Per-layer KV head counts when they differ between layers */
	n_head_kv_layers: number[] | null;
	d_k: number;
	d_v: number;
	n_vocab: number;
	files: ApiAdminModelFile[];
	file_bytes: number;
	tensor_bytes: number;
	/** Bytes of each repeating layer */
	layer_bytes: number[];
	/** Output layer (incl. output_norm, and the token_embd copy with tied embeddings) */
	output_bytes: number;
	/** Input embeddings, always kept on the CPU */
	token_embd_bytes: number;
	other_bytes: number;
	tied_embeddings: boolean;
	mmproj_bytes: number;
	quant: string;
	sliding_window: number;
	pooling: string | null;
	causal: boolean;
	/** KV bytes per token over all layers, per cache type; null = not supported */
	kv_bytes_per_token: Record<string, number | null> | null;
	kv_estimate: 'exact' | 'approximate' | 'unsupported';
	kv_note: string;
	error: string;
}

/** One model of GET /admin/models/settings. */
export interface ApiAdminModelSettingsRow {
	id: string;
	source: ServerModelSource;
	status: string;
	exists: true;
	hidden: boolean;
	/** Saved per-model values (canonical strings) */
	overrides: Record<string, string>;
	/** Saved raw values that are not applied (unknown key, invalid value) */
	kept: Record<string, string>;
	fields: Record<string, ApiAdminSettingField>;
	apply_error: string;
	phase: string | null;
	failures: number;
	updated: string | null;
	last_good: Record<string, string> | null;
	reverted: ApiAdminReverted | null;
	reverted_at_startup: boolean;
	last_failure: ApiAdminFailure | null;
	last_run: ApiAdminLastRun;
	device_moved: ApiAdminDeviceMoved | null;
	device_missing: ApiAdminDeviceMissing | null;
	context: ApiAdminContextInfo;
	facts_status: 'ready' | 'pending' | 'error' | 'unsupported';
	facts: ApiAdminModelFacts | null;
	warnings: string[];
}

/** Saved settings of an id that is not in the model list. */
export interface ApiAdminUnattachedEntry {
	id: string;
	/**
	 * orphaned: no model of that name; hidden: a cache model the list hides
	 * because a models.ini section deduplicates it (its settings still apply
	 * when a request names it); unreadable: the file entry could not be read
	 * and is kept as it is (only `raw`, no other field)
	 */
	kind: 'orphaned' | 'hidden' | 'unreadable';
	overrides: Record<string, string>;
	/** The entry as it is in the file, for kind unreadable */
	raw?: unknown;
	phase: string | null;
	failures: number;
	updated: string | null;
	reverted: ApiAdminReverted | null;
	exists: false;
}

/** GET /admin/models/settings. */
export interface ApiAdminModelSettingsList {
	schema: ApiAdminSettingSchema[];
	build_devices: string[];
	models: ApiAdminModelSettingsRow[];
	unattached: ApiAdminUnattachedEntry[];
	state: ApiAdminModelSettingsFileState;
	warnings: string[];
}

/** POST /admin/models/settings body; also the `settings` shape of an export file. */
export interface ApiAdminModelSettingsUpdate {
	model: string;
	/** A value sets the key, null removes it, keys not given stay as they are */
	settings?: Record<string, string | number | boolean | null>;
	/** Remove every saved value of the model */
	reset?: boolean;
	/** Restart a running model, or load a stopped one, with the new settings */
	restart?: boolean;
}

export interface ApiAdminNotRestarted {
	id: string;
	error: string;
}

/** POST /admin/models/settings answer. */
export interface ApiAdminModelSettingsSaveResult {
	model: ApiAdminModelSettingsRow;
	result: {
		changed: boolean;
		reloaded: boolean;
		restarted: boolean;
		was_running?: boolean;
		/** Other models a rescan side effect unloaded and that were started again */
		restarted_also: string[];
		not_restarted: ApiAdminNotRestarted[];
		/** Saved, but the model could not be started right now (e.g. model limit reached) */
		load_error: string;
		reload_error: string;
	};
	/** Saved values before this change, for every key touched (null = none) */
	previous: Record<string, string | null>;
	warnings: string[];
}

/** Memory a model needs on one device, in bytes. */
export interface ApiAdminMemoryNeed {
	model: number;
	context: number;
	compute: number;
	total: number;
}

export interface ApiAdminFitCheckDevice {
	device: string;
	description: string;
	need: ApiAdminMemoryNeed;
	free: number;
	total: number;
	free_source: string;
	/** What this model uses there now (added back: a restart frees it) */
	own_usage: number;
	own_usage_source: string | null;
	available: number;
	settling: boolean;
	verdict: string;
}

/** POST /admin/models/settings/check: the dry run of llama.cpp on the server. */
export interface ApiAdminFitCheck {
	ok: boolean;
	model: string;
	checked_at: string;
	cached: boolean;
	duration_ms?: number;
	args?: string[];
	devices: ApiAdminFitCheckDevice[];
	host: { model: number; context: number; compute: number; free?: number; total?: number } | null;
	host_bytes: number | null;
	verdict: string;
	margin: number;
	n_ctx?: number | null;
	n_ctx_train?: number | null;
	n_layer?: number | null;
	n_gpu_layers?: number | null;
	fit?: string | null;
	error: { code: string; message: string; line: string } | null;
	warnings: string[];
}

/** GET /admin/models/log. */
export interface ApiAdminModelLog {
	model: string;
	status: string;
	instance: {
		port: number;
		running: boolean;
		started: string;
		/** Start in ms since the epoch, comparable with `last_failure.time_ms` (absent on older servers) */
		started_ms?: number;
		lines_total: number;
	} | null;
	/** null while a healthy instance runs */
	reason: ApiAdminFailureReason | null;
	lines: string[];
	truncated: boolean;
	last_failure: ApiAdminFailure | null;
}

export interface ApiAdminGpuModelMemory {
	bytes: number;
	source: string;
	/** model | nvidia-smi | llama.cpp | formula */
	by: string;
}

/** A model placed on a GPU card. */
export interface ApiAdminGpuModel {
	id: string;
	status: string;
	/** The model has no device setting: it may use every GPU */
	all_gpus: boolean;
	pinned: boolean;
	memory: ApiAdminGpuModelMemory | null;
	facts_status: string;
}

/** One GPU card of GET /admin/gpus. */
export interface ApiAdminGpu {
	device: string;
	backend: string;
	vendor: string;
	type: string;
	description: string;
	pci_bus_id: string | null;
	mapping: string;
	tool: string | null;
	index: number | null;
	uuid: string | null;
	name: string | null;
	/** e.g. a MIG / vGPU row without usable numbers */
	unsupported: boolean;
	/** A model on it is loading or stopping, or its status changed a moment ago */
	settling: boolean;
	total: number | null;
	used: number | null;
	free: number | null;
	memory_source: string;
	memory_age_ms: number | null;
	/** Other device names on the same physical GPU (e.g. CUDA0 and Vulkan0) */
	same_gpu_as: string[];
	models: ApiAdminGpuModel[];
}

/** A vendor-tool row that matched no device. */
export interface ApiAdminGpuUnmatched {
	tool: string;
	index: number | null;
	pci_bus_id: string | null;
	uuid: string | null;
	name: string | null;
	unsupported: boolean;
	total: number | null;
	used: number | null;
	free: number | null;
}

export interface ApiAdminInventoryDevice {
	name: string;
	backend: string;
	description: string;
	type: string;
	device_id?: string | null;
	pci_bus_id?: string | null;
	vendor?: string;
	memory_free: number | null;
	memory_total: number | null;
}

/** The device list the server measured itself (helper process). */
export interface ApiAdminInventory {
	ran: boolean;
	ok: boolean;
	source: string;
	error: string;
	message: string;
	time: string | null;
	time_ms: number | null;
	age_ms: number | null;
	duration_ms: number | null;
	devices: ApiAdminInventoryDevice[];
	backends: string[];
}

export interface ApiAdminProbeTool {
	tool: string;
	path: string;
	ok: boolean;
	error: string;
	message: string;
	age_ms: number | null;
	rows?: number;
}

/** Live memory reading by a vendor tool. */
export interface ApiAdminProbe {
	mode: string;
	source: string;
	ok: boolean;
	error: string;
	message: string;
	age_ms: number | null;
	stale: boolean;
	mapping: string;
	tools: ApiAdminProbeTool[];
}

export interface ApiAdminGpuBackend {
	state: string;
	message: string;
	libraries: string[];
	backends: { name: string; devices: number }[];
}

/** GET /admin/gpus and POST /admin/gpus/rescan. */
export interface ApiAdminGpus {
	build_devices: string[];
	gpu_backend: ApiAdminGpuBackend;
	inventory: ApiAdminInventory;
	probe: ApiAdminProbe;
	margin: number;
	settle_ms: number;
	gpus: ApiAdminGpu[];
	unmatched: ApiAdminGpuUnmatched[];
	warnings: string[];
}

export interface ApiAdminDiagnosticItem {
	id: string;
	label: string;
	status: string;
	value: unknown;
	hint: string;
}

/** GET /admin/diagnostics. */
export interface ApiAdminDiagnostics {
	generated_at: string;
	items: ApiAdminDiagnosticItem[];
}

/**
 * Editable copy of the per-model settings in the settings dialog: per key
 * the value to save (canonical string) or null for "not set here".
 */
export interface AdminModelSettingsDraft {
	values: Record<string, string | null>;
}

/** Per-key messages of the settings dialog, plus `form` for cross-field rules. */
export interface AdminModelSettingsValidation {
	errors: Record<string, string>;
	valid: boolean;
}

/** The value a key will have with the draft, and where it comes from. */
export interface AdminEffectiveSetting {
	value: string | null;
	origin: string;
	locked: boolean;
}

/** Memory a model needs on one target, split into the parts llama.cpp reports. */
export interface AdminMemoryParts {
	model: number;
	context: number;
	compute: number;
	total: number;
}

/** Fit of a model on one GPU. */
export interface AdminFitDevice {
	device: string;
	description: string;
	need: AdminMemoryParts;
	/** Free now, null when unknown */
	free: number | null;
	total: number | null;
	/** What the model uses there now and gets back on a restart */
	ownUsage: number;
	/** free + own usage, null when free is unknown */
	available: number | null;
	settling: boolean;
	/** vendor_tool / inventory / helper / none */
	memorySource: string;
	/** Age of an inventory reading in ms, null for a live one */
	memoryAgeMs: number | null;
	verdict: AdminFitVerdict;
}

/** Result of the fit check or of the instant formula. */
export interface AdminFitResult {
	source: AdminFitSource;
	verdict: AdminFitVerdict;
	devices: AdminFitDevice[];
	/** Memory on the CPU side (layers not offloaded, input embeddings, ...) */
	host: AdminMemoryParts | null;
	margin: number;
	/** Why the verdict is unknown, or other remarks */
	notes: string[];
	/** The dry run rejected the settings */
	error: { code: string; message: string; line: string } | null;
	checkedAt: string | null;
	nCtx: number | null;
}

/** GPU state the formula needs, per device name. */
export interface AdminFitGpuState {
	device: string;
	description: string;
	free: number | null;
	total: number | null;
	settling: boolean;
	memorySource: string;
	memoryAgeMs: number | null;
	/** Memory the model itself uses on this GPU now (0 when not running there) */
	ownUsage: number;
}

/** One coloured part of a GPU memory bar. */
export interface AdminGpuBarSegment {
	key: string;
	label: string;
	bytes: number;
	/** 0..1 of the total */
	fraction: number;
	kind: 'model' | 'pending' | 'other' | 'free';
	/** reported / measured / estimate for a model part */
	source: string | null;
	modelId: string | null;
}

/** One model of an import file, checked against the model in the dialog. */
export interface AdminImportField {
	key: string;
	/** Value now (effective), null = not set */
	current: string | null;
	/** Value from the file, canonical string */
	value: string;
	/** Ticked = loaded into the draft */
	selected: boolean;
	/** The field cannot be imported at all */
	skipped: boolean;
	issues: AdminImportIssue[];
}

/** What an import file would change, before anything is loaded into the draft. */
export interface AdminImportPreview {
	/** id the file was exported for */
	sourceId: string;
	exportedAt: string | null;
	/** The file belongs to another model, or arch / quant / file differ: confirm first */
	differentModel: boolean;
	differences: string[];
	fields: AdminImportField[];
	/** Keys that are not per-model settings: listed and ignored */
	unknownKeys: string[];
}

/** File written by "Export settings" and read by "Import settings". */
export interface AdminSettingsExportFile {
	format: string;
	version: number;
	exported_at: string;
	models: {
		id: string;
		about: { arch?: string; file?: string; quant?: string; n_ctx_train?: number };
		settings: Record<string, string | number | boolean>;
	}[];
}

/** Busy slots of a running model, from GET /slots. */
export interface AdminSlotsBusy {
	busy: number;
	total: number;
}
