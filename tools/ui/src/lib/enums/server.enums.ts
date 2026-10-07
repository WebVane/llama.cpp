/**
 * Server role enum - used for single/multi-model mode
 */
export enum ServerRole {
	/** Single model mode - server running with a specific model loaded */
	MODEL = 'model',
	/** Router mode - server managing multiple model instances */
	ROUTER = 'router'
}

/**
 * Model status enum - matches tools/server/server-models.h from C++ server
 * Used as the `value` field in the status object from /models endpoint
 */
export enum ServerModelStatus {
	/** Transient placeholder row: a download child just finished, the row goes away on the next reload */
	DOWNLOADED = 'downloaded',
	/** Transient placeholder row: a Hugging Face download is running under the name as requested */
	DOWNLOADING = 'downloading',
	FAILED = 'failed',
	LOADED = 'loaded',
	LOADING = 'loading',
	SLEEPING = 'sleeping',
	UNLOADED = 'unloaded'
}

/**
 * /models/sse event type enum - discriminates the records broadcast on the
 * model status feed in ROUTER mode. Matches the event names emitted by
 * tools/server/server-models.cpp from the C++ server.
 */
export enum ServerModelsSseEventType {
	DOWNLOAD_FAILED = 'download_failed',
	DOWNLOAD_FINISHED = 'download_finished',
	DOWNLOAD_PROGRESS = 'download_progress',
	MODEL_REMOVE = 'model_remove',
	MODEL_STATUS = 'model_status',
	MODELS_RELOAD = 'models_reload',
	STATUS_CHANGE = 'status_change',
	STATUS_UPDATE = 'status_update'
}

/**
 * Where the router found a model - matches server_model_source in
 * tools/server/server-models.h. Only `cache` models can be deleted.
 */
export enum ServerModelSource {
	CACHE = 'cache',
	MODELS_DIR = 'models_dir',
	PRESET = 'preset'
}
