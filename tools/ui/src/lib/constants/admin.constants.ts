/**
 * Admin panel constants (provision-server, router mode with PROVISION_ADMIN=1).
 */

import { STORAGE_APP_NAME } from './storage.constants';
import {
	AdminFailureCode,
	AdminFeature,
	AdminFitVerdict,
	AdminLockSource,
	AdminModelRole,
	AdminSettingKey,
	AdminSettingOrigin,
	AdminTab,
	ServerModelSource,
	ServerModelStatus
} from '$lib/enums';
import {
	AdminAuditActionGroup,
	AdminAuditKind,
	AdminAuditResult,
	AdminAuditResultGroup
} from '$lib/enums/admin.enums';

/** Header carrying the admin key. Only AdminService sends it. */
export const ADMIN_KEY_HEADER = 'X-Admin-Key';

/**
 * Client-side request timeouts in milliseconds. They only bound the HTTP
 * calls: the outcome of a load, unload, download or cancel is read from the
 * /models/sse feed.
 */
export const ADMIN_TIMEOUTS = {
	/** POST /models/unload on a download or on a model that is still loading */
	CANCEL_MS: 30_000,
	/** Pre-flight check, the server spends up to ~10 s talking to Hugging Face */
	DOWNLOAD_CHECK_MS: 20_000,
	/** POST /models validates the repository against Hugging Face before it answers */
	DOWNLOAD_START_MS: 60_000,
	/** The fit check helper has 30 s on the server, plus spawning and the GPU reading */
	FIT_CHECK_MS: 45_000,
	/** GET /admin/gpus waits at most ~5 s for a vendor tool */
	GPUS_MS: 15_000,
	/** POST /admin/gpus/rescan runs the inventory helper (up to 20 s on the server) */
	GPUS_RESCAN_MS: 40_000,
	/** POST /models/load may first wait for the least recently used model to unload */
	LOAD_REQUEST_MS: 60_000,
	/** Large models take minutes to load; after this the list is re-read instead of waiting on */
	LOAD_STATUS_MS: 15 * 60_000,
	/** A running model is stopped (stop timeout + 10 s), the presets reloaded, then started */
	MODEL_SETTINGS_SAVE_MS: 90_000,
	/** One-shot GET /admin/status probe; a firewall that drops packets must not hang the page */
	PROBE_MS: 8_000,
	/** DELETE /models stops a running instance before it removes the files */
	REMOVE_MS: 60_000,
	/** GET settings / storage */
	REQUEST_MS: 15_000,
	RESCAN_MS: 60_000,
	/** A changed device makes the server unload running models before it answers */
	SETTINGS_SAVE_MS: 60_000,
	/** GET /slots of a running model (public route, answered by the model itself) */
	SLOTS_MS: 10_000,
	UNLOAD_REQUEST_MS: 30_000,
	UNLOAD_STATUS_MS: 60_000
} as const;

/** Delay after the last edit in the settings dialog before the exact fit check runs. */
export const ADMIN_FIT_CHECK_DEBOUNCE_MS = 1_500;

/** A fit check answered "busy" (another one runs): retry this often, at most this many times. */
export const ADMIN_FIT_CHECK_BUSY_RETRY = { DELAY_MS: 2_000, MAX: 3 } as const;

/**
 * GPU cards: re-read this long after a model status change (memory settles
 * after a load or unload), and on a timer only while the Models tab and the
 * browser page are visible.
 */
export const ADMIN_GPU_REFRESH = { AFTER_STATUS_MS: 1_500, INTERVAL_MS: 15_000 } as const;

/** "Restart when idle": poll the busy slots this often, give up after this long. */
export const ADMIN_RESTART_WHEN_IDLE = { MAX_MS: 120_000, POLL_MS: 2_000 } as const;

/** No status from a restarted model after this long: "Still loading..." plus the output. */
export const ADMIN_SLOW_LOAD_MS = 120_000;

/** Safety margin of the fit verdict when the server does not send one (1 GiB, as llama.cpp fit). */
export const ADMIN_FIT_MARGIN_BYTES = 1024 * 1024 * 1024;

/**
 * Rough compute-buffer guess of the instant formula: bytes per element times
 * ubatch x (vocabulary + this many x embedding width), plus the attention
 * scores when flash attention is off. Only a placeholder until the exact
 * check by llama.cpp answers; never shown as a measured value.
 */
export const ADMIN_FIT_ROUGH_COMPUTE = { BYTES_PER_ELEMENT: 4, EMBD_FACTOR: 6 } as const;

/** llama.cpp rounds the context (KV cells) up to a multiple of this. */
export const ADMIN_CONTEXT_PAD = 256;

/** Slots the server creates when `parallel` is automatic (-1). */
export const ADMIN_PARALLEL_AUTO_SLOTS = 4;

/** Quick picks of the context size field (tokens). */
export const ADMIN_CONTEXT_QUICK_PICKS = [4096, 8192, 16384, 32768];

/** Choices of the parallel requests field besides "Auto" and a custom number. */
export const ADMIN_PARALLEL_CHOICES = [1, 2, 4, 8];

/** A model may be split across at most this many GPUs. */
export const ADMIN_MODEL_DEVICES_MAX = 4;

/** Export / import of the settings of one model. */
export const ADMIN_SETTINGS_FILE = {
	EXTENSION: '.settings.json',
	FORMAT: 'provision-llm-model-settings',
	/** Larger files are refused before they are parsed */
	MAX_BYTES: 64 * 1024,
	VERSION: 1
} as const;

/** Hint for NVIDIA images without nvidia-smi inside the container. */
export const ADMIN_NVIDIA_DOCKER_HINT =
	'In Docker, nvidia-smi is available only when NVIDIA_DRIVER_CAPABILITIES includes "utility" (for example compute,utility).';

/** `user/repo[:QUANT]`, the same pattern the server enforces on the pre-flight route. */
export const ADMIN_HF_MODEL_RE =
	/^[A-Za-z0-9][A-Za-z0-9._-]*\/[A-Za-z0-9][A-Za-z0-9._-]*(:[A-Za-z0-9._-]+)?$/;

/** Leading part of a pasted Hugging Face page URL, dropped before validation. */
export const ADMIN_HF_URL_PREFIX_RE = /^https?:\/\/(www\.)?(huggingface\.co|hf\.co)\//i;

/** One device name of the "GPU for new models" setting, e.g. CUDA0. */
export const ADMIN_DEVICE_NAME_RE = /^[A-Za-z][A-Za-z0-9_]*[0-9]+$/;

/** Device value that keeps new models on the CPU. */
export const ADMIN_DEVICE_NONE = 'none';

/** How many result lines stay visible above the model table. */
export const ADMIN_NOTICES_MAX = 5;

/** Delay that folds a burst of feed events into one settings / storage refresh. */
export const ADMIN_REFRESH_DEBOUNCE_MS = 400;

/**
 * The model list is re-read when the /models/sse feed reconnects, at most
 * once per this interval: a reconnect that comes sooner is served when the
 * interval ends (a feed that keeps dropping must not turn into fast polling).
 */
export const ADMIN_FEED_REOPEN_REFRESH_MIN_MS = 3_000;

/** Tabs of the admin page; a tab with a `feature` is shown only when the server lists it. */
export const ADMIN_TABS: { feature?: AdminFeature; id: AdminTab; label: string }[] = [
	{ id: AdminTab.MODELS, label: 'Models' },
	{ id: AdminTab.SETTINGS, label: 'Settings' },
	{ feature: AdminFeature.AUDIT, id: AdminTab.ACTIVITY, label: 'Activity' }
];

export const ADMIN_MODEL_SOURCE_LABELS: Record<ServerModelSource, string> = {
	[ServerModelSource.CACHE]: 'cache',
	[ServerModelSource.MODELS_DIR]: 'models dir',
	[ServerModelSource.PRESET]: 'preset'
};

export const ADMIN_MODEL_SOURCE_HINTS: Record<ServerModelSource, string> = {
	[ServerModelSource.CACHE]: 'Downloaded from Hugging Face into the model cache',
	[ServerModelSource.MODELS_DIR]: 'GGUF file found in the models directory',
	[ServerModelSource.PRESET]: 'Defined in the preset file (models.ini)'
};

export const ADMIN_MODEL_STATUS_LABELS: Record<ServerModelStatus, string> = {
	[ServerModelStatus.DOWNLOADED]: 'Downloaded',
	[ServerModelStatus.DOWNLOADING]: 'Downloading',
	[ServerModelStatus.FAILED]: 'Failed',
	[ServerModelStatus.LOADED]: 'Loaded',
	[ServerModelStatus.LOADING]: 'Loading',
	[ServerModelStatus.SLEEPING]: 'Sleeping',
	[ServerModelStatus.UNLOADED]: 'Unloaded'
};

/** Shown permanently on the Models tab. */
export const ADMIN_PUBLIC_API_NOTE =
	"Loading, unloading, downloading and deleting models uses the server's public model API; only the settings on this page are protected by the admin key.";

/** Shown when the probe failed (404 / 403 / network). */
export const ADMIN_UNAVAILABLE_TITLE = 'Admin API is not available from this address';

/** Field names of the per-model settings, as shown in the dialog and in confirmations. */
export const ADMIN_SETTING_LABELS: Record<AdminSettingKey, string> = {
	[AdminSettingKey.CACHE_TYPE_K]: 'KV cache precision (K)',
	[AdminSettingKey.CACHE_TYPE_V]: 'KV cache precision (V)',
	[AdminSettingKey.CTX_SIZE]: 'Context size',
	[AdminSettingKey.DEVICE]: 'GPU',
	[AdminSettingKey.EMBEDDINGS]: 'Embeddings',
	[AdminSettingKey.FLASH_ATTN]: 'Flash attention',
	[AdminSettingKey.KV_UNIFIED_PER_SLOT]: 'Context per request',
	[AdminSettingKey.N_GPU_LAYERS]: 'GPU layers',
	[AdminSettingKey.PARALLEL]: 'Parallel requests',
	[AdminSettingKey.POOLING]: 'Pooling',
	[AdminSettingKey.RERANKING]: 'Reranking',
	[AdminSettingKey.UBATCH_SIZE]: 'Micro-batch (ubatch)'
};

/** Every whitelisted key, in the order of the dialog. */
export const ADMIN_SETTING_KEYS: AdminSettingKey[] = [
	AdminSettingKey.DEVICE,
	AdminSettingKey.CTX_SIZE,
	AdminSettingKey.KV_UNIFIED_PER_SLOT,
	AdminSettingKey.PARALLEL,
	AdminSettingKey.N_GPU_LAYERS,
	AdminSettingKey.FLASH_ATTN,
	AdminSettingKey.CACHE_TYPE_K,
	AdminSettingKey.CACHE_TYPE_V,
	AdminSettingKey.UBATCH_SIZE,
	AdminSettingKey.EMBEDDINGS,
	AdminSettingKey.POOLING,
	AdminSettingKey.RERANKING
];

/** Keys whose values are whole numbers (exported as JSON numbers). */
export const ADMIN_SETTING_INT_KEYS: AdminSettingKey[] = [
	AdminSettingKey.CTX_SIZE,
	AdminSettingKey.KV_UNIFIED_PER_SLOT,
	AdminSettingKey.PARALLEL,
	AdminSettingKey.UBATCH_SIZE
];

/** Keys whose values are true / false (exported as JSON booleans). */
export const ADMIN_SETTING_BOOL_KEYS: AdminSettingKey[] = [
	AdminSettingKey.EMBEDDINGS,
	AdminSettingKey.RERANKING
];

/** Short source words of a value, e.g. "1024 from models.ini". */
export const ADMIN_ORIGIN_LABELS: Record<AdminSettingOrigin, string> = {
	[AdminSettingOrigin.ADMIN]: 'custom',
	[AdminSettingOrigin.DEFAULT]: 'built-in default',
	[AdminSettingOrigin.DEFAULT_DEVICE]: 'GPU for new models',
	[AdminSettingOrigin.MODEL_DEVICES]: 'MODEL_DEVICES',
	[AdminSettingOrigin.MODELS_INI]: 'models.ini',
	[AdminSettingOrigin.ROUTER_CLI]: 'server command line'
};

/** Why a field cannot be changed here. */
export const ADMIN_LOCK_TEXTS: Record<AdminLockSource, string> = {
	[AdminLockSource.MODEL_DEVICES]:
		'Set by the MODEL_DEVICES environment variable of the deployment. Change it there and restart the server.',
	[AdminLockSource.ROUTER_CLI]:
		'Set on the server command line, which applies to every model. Change it there.'
};

export const ADMIN_FIT_VERDICT_LABELS: Record<AdminFitVerdict, string> = {
	[AdminFitVerdict.CPU_ONLY]: 'Runs on the CPU',
	[AdminFitVerdict.FITS]: 'Fits',
	[AdminFitVerdict.NO]: 'Probably does not fit',
	[AdminFitVerdict.TIGHT]: 'Fits, but tight',
	[AdminFitVerdict.UNKNOWN]: 'Memory unknown'
};

/** Short labels of a failed start, for the table row. */
export const ADMIN_FAILURE_LABELS: Record<AdminFailureCode, string> = {
	[AdminFailureCode.INVALID_DEVICE]: 'Device not found',
	[AdminFailureCode.INVALID_SETTINGS]: 'Invalid settings',
	[AdminFailureCode.MODEL_LOAD_FAILED]: 'Model file could not be loaded',
	[AdminFailureCode.NONE]: 'Stopped',
	[AdminFailureCode.OUT_OF_MEMORY]: 'Out of memory',
	[AdminFailureCode.UNKNOWN]: 'Failed to start'
};

export const ADMIN_MODEL_ROLE_LABELS: Record<AdminModelRole, string> = {
	[AdminModelRole.CHAT]: 'Chat / completion',
	[AdminModelRole.EMBEDDINGS]: 'Embeddings',
	[AdminModelRole.RERANKING]: 'Reranking'
};

/** Values of the pooling key with what they mean. */
export const ADMIN_POOLING_LABELS: Record<string, string> = {
	cls: 'cls (first token)',
	last: 'last (last token)',
	mean: 'mean (average of all tokens)',
	none: 'none (one vector per token)',
	rank: 'rank (reranking score)'
};

export const ADMIN_FLASH_ATTN_LABELS: Record<string, string> = {
	auto: 'Auto (on where the GPU supports it)',
	off: 'Off',
	on: 'On'
};

export const ADMIN_KV_PRECISION_LABELS: Record<string, string> = {
	bf16: 'bf16 (16-bit)',
	f16: 'f16 (16-bit, default)',
	q4_0: 'q4_0 (about a quarter of the memory)',
	q8_0: 'q8_0 (about half the memory)'
};

/** Help text of the KV precision control, shown on every server (no host gate). */
export const ADMIN_KV_PRECISION_HELP =
	'Applies to both the K and the V cache. q8_0 halves KV memory with a small quality loss; q4_0 saves more and costs more quality. On some GPUs prompt processing is slower with a quantized cache. A quantized cache needs flash attention on or auto.';

/**
 * The model may not start with new settings: what that means for its users.
 * The server's circuit breaker reverts settings saved here after 2 failed
 * starts (ADMIN_MAX_FAILURES); a models.ini value that fails is not reverted.
 */
export const ADMIN_RESTART_RISK_NOTE =
	'If it does not start, requests to this model fail. After 2 failed starts with settings saved here, the server puts back the settings that last worked by itself.';

/*
 * Stage 3: GGUF upload into the models directory, delete of models_dir models.
 */

const KIB = 1024;
const MIB = 1024 * 1024;

/**
 * Chunked upload. Every chunk is one POST with a raw body; the server keeps
 * the offset, so a retry or a resume is always safe (the part of a chunk the
 * server already has is compared byte for byte, the rest is appended).
 */
export const ADMIN_UPLOAD = {
	/** Retry delays grow from MIN to MAX (doubling) while the server cannot be reached */
	BACKOFF_MAX_MS: 30_000,
	BACKOFF_MIN_MS: 1_000,
	/** Wait this long after a 409 chunk_in_progress before the session is read again */
	BUSY_RETRY_MS: 1_000,
	/** A success faster than this doubles the chunk size (up to the ceiling) */
	CHUNK_GROW_BELOW_MS: 2_000,
	/** A chunk slower than this halves the chunk size (down to the floor) */
	CHUNK_SHRINK_ABOVE_MS: 15_000,
	/** A damaged chunk (crc_mismatch) is sent again at most this many times in a row */
	CRC_RETRIES: 3,
	/** The server's resume identity: CRC32 of the first MiB of every file */
	HEAD_CRC_BYTES: MIB,
	/** The local GGUF reader reads at most this much of a file (the server's own bound) */
	HEADER_MAX_BYTES: 64 * MIB,
	/** First read of the local GGUF reader; it reads more when the metadata is longer */
	HEADER_READ_BYTES: MIB,
	/** Network errors in a row on the first chunk of a size before the size steps down */
	NET_FAILS_BEFORE_STEP_DOWN: 2,
	/** Another client wrote this recently (the server's rule): "In another tab" */
	OTHER_CLIENT_MS: 30_000,
	/** Bytes sent again before the server's offset after a reload or a network error */
	OVERLAP_BYTES: MIB,
	/** Session poll while the server checks or publishes */
	POLL_CHECKING_MS: 1_000,
	/** List poll while a queued model waits for a free upload slot (the server's max_sessions) */
	POLL_SLOT_MS: 10_000,
	/** Session poll while chunks are sent: the header checks of the files */
	POLL_UPLOADING_MS: 4_000,
	/** No answer this long after the whole chunk was sent: treated as a network error */
	RESPONSE_TIMEOUT_MS: 120_000,
	/** Window of the speed / ETA figures */
	SPEED_WINDOW_MS: 10_000,
	/** No upload progress this long while a chunk is sent: treated as a network error */
	STALL_TIMEOUT_MS: 60_000,
	/**
	 * Chunk sizes tried when a proxy refuses the request size (HTML 413) or drops
	 * the connection on the first chunk of a size. The floor leaves headroom under
	 * "1 MB" limits that count decimal bytes or include the headers.
	 */
	STEP_LADDER: [8 * MIB, 960 * KIB, 480 * KIB, 240 * KIB],
	/** Failing this long without a break: "server unreachable", then a retry every minute */
	UNREACHABLE_AFTER_MS: 30 * 60_000,
	UNREACHABLE_RETRY_MS: 60_000,
	/** A 500 write_failed is retried this many times before the upload pauses */
	WRITE_FAILED_RETRIES: 3
} as const;

/**
 * Routes of the upload and delete (stage 3). Kept here, not in the upstream
 * api-endpoints file: like every /admin route they take the X-Admin-Key header.
 */
export const API_ADMIN_UPLOAD = {
	/** POST: cancel a session (frees its data) */
	CANCEL: '/admin/models/upload/cancel',
	/** POST ?id=&file=&offset=[&takeover=1][&verify=1]: one raw chunk */
	CHUNK: '/admin/models/upload/chunk',
	/** POST {model, dry_run?, stop?, remove_settings?, projector_only?}: delete a models_dir model */
	DELETE: '/admin/models/delete',
	/** POST: publish a ready session (also Retry of the checks) */
	PUBLISH: '/admin/models/upload/publish',
	/** POST: name, settings, expected SHA-256, drop a file */
	UPDATE: '/admin/models/upload/update',
	/** GET: availability, limits, sessions; GET ?id=: one session; POST: create (or dry run) */
	UPLOAD: '/admin/models/upload'
} as const;

/** Client-side timeouts of the JSON upload / delete routes. */
export const ADMIN_UPLOAD_TIMEOUTS = {
	CANCEL_MS: 60_000,
	CREATE_MS: 30_000,
	/** A running model is stopped (stop timeout + 10 s), then the list is reloaded */
	DELETE_MS: 150_000,
	DELETE_PREVIEW_MS: 30_000,
	GET_MS: 15_000,
	PUBLISH_MS: 30_000,
	UPDATE_MS: 30_000
} as const;

/** sessionStorage keys of the upload: survive a reload of the tab, differ between tabs. */
export const ADMIN_UPLOAD_STORAGE = {
	/** The chunk size a proxy allowed, for the rest of the browser session */
	CHUNK_CEILING: `${STORAGE_APP_NAME}.adminUploadChunkCeiling`,
	/** X-Upload-Client: the server tells this tab from another one by it */
	CLIENT_ID: `${STORAGE_APP_NAME}.adminUploadClient`
} as const;

/** Headers of a chunk request. */
export const ADMIN_UPLOAD_HEADERS = {
	CLIENT: 'X-Upload-Client',
	CRC32: 'X-Chunk-CRC32'
} as const;

/** File names the models_dir scanner takes as draft models; they cannot be uploaded yet. */
export const ADMIN_UPLOAD_DRAFT_PREFIXES = ['mtp-', 'dspark-', 'dflash-', 'eagle3-'];

/** `<prefix>-NNNNN-of-NNNNN.gguf`, the shard names llama.cpp loads (case-insensitive). */
export const ADMIN_UPLOAD_SHARD_RE = /^(.*)-(\d{5})-of-(\d{5})\.gguf$/i;

/** The id rule of the server: what a published model is listed as. */
export const ADMIN_UPLOAD_NAME_MAX = 128;

/** Windows device names; the server refuses them as model names. */
export const ADMIN_UPLOAD_RESERVED_NAMES = [
	'con',
	'prn',
	'aux',
	'nul',
	'com1',
	'com2',
	'com3',
	'com4',
	'com5',
	'com6',
	'com7',
	'com8',
	'com9',
	'lpt1',
	'lpt2',
	'lpt3',
	'lpt4',
	'lpt5',
	'lpt6',
	'lpt7',
	'lpt8',
	'lpt9'
];

/** Shown when the chunk size had to step down: what an operator adds to an own nginx. */
export const ADMIN_UPLOAD_NGINX_SNIPPET = `location ^~ /admin/ {
    client_max_body_size 32m;
    proxy_request_buffering off;
    # ...the existing proxy_pass and allow / deny lines
}`;

/** llama.cpp's LLAMA_FTYPE_* values (general.file_type) as quantization labels. */
export const ADMIN_GGUF_FILE_TYPES: Record<number, string> = {
	0: 'F32',
	1: 'F16',
	2: 'Q4_0',
	3: 'Q4_1',
	7: 'Q8_0',
	8: 'Q5_0',
	9: 'Q5_1',
	10: 'Q2_K',
	11: 'Q3_K_S',
	12: 'Q3_K_M',
	13: 'Q3_K_L',
	14: 'Q4_K_S',
	15: 'Q4_K_M',
	16: 'Q5_K_S',
	17: 'Q5_K_M',
	18: 'Q6_K',
	19: 'IQ2_XXS',
	20: 'IQ2_XS',
	21: 'Q2_K_S',
	22: 'IQ3_XS',
	23: 'IQ3_XXS',
	24: 'IQ1_S',
	25: 'IQ4_NL',
	26: 'IQ3_S',
	27: 'IQ3_M',
	28: 'IQ2_S',
	29: 'IQ2_M',
	30: 'IQ4_XS',
	31: 'IQ1_M',
	32: 'BF16',
	36: 'TQ1_0',
	37: 'TQ2_0',
	38: 'MXFP4_MOE',
	39: 'NVFP4',
	40: 'Q1_0',
	41: 'Q2_0'
};

/*
 * Storage root: in Docker every writable model directory lives under one
 * root (hf/ = the model cache, local/ = the models directory), so one mount
 * keeps everything across a recreate.
 */

/** The root the Docker entrypoint creates when nothing is mounted there. */
export const ADMIN_MODELS_RW_ROOT = '/models-rw';

/** Warning of GET /admin/status: the model cache or the models directory is not persistent. */
export const ADMIN_STORAGE_EPHEMERAL_WARNING = 'models_storage_ephemeral';

/*
 * Stage 3, milestone M3: the audit log of admin actions (Activity tab).
 */

/** GET ?before=&limit=&kind=&action=&result=[&format=jsonl]: the audit log, newest first. */
export const API_ADMIN_AUDIT = '/admin/audit';

export const ADMIN_AUDIT = {
	/** Name of the downloaded file when the answer does not name one */
	DOWNLOAD_NAME: 'provision-admin-audit.jsonl',
	/** The server's largest `limit` */
	LIMIT_MAX: 500,
	/** Warning of GET /admin/status: the audit log is held in memory only */
	NOT_PERSISTED_WARNING: 'audit_not_persisted',
	/** Records per request ("Load more" asks for the next ones) */
	PAGE_SIZE: 100,
	/** Changed keys named in the one-line summary of a record before "+N more" */
	SUMMARY_CHANGES_MAX: 3,
	/**
	 * Width of the Activity panel (rem) below which the records are cards:
	 * the four fixed columns take 39rem, the Action column keeps 17rem. Also
	 * the min-width of the table (min-w-[56rem] in AdminActivityTab).
	 */
	TABLE_MIN_REM: 56,
	/** Panel width (rem) from which Actor and Target get their full width */
	TABLE_WIDE_REM: 64
} as const;

/** Client-side timeouts of the audit routes. */
export const ADMIN_AUDIT_TIMEOUTS = {
	/** The raw log of the three files (up to about 24 MiB) */
	DOWNLOAD_MS: 120_000,
	LIST_MS: 20_000
} as const;

/** Names of the kinds in the filter and the details. */
export const ADMIN_AUDIT_KIND_LABELS: Record<AdminAuditKind, string> = {
	[AdminAuditKind.CHANGE]: 'Changes',
	[AdminAuditKind.OBSERVED]: 'Public API',
	[AdminAuditKind.REFUSAL]: 'Refusals',
	[AdminAuditKind.SERVER]: 'Server'
};

/** Order of the kind filter. */
export const ADMIN_AUDIT_KIND_ORDER: AdminAuditKind[] = [
	AdminAuditKind.CHANGE,
	AdminAuditKind.SERVER,
	AdminAuditKind.OBSERVED,
	AdminAuditKind.REFUSAL
];

/** What each kind holds (tooltips of the kind filter). */
export const ADMIN_AUDIT_KIND_HINTS: Record<AdminAuditKind, string> = {
	[AdminAuditKind.CHANGE]:
		'Changes made on this page or through the /admin routes with the admin key',
	[AdminAuditKind.OBSERVED]:
		'Effects of the public model API (downloads, removed models, failed starts); who caused them is not known',
	[AdminAuditKind.REFUSAL]:
		'Requests to /admin refused for a missing or wrong key or a refused address, merged per minute',
	[AdminAuditKind.SERVER]:
		'What the server did by itself: settings reverted after failed starts, expired uploads, log started'
};

export const ADMIN_AUDIT_RESULT_LABELS: Record<AdminAuditResult, string> = {
	[AdminAuditResult.ERROR]: 'Error',
	[AdminAuditResult.FAILED]: 'Failed',
	[AdminAuditResult.OK]: 'OK',
	[AdminAuditResult.REFUSED]: 'Refused',
	[AdminAuditResult.REJECTED]: 'Rejected'
};

/** The action filter: each group asks the server for these action names (a name also matches `name.*`). */
export const ADMIN_AUDIT_ACTION_GROUPS: Record<
	AdminAuditActionGroup,
	{ actions: string[]; label: string }
> = {
	[AdminAuditActionGroup.ACCESS]: { actions: ['access'], label: 'Refused requests' },
	[AdminAuditActionGroup.ALL]: { actions: [], label: 'All actions' },
	[AdminAuditActionGroup.AUDIT_LOG]: { actions: ['audit'], label: 'Audit log' },
	[AdminAuditActionGroup.DELETES]: {
		actions: ['model.delete', 'model.remove_projector', 'model.removed'],
		label: 'Deletes'
	},
	[AdminAuditActionGroup.DOWNLOADS]: { actions: ['download'], label: 'Downloads' },
	[AdminAuditActionGroup.GPUS]: { actions: ['gpus'], label: 'GPU re-detection' },
	[AdminAuditActionGroup.SETTINGS]: { actions: ['settings', 'model_settings'], label: 'Settings' },
	[AdminAuditActionGroup.STARTS]: { actions: ['model.start_failed'], label: 'Failed starts' },
	[AdminAuditActionGroup.UPLOADS]: { actions: ['upload'], label: 'Uploads' }
};

/** Order of the action filter. */
export const ADMIN_AUDIT_ACTION_GROUP_ORDER: AdminAuditActionGroup[] = [
	AdminAuditActionGroup.ALL,
	AdminAuditActionGroup.SETTINGS,
	AdminAuditActionGroup.UPLOADS,
	AdminAuditActionGroup.DELETES,
	AdminAuditActionGroup.DOWNLOADS,
	AdminAuditActionGroup.STARTS,
	AdminAuditActionGroup.GPUS,
	AdminAuditActionGroup.ACCESS,
	AdminAuditActionGroup.AUDIT_LOG
];

/** The result filter: each group asks the server for these results. */
export const ADMIN_AUDIT_RESULT_GROUPS: Record<
	AdminAuditResultGroup,
	{ label: string; results: AdminAuditResult[] }
> = {
	[AdminAuditResultGroup.ALL]: { label: 'Any result', results: [] },
	[AdminAuditResultGroup.OK]: { label: 'OK', results: [AdminAuditResult.OK] },
	[AdminAuditResultGroup.PROBLEMS]: {
		label: 'Errors and failures',
		results: [AdminAuditResult.ERROR, AdminAuditResult.FAILED, AdminAuditResult.REJECTED]
	},
	[AdminAuditResultGroup.REFUSED]: { label: 'Refused', results: [AdminAuditResult.REFUSED] }
};

/** Order of the result filter. */
export const ADMIN_AUDIT_RESULT_GROUP_ORDER: AdminAuditResultGroup[] = [
	AdminAuditResultGroup.ALL,
	AdminAuditResultGroup.OK,
	AdminAuditResultGroup.PROBLEMS,
	AdminAuditResultGroup.REFUSED
];

/** What an action is, in words; an action not listed here shows its name. */
export const ADMIN_AUDIT_ACTION_LABELS: Record<string, string> = {
	'access.refused': 'Request refused',
	'access.refused_more': 'More refused requests',
	'audit.persisted_again': 'Audit log written again',
	'audit.started': 'Audit log started',
	'download.failed': 'Download failed',
	'download.finished': 'Download finished',
	'gpus.rescan': 'GPUs re-detected',
	'model.delete': 'Model files deleted',
	'model.remove_projector': 'Projector removed',
	'model.removed': 'Model removed',
	'model.start_failed': 'Model failed to start',
	'model_settings.reset': 'Model settings reset',
	'model_settings.reverted': 'Model settings reverted',
	'model_settings.reverted_at_startup': 'Model settings reverted at startup',
	'model_settings.update': 'Model settings changed',
	'observed.more': 'More public API events',
	'settings.update': 'Settings changed',
	'upload.cancel': 'Upload cancelled',
	'upload.create': 'Upload started',
	'upload.expired': 'Upload expired',
	'upload.published': 'Upload published',
	'upload.rejected': 'Upload rejected',
	'upload.resumed_after_restart': 'Upload resumed after a restart',
	'upload.swept': 'Abandoned upload removed',
	'upload.takeover': 'Upload taken over'
};

/** A change route that answered with an error: "<attempt> failed". */
export const ADMIN_AUDIT_ATTEMPT_LABELS: Record<string, string> = {
	'gpus.rescan': 'GPU re-detection',
	'model.delete': 'Delete of the model files',
	'model.remove_projector': 'Projector removal',
	'model_settings.reset': 'Model settings reset',
	'model_settings.update': 'Model settings change',
	'settings.update': 'Settings change',
	'upload.cancel': 'Upload cancel',
	'upload.create': 'Upload start',
	'upload.takeover': 'Upload takeover'
};

/** `details.reason` of a refused request. */
export const ADMIN_AUDIT_REFUSAL_REASONS: Record<string, string> = {
	allow_list_locked: 'the allow-list is invalid, /admin is locked',
	no_key: 'no admin key',
	not_allowed: 'address not allowed',
	wrong_key: 'wrong admin key'
};

/** `details.to` of reverted settings. */
export const ADMIN_AUDIT_REVERT_TARGETS: Record<string, string> = {
	last_good: 'the settings that last worked',
	models_ini: 'the models.ini values'
};

/** Names of the detail fields; a field not listed here shows its key. */
export const ADMIN_AUDIT_DETAIL_LABELS: Record<string, string> = {
	arch: 'Architecture',
	bytes: 'Size',
	changed: 'Changed anything',
	clients: 'Clients',
	code: 'Code',
	count: 'Times',
	devices: 'Devices found',
	error: 'Error',
	exit_code: 'Exit code',
	failures: 'Failed starts',
	file: 'File (index)',
	files: 'Files',
	first: 'First',
	folder: 'Folder',
	folder_removed: 'Folder removed',
	foreign: 'Of another server',
	id: 'Upload session',
	items: 'Items',
	keys: 'Fields sent',
	last: 'Last',
	layout: 'Layout',
	load_failed: 'Load failed',
	lost: 'Records lost',
	max_bytes: 'Rotation size',
	model_listed: 'Model listed',
	name: 'Name',
	not_persisted_since: 'Not written since',
	note: 'Note',
	paths: 'Files (in the models directory)',
	persisted: 'Written to disk',
	phase: 'Phase',
	pin_removed: 'Pin removed',
	probe: 'Status probe',
	projector_only: 'Projector only',
	reason: 'Reason',
	reload_failed: 'Model list reload failed',
	restart: 'Restart asked for',
	restarted: 'Restarted',
	sample: 'Some of the clients',
	settings: 'Placement',
	settings_removed: 'Saved settings removed',
	settings_reverted: 'Settings reverted',
	sha256: 'SHA-256',
	state: 'State',
	status: 'HTTP status',
	stopped: 'Stopped first',
	store: 'Store',
	stored: 'Saved entry rewritten',
	text: 'Text',
	to: 'Reverted to',
	truncated: 'Cut to fit',
	version: 'Version',
	was_running: 'Was running'
};
