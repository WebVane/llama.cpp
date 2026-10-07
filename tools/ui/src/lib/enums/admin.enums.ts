/**
 * Admin panel enums (provision-server, router mode with PROVISION_ADMIN=1).
 */

/**
 * Result of the one-shot GET /admin/status probe. The sidebar item and the
 * admin page are driven by it; there is no retry loop.
 */
export enum AdminProbeState {
	/** The probe answered 200 with an admin status body */
	AVAILABLE = 'available',
	CHECKING = 'checking',
	/** Not probed yet */
	IDLE = 'idle',
	/** 404 / 403 / network error: no admin API at this address */
	UNAVAILABLE = 'unavailable'
}

export enum AdminTab {
	MODELS = 'models',
	SETTINGS = 'settings'
}

/**
 * `error.type` values the admin API adds to the server's standard ones.
 */
export enum AdminErrorType {
	/** Another fit check is running on the server: try again in a moment */
	CHECK_BUSY = 'check_busy',
	DEFAULT_DEVICE_LOCKED = 'default_device_locked',
	/** The setting is fixed by MODEL_DEVICES or the router command line (`param` names it) */
	FIELD_LOCKED = 'field_locked',
	/** The client address is not on the /admin allow-list (the server's own or the nginx one) */
	FORBIDDEN = 'admin_forbidden',
	KEY_REQUIRED = 'admin_key_required',
	KEY_UNAVAILABLE = 'admin_key_unavailable',
	/** The per-model settings file could not be read (or is newer): nothing can be saved */
	MODEL_SETTINGS_READ_ONLY = 'model_settings_read_only',
	/** A device re-detection is already running */
	RESCAN_BUSY = 'rescan_busy',
	/** The model is running: the change needs `restart: true` */
	RESTART_REQUIRED = 'restart_required',
	/** The running model did not stop in time: nothing was saved */
	STOP_TIMEOUT = 'stop_timeout'
}

/**
 * Warning codes of POST /admin/models/download/check. Advisory only:
 * they never block a download.
 */
export enum AdminDownloadWarning {
	ALREADY_PRESENT = 'already_present',
	HF_UNREACHABLE = 'hf_unreachable',
	NO_GGUF_FOR_QUANT = 'no_gguf_for_quant',
	NOT_ENOUGH_SPACE = 'not_enough_space',
	OFFLINE_MODE = 'offline_mode',
	/** The repository holds a preset.ini: only that file is downloaded, no model appears */
	PRESET_REPOSITORY = 'preset_repository',
	QUANT_NOT_SPECIFIED = 'quant_not_specified',
	REPO_NOT_FOUND = 'repo_not_found',
	SIZE_UNKNOWN = 'size_unknown'
}

/**
 * How a download ended, decided after re-reading the model list: the feed
 * sends download_finished even when nothing was downloaded.
 */
export enum AdminDownloadOutcome {
	/** download_failed after the user's own Cancel */
	CANCELLED = 'cancelled',
	FAILED = 'failed',
	/** Finished, but no new cache model appeared */
	NOTHING = 'nothing',
	/** A new cache model appeared in the list */
	SUCCESS = 'success'
}

export enum AdminNoticeLevel {
	ERROR = 'error',
	INFO = 'info',
	SUCCESS = 'success',
	WARNING = 'warning'
}

/**
 * Row action that is waiting for the server (shown as a busy row).
 */
export enum AdminModelOperation {
	CANCELLING = 'cancelling',
	DELETING = 'deleting',
	STOPPING = 'stopping'
}

/**
 * `kind` of a directory reported by GET /admin/storage.
 */
export enum AdminStorageDirKind {
	CACHE = 'cache',
	MODELS_DIR = 'models_dir',
	STATE = 'state'
}

/**
 * Parts of the admin API a server announces in GET /admin/status `features`.
 * A browser can run a newer cached bundle against an older server: each part
 * of the page is shown only when its feature is listed.
 */
export enum AdminFeature {
	/** POST /admin/models/delete: delete models_dir models, remove a projector (stage 3) */
	DELETE = 'delete',
	DIAGNOSTICS = 'diagnostics',
	FIT_CHECK = 'fit_check',
	GPUS = 'gpus',
	MODEL_LOG = 'model_log',
	MODEL_SETTINGS = 'model_settings',
	/** /admin/models/upload*: GGUF upload into the models directory (stage 3) */
	UPLOAD = 'upload'
}

/**
 * Per-model settings whitelist. The API keys, the models.ini names and the
 * keys of the server's settings file are the same strings.
 */
export enum AdminSettingKey {
	CACHE_TYPE_K = 'cache-type-k',
	CACHE_TYPE_V = 'cache-type-v',
	CTX_SIZE = 'ctx-size',
	DEVICE = 'device',
	EMBEDDINGS = 'embeddings',
	FLASH_ATTN = 'flash-attn',
	KV_UNIFIED_PER_SLOT = 'kv-unified-per-slot',
	N_GPU_LAYERS = 'n-gpu-layers',
	PARALLEL = 'parallel',
	POOLING = 'pooling',
	RERANKING = 'reranking',
	UBATCH_SIZE = 'ubatch-size'
}

/** Where the value a model gets comes from. */
export enum AdminSettingOrigin {
	/** A per-model setting saved on this page */
	ADMIN = 'admin',
	/** Nothing sets it: the built-in default of the server */
	DEFAULT = 'default',
	/** "GPU for new models" (Settings tab) */
	DEFAULT_DEVICE = 'default_device',
	MODEL_DEVICES = 'model_devices',
	MODELS_INI = 'models_ini',
	ROUTER_CLI = 'router_cli'
}

/** What fixes a setting so it cannot be changed on this page. */
export enum AdminLockSource {
	MODEL_DEVICES = 'model_devices',
	ROUTER_CLI = 'router_cli'
}

/** Will the model fit into the free GPU memory (warns, never blocks). */
export enum AdminFitVerdict {
	/** The model would use no GPU */
	CPU_ONLY = 'cpu_only',
	FITS = 'fits',
	NO = 'no',
	/** Fits, but less than the safety margin stays free */
	TIGHT = 'tight',
	/** No memory reading, memory is settling, or the model facts are missing */
	UNKNOWN = 'unknown'
}

/** What a fit check answer with an error means. */
export enum AdminFitErrorKind {
	/** A GPU the settings name is not on this server */
	DEVICE_MISSING = 'device_missing',
	/** llama.cpp could not place the settings into the memory */
	NO_MEMORY = 'no_memory',
	/** The check itself did not run or finish (timeout, helper failed, unsupported build) */
	NOT_CHECKED = 'not_checked',
	/** llama.cpp refused the settings (invalid value, device, model file) */
	REJECTED = 'rejected'
}

/** Where a fit result comes from. */
export enum AdminFitSource {
	/** POST /admin/models/settings/check: the dry run of llama.cpp itself on the server */
	CHECK = 'check',
	/** The instant formula in the browser: a placeholder while the check runs or when it fails */
	FORMULA = 'formula'
}

/** How the context size is entered in the model settings dialog. */
export enum AdminContextMode {
	/** ctx-size = slots x N plus kv-unified-per-slot = N: every slot can use N tokens at once */
	PER_SLOT = 'per_slot',
	/** ctx-size only: one pool shared by the parallel slots */
	POOL = 'pool'
}

/** What a model is used for (the embeddings / pooling / reranking keys). */
export enum AdminModelRole {
	CHAT = 'chat',
	EMBEDDINGS = 'embeddings',
	RERANKING = 'reranking'
}

/** Two-phase commit state of saved per-model settings. */
export enum AdminSettingsPhase {
	/** The model reached "loaded" with these settings */
	COMMITTED = 'committed',
	/** The model exited before it was loaded */
	FAILED = 'failed',
	/** Saved, not tried yet */
	PENDING = 'pending',
	/** A start with these settings is running */
	TRIAL = 'trial'
}

/** Classified reason of a failed model start (GET /admin/models/log). */
export enum AdminFailureCode {
	INVALID_DEVICE = 'invalid_device',
	INVALID_SETTINGS = 'invalid_settings',
	MODEL_LOAD_FAILED = 'model_load_failed',
	NONE = 'none',
	OUT_OF_MEMORY = 'out_of_memory',
	UNKNOWN = 'unknown'
}

/** Where the total / used / free memory of a GPU card comes from. */
export enum AdminGpuMemorySource {
	/** The device inventory: measured at startup or at the last re-detect */
	INVENTORY = 'inventory',
	NONE = 'none',
	/** nvidia-smi / amd-smi / rocm-smi, live */
	VENDOR_TOOL = 'vendor_tool'
}

/** How vendor-tool rows were matched to the devices of the inventory. */
export enum AdminGpuMapping {
	MIXED = 'mixed',
	NONE = 'none',
	/** By PCI bus id, the reliable way */
	PCI = 'pci',
	/** Exactly one device and one vendor row */
	SINGLE = 'single',
	UNKNOWN = 'unknown'
}

/** Whether the server binary found GPUs at all. */
export enum AdminGpuBackendState {
	/** No GPU backend library and no GPU device: a CPU-only build */
	CPU_ONLY = 'cpu_only',
	/** A GPU backend is present but found no device (driver, permissions, container without GPUs) */
	NO_DEVICES = 'no_devices',
	OK = 'ok'
}

/** Where the memory of a model on a GPU card comes from. */
export enum AdminModelMemorySource {
	ESTIMATE = 'estimate',
	/** Per process from the vendor tool (host with NVIDIA only) */
	MEASURED = 'measured',
	/** The memory breakdown lines of the model itself */
	REPORTED = 'reported'
}

/** `probe.error` of GET /admin/gpus (live memory from a vendor tool). */
export enum AdminProbeError {
	DISABLED = 'disabled',
	FAILED = 'failed',
	NOT_FOUND = 'not_found',
	PARSE_ERROR = 'parse_error',
	PROBE_STUCK = 'probe_stuck',
	TIMEOUT = 'timeout',
	UNSUPPORTED = 'unsupported'
}

export enum AdminDiagnosticStatus {
	ERROR = 'error',
	INFO = 'info',
	OK = 'ok',
	WARNING = 'warning'
}

/** Progress of "Save and restart" / "Save and load" in the model settings dialog. */
export enum AdminRestartPhase {
	FAILED = 'failed',
	LOADING = 'loading',
	READY = 'ready',
	/** The POST is running (a running model is stopped first) */
	SAVING = 'saving',
	/** Saved; waiting for the new instance to report */
	STARTING = 'starting'
}

/** What a settings export file holds. */
export enum AdminExportMode {
	/** Effective values of every whitelisted key */
	ALL = 'all',
	/** Only the custom (saved) settings of the model */
	CUSTOM = 'custom'
}

/** Why a value of an imported settings file needs attention. */
export enum AdminImportIssue {
	/** Above the trained context of the model */
	CTX_ABOVE_TRAIN = 'ctx_above_train',
	/** The device is not on this server: the field starts unticked */
	DEVICE_UNKNOWN = 'device_unknown',
	/** Not a valid value for this key: skipped */
	INVALID_VALUE = 'invalid_value',
	/** Fixed by MODEL_DEVICES or the router command line: skipped */
	LOCKED = 'locked'
}

/*
 * Stage 3: GGUF upload and delete. These enums are imported by path
 * ('$lib/enums/admin.enums'), not through the enums barrel.
 */

/** Tab of the add-model dialog. */
export enum AdminAddModelTab {
	HUGGING_FACE = 'hf',
	UPLOAD = 'upload'
}

/** What a picked file is, from its GGUF header (or its name when the header is unreadable). */
export enum AdminUploadFileRole {
	MODEL = 'model',
	PROJECTOR = 'projector',
	/** Cannot be uploaded: not GGUF, an adapter, an imatrix file, a draft model */
	REJECTED = 'rejected',
	/** One part of a split model (name ends in -NNNNN-of-NNNNN.gguf) */
	SHARD = 'shard'
}

/** `state` of an upload session on the server. */
export enum AdminUploadState {
	CANCELLED = 'cancelled',
	/** The last byte arrived: hashing, classifying, the load check */
	CHECKING = 'checking',
	EXPIRED = 'expired',
	PUBLISHED = 'published',
	PUBLISHING = 'publishing',
	/** Complete and checked, waiting for the admin (see AdminUploadBlock) */
	READY = 'ready',
	RECEIVING = 'receiving',
	REJECTED = 'rejected'
}

/** `blocked_by` of a session in the ready state. */
export enum AdminUploadBlock {
	/** The checks could not run (helper timeout, crash): Retry runs them again */
	CHECK_FAILED = 'check_failed',
	/** "Publish automatically" was off */
	MANUAL = 'manual',
	/** Another model took the name meanwhile: rename, nothing is uploaded again */
	NAME_TAKEN = 'name_taken',
	PUBLISH_ERROR = 'publish_error',
	SETTINGS_INVALID = 'settings_invalid',
	/** The placement could not be saved (state directory not writable) */
	SETTINGS_NOT_SAVED = 'settings_not_saved',
	/** The SHA-256 typed in does not match the uploaded bytes */
	SHA256_MISMATCH = 'sha256_mismatch'
}

/** Where a job of the upload queue stands (the browser's view of a session). */
export enum AdminUploadJobPhase {
	CANCELLED = 'cancelled',
	CHECKING = 'checking',
	/** The session is being created on the server */
	CREATING = 'creating',
	EXPIRED = 'expired',
	/** The server refused to create the session, or something unrecoverable */
	FAILED = 'failed',
	PUBLISHED = 'published',
	PUBLISHING = 'publishing',
	/** Waiting for its turn: one model is uploaded at a time */
	QUEUED = 'queued',
	READY = 'ready',
	REJECTED = 'rejected',
	UPLOADING = 'uploading'
}

/** State of the chunk scheduler of one session. */
export enum AdminUploadSendState {
	/** The disk of the models directory is full: free space, then Resume */
	DISK_FULL = 'disk_full',
	/** Every file is on the server: the server checks it now */
	DONE = 'done',
	/** Something that a retry does not fix */
	FAILED = 'failed',
	/** The server refused a file (not GGUF, an adapter ...): drop it or cancel */
	FILE_REJECTED = 'file_rejected',
	/** The address may no longer use /admin (HTTP 403) */
	FORBIDDEN = 'forbidden',
	/** The session is finished on the server (cancelled, expired, rejected) */
	GONE = 'gone',
	IDLE = 'idle',
	/** The admin key was rejected: enter it again, then Resume */
	KEY_REQUIRED = 'key_required',
	/** A file was not picked again after a reload */
	NEEDS_FILE = 'needs_file',
	/** The bytes differ from the ones the server has: another file was picked */
	NOT_SAME_FILE = 'not_same_file',
	/** Another browser tab or client sends this upload right now: Take over */
	OTHER_CLIENT = 'other_client',
	/** Paused by the user */
	PAUSED = 'paused',
	RETRYING = 'retrying',
	/** The session is not receiving any more (checking, publishing, ready) */
	SERVER_BUSY = 'server_busy',
	/** HTTP 503 from the server while it starts */
	SERVER_RESTARTING = 'server_restarting',
	/** Failed for a long time: retried every minute while the page is open */
	UNREACHABLE = 'unreachable',
	UPLOADING = 'uploading',
	/** Comparing the first bytes of a picked file with the ones on the server */
	VERIFYING = 'verifying'
}
