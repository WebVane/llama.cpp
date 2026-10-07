/**
 * Builders of stage-2 admin API shapes for the unit tests. The values are
 * taken from real answers of the CPU build (stories260K: 5 layers, 4 KV
 * heads, 640 bytes of f16 KV per token).
 */

import { ADMIN_SETTING_KEYS } from '$lib/constants';
import { ServerModelSource } from '$lib/enums';
import type {
	ApiAdminGpu,
	ApiAdminGpus,
	ApiAdminModelFacts,
	ApiAdminModelSettingsRow,
	ApiAdminSettingField,
	ApiAdminSettingSchema
} from '$lib/types/admin';

export const GIB = 1024 * 1024 * 1024;
export const MIB = 1024 * 1024;

/** Real schema of GET /admin/models/settings. */
export const SCHEMA: ApiAdminSettingSchema[] = [
	{ default: null, group: 'placement', key: 'device', type: 'device' },
	{ default: null, group: 'placement', key: 'ctx-size', max: 1048576, min: 256, type: 'int' },
	{
		default: null,
		group: 'placement',
		key: 'kv-unified-per-slot',
		max: 1048576,
		min: 256,
		type: 'int'
	},
	{ auto: -1, default: '-1', group: 'placement', key: 'parallel', max: 16, min: 1, type: 'int' },
	{
		default: 'auto',
		group: 'advanced',
		key: 'n-gpu-layers',
		max: 999,
		min: 0,
		type: 'ngl',
		values: ['auto', 'all']
	},
	{
		default: 'auto',
		group: 'advanced',
		key: 'flash-attn',
		type: 'enum',
		values: ['on', 'off', 'auto']
	},
	{
		default: 'f16',
		group: 'advanced',
		key: 'cache-type-k',
		pair: 'cache-type-v',
		type: 'enum',
		values: ['f16', 'bf16', 'q8_0', 'q4_0']
	},
	{
		default: 'f16',
		group: 'advanced',
		key: 'cache-type-v',
		pair: 'cache-type-k',
		type: 'enum',
		values: ['f16', 'bf16', 'q8_0', 'q4_0']
	},
	{ default: '512', group: 'advanced', key: 'ubatch-size', max: 8192, min: 32, type: 'int' },
	{ default: 'false', group: 'role', key: 'embeddings', type: 'bool' },
	{
		default: null,
		group: 'role',
		key: 'pooling',
		type: 'enum',
		values: ['none', 'mean', 'cls', 'last', 'rank']
	},
	{ default: 'false', group: 'role', key: 'reranking', type: 'bool' }
];

const DEFAULTS: Record<string, string | null> = Object.fromEntries(
	SCHEMA.map((entry) => [entry.key, entry.default])
);

/** stories260K as the server reads it. */
export function makeFacts(overrides: Partial<ApiAdminModelFacts> = {}): ApiAdminModelFacts {
	return {
		arch: 'llama',
		causal: true,
		d_k: 8,
		d_v: 8,
		error: '',
		file_bytes: 1185376,
		files: [{ bytes: 1185376, name: 'tinyA.gguf' }],
		kv_bytes_per_token: { bf16: 640, f16: 640, q4_0: 180, q8_0: 340 },
		kv_estimate: 'exact',
		kv_note: '',
		layer_bytes: [181760, 181760, 181760, 181760, 181760],
		mmproj_bytes: 0,
		n_ctx_train: 2048,
		n_embd: 64,
		n_head: 8,
		n_head_kv: 4,
		n_head_kv_layers: null,
		n_layer: 5,
		n_vocab: 512,
		other_bytes: 0,
		output_bytes: 131328,
		pooling: null,
		quant: 'F32',
		sliding_window: 0,
		tensor_bytes: 1171200,
		tied_embeddings: false,
		token_embd_bytes: 131072,
		...overrides
	};
}

/** A field with the given inherited / override / lock, as the server builds it. */
export function makeField(
	key: string,
	options: {
		inherited?: string | null;
		origin?: string;
		override?: string | null;
		locked?: { by: string; value: string | null } | null;
	} = {}
): ApiAdminSettingField {
	const inherited = options.inherited ?? null;
	const override = options.override ?? null;
	const locked = options.locked ?? null;
	const effective = locked?.value ?? override ?? inherited ?? null;

	return {
		active: override !== null && !locked,
		default: DEFAULTS[key] ?? null,
		effective,
		inactive_reason: override !== null && locked ? locked.by : null,
		inherited,
		inherited_origin: inherited !== null ? (options.origin ?? 'models_ini') : 'default',
		locked,
		origin: locked
			? locked.by
			: override !== null
				? 'admin'
				: inherited !== null
					? (options.origin ?? 'models_ini')
					: 'default',
		override
	};
}

/**
 * A model row. `inherited` and `overrides` are per key; `locked` maps a key
 * to its lock.
 */
export function makeRow(
	options: {
		id?: string;
		status?: string;
		source?: ServerModelSource;
		inherited?: Record<string, string>;
		overrides?: Record<string, string>;
		locked?: Record<string, { by: string; value: string | null }>;
		facts?: ApiAdminModelFacts | null;
		pool?: number | null;
	} = {}
): ApiAdminModelSettingsRow {
	const fields: Record<string, ApiAdminSettingField> = {};
	const overrides = options.overrides ?? {};

	for (const key of ADMIN_SETTING_KEYS) {
		fields[key] = makeField(key, {
			inherited: options.inherited?.[key] ?? null,
			locked: options.locked?.[key] ?? null,
			override: overrides[key] ?? null
		});
	}

	const facts = options.facts === undefined ? makeFacts() : options.facts;

	return {
		apply_error: '',
		context: {
			max_per_request: null,
			n_ctx_train: facts?.n_ctx_train ?? null,
			per_request_all_busy: null,
			per_slot_cap: null,
			pool: options.pool ?? null,
			slots: 4,
			unified: true
		},
		device_missing: null,
		device_moved: null,
		exists: true,
		facts,
		facts_status: facts ? 'ready' : 'pending',
		failures: 0,
		fields,
		hidden: false,
		id: options.id ?? 'tinyA',
		kept: {},
		last_failure: null,
		last_good: null,
		last_run: {
			exit_code: 0,
			failed: false,
			port: 0,
			reason: null,
			running: false,
			status: 'unloaded'
		},
		overrides,
		phase: null,
		reverted: null,
		reverted_at_startup: false,
		source: options.source ?? ServerModelSource.MODELS_DIR,
		status: options.status ?? 'unloaded',
		updated: null,
		warnings: []
	};
}

/** One GPU card of GET /admin/gpus. */
export function makeGpu(device: string, options: Partial<ApiAdminGpu> = {}): ApiAdminGpu {
	return {
		backend: 'CUDA',
		description: 'NVIDIA GeForce GTX 1080 Ti',
		device,
		free: 8 * GIB,
		index: 0,
		mapping: 'pci',
		memory_age_ms: 500,
		memory_source: 'vendor_tool',
		models: [],
		name: 'NVIDIA GeForce GTX 1080 Ti',
		pci_bus_id: '0000:01:00.0',
		same_gpu_as: [],
		settling: false,
		tool: 'nvidia-smi',
		total: 11 * GIB,
		type: 'gpu',
		unsupported: false,
		used: 3 * GIB,
		uuid: 'GPU-a',
		vendor: 'nvidia',
		...options
	};
}

export function makeGpus(gpus: ApiAdminGpu[], options: Partial<ApiAdminGpus> = {}): ApiAdminGpus {
	return {
		build_devices: gpus.map((gpu) => gpu.device),
		gpu_backend: { backends: [], libraries: [], message: '', state: 'ok' },
		gpus,
		inventory: {
			age_ms: 1000,
			backends: ['CPU', 'CUDA'],
			devices: [],
			duration_ms: 50,
			error: '',
			message: '',
			ok: true,
			ran: true,
			source: 'helper',
			time: '2026-10-06T10:00:00Z',
			time_ms: 0
		},
		margin: GIB,
		probe: {
			age_ms: 100,
			error: '',
			mapping: 'pci',
			message: '',
			mode: 'auto',
			ok: true,
			source: 'nvidia-smi',
			stale: false,
			tools: []
		},
		settle_ms: 1500,
		unmatched: [],
		warnings: [],
		...options
	};
}
