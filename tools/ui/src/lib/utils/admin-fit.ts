/**
 * Fit of a model on its GPUs (admin panel, stage 2).
 *
 * The verdict that counts comes from the server: POST
 * /admin/models/settings/check runs llama.cpp itself on the draft settings
 * (a dry run without allocating weights) and measures model, context and
 * compute memory per device. This file turns that answer into the view
 * model of the fit indicator.
 *
 * Until the check answers, or when it fails, the dialog shows an instant
 * formula over the GGUF facts as a labelled "rough estimate": the same
 * parts llama.cpp reports (model, context, compute), with the layer
 * placement rules of llama.cpp (output layer first, input embeddings on the
 * CPU, layers split by free memory). It is never shown as a measurement.
 */

import { ADMIN_DEVICE_NONE, ADMIN_FIT_MARGIN_BYTES, ADMIN_FIT_ROUGH_COMPUTE } from '$lib/constants';
import {
	AdminFitErrorKind,
	AdminFitSource,
	AdminFitVerdict,
	AdminGpuMemorySource,
	AdminSettingKey,
	ServerModelStatus
} from '$lib/enums';
import type {
	AdminFitDevice,
	AdminFitGpuState,
	AdminFitResult,
	AdminMemoryParts,
	ApiAdminFitCheck,
	ApiAdminGpus,
	ApiAdminModelFacts
} from '$lib/types/admin';
import { formatBytes } from '$lib/utils/admin';
import { isQuantizedCacheType, padContext, splitDeviceList } from '$lib/utils/admin-model-settings';

/** Inputs of the instant formula. */
export interface AdminFitFormulaInput {
	facts: ApiAdminModelFacts | null;
	/** The value a key will have with the draft (null = not set) */
	value: (key: string) => string | null;
	/** Pool and slots of the draft (see draftContext) */
	context: { pool: number | null; slots: number; unified: boolean };
	/** GPU devices of the server (`build_devices`) */
	devices: string[];
	gpus: AdminFitGpuState[];
	margin?: number;
}

const VERDICT_ORDER = [AdminFitVerdict.TIGHT, AdminFitVerdict.UNKNOWN, AdminFitVerdict.NO];

function emptyParts(): AdminMemoryParts {
	return { compute: 0, context: 0, model: 0, total: 0 };
}

function withTotal(parts: AdminMemoryParts): AdminMemoryParts {
	return { ...parts, total: parts.model + parts.context + parts.compute };
}

/** A verdict string of the server as the enum, anything unknown as UNKNOWN. */
export function toFitVerdict(value: string | null | undefined): AdminFitVerdict {
	return Object.values(AdminFitVerdict).includes(value as AdminFitVerdict)
		? (value as AdminFitVerdict)
		: AdminFitVerdict.UNKNOWN;
}

/** fits / tight / no for a need against the available memory, with the safety margin. */
export function fitVerdict(need: number, available: number, margin: number): AdminFitVerdict {
	if (need + margin <= available) return AdminFitVerdict.FITS;

	if (need <= available) return AdminFitVerdict.TIGHT;

	return AdminFitVerdict.NO;
}

/**
 * The verdict of a model over all its GPUs, the same rule as the server:
 * "no" beats "unknown" beats "tight" beats "fits"; no GPU at all is
 * "cpu_only".
 */
export function worstFitVerdict(verdicts: AdminFitVerdict[]): AdminFitVerdict {
	if (verdicts.length === 0) return AdminFitVerdict.CPU_ONLY;

	let overall = AdminFitVerdict.FITS;

	for (const verdict of VERDICT_ORDER) {
		if (verdicts.includes(verdict)) overall = verdict;
	}

	return overall;
}

/** GPUs the model would use: "none" = none, a list = those, not set = every GPU of the server. */
export function fitTargetDevices(device: string | null, devices: string[]): string[] {
	if (device === ADMIN_DEVICE_NONE) return [];

	if (device === null) return [...devices];

	return splitDeviceList(device);
}

/**
 * How many of the n_layer + 1 offloadable layers (repeating layers plus the
 * output layer) go to the GPUs. "auto" (the default) is counted as all of
 * them: the server only moves layers back when memory is short.
 */
export function gpuLayerCount(
	value: string | null,
	nLayer: number
): { count: number; auto: boolean } {
	if (value === null || value === 'auto') return { auto: true, count: nLayer + 1 };

	if (value === 'all') return { auto: false, count: nLayer + 1 };

	const layers = Number(value);

	if (!Number.isFinite(layers) || layers < 0) return { auto: false, count: nLayer + 1 };

	return { auto: false, count: Math.min(Math.floor(layers), nLayer + 1) };
}

/**
 * Device of each layer, as llama.cpp places them: index 0..n_layer-1 are
 * the repeating layers, index n_layer is the output layer. The last `count`
 * of them are offloaded, so the output layer is the first one on a GPU.
 * Offloaded layers are split across the GPUs in proportion to `weights`
 * (free memory). -1 means the CPU.
 */
export function layerPlacement(nLayer: number, count: number, weights: number[]): number[] {
	const placement: number[] = new Array(nLayer + 1).fill(-1);

	if (weights.length === 0 || count <= 0) return placement;

	const total = weights.reduce((sum, weight) => sum + Math.max(0, weight), 0);
	const splits: number[] = [];

	let running = 0;

	for (const weight of weights) {
		running += total > 0 ? Math.max(0, weight) / total : 1 / weights.length;
		splits.push(running);
	}

	const start = Math.max(nLayer + 1 - count, 0);
	const active = Math.min(count, nLayer + 1);

	for (let il = start; il <= nLayer; il++) {
		const position = (il - start) / active;

		// std::upper_bound: the first split point greater than the position
		let device = splits.findIndex((point) => point > position);

		if (device === -1) device = weights.length - 1;

		placement[il] = device;
	}

	return placement;
}

/**
 * KV cells llama.cpp allocates: the pool rounded up to 256, per slot when
 * the slots do not share it. Without a context size the model uses its
 * trained context.
 */
export function kvCells(
	context: { pool: number | null; slots: number; unified: boolean },
	nCtxTrain: number
): number {
	const pool = context.pool ?? padContext(nCtxTrain);

	if (context.unified || context.slots <= 1) return padContext(pool);

	return padContext(Math.floor(padContext(pool) / context.slots)) * context.slots;
}

/**
 * Rough compute buffer: logits and activations of one micro-batch, the
 * attention scores when flash attention is off, and one layer of K/V
 * converted to f16 at full context for a quantized cache.
 */
export function roughComputeBytes(
	facts: ApiAdminModelFacts,
	options: { ubatch: number; cells: number; flashAttn: string | null; cacheType: string | null }
): number {
	const { BYTES_PER_ELEMENT, EMBD_FACTOR } = ADMIN_FIT_ROUGH_COMPUTE;

	let bytes = BYTES_PER_ELEMENT * options.ubatch * (facts.n_vocab + EMBD_FACTOR * facts.n_embd);

	if (options.flashAttn === 'off') {
		bytes += BYTES_PER_ELEMENT * options.ubatch * options.cells * facts.n_head;
	}

	if (isQuantizedCacheType(options.cacheType)) {
		bytes += 2 * facts.n_head_kv * Math.max(facts.d_k, facts.d_v) * 2 * options.cells;
	}

	return bytes;
}

/** Share of the KV cache each repeating layer holds (layers without KV heads hold none). */
function kvLayerShares(facts: ApiAdminModelFacts): number[] {
	const n = facts.n_layer;
	const heads = facts.n_head_kv_layers;

	if (heads && heads.length === n) {
		const sum = heads.reduce((total, value) => total + Math.max(0, value), 0);

		if (sum > 0) return heads.map((value) => Math.max(0, value) / sum);
	}

	return new Array(n).fill(1 / n);
}

function deviceFit(
	device: string,
	state: AdminFitGpuState | undefined,
	need: AdminMemoryParts,
	margin: number
): AdminFitDevice {
	const free = state?.free ?? null;
	const ownUsage = state?.ownUsage ?? 0;
	const available = free === null ? null : free + ownUsage;

	let verdict: AdminFitVerdict;

	if (!state || available === null || state.memorySource === AdminGpuMemorySource.NONE) {
		verdict = AdminFitVerdict.UNKNOWN;
	} else if (state.settling) {
		verdict = AdminFitVerdict.UNKNOWN;
	} else {
		verdict = fitVerdict(need.total, available, margin);
	}

	return {
		available,
		description: state?.description ?? '',
		device,
		free,
		memoryAgeMs: state?.memoryAgeMs ?? null,
		memorySource: state?.memorySource ?? AdminGpuMemorySource.NONE,
		need,
		ownUsage,
		settling: state?.settling ?? false,
		total: state?.total ?? null,
		verdict
	};
}

function findGpu(gpus: AdminFitGpuState[], name: string): AdminFitGpuState | undefined {
	return gpus.find((gpu) => gpu.device.toLowerCase() === name.toLowerCase());
}

/** Minutes or seconds since a reading, for "measured 5 min ago". */
export function formatAge(ms: number | null | undefined): string {
	if (ms === null || ms === undefined || !Number.isFinite(ms)) return 'some time';

	const seconds = Math.max(0, Math.round(ms / 1000));

	if (seconds < 60) return `${seconds} s`;

	const minutes = Math.round(seconds / 60);

	if (minutes < 120) return `${minutes} min`;

	return `${Math.round(minutes / 60)} h`;
}

/**
 * The instant formula. Same parts as the exact check (model, context,
 * compute per device; the rest on the host), verdicts with the same margin.
 */
export function estimateFitByFormula(input: AdminFitFormulaInput): AdminFitResult {
	const margin = input.margin ?? ADMIN_FIT_MARGIN_BYTES;
	const facts = input.facts;
	const base = {
		checkedAt: null,
		error: null,
		margin,
		source: AdminFitSource.FORMULA
	};

	if (!facts || facts.error || !(facts.n_layer > 0)) {
		return {
			...base,
			devices: [],
			host: null,
			nCtx: null,
			notes: [
				facts?.error
					? `The model file could not be read: ${facts.error}`
					: 'The model file has not been read yet.'
			],
			verdict: AdminFitVerdict.UNKNOWN
		};
	}

	const notes: string[] = [];
	const nLayer = facts.n_layer;
	const targets = fitTargetDevices(input.value(AdminSettingKey.DEVICE), input.devices);
	const layers = gpuLayerCount(input.value(AdminSettingKey.N_GPU_LAYERS), nLayer);
	const cells = kvCells(input.context, facts.n_ctx_train);
	const cacheType = input.value(AdminSettingKey.CACHE_TYPE_K) ?? 'f16';
	const perToken = facts.kv_bytes_per_token?.[cacheType] ?? null;
	const kvKnown = perToken !== null && facts.kv_estimate !== 'unsupported';

	if (!kvKnown) {
		notes.push(facts.kv_note || 'Context memory cannot be estimated for this architecture.');
	} else if (facts.kv_estimate === 'approximate' && facts.kv_note) {
		notes.push(facts.kv_note);
	}

	const kvTotal = kvKnown ? (perToken as number) * cells : 0;
	const shares = kvLayerShares(facts);
	const weights = targets.map((name) => {
		const gpu = findGpu(input.gpus, name);

		return Math.max(0, gpu?.free ?? gpu?.total ?? 0);
	});
	const placement = layerPlacement(nLayer, targets.length > 0 ? layers.count : 0, weights);
	const parts = targets.map(() => emptyParts());
	const host = emptyParts();
	const repeating =
		facts.tensor_bytes - facts.output_bytes - facts.token_embd_bytes - facts.other_bytes;
	const layerBytes = (il: number) =>
		facts.layer_bytes?.[il] ?? Math.max(0, repeating) / Math.max(1, nLayer);

	host.model += facts.token_embd_bytes + facts.other_bytes;

	if (facts.tied_embeddings && placement[nLayer] < 0) {
		// tied embeddings: the output layer is token_embd itself and stays with it on the CPU
		host.model += Math.max(0, facts.output_bytes - facts.token_embd_bytes);
	} else {
		const output = placement[nLayer] < 0 ? host : parts[placement[nLayer]];

		output.model += facts.output_bytes;
	}

	for (let il = 0; il < nLayer; il++) {
		const target = placement[il] < 0 ? host : parts[placement[il]];

		target.model += layerBytes(il);
		target.context += kvTotal * shares[il];
	}

	const ubatch = Number(input.value(AdminSettingKey.UBATCH_SIZE) ?? 512) || 512;
	const compute = roughComputeBytes(facts, {
		cacheType,
		cells,
		flashAttn: input.value(AdminSettingKey.FLASH_ATTN),
		ubatch
	});
	const onGpu = parts.some((part) => part.model > 0);

	for (const part of parts) {
		if (part.model > 0) part.compute = compute;
	}

	host.compute = onGpu
		? ADMIN_FIT_ROUGH_COMPUTE.BYTES_PER_ELEMENT * ubatch * facts.n_embd * 2
		: compute;

	if (layers.auto && onGpu) {
		notes.push(
			'GPU layers is Auto: when memory is short the server keeps some layers on the CPU or uses a smaller context instead of failing.'
		);
	}

	if (facts.mmproj_bytes > 0) notes.push('The multimodal projector is not included.');

	const devices = onGpu
		? targets
				.map((name, index) => ({ index, name }))
				.filter(({ index }) => parts[index].model > 0)
				.map(({ index, name }) =>
					deviceFit(name, findGpu(input.gpus, name), withTotal(parts[index]), margin)
				)
		: [];

	if (!kvKnown) {
		for (const device of devices) device.verdict = AdminFitVerdict.UNKNOWN;
	}

	const measured = devices.find((device) => device.memorySource === AdminGpuMemorySource.INVENTORY);

	if (measured) {
		notes.push(
			`Free memory was measured ${formatAge(measured.memoryAgeMs)} ago (no live reading), so it may be out of date.`
		);
	}

	return {
		...base,
		devices,
		host: withTotal(host),
		nCtx: cells,
		notes,
		verdict: worstFitVerdict(devices.map((device) => device.verdict))
	};
}

/** Remarks for the warning codes of a check answer. */
function checkNotes(check: ApiAdminFitCheck): string[] {
	const notes: string[] = [];

	for (const warning of check.warnings ?? []) {
		if (warning.startsWith('field_locked:')) {
			notes.push(
				`${warning.slice('field_locked:'.length)} is fixed for this model: it was checked with the locked value.`
			);
		} else if (warning === 'mmproj_not_included') {
			notes.push('The multimodal projector is not included in the check.');
		} else {
			notes.push(warning);
		}
	}

	if (check.fit === 'failure') {
		notes.push(
			'llama.cpp could not fit the settings into the free memory while it planned the load.'
		);
	}

	return notes;
}

/** Error codes of a check answer whose dry run did not run or finish (it judged nothing). */
const CHECK_NOT_RUN_CODES = new Set(['failed', 'timeout', 'unsupported']);

/**
 * What an error of the check answer means: llama.cpp rejected the settings,
 * could not place them into the memory, a GPU is missing, or the check
 * itself could not run (then the settings were not judged at all).
 */
export function fitCheckErrorKind(code: string | null | undefined): AdminFitErrorKind {
	if (code && CHECK_NOT_RUN_CODES.has(code)) return AdminFitErrorKind.NOT_CHECKED;

	if (code === 'device_missing') return AdminFitErrorKind.DEVICE_MISSING;

	if (code === 'out_of_memory') return AdminFitErrorKind.NO_MEMORY;

	return AdminFitErrorKind.REJECTED;
}

/** The answer of POST /admin/models/settings/check as the view model of the indicator. */
export function fitFromCheck(check: ApiAdminFitCheck): AdminFitResult {
	const devices: AdminFitDevice[] = (check.devices ?? []).map((device) => {
		// neither the vendor tool nor the helper read the memory of this device (the server says "unknown")
		const reading = device.free > 0 || device.total > 0;

		return {
			available: reading ? device.available : null,
			description: device.description,
			device: device.device,
			free: reading ? device.free : null,
			memoryAgeMs: null,
			memorySource: device.free_source,
			need: device.need,
			ownUsage: device.own_usage,
			settling: device.settling,
			total: reading ? device.total : null,
			verdict: toFitVerdict(device.verdict)
		};
	});
	const host = check.host
		? withTotal({
				compute: check.host.compute ?? 0,
				context: check.host.context ?? 0,
				model: check.host.model ?? 0,
				total: 0
			})
		: null;

	return {
		checkedAt: check.checked_at ?? null,
		devices,
		error: check.ok ? null : check.error,
		host,
		margin: check.margin ?? ADMIN_FIT_MARGIN_BYTES,
		nCtx: check.n_ctx ?? null,
		notes: checkNotes(check),
		source: AdminFitSource.CHECK,
		verdict: check.ok ? toFitVerdict(check.verdict) : AdminFitVerdict.UNKNOWN
	};
}

/** GPU state for the formula from GET /admin/gpus, with the model's own current usage. */
export function fitGpuStates(gpus: ApiAdminGpus | null, modelId: string): AdminFitGpuState[] {
	if (!gpus) return [];

	return gpus.gpus.map((gpu) => {
		const own = gpu.models.find((model) => model.id === modelId);
		const running =
			own?.status === ServerModelStatus.LOADED ||
			own?.status === ServerModelStatus.LOADING ||
			own?.status === ServerModelStatus.SLEEPING;

		return {
			description: gpu.description,
			device: gpu.device,
			free: gpu.free,
			memoryAgeMs: gpu.memory_source === AdminGpuMemorySource.INVENTORY ? gpu.memory_age_ms : null,
			memorySource: gpu.memory_source,
			ownUsage: running ? (own?.memory?.bytes ?? 0) : 0,
			settling: gpu.settling,
			total: gpu.total
		};
	});
}

/** Headline and detail lines of a fit result. */
export interface AdminFitText {
	title: string;
	lines: string[];
}

function deviceNames(devices: AdminFitDevice[]): string {
	return devices.map((device) => device.device).join(' + ');
}

const FIT_ERROR_TITLES: Record<AdminFitErrorKind, string> = {
	[AdminFitErrorKind.DEVICE_MISSING]: 'The GPU is not on this server',
	[AdminFitErrorKind.NO_MEMORY]: 'llama.cpp could not fit these settings into the memory',
	[AdminFitErrorKind.NOT_CHECKED]: 'The exact check could not run',
	[AdminFitErrorKind.REJECTED]: 'llama.cpp rejected these settings'
};

/** Sentences of the fit indicator: what the verdict means, in bytes. */
export function describeFit(result: AdminFitResult): AdminFitText {
	if (result.error) {
		return {
			lines: [result.error.message, result.error.line].filter(Boolean),
			title: FIT_ERROR_TITLES[fitCheckErrorKind(result.error.code)]
		};
	}

	const devices = result.devices;
	const left = (device: AdminFitDevice) => (device.available ?? 0) - device.need.total;

	switch (result.verdict) {
		case AdminFitVerdict.CPU_ONLY:
			return {
				lines: result.host
					? [`Needs about ${formatBytes(result.host.total)} of system memory.`]
					: [],
				title: 'Runs on the CPU only'
			};
		case AdminFitVerdict.FITS: {
			const least = Math.min(...devices.map(left));

			return {
				lines: [],
				title:
					devices.length === 1
						? `Fits on ${devices[0].device}: ${formatBytes(least)} left`
						: `Fits on ${deviceNames(devices)}: at least ${formatBytes(least)} left on each`
			};
		}
		case AdminFitVerdict.TIGHT: {
			const tight = devices.filter((device) => device.verdict === AdminFitVerdict.TIGHT);

			return {
				lines: [
					`Less than the safety margin of ${formatBytes(result.margin)} stays free; a long prompt or another process can make the start fail.`
				],
				title: `Fits on ${deviceNames(tight)}, but only ${formatBytes(Math.min(...tight.map(left)))} left`
			};
		}
		case AdminFitVerdict.NO: {
			const short = devices
				.filter((device) => device.verdict === AdminFitVerdict.NO)
				.sort((a, b) => left(a) - left(b));

			return {
				lines: [
					'Lower the context size, use a quantized KV cache (q8_0), fewer GPU layers or another GPU.'
				],
				title: `Probably does not fit on ${short[0].device}: short by ${formatBytes(-left(short[0]))}`
			};
		}
		default: {
			const settling = devices.filter((device) => device.settling);
			const unknown = devices.filter((device) => device.available === null);

			if (settling.length > 0) {
				return {
					lines: ['A model on this GPU is loading or stopping; the check runs again in a moment.'],
					title: `GPU memory of ${deviceNames(settling)} is settling`
				};
			}

			if (unknown.length > 0) {
				return {
					lines: [
						'The memory the model needs is shown, but there is no reading of the free memory.'
					],
					title: `Free memory of ${deviceNames(unknown)} is unknown`
				};
			}

			return { lines: [], title: 'Memory need unknown' };
		}
	}
}
