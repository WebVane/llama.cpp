import { GIB, makeFacts, makeGpu, makeGpus, makeRow, MIB } from './admin-fixtures';
import { AdminFitErrorKind, AdminFitSource, AdminFitVerdict, AdminSettingKey } from '$lib/enums';
import type { AdminFitGpuState, ApiAdminFitCheck } from '$lib/types/admin';
import {
	describeFit,
	estimateFitByFormula,
	fitCheckErrorKind,
	fitFromCheck,
	fitGpuStates,
	fitTargetDevices,
	fitVerdict,
	gpuLayerCount,
	kvCells,
	layerPlacement,
	roughComputeBytes,
	worstFitVerdict
} from '$lib/utils/admin-fit';
import { describe, expect, it } from 'vitest';

const FACTS = makeFacts();
const LAYER = 181760;
const OUTPUT = 131328;
const EMBD = 131072;

function gpu(
	device: string,
	free: number | null,
	extra: Partial<AdminFitGpuState> = {}
): AdminFitGpuState {
	return {
		description: 'test GPU',
		device,
		free,
		memoryAgeMs: null,
		memorySource: 'vendor_tool',
		ownUsage: 0,
		settling: false,
		total: 11 * GIB,
		...extra
	};
}

function formula(
	values: Record<string, string | null>,
	options: {
		gpus?: AdminFitGpuState[];
		devices?: string[];
		pool?: number | null;
		slots?: number;
		unified?: boolean;
		facts?: typeof FACTS | null;
		margin?: number;
	} = {}
) {
	return estimateFitByFormula({
		context: {
			pool: options.pool === undefined ? 1024 : options.pool,
			slots: options.slots ?? 4,
			unified: options.unified ?? true
		},
		devices: options.devices ?? ['CUDA0'],
		facts: options.facts === undefined ? FACTS : options.facts,
		gpus: options.gpus ?? [gpu('CUDA0', 8 * GIB)],
		margin: options.margin,
		value: (key) => values[key] ?? null
	});
}

describe('layer placement (llama.cpp rules)', () => {
	it('puts the output layer on the GPU first', () => {
		expect(layerPlacement(5, 1, [1])).toEqual([-1, -1, -1, -1, -1, 0]);
	});

	it('offloads the last repeating layers after the output layer', () => {
		expect(layerPlacement(5, 3, [1])).toEqual([-1, -1, -1, 0, 0, 0]);
	});

	it('keeps everything on the CPU with 0 layers or no GPU', () => {
		expect(layerPlacement(5, 0, [1])).toEqual([-1, -1, -1, -1, -1, -1]);
		expect(layerPlacement(5, 6, [])).toEqual([-1, -1, -1, -1, -1, -1]);
	});

	it('splits the layers by free memory across GPUs', () => {
		expect(layerPlacement(5, 6, [1, 1])).toEqual([0, 0, 0, 1, 1, 1]);
		expect(layerPlacement(5, 6, [3, 1])).toEqual([0, 0, 0, 0, 0, 1]);
		expect(layerPlacement(5, 6, [1, 1, 1, 1])).toEqual([0, 0, 1, 2, 2, 3]);
	});

	it('counts auto and all as every layer, large numbers capped', () => {
		expect(gpuLayerCount(null, 5)).toEqual({ auto: true, count: 6 });
		expect(gpuLayerCount('auto', 5)).toEqual({ auto: true, count: 6 });
		expect(gpuLayerCount('all', 5)).toEqual({ auto: false, count: 6 });
		expect(gpuLayerCount('99', 5)).toEqual({ auto: false, count: 6 });
		expect(gpuLayerCount('2', 5)).toEqual({ auto: false, count: 2 });
	});

	it('targets every GPU when no device is set, none for CPU only', () => {
		expect(fitTargetDevices(null, ['CUDA0', 'CUDA1'])).toEqual(['CUDA0', 'CUDA1']);
		expect(fitTargetDevices('none', ['CUDA0'])).toEqual([]);
		expect(fitTargetDevices('ROCm1', ['ROCm0', 'ROCm1'])).toEqual(['ROCm1']);
		expect(fitTargetDevices('CUDA0,CUDA2', [])).toEqual(['CUDA0', 'CUDA2']);
	});
});

describe('KV cells and padding', () => {
	it('rounds the pool up to 256 with a shared pool', () => {
		expect(kvCells({ pool: 1000, slots: 4, unified: true }, 2048)).toBe(1024);
	});

	it('pads per slot when the slots do not share the pool', () => {
		expect(kvCells({ pool: 1024, slots: 4, unified: false }, 2048)).toBe(1024);
		expect(kvCells({ pool: 1100, slots: 4, unified: false }, 2048)).toBe(2048);
	});

	it('uses the trained context when no context size is set', () => {
		expect(kvCells({ pool: null, slots: 4, unified: true }, 2000)).toBe(2048);
	});
});

describe('instant formula', () => {
	it('puts the whole model on one GPU and the input embeddings on the CPU', () => {
		const result = formula({});
		const device = result.devices[0];

		expect(result.source).toBe(AdminFitSource.FORMULA);
		expect(device.device).toBe('CUDA0');
		expect(device.need.model).toBe(5 * LAYER + OUTPUT);
		expect(device.need.context).toBe(640 * 1024);
		expect(result.host?.model).toBe(EMBD);
		expect(device.verdict).toBe(AdminFitVerdict.FITS);
	});

	it('offloads the output layer first with one GPU layer', () => {
		const result = formula({ [AdminSettingKey.N_GPU_LAYERS]: '1' });

		expect(result.devices[0].need.model).toBe(OUTPUT);
		expect(result.devices[0].need.context).toBe(0);
		expect(result.host?.model).toBe(EMBD + 5 * LAYER);
	});

	it('handles partial offload: output plus the last layers', () => {
		const result = formula({ [AdminSettingKey.N_GPU_LAYERS]: '3' });

		expect(result.devices[0].need.model).toBe(OUTPUT + 2 * LAYER);
		expect(result.devices[0].need.context).toBeCloseTo((640 * 1024 * 2) / 5, 6);
		expect(result.host?.model).toBe(EMBD + 3 * LAYER);
	});

	it('counts the GPU copy of tied embeddings, and no copy on the CPU', () => {
		const tied = makeFacts({ output_bytes: EMBD + 256, tied_embeddings: true });
		const gpuResult = formula({}, { facts: tied });

		expect(gpuResult.devices[0].need.model).toBe(5 * LAYER + EMBD + 256);
		expect(gpuResult.host?.model).toBe(EMBD);

		const cpuResult = formula({ [AdminSettingKey.N_GPU_LAYERS]: '0' }, { facts: tied });

		expect(cpuResult.verdict).toBe(AdminFitVerdict.CPU_ONLY);
		expect(cpuResult.host?.model).toBe(EMBD + 5 * LAYER + 256);
	});

	it('pads the context and scales with the KV precision', () => {
		const f16 = formula({}, { pool: 1000 });
		const q8 = formula({ [AdminSettingKey.CACHE_TYPE_K]: 'q8_0' }, { pool: 1000 });

		expect(f16.devices[0].need.context).toBe(640 * 1024);
		expect(q8.devices[0].need.context).toBe(340 * 1024);
		expect(q8.nCtx).toBe(1024);
	});

	it('gives a shared pool and a per-slot pool the same total for the same cells', () => {
		const shared = formula({}, { pool: 2048, slots: 4, unified: true });
		const split = formula({}, { pool: 2048, slots: 4, unified: false });

		expect(shared.devices[0].need.context).toBe(split.devices[0].need.context);
		expect(formula({}, { pool: 1100, unified: false }).nCtx).toBe(2048);
		expect(formula({}, { pool: 1100, unified: true }).nCtx).toBe(1280);
	});

	it('splits a model across GPUs in proportion to their free memory', () => {
		const result = formula(
			{},
			{
				devices: ['CUDA0', 'CUDA1'],
				gpus: [gpu('CUDA0', 3 * GIB), gpu('CUDA1', GIB)]
			}
		);

		expect(result.devices.map((device) => device.device)).toEqual(['CUDA0', 'CUDA1']);
		expect(result.devices[0].need.model).toBe(5 * LAYER);
		expect(result.devices[1].need.model).toBe(OUTPUT);
		expect(result.devices[1].need.context).toBe(0);
	});

	it('runs on the CPU when the device is none', () => {
		const result = formula({ [AdminSettingKey.DEVICE]: 'none' });

		expect(result.verdict).toBe(AdminFitVerdict.CPU_ONLY);
		expect(result.devices).toEqual([]);
		expect(result.host?.model).toBe(EMBD + 5 * LAYER + OUTPUT);
	});

	it('gives fits, tight and no against the margin', () => {
		const need = 5 * LAYER + OUTPUT + 640 * 1024;
		const compute = roughComputeBytes(FACTS, {
			cacheType: 'f16',
			cells: 1024,
			flashAttn: null,
			ubatch: 512
		});
		const total = need + compute;

		expect(formula({}, { gpus: [gpu('CUDA0', total + 2 * MIB)], margin: MIB }).verdict).toBe(
			AdminFitVerdict.FITS
		);
		expect(formula({}, { gpus: [gpu('CUDA0', total + MIB / 2)], margin: MIB }).verdict).toBe(
			AdminFitVerdict.TIGHT
		);
		expect(formula({}, { gpus: [gpu('CUDA0', total - 1)], margin: MIB }).verdict).toBe(
			AdminFitVerdict.NO
		);
	});

	it('adds back what the model uses now on that GPU', () => {
		const result = formula({}, { gpus: [gpu('CUDA0', 0, { ownUsage: 2 * GIB })], margin: MIB });

		expect(result.devices[0].available).toBe(2 * GIB);
		expect(result.verdict).toBe(AdminFitVerdict.FITS);
	});

	it('is unknown while memory settles, without a reading, or without facts', () => {
		expect(formula({}, { gpus: [gpu('CUDA0', 8 * GIB, { settling: true })] }).verdict).toBe(
			AdminFitVerdict.UNKNOWN
		);
		expect(formula({}, { gpus: [gpu('CUDA0', null)] }).verdict).toBe(AdminFitVerdict.UNKNOWN);
		expect(formula({}, { gpus: [] }).verdict).toBe(AdminFitVerdict.UNKNOWN);
		expect(formula({}, { gpus: [gpu('CUDA0', 8 * GIB, { memorySource: 'none' })] }).verdict).toBe(
			AdminFitVerdict.UNKNOWN
		);
		expect(formula({}, { facts: null }).verdict).toBe(AdminFitVerdict.UNKNOWN);
		expect(
			formula({}, { facts: makeFacts({ kv_bytes_per_token: null, kv_estimate: 'unsupported' }) })
				.verdict
		).toBe(AdminFitVerdict.UNKNOWN);
	});

	it('says when free memory is an old inventory reading', () => {
		const result = formula(
			{},
			{
				devices: ['ROCm0'],
				gpus: [gpu('ROCm0', 8 * GIB, { memoryAgeMs: 5 * 60_000, memorySource: 'inventory' })]
			}
		);

		expect(result.verdict).toBe(AdminFitVerdict.FITS);
		expect(result.notes.join(' ')).toContain('measured 5 min ago');
	});

	it('adds attention scores without flash attention and the f16 copy for a quantized cache', () => {
		const base = roughComputeBytes(FACTS, {
			cacheType: 'f16',
			cells: 1024,
			flashAttn: 'on',
			ubatch: 512
		});
		const noFa = roughComputeBytes(FACTS, {
			cacheType: 'f16',
			cells: 1024,
			flashAttn: 'off',
			ubatch: 512
		});
		const q8 = roughComputeBytes(FACTS, {
			cacheType: 'q8_0',
			cells: 1024,
			flashAttn: 'on',
			ubatch: 512
		});

		expect(noFa - base).toBe(4 * 512 * 1024 * 8);
		expect(q8 - base).toBe(2 * 4 * 8 * 2 * 1024);
	});
});

describe('verdicts', () => {
	it('compares need plus margin with the available memory', () => {
		expect(fitVerdict(10, 20, 5)).toBe(AdminFitVerdict.FITS);
		expect(fitVerdict(10, 12, 5)).toBe(AdminFitVerdict.TIGHT);
		expect(fitVerdict(10, 9, 5)).toBe(AdminFitVerdict.NO);
	});

	it('takes the worst verdict like the server', () => {
		expect(worstFitVerdict([])).toBe(AdminFitVerdict.CPU_ONLY);
		expect(worstFitVerdict([AdminFitVerdict.FITS])).toBe(AdminFitVerdict.FITS);
		expect(worstFitVerdict([AdminFitVerdict.FITS, AdminFitVerdict.TIGHT])).toBe(
			AdminFitVerdict.TIGHT
		);
		expect(worstFitVerdict([AdminFitVerdict.TIGHT, AdminFitVerdict.UNKNOWN])).toBe(
			AdminFitVerdict.UNKNOWN
		);
		expect(worstFitVerdict([AdminFitVerdict.UNKNOWN, AdminFitVerdict.NO])).toBe(AdminFitVerdict.NO);
	});
});

describe('exact check answer', () => {
	const check: ApiAdminFitCheck = {
		cached: false,
		checked_at: '2026-10-06T10:00:00Z',
		devices: [
			{
				available: 9 * GIB,
				description: 'NVIDIA GeForce GTX 1080 Ti',
				device: 'CUDA1',
				free: 8 * GIB,
				free_source: 'vendor_tool',
				need: {
					compute: 300 * MIB,
					context: 2 * GIB,
					model: 5 * GIB,
					total: 5 * GIB + 2 * GIB + 300 * MIB
				},
				own_usage: GIB,
				own_usage_source: 'reported',
				settling: false,
				total: 11 * GIB,
				verdict: 'tight'
			}
		],
		error: null,
		host: { compute: 1, context: 2, model: 3 },
		host_bytes: 6,
		margin: GIB,
		model: 'qwen',
		n_ctx: 32768,
		ok: true,
		verdict: 'tight',
		warnings: ['field_locked:device', 'mmproj_not_included']
	};

	it('becomes the indicator view model', () => {
		const result = fitFromCheck(check);

		expect(result.source).toBe(AdminFitSource.CHECK);
		expect(result.verdict).toBe(AdminFitVerdict.TIGHT);
		expect(result.devices[0].ownUsage).toBe(GIB);
		expect(result.host?.total).toBe(6);
		expect(result.nCtx).toBe(32768);
		expect(result.notes).toHaveLength(2);
	});

	it('keeps the error of a rejected dry run and is not a verdict', () => {
		const result = fitFromCheck({
			...check,
			devices: [],
			error: {
				code: 'invalid_settings',
				line: 'x',
				message: 'quantized V cache requires flash attention'
			},
			ok: false,
			verdict: 'unknown'
		});

		expect(result.error?.code).toBe('invalid_settings');
		expect(result.verdict).toBe(AdminFitVerdict.UNKNOWN);
		expect(describeFit(result).title).toBe('llama.cpp rejected these settings');
	});

	it('titles an error by what it means: rejected, no memory, missing GPU, or not run', () => {
		const failed = (code: string) =>
			fitFromCheck({
				...check,
				devices: [],
				error: { code, line: '', message: 'm' },
				ok: false,
				verdict: 'unknown'
			});

		for (const code of ['invalid_settings', 'invalid_device', 'model_load_failed']) {
			expect(fitCheckErrorKind(code)).toBe(AdminFitErrorKind.REJECTED);
			expect(describeFit(failed(code)).title).toBe('llama.cpp rejected these settings');
		}

		for (const code of ['timeout', 'failed', 'unsupported']) {
			expect(fitCheckErrorKind(code)).toBe(AdminFitErrorKind.NOT_CHECKED);
			expect(describeFit(failed(code)).title).toBe('The exact check could not run');
		}

		expect(describeFit(failed('device_missing')).title).toBe('The GPU is not on this server');
		expect(describeFit(failed('out_of_memory')).title).toBe(
			'llama.cpp could not fit these settings into the memory'
		);
	});

	it('has no free memory for a device without a reading, but keeps its need', () => {
		const result = fitFromCheck({
			...check,
			devices: [
				{ ...check.devices[0], available: 0, free: 0, own_usage: 0, total: 0, verdict: 'unknown' }
			],
			verdict: 'unknown'
		});

		expect(result.devices[0].available).toBeNull();
		expect(result.devices[0].free).toBeNull();
		expect(result.devices[0].need.total).toBe(check.devices[0].need.total);
		expect(describeFit(result).title).toBe('Free memory of CUDA1 is unknown');
	});

	it('maps an unknown verdict word to unknown', () => {
		expect(fitFromCheck({ ...check, verdict: 'maybe' }).verdict).toBe(AdminFitVerdict.UNKNOWN);
	});
});

describe('fit texts', () => {
	it('names the device and the bytes left or missing', () => {
		expect(
			describeFit(formula({}, { devices: ['CUDA1'], gpus: [gpu('CUDA1', 8 * GIB)] })).title
		).toMatch(/^Fits on CUDA1: .* left$/);
		expect(describeFit(formula({}, { gpus: [gpu('CUDA0', 1000)] })).title).toMatch(
			/^Probably does not fit on CUDA0: short by /
		);
		expect(
			describeFit(formula({}, { gpus: [gpu('CUDA0', 8 * GIB, { settling: true })] })).title
		).toContain('settling');
		expect(describeFit(formula({ [AdminSettingKey.DEVICE]: 'none' })).title).toBe(
			'Runs on the CPU only'
		);
	});
});

describe('GPU state for the formula', () => {
	it('adds own usage only for a running model', () => {
		const gpus = makeGpus([
			makeGpu('CUDA0', {
				models: [
					{
						all_gpus: false,
						facts_status: 'ready',
						id: 'tinyA',
						memory: { by: 'formula', bytes: 123, source: 'estimate' },
						pinned: false,
						status: 'loaded'
					}
				]
			}),
			makeGpu('CUDA1', {
				memory_age_ms: 60_000,
				memory_source: 'inventory',
				models: [
					{
						all_gpus: false,
						facts_status: 'ready',
						id: 'tinyA',
						memory: null,
						pinned: false,
						status: 'unloaded'
					}
				]
			})
		]);
		const states = fitGpuStates(gpus, 'tinyA');

		expect(states[0].ownUsage).toBe(123);
		expect(states[1].ownUsage).toBe(0);
		expect(states[1].memoryAgeMs).toBe(60_000);
		expect(fitGpuStates(null, 'x')).toEqual([]);
		expect(makeRow().id).toBe('tinyA');
	});
});
