/**
 * Pure helpers of the per-model settings dialog (admin panel, stage 2):
 * canonical values, the draft (create / rebase / validate / build the POST
 * body), the values a draft gives the model, context modes, model role,
 * the texts of origins, warnings, failure reasons and the restart progress.
 * No state and no I/O, covered by unit tests.
 */

import {
	ADMIN_CONTEXT_PAD,
	ADMIN_DEVICE_NONE,
	ADMIN_FAILURE_LABELS,
	ADMIN_MODEL_DEVICES_MAX,
	ADMIN_MODELS_RW_ROOT,
	ADMIN_ORIGIN_LABELS,
	ADMIN_PARALLEL_AUTO_SLOTS,
	ADMIN_SETTING_KEYS,
	ADMIN_SETTING_LABELS,
	ADMIN_STORAGE_EPHEMERAL_WARNING
} from '$lib/constants';
import {
	AdminContextMode,
	AdminFailureCode,
	AdminModelRole,
	AdminRestartPhase,
	AdminSettingKey,
	AdminSettingOrigin,
	AdminStorageDirKind,
	ServerModelStatus
} from '$lib/enums';
import type {
	AdminEffectiveSetting,
	AdminModelSettingsDraft,
	AdminModelSettingsValidation,
	ApiAdminFailure,
	ApiAdminFailureReason,
	ApiAdminModelLog,
	ApiAdminModelSettingsRow,
	ApiAdminModelSettingsSaveResult,
	ApiAdminSettingField,
	ApiAdminSettingSchema,
	ApiAdminStatus,
	ApiAdminStatusStorage
} from '$lib/types/admin';

/** List warning of GET /admin/models/settings, followed by the entry name. */
const ADMIN_WARNING_UNKNOWN_SECTION = 'model_devices_unknown_section:';
const TRUE_WORDS = new Set(['1', 'enabled', 'on', 'true', 'yes']);
const FALSE_WORDS = new Set(['0', 'disabled', 'false', 'no', 'off']);
const QUANTIZED_CACHE_TYPES = new Set(['q4_0', 'q8_0']);
const INT_RE = /^[+-]?\d+$/;
/** A field the server did not send (older server, unknown key): nothing is set. */
const EMPTY_FIELD: ApiAdminSettingField = {
	active: false,
	default: null,
	effective: null,
	inactive_reason: null,
	inherited: null,
	inherited_origin: AdminSettingOrigin.DEFAULT,
	locked: null,
	origin: AdminSettingOrigin.DEFAULT,
	override: null
};

/** A value in any accepted form, as it arrives from a form field or an import file. */
export type AdminRawSettingValue = string | number | boolean | null | undefined;

/** Whether a key belongs to the per-model settings whitelist. */
export function isAdminSettingKey(key: string): key is AdminSettingKey {
	return (ADMIN_SETTING_KEYS as string[]).includes(key);
}

/** Split a device list on `,` or `;`, trimmed, empty names dropped. */
export function splitDeviceList(value: string | null | undefined): string[] {
	return (value ?? '')
		.split(/[,;]/)
		.map((name) => name.trim())
		.filter((name) => name !== '');
}

/**
 * The spelling the server stores: plain decimal integers, lower-case words,
 * `true` / `false`, device names as the server lists them (`CUDA1`, matched
 * without regard to case). An empty value is null ("not set"). A value that
 * cannot be read stays as typed, so validation can name it.
 */
export function canonicalSettingValue(
	key: string,
	value: AdminRawSettingValue,
	devices: string[] = []
): string | null {
	if (value === null || value === undefined) return null;

	const text = String(value).trim();

	if (text === '') return null;

	const lower = text.toLowerCase();

	switch (key) {
		case AdminSettingKey.DEVICE: {
			const names = splitDeviceList(text);

			if (names.length === 1 && names[0].toLowerCase() === ADMIN_DEVICE_NONE) {
				return ADMIN_DEVICE_NONE;
			}

			return names
				.map(
					(name) => devices.find((device) => device.toLowerCase() === name.toLowerCase()) ?? name
				)
				.join(',');
		}
		case AdminSettingKey.EMBEDDINGS:
		case AdminSettingKey.RERANKING:
			if (typeof value === 'boolean') return value ? 'true' : 'false';

			if (TRUE_WORDS.has(lower)) return 'true';

			if (FALSE_WORDS.has(lower)) return 'false';

			return text;
		case AdminSettingKey.FLASH_ATTN:
			if (lower === 'auto') return 'auto';

			if (TRUE_WORDS.has(lower)) return 'on';

			if (FALSE_WORDS.has(lower)) return 'off';

			return lower;
		case AdminSettingKey.N_GPU_LAYERS:
			if (lower === 'auto' || lower === 'all') return lower;

			if (INT_RE.test(text)) {
				const layers = Number(text);

				if (layers === -1) return 'auto';

				if (layers < -1) return 'all';

				return String(layers);
			}

			return text;
		case AdminSettingKey.PARALLEL:
			if (lower === 'auto') return '-1';

			return INT_RE.test(text) ? String(Number(text)) : text;
		case AdminSettingKey.CTX_SIZE:
		case AdminSettingKey.KV_UNIFIED_PER_SLOT:
		case AdminSettingKey.UBATCH_SIZE:
			return INT_RE.test(text) ? String(Number(text)) : text;
		default:
			return lower;
	}
}

/** Same value in the meaning the server compares them (canonical spelling). */
export function settingValuesEqual(
	key: string,
	left: AdminRawSettingValue,
	right: AdminRawSettingValue,
	devices: string[] = []
): boolean {
	return canonicalSettingValue(key, left, devices) === canonicalSettingValue(key, right, devices);
}

/** The field of a row, or an empty "not set" field when the server did not send it. */
export function settingField(row: ApiAdminModelSettingsRow, key: string): ApiAdminSettingField {
	return row.fields?.[key] ?? EMPTY_FIELD;
}

/** The schema entry of a key, if the server sent one. */
export function settingSchema(
	schema: ApiAdminSettingSchema[],
	key: string
): ApiAdminSettingSchema | undefined {
	return schema.find((entry) => entry.key === key);
}

/** Whether the server fixes this key for the model (MODEL_DEVICES, router command line). */
export function isSettingLocked(row: ApiAdminModelSettingsRow, key: string): boolean {
	return settingField(row, key).locked !== null;
}

export function isQuantizedCacheType(value: string | null | undefined): boolean {
	return QUANTIZED_CACHE_TYPES.has((value ?? '').toLowerCase());
}

/** A draft that holds exactly the saved per-model values of a row. */
export function createModelSettingsDraft(row: ApiAdminModelSettingsRow): AdminModelSettingsDraft {
	const values: Record<string, string | null> = {};

	for (const key of ADMIN_SETTING_KEYS) values[key] = row.overrides?.[key] ?? null;

	return { values };
}

/**
 * Carry a draft over to a newer row of the same model. A key the user did
 * not touch (it still equals the previous saved value) follows the server,
 * an edited key is kept: a background refresh neither throws away what is
 * being edited nor turns stale values into changes.
 */
export function rebaseModelSettingsDraft(
	draft: AdminModelSettingsDraft,
	previous: ApiAdminModelSettingsRow,
	next: ApiAdminModelSettingsRow
): AdminModelSettingsDraft {
	const values: Record<string, string | null> = {};

	for (const key of ADMIN_SETTING_KEYS) {
		const draftValue = draft.values[key] ?? null;
		const before = previous.overrides?.[key] ?? null;

		values[key] = settingValuesEqual(key, draftValue, before)
			? (next.overrides?.[key] ?? null)
			: draftValue;
	}

	return { values };
}

/**
 * The value a key will have when the draft is saved, and where it comes
 * from: a lock wins, then the draft, then the inherited value, then the
 * built-in default.
 */
export function effectiveDraftSetting(
	row: ApiAdminModelSettingsRow,
	draft: AdminModelSettingsDraft,
	key: string
): AdminEffectiveSetting {
	const field = settingField(row, key);

	if (field.locked) {
		return { locked: true, origin: field.locked.by, value: field.locked.value ?? field.effective };
	}

	const value = draft.values[key] ?? null;

	if (value !== null) return { locked: false, origin: AdminSettingOrigin.ADMIN, value };

	if (field.inherited !== null) {
		return { locked: false, origin: field.inherited_origin, value: field.inherited };
	}

	return { locked: false, origin: AdminSettingOrigin.DEFAULT, value: field.default };
}

/** Shortcut: the value only. */
export function draftSettingValue(
	row: ApiAdminModelSettingsRow,
	draft: AdminModelSettingsDraft,
	key: string
): string | null {
	return effectiveDraftSetting(row, draft, key).value;
}

/**
 * What to keep in the draft for a value chosen in a field: null only when
 * nothing is inherited and the value is the built-in default, so choosing
 * "Auto" or "f16" saves nothing. A value equal to the inherited one is
 * kept: it pins the model to it (e.g. its GPU), so a later change of
 * models.ini or of "GPU for new models" no longer moves it. The server
 * stores such a value without a restart (it changes nothing now).
 */
export function normalizeDraftValue(
	row: ApiAdminModelSettingsRow,
	key: string,
	value: AdminRawSettingValue,
	devices: string[] = []
): string | null {
	const canonical = canonicalSettingValue(key, value, devices);

	if (canonical === null) return null;

	const field = settingField(row, key);

	if (
		field.inherited === null &&
		field.default !== null &&
		settingValuesEqual(key, canonical, field.default, devices)
	) {
		return null;
	}

	return canonical;
}

/**
 * For a value the operator did not pick in its own field (the pool kept
 * when the context mode changes, the keys behind the model role, a value
 * loaded from an import file): null when it is what the model gets without
 * a per-model value anyway, so switching back and forth or importing the
 * values a model has already leaves no change behind.
 */
export function draftValueUnlessInherited(
	row: ApiAdminModelSettingsRow,
	key: string,
	value: AdminRawSettingValue,
	devices: string[] = []
): string | null {
	const canonical = canonicalSettingValue(key, value, devices);

	if (canonical === null) return null;

	const field = settingField(row, key);
	const without = field.inherited ?? field.default;

	return without !== null && settingValuesEqual(key, canonical, without, devices)
		? null
		: canonical;
}

/**
 * Whether saving the draft changes what the model runs with (and so needs
 * a restart of a running model). A value that only pins what the model gets
 * already does not; neither does removing a saved value equal to it. An
 * explicit parallel value couples a shared KV pool, which counts too.
 */
export function draftChangesEffect(
	draft: AdminModelSettingsDraft,
	row: ApiAdminModelSettingsRow
): boolean {
	const saved = createModelSettingsDraft(row);

	for (const key of Object.keys(buildModelSettingsUpdate(draft, row))) {
		const now = draftSettingValue(row, saved, key);
		const next = draftSettingValue(row, draft, key);

		if (!settingValuesEqual(key, now, next)) return true;
	}

	return draftContext(row, draft).unified !== draftContext(row, saved).unified;
}

/**
 * The `settings` object of the POST body: every key whose draft value
 * differs from the saved one (null removes a saved value). A locked key is
 * only ever removed, never set.
 */
export function buildModelSettingsUpdate(
	draft: AdminModelSettingsDraft,
	row: ApiAdminModelSettingsRow
): Record<string, string | null> {
	const update: Record<string, string | null> = {};

	for (const key of ADMIN_SETTING_KEYS) {
		const saved = row.overrides?.[key] ?? null;
		const value = draft.values[key] ?? null;

		if (settingValuesEqual(key, value, saved)) continue;

		if (isSettingLocked(row, key) && value !== null) continue;

		update[key] = value;
	}

	return update;
}

/** Keys the draft changes compared to the saved values. */
export function changedSettingKeys(
	draft: AdminModelSettingsDraft,
	row: ApiAdminModelSettingsRow
): string[] {
	return Object.keys(buildModelSettingsUpdate(draft, row));
}

/**
 * Check one value against the whitelist rules the server applies.
 * @returns a sentence for the field, or null when the value is fine
 */
export function validateSettingValue(
	key: string,
	value: string,
	schema: ApiAdminSettingSchema | undefined,
	devices: string[]
): string | null {
	const label = isAdminSettingKey(key) ? ADMIN_SETTING_LABELS[key] : key;
	const min = schema?.min;
	const max = schema?.max;

	switch (key) {
		case AdminSettingKey.DEVICE: {
			if (value === ADMIN_DEVICE_NONE) return null;

			const names = splitDeviceList(value);

			if (names.length === 0) return 'Choose a GPU.';

			if (names.some((name) => name.toLowerCase() === ADMIN_DEVICE_NONE)) {
				return '"none" (CPU only) cannot be combined with GPUs.';
			}

			if (names.length > ADMIN_MODEL_DEVICES_MAX) {
				return `A model can be split across at most ${ADMIN_MODEL_DEVICES_MAX} GPUs.`;
			}

			if (new Set(names.map((name) => name.toLowerCase())).size !== names.length) {
				return 'Each GPU can be chosen only once.';
			}

			const unknown = names.filter(
				(name) => !devices.some((device) => device.toLowerCase() === name.toLowerCase())
			);

			if (unknown.length > 0) {
				return devices.length === 0
					? `${unknown.join(', ')} is not on this server: it has no GPU.`
					: `${unknown.join(', ')} is not on this server (it has ${devices.join(', ')}).`;
			}

			return null;
		}
		case AdminSettingKey.N_GPU_LAYERS:
			if (value === 'auto' || value === 'all') return null;

			if (!INT_RE.test(value) || Number(value) < (min ?? 0) || Number(value) > (max ?? 999)) {
				return `${label}: All, Auto or a number from ${min ?? 0} to ${max ?? 999}.`;
			}

			return null;
		case AdminSettingKey.PARALLEL:
			if (value === '-1') return null;

			if (!INT_RE.test(value) || Number(value) < (min ?? 1) || Number(value) > (max ?? 16)) {
				return `${label}: Auto or a number from ${min ?? 1} to ${max ?? 16}.`;
			}

			return null;
		case AdminSettingKey.CTX_SIZE:
		case AdminSettingKey.KV_UNIFIED_PER_SLOT:
		case AdminSettingKey.UBATCH_SIZE:
			if (
				!INT_RE.test(value) ||
				(min !== undefined && Number(value) < min) ||
				(max !== undefined && Number(value) > max)
			) {
				return min !== undefined && max !== undefined
					? `${label}: a whole number from ${min.toLocaleString('en-US')} to ${max.toLocaleString('en-US')}.`
					: `${label}: a whole number.`;
			}

			return null;
		case AdminSettingKey.EMBEDDINGS:
		case AdminSettingKey.RERANKING:
			return value === 'true' || value === 'false' ? null : `${label}: on or off.`;
		default: {
			const values = schema?.values;

			if (values && !values.includes(value)) return `${label}: one of ${values.join(', ')}.`;

			return null;
		}
	}
}

/**
 * Client-side rules of the settings dialog, mirroring the server: each key
 * the draft changes must be a valid value, the two cache types must be
 * equal, and a quantized V cache needs flash attention. Values that were
 * saved before and are not touched are not held against the user.
 */
export function validateModelSettingsDraft(
	draft: AdminModelSettingsDraft,
	row: ApiAdminModelSettingsRow,
	schema: ApiAdminSettingSchema[],
	devices: string[]
): AdminModelSettingsValidation {
	const errors: Record<string, string> = {};
	const update = buildModelSettingsUpdate(draft, row);

	for (const [key, value] of Object.entries(update)) {
		if (value === null) continue;

		const error = validateSettingValue(key, value, settingSchema(schema, key), devices);

		if (error) errors[key] = error;
	}

	const touched = (key: string) => key in update;
	const typeK = draftSettingValue(row, draft, AdminSettingKey.CACHE_TYPE_K) ?? 'f16';
	const typeV = draftSettingValue(row, draft, AdminSettingKey.CACHE_TYPE_V) ?? 'f16';

	if (
		(touched(AdminSettingKey.CACHE_TYPE_K) || touched(AdminSettingKey.CACHE_TYPE_V)) &&
		typeK !== typeV &&
		!errors[AdminSettingKey.CACHE_TYPE_V]
	) {
		errors[AdminSettingKey.CACHE_TYPE_V] =
			`The K and V cache precision must be the same (${typeK} / ${typeV}).`;
	}

	if (
		(touched(AdminSettingKey.CACHE_TYPE_V) || touched(AdminSettingKey.FLASH_ATTN)) &&
		isQuantizedCacheType(typeV) &&
		draftSettingValue(row, draft, AdminSettingKey.FLASH_ATTN) === 'off'
	) {
		errors[AdminSettingKey.FLASH_ATTN] =
			`A quantized KV cache (${typeV}) needs flash attention on or auto.`;
	}

	return { errors, valid: Object.keys(errors).length === 0 };
}

/** Round a token count up to what llama.cpp allocates. */
export function padContext(tokens: number): number {
	return Math.ceil(tokens / ADMIN_CONTEXT_PAD) * ADMIN_CONTEXT_PAD;
}

function parseIntValue(value: string | null): number | null {
	if (value === null || !INT_RE.test(value)) return null;

	return Number(value);
}

/** What the context settings of a draft mean for one request (as the server computes it). */
export interface AdminDraftContext {
	slots: number;
	/** The parallel slots share one KV pool */
	unified: boolean;
	/** KV pool in tokens, rounded up to 256; null when the model default (trained context) applies */
	pool: number | null;
	perSlotCap: number | null;
	nCtxTrain: number | null;
	/** min(pool or pool / slots, per-slot cap, trained context) */
	maxPerRequest: number | null;
	/** What each request can count on when every slot is busy */
	perRequestAllBusy: number | null;
}

/**
 * Context figures of the draft, the same rules as the server row `context`:
 * `parallel` auto means 4 slots sharing one pool; an explicit `parallel`
 * saved here is coupled to a shared pool too; a pool comes from ctx-size,
 * or from slots x per-slot value when only that is set.
 */
export function draftContext(
	row: ApiAdminModelSettingsRow,
	draft: AdminModelSettingsDraft,
	nCtxTrain: number | null = row.facts?.n_ctx_train ?? row.context?.n_ctx_train ?? null
): AdminDraftContext {
	const parallel = parseIntValue(draftSettingValue(row, draft, AdminSettingKey.PARALLEL));
	const auto = parallel === null || parallel <= 0;
	const slots = auto ? ADMIN_PARALLEL_AUTO_SLOTS : parallel;
	const unified =
		auto || draft.values[AdminSettingKey.PARALLEL] !== null || (row.context?.unified ?? true);
	const ctx = parseIntValue(draftSettingValue(row, draft, AdminSettingKey.CTX_SIZE));
	const perSlot = parseIntValue(draftSettingValue(row, draft, AdminSettingKey.KV_UNIFIED_PER_SLOT));
	const perSlotCap = perSlot !== null && perSlot > 0 ? perSlot : null;

	let pool: number | null = null;

	if (ctx !== null && ctx > 0) pool = padContext(ctx);
	else if (perSlotCap !== null) pool = padContext(slots * perSlotCap);

	if (pool === null) {
		return {
			maxPerRequest: null,
			nCtxTrain,
			perRequestAllBusy: null,
			perSlotCap,
			pool,
			slots,
			unified
		};
	}

	const sequence = unified ? pool : padContext(Math.floor(pool / slots));

	let maxPerRequest = sequence;

	if (perSlotCap !== null) maxPerRequest = Math.min(maxPerRequest, perSlotCap);

	if (nCtxTrain !== null && nCtxTrain > 0) maxPerRequest = Math.min(maxPerRequest, nCtxTrain);

	return {
		maxPerRequest,
		nCtxTrain,
		perRequestAllBusy: unified ? Math.floor(pool / slots) : sequence,
		perSlotCap,
		pool,
		slots,
		unified
	};
}

/** The context mode a draft is in: per request when a per-slot value applies. */
export function contextModeOf(
	row: ApiAdminModelSettingsRow,
	draft: AdminModelSettingsDraft
): AdminContextMode {
	return draftSettingValue(row, draft, AdminSettingKey.KV_UNIFIED_PER_SLOT) !== null
		? AdminContextMode.PER_SLOT
		: AdminContextMode.POOL;
}

/**
 * Switch the context mode. "Per request x slots" writes ctx-size = slots x N
 * and kv-unified-per-slot = N; "Shared pool" removes the per-slot value and
 * keeps the pool size.
 */
export function withContextMode(
	draft: AdminModelSettingsDraft,
	row: ApiAdminModelSettingsRow,
	mode: AdminContextMode,
	perRequest: number
): AdminModelSettingsDraft {
	const context = draftContext(row, draft);
	const values = { ...draft.values };

	if (mode === AdminContextMode.PER_SLOT) {
		const tokens = Math.max(1, Math.round(perRequest));

		values[AdminSettingKey.KV_UNIFIED_PER_SLOT] = normalizeDraftValue(
			row,
			AdminSettingKey.KV_UNIFIED_PER_SLOT,
			tokens
		);
		values[AdminSettingKey.CTX_SIZE] = normalizeDraftValue(
			row,
			AdminSettingKey.CTX_SIZE,
			context.slots * tokens
		);
	} else {
		values[AdminSettingKey.KV_UNIFIED_PER_SLOT] = null;

		if (context.pool !== null) {
			const saved = row.overrides?.[AdminSettingKey.CTX_SIZE] ?? null;

			// back to the saved value when the pool is that, else inherit when it is what the model gets
			values[AdminSettingKey.CTX_SIZE] =
				saved !== null && settingValuesEqual(AdminSettingKey.CTX_SIZE, saved, context.pool)
					? saved
					: draftValueUnlessInherited(row, AdminSettingKey.CTX_SIZE, context.pool);
		}
	}

	return { values };
}

/** The role of a model from its effective embeddings / reranking values. */
export function modelRoleOf(
	row: ApiAdminModelSettingsRow,
	draft: AdminModelSettingsDraft
): AdminModelRole {
	if (draftSettingValue(row, draft, AdminSettingKey.RERANKING) === 'true') {
		return AdminModelRole.RERANKING;
	}

	if (draftSettingValue(row, draft, AdminSettingKey.EMBEDDINGS) === 'true') {
		return AdminModelRole.EMBEDDINGS;
	}

	return AdminModelRole.CHAT;
}

/**
 * Set the role keys for a role: chat turns embeddings and reranking off,
 * embeddings turns embeddings on (with an optional pooling), reranking turns
 * reranking on (which implies embeddings with rank pooling on the server).
 * The role select has no "inherit" entry: choosing the role the model gets
 * anyway removes the per-model role values.
 */
export function withModelRole(
	draft: AdminModelSettingsDraft,
	row: ApiAdminModelSettingsRow,
	role: AdminModelRole,
	pooling: string | null = null
): AdminModelSettingsDraft {
	const values = { ...draft.values };
	const set = (key: string, value: AdminRawSettingValue) => {
		values[key] = draftValueUnlessInherited(row, key, value);
	};

	switch (role) {
		case AdminModelRole.CHAT:
			set(AdminSettingKey.EMBEDDINGS, false);
			set(AdminSettingKey.RERANKING, false);
			values[AdminSettingKey.POOLING] = null;

			break;
		case AdminModelRole.EMBEDDINGS:
			set(AdminSettingKey.EMBEDDINGS, true);
			set(AdminSettingKey.RERANKING, false);
			values[AdminSettingKey.POOLING] =
				pooling === null ? null : normalizeDraftValue(row, AdminSettingKey.POOLING, pooling);

			break;
		case AdminModelRole.RERANKING:
			set(AdminSettingKey.RERANKING, true);
			values[AdminSettingKey.EMBEDDINGS] = null;
			values[AdminSettingKey.POOLING] = null;

			break;
	}

	return { values };
}

/** Thousands separators: `32,768`. */
export function formatTokenCount(tokens: number): string {
	return tokens.toLocaleString('en-US');
}

/** Compact token count for the table: `512`, `8k`, `32k`, `1.5k`, `1M`. */
export function formatTokensShort(tokens: number): string {
	if (tokens >= 1024 * 1024 && tokens % (1024 * 1024) === 0) return `${tokens / (1024 * 1024)}M`;

	if (tokens >= 1024) {
		const k = tokens / 1024;

		return Number.isInteger(k) ? `${k}k` : `${k.toFixed(1)}k`;
	}

	return String(tokens);
}

/** Words for where a value comes from. */
export function describeSettingOrigin(origin: string): string {
	return ADMIN_ORIGIN_LABELS[origin as AdminSettingOrigin] ?? origin;
}

/** A device list as text: `CUDA0`, `CUDA0 + CUDA1`, `CPU only`, `all GPUs`. */
export function describeDeviceValue(value: string | null, hasGpus: boolean): string {
	if (value === null) return hasGpus ? 'all GPUs' : 'CPU';

	if (value === ADMIN_DEVICE_NONE) return 'CPU only';

	return splitDeviceList(value).join(' + ');
}

/**
 * A setting value as the dialog shows it, e.g. "Auto (4 slots)", "All",
 * "8,192 tokens", "trained context". `nLayer` turns a large GPU-layers
 * number into "all".
 */
export function describeSettingValue(
	key: string,
	value: string | null,
	options: { hasGpus?: boolean; nLayer?: number | null } = {}
): string {
	switch (key) {
		case AdminSettingKey.DEVICE:
			return describeDeviceValue(value, options.hasGpus ?? true);
		case AdminSettingKey.CTX_SIZE:
			if (value === null) return 'trained context of the model';

			return INT_RE.test(value) ? `${formatTokenCount(Number(value))} tokens` : value;
		case AdminSettingKey.KV_UNIFIED_PER_SLOT:
			if (value === null) return 'not set';

			return INT_RE.test(value) ? `${formatTokenCount(Number(value))} tokens` : value;
		case AdminSettingKey.PARALLEL:
			if (value === null || value === '-1') return `Auto (${ADMIN_PARALLEL_AUTO_SLOTS} slots)`;

			return value;
		case AdminSettingKey.N_GPU_LAYERS:
			if (value === null || value === 'auto') return 'Auto (fit to free memory)';

			if (value === 'all') return 'All';

			if (options.nLayer && INT_RE.test(value) && Number(value) > options.nLayer) {
				return `${value} (all)`;
			}

			return value;
		case AdminSettingKey.FLASH_ATTN:
			return value ?? 'auto';
		case AdminSettingKey.CACHE_TYPE_K:
		case AdminSettingKey.CACHE_TYPE_V:
			return value ?? 'f16';
		case AdminSettingKey.EMBEDDINGS:
		case AdminSettingKey.RERANKING:
			return value === 'true' ? 'on' : 'off';
		case AdminSettingKey.POOLING:
			return value ?? 'model default';
		default:
			return value ?? 'not set';
	}
}

/** "1024 from models.ini", "Auto (4 slots), built-in default", "custom". */
export function describeInheritedValue(
	row: ApiAdminModelSettingsRow,
	key: string,
	options: { hasGpus?: boolean; nLayer?: number | null } = {}
): string {
	const field = settingField(row, key);

	if (field.inherited !== null) {
		return `${describeSettingValue(key, field.inherited, options)} from ${describeSettingOrigin(field.inherited_origin)}`;
	}

	return `${describeSettingValue(key, field.default, options)}, ${describeSettingOrigin(AdminSettingOrigin.DEFAULT)}`;
}

/**
 * Lines of the "Remove custom settings" confirmation: each saved value with
 * the value the model gets back, e.g. "Context size: 32,768 tokens -> 8,192
 * tokens (models.ini)".
 */
export function describeResetChanges(
	row: ApiAdminModelSettingsRow,
	options: { hasGpus?: boolean; nLayer?: number | null } = {}
): string[] {
	const lines: string[] = [];

	for (const key of ADMIN_SETTING_KEYS) {
		const saved = row.overrides?.[key];

		if (saved === undefined) continue;

		const field = settingField(row, key);
		const label = ADMIN_SETTING_LABELS[key];
		const back = field.inherited ?? field.default;
		const origin = describeSettingOrigin(
			field.inherited !== null ? field.inherited_origin : AdminSettingOrigin.DEFAULT
		);

		lines.push(
			`${label}: ${describeSettingValue(key, saved, options)} → ${describeSettingValue(key, back, options)} (${origin})`
		);
	}

	return lines;
}

/** Placement column of the model table: `CUDA1 · 32k`. */
export interface AdminPlacementSummary {
	device: string;
	context: string | null;
	/** The model has saved per-model values */
	custom: boolean;
	/** A saved value is not applied (locked, missing device...) */
	problem: boolean;
	/** The model gets a GPU this server does not have (e.g. a models.ini from another host) */
	unknownDevice: boolean;
}

/**
 * @param devices - GPU devices of the server; a device the model gets that
 * is not among them is flagged (only when the server listed its devices)
 */
export function placementSummary(
	row: ApiAdminModelSettingsRow,
	hasGpus: boolean,
	devices: string[] | null = null
): AdminPlacementSummary {
	const device = settingField(row, AdminSettingKey.DEVICE).effective;
	const pool = row.context?.pool ?? null;
	const inactive = Object.keys(row.overrides ?? {}).some(
		(key) => settingField(row, key).inactive_reason !== null
	);
	const unknownDevice =
		devices !== null &&
		device !== null &&
		device !== ADMIN_DEVICE_NONE &&
		splitDeviceList(device).some(
			(name) => !devices.some((known) => known.toLowerCase() === name.toLowerCase())
		);

	return {
		context: pool !== null ? formatTokensShort(pool) : null,
		custom: Object.keys(row.overrides ?? {}).length > 0,
		device: describeDeviceValue(device, hasGpus),
		problem:
			inactive || row.device_missing !== null || row.device_moved !== null || !!row.apply_error,
		unknownDevice
	};
}

/**
 * models_storage_ephemeral: the model cache or the models directory does not
 * survive a recreate. The way out depends on where the server runs: in a
 * container the root must be mounted, on a host the directories moved.
 */
function describeStorageEphemeral(storage: ApiAdminStatusStorage | null | undefined): string {
	const what = 'Model storage is not persistent: models downloaded or uploaded here';

	if (storage?.container === true) {
		return `${what} live inside the container and are lost when it is recreated (docker compose down/up, an image update); a plain restart keeps them. Mount a host folder at ${ADMIN_MODELS_RW_ROOT} (compose: MODELS_RW_DIR), then recreate the container.`;
	}

	if (storage?.container === false) {
		// only the setting of each directory concerned (both when `ephemeral` names none, which should not happen)
		const settings = [
			storage.ephemeral.includes(AdminStorageDirKind.MODELS_DIR) ? '--models-dir' : '',
			storage.ephemeral.includes(AdminStorageDirKind.CACHE) ? 'LLAMA_CACHE' : ''
		].filter(Boolean);
		const point = settings.length > 0 ? settings.join(' and ') : '--models-dir and LLAMA_CACHE';

		return `${what} are in a temporary directory and may be lost on reboot. Point ${point} at a permanent directory.`;
	}

	// a status without `storage` (it is only ever sent with the warning): both ways out
	return `${what} may be lost when the container is recreated or the host reboots. Docker: mount a host folder at ${ADMIN_MODELS_RW_ROOT} (compose: MODELS_RW_DIR). Host: point --models-dir and LLAMA_CACHE at a permanent directory.`;
}

/**
 * Sentence for a warning code of a row, of the draft, of the model list or
 * of the status.
 * @param status - GET /admin/status, for the codes whose words depend on it
 * (models_storage_ephemeral: container or host)
 */
export function describeModelWarning(
	code: string,
	row?: ApiAdminModelSettingsRow,
	status?: Pick<ApiAdminStatus, 'storage'> | null
): string {
	if (code.startsWith(ADMIN_WARNING_UNKNOWN_SECTION)) {
		const name = code.slice(ADMIN_WARNING_UNKNOWN_SECTION.length);

		return `MODEL_DEVICES names "${name}", which is not a section of models.ini; that entry is ignored, so the model is not locked to a GPU.`;
	}

	switch (code) {
		case 'apply_error':
			return `A saved value could not be applied: ${row?.apply_error || 'see the server log'}.`;
		case 'audit_not_persisted':
			return 'The audit log cannot be written to the state directory: new records are kept in memory only and are lost when the server restarts. The Activity tab has the details.';
		case ADMIN_STORAGE_EPHEMERAL_WARNING:
			return describeStorageEphemeral(status?.storage);
		case 'ctx_above_train':
			return 'The context is larger than the model was trained for (times the slots): quality may drop beyond the trained length.';
		case 'default_device_not_present':
			return 'The "GPU for new models" setting names a GPU this server does not have; it is ignored.';
		case 'device_missing':
			return (
				row?.device_missing?.message ||
				'The saved GPU is not on this server; the model uses its inherited device instead.'
			);
		case 'device_moved':
			return row?.device_moved
				? `The saved GPU (PCI ${row.device_moved.saved_pci}) is no longer ${row.device_moved.device}${row.device_moved.now_named ? `; it is now ${row.device_moved.now_named}` : ''}. Check the GPU choice.`
				: 'The saved GPU name now points at another physical GPU. Check the GPU choice.';
		case 'kv_per_slot_no_effect':
			return 'The per-request limit is larger than what each slot gets from the pool, so it has no effect.';
		case 'model_devices_not_applied':
			return 'MODEL_DEVICES names this model, but the value was not applied to models.ini (for example a CRLF models.ini): the GPU is not locked.';
		case 'pool_overcommit':
			return 'The slots share a small pool: when all of them are busy, each request gets only a short context and long requests can fail together.';
		case 'pooling_without_embeddings':
			return 'Pooling is set, but the model is not in embeddings or reranking mode, so it has no effect.';
		case 'ubatch_above_ctx':
			return 'The micro-batch is larger than the context.';
		default:
			return code;
	}
}

/** Draft-time warnings (same codes as the server row warnings), recomputed while editing. */
export function draftWarnings(
	row: ApiAdminModelSettingsRow,
	draft: AdminModelSettingsDraft
): string[] {
	const warnings: string[] = [];
	const context = draftContext(row, draft);
	const train = context.nCtxTrain;
	const role = modelRoleOf(row, draft);

	if (
		draftSettingValue(row, draft, AdminSettingKey.POOLING) !== null &&
		role === AdminModelRole.CHAT
	) {
		warnings.push('pooling_without_embeddings');
	}

	if (context.pool !== null) {
		if (train !== null && train > 0 && context.pool > train * context.slots) {
			warnings.push('ctx_above_train');
		}

		const ubatch = parseIntValue(draftSettingValue(row, draft, AdminSettingKey.UBATCH_SIZE));

		if (ubatch !== null && ubatch > context.pool) warnings.push('ubatch_above_ctx');

		if (
			context.unified &&
			context.slots > 1 &&
			train !== null &&
			train > 0 &&
			Math.floor(context.pool / context.slots) < Math.min(train, 4096)
		) {
			warnings.push('pool_overcommit');
		}

		if (context.perSlotCap !== null) {
			const sequence = context.unified
				? context.pool
				: padContext(Math.floor(context.pool / context.slots));

			if (context.perSlotCap > sequence) warnings.push('kv_per_slot_no_effect');
		}
	}

	return warnings;
}

/** Help text of the micro-batch field, by the effective pooling. */
export function ubatchHelp(pooling: string | null, role: AdminModelRole): string {
	if (role === AdminModelRole.CHAT) {
		return 'Tokens processed in one step while reading a prompt. Larger values use more compute memory; the default (512) suits most GPUs.';
	}

	if (pooling === 'last' || pooling === 'none') {
		return 'With this pooling the micro-batch affects speed and compute memory only: long inputs are processed in several steps.';
	}

	return 'For this pooling the micro-batch is also the longest input the model accepts, in tokens (default 512).';
}

/** Words for a classified failure. */
export interface AdminFailureText {
	label: string;
	message: string;
	hint: string;
}

/** An error line about the projector (mmproj): clip / mtmd, "failed to load multimodal model". */
const PROJECTOR_LINE = /mmproj|multimodal model|\bclip[_:.]|\bmtmd[_:]/i;
const ERROR_WORDS = /error|fail|mismatch|wrong|unable|invalid|cannot|not supported/i;

/** The output of a failed start says that the projector (mmproj) could not be used. */
export function failureMentionsProjector(
	reason: ApiAdminFailureReason | null | undefined,
	lines: readonly string[] = []
): boolean {
	return [reason?.line ?? '', ...lines].some(
		(line) => PROJECTOR_LINE.test(line) && ERROR_WORDS.test(line)
	);
}

export function describeFailureReason(
	reason: ApiAdminFailureReason | null | undefined,
	context: {
		/** Output tail of the failed instance */
		lines?: readonly string[];
		/** A models_dir model: "Delete files..." can remove only its projector */
		projectorRemovable?: boolean;
	} = {}
): AdminFailureText {
	// a projector that does not fit the model (n_embd, a damaged file) makes every start fail,
	// and the generic "model could not be loaded" points at the wrong file
	const projector =
		reason?.code !== AdminFailureCode.OUT_OF_MEMORY &&
		reason?.code !== AdminFailureCode.INVALID_DEVICE &&
		failureMentionsProjector(reason, context.lines)
			? context.projectorRemovable
				? 'The projector (mmproj) may not match this model: Delete files... -> Remove only the projector keeps the model.'
				: 'The projector (mmproj) may not match this model: check the projector set for it.'
			: null;

	if (!reason) {
		return {
			hint: [projector, 'Show the output for details.'].filter(Boolean).join(' '),
			label: ADMIN_FAILURE_LABELS[AdminFailureCode.UNKNOWN],
			message: 'The model stopped before it was ready.'
		};
	}

	const label = ADMIN_FAILURE_LABELS[reason.code as AdminFailureCode] ?? reason.code;
	const hints = projector ? [projector] : [reason.hint];

	if (reason.hints?.includes('memory_tight')) {
		hints.push('llama.cpp warned that memory was tight while it planned the load.');
	}

	return {
		hint: hints.filter(Boolean).join(' '),
		label,
		message: reason.message || label
	};
}

/** Notices for the parts of a save result the operator should know about. */
export function describeSaveResult(result: ApiAdminModelSettingsSaveResult): string[] {
	const lines: string[] = [];
	const r = result.result;

	if (r.load_error) {
		lines.push(
			`Settings saved. The model could not be started right now (${r.load_error}); it starts on its next request.`
		);
	}

	if (r.reload_error)
		lines.push(`Settings saved, but reloading the model list failed: ${r.reload_error}`);

	if (r.restarted_also.length > 0) {
		lines.push(`Also restarted after the model list was reloaded: ${r.restarted_also.join(', ')}.`);
	}

	for (const item of r.not_restarted) {
		lines.push(
			`${item.id} was stopped by the reload and could not be started again: ${item.error}`
		);
	}

	return lines;
}

/** What "Revert" in the failure view sends, and the words of its button. */
export interface AdminRevertPlan {
	settings: Record<string, string | null>;
	label: string;
}

/** Saved values without the keys a POST removes (null): the overrides they leave. */
function savedValues(settings: Record<string, string | null>): Record<string, string> {
	const out: Record<string, string> = {};

	for (const [key, value] of Object.entries(settings)) {
		if (value !== null && value !== undefined) out[key] = value;
	}

	return out;
}

function sameSavedValues(left: Record<string, string>, right: Record<string, string>): boolean {
	const keys = Object.keys(left);

	return (
		keys.length === Object.keys(right).length &&
		keys.every((key) => settingValuesEqual(key, left[key], right[key] ?? null))
	);
}

/**
 * `failed` plus the saved values `settings` the model failed to start with,
 * unless that set is in it already (the sets revertPlan never goes back to).
 */
export function withFailedSettings(
	failed: Record<string, string>[],
	settings: Record<string, string>
): Record<string, string>[] {
	return failed.some((set) => sameSavedValues(set, settings)) ? failed : [...failed, settings];
}

/**
 * The settings to put back after a failed start. `previous` (the saved
 * values before the last save of this dialog) undoes that save, unless the
 * model is known to have failed with them (`failed`: a second failed save in
 * a row, or a row the server reported as failed): then, as for a failure
 * from an earlier visit, it is the server's `last_good`, the settings the
 * model last reached "loaded" with (none: the per-model values are removed).
 */
export function revertPlan(
	row: ApiAdminModelSettingsRow,
	previous: Record<string, string | null> | null,
	failed: Record<string, string>[] = []
): AdminRevertPlan {
	if (previous && !failed.some((set) => sameSavedValues(set, savedValues(previous)))) {
		return { label: 'Revert to previous settings and restart', settings: { ...previous } };
	}

	const good = row.last_good ?? {};
	const settings: Record<string, string | null> = {};

	for (const key of new Set([...Object.keys(row.overrides ?? {}), ...Object.keys(good)])) {
		settings[key] = good[key] ?? null;
	}

	return {
		label:
			Object.keys(good).length > 0
				? 'Revert to the settings that last worked and restart'
				: 'Remove the custom settings and restart',
		settings
	};
}

/**
 * Whether the instance of GET /admin/models/log is a later start than the
 * failure snapshot `last_failure` (so its output is not that failure). A
 * running (or loading) instance always is, since the failed one has exited,
 * and so is one on another port. A stopped instance on the port of the
 * failure is later when it started after the failure: `started_ms` against
 * `time_ms`, both in ms. An older server sends the start in whole seconds
 * only: then the instance is later only when it started in a later second,
 * a stopped instance from the second of the failure is the failed one
 * itself (a start that fails at once), not a retry.
 */
export function isInstanceAfterFailure(
	instance: ApiAdminModelLog['instance'],
	failure: ApiAdminFailure | null
): boolean {
	if (!instance) return false;

	if (!failure || instance.running || instance.port !== failure.port) return true;

	if (typeof instance.started_ms === 'number' && instance.started_ms > 0 && failure.time_ms > 0) {
		return instance.started_ms > failure.time_ms;
	}

	const started = Date.parse(instance.started);
	const failed = failure.time_ms > 0 ? failure.time_ms : Date.parse(failure.time);

	if (!Number.isFinite(started) || !Number.isFinite(failed)) return true;

	return Math.floor(started / 1000) > Math.floor(failed / 1000);
}

/** Follows a restart on the /models/sse feed (see restartPhase). */
export interface AdminRestartTracker {
	/** The POST is still running */
	saving: boolean;
	/** A start of the new instance was seen (status loading) */
	sawStart: boolean;
	ready: boolean;
	failed: boolean;
	exitCode: number | null;
}

export function createRestartTracker(): AdminRestartTracker {
	return { exitCode: null, failed: false, ready: false, saving: true, sawStart: false };
}

/**
 * Apply one status of the restarted model. The stop of the old instance
 * (unloaded, exit code 0) is not a failure; a non-zero exit after the new
 * start, or before it once the POST returned, is.
 */
export function trackRestartStatus(
	tracker: AdminRestartTracker,
	status: string,
	exitCode: number | null = null
): AdminRestartTracker {
	if (tracker.ready || tracker.failed) return tracker;

	switch (status) {
		case ServerModelStatus.LOADING:
			return { ...tracker, sawStart: true };
		case ServerModelStatus.LOADED:
		case ServerModelStatus.SLEEPING:
			return tracker.sawStart || !tracker.saving ? { ...tracker, ready: true } : tracker;
		case ServerModelStatus.FAILED:
			return { ...tracker, exitCode, failed: true };
		case ServerModelStatus.UNLOADED:
			if ((exitCode ?? 0) !== 0 && (tracker.sawStart || !tracker.saving)) {
				return { ...tracker, exitCode, failed: true };
			}

			return tracker;
		default:
			return tracker;
	}
}

export function restartPhase(tracker: AdminRestartTracker): AdminRestartPhase {
	if (tracker.failed) return AdminRestartPhase.FAILED;

	if (tracker.saving) return AdminRestartPhase.SAVING;

	if (tracker.ready) return AdminRestartPhase.READY;

	return tracker.sawStart ? AdminRestartPhase.LOADING : AdminRestartPhase.STARTING;
}

/** A model that holds a slot: loaded, loading or sleeping. */
export function isRunningStatus(status: string | null | undefined): boolean {
	return (
		status === ServerModelStatus.LOADED ||
		status === ServerModelStatus.LOADING ||
		status === ServerModelStatus.SLEEPING
	);
}
