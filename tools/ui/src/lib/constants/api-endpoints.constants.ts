export const API_MODELS = {
	/** POST {model}: start a Hugging Face download (router mode) */
	DOWNLOAD: '/models',
	LIST: '/v1/models',
	LOAD: '/models/load',
	/** GET: re-scan the model sources (cache, models dir, preset file), then list (router mode) */
	RELOAD: '/v1/models?reload=1',
	/** DELETE ?model=<id>: remove a cache model from disk (router mode) */
	REMOVE: '/models',
	SSE: '/models/sse',
	UNLOAD: '/models/unload'
};

// chat completion routes, the control route drives realtime inference (e.g. end reasoning)
export const API_CHAT = {
	COMPLETIONS: './v1/chat/completions',
	CONTROL: './v1/chat/completions/control'
};

// slot introspection, requires the --slots flag on the server
export const API_SLOTS = {
	LIST: './slots'
};

export const API_TOOLS = {
	EXECUTE: '/tools',
	LIST: '/tools'
};

// resumable stream routes, the conv::model identity travels as the conv_id query param
// because model names can contain slashes that a path segment cannot carry
// resume retry cadence while the owning model is still loading (server answers 503)
export const STREAM_RESUME_RETRY_MS = 2000;

export const API_STREAM = {
	BASE: './v1/stream',
	LOOKUP: './v1/streams/lookup'
};

// query params for the resumable stream routes
export const STREAM_QUERY_PARAMS = {
	CONV_ID: 'conv_id',
	FROM: 'from'
} as const;

/** CORS proxy endpoint path */
export const CORS_PROXY_ENDPOINT = '/cors-proxy';

// license subsystem (provision-server): read-only status + online activation
export const API_LICENSE = {
	ACTIVATE: '/license/activate',
	STATUS: '/license/status'
};

// admin panel (provision-server, router mode with PROVISION_ADMIN=1): the only
// routes that take the X-Admin-Key header; everything else on the admin page
// goes through the public model routes above
export const API_ADMIN = {
	/** GET: deployment, GPU backend, vendor tool, state dir... as ok / warning / error items */
	DIAGNOSTICS: '/admin/diagnostics',
	DOWNLOAD_CHECK: '/admin/models/download/check',
	/** GET [?refresh=1]: GPU cards with memory and the models placed on them */
	GPUS: '/admin/gpus',
	/** POST: run the device inventory again ("Re-detect GPUs") */
	GPUS_RESCAN: '/admin/gpus/rescan',
	/** GET ?model=<id>: output tail of the last instance, failure reason, last failure */
	MODEL_LOG: '/admin/models/log',
	/** GET [?model=<id>] / POST: per-model settings */
	MODEL_SETTINGS: '/admin/models/settings',
	/** POST: exact fit of draft settings, a dry run by llama.cpp in a helper process */
	MODEL_SETTINGS_CHECK: '/admin/models/settings/check',
	SETTINGS: '/admin/settings',
	STATUS: '/admin/status',
	STORAGE: '/admin/storage'
};
