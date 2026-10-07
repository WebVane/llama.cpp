import { makeFacts, makeGpu, makeGpus, makeRow, SCHEMA } from './admin-fixtures';
import { ADMIN_SETTINGS_FILE } from '$lib/constants';
import { AdminExportMode, AdminImportIssue } from '$lib/enums';
import type { AdminSettingsExportFile } from '$lib/types/admin';
import {
	describeGpuBackend,
	describeGpuMapping,
	describeGpuMemorySource,
	describeProbeState,
	formatDiagnosticsReport,
	gpuBarSegments,
	summarizeDiagnosticValue
} from '$lib/utils/admin-gpus';
import { createModelSettingsDraft } from '$lib/utils/admin-model-settings';
import {
	applyImportToDraft,
	buildImportPreview,
	buildSettingsExport,
	parseSettingsFile,
	settingsFileName
} from '$lib/utils/admin-transfer';
import { describe, expect, it } from 'vitest';

const DEVICES = ['CUDA0', 'CUDA1'];
const NOW = new Date('2026-10-06T10:00:00Z');

function file(models: AdminSettingsExportFile['models']): string {
	return JSON.stringify({
		exported_at: NOW.toISOString(),
		format: ADMIN_SETTINGS_FILE.FORMAT,
		models,
		version: 1
	});
}

function parsed(text: string): AdminSettingsExportFile {
	const result = parseSettingsFile(text);

	if (!result.ok) throw new Error(result.error);

	return result.file;
}

describe('export', () => {
	const row = makeRow({
		id: 'bartowski/Qwen2.5-7B-Instruct-GGUF:Q5_K_M',
		inherited: { 'ctx-size': '8192', 'n-gpu-layers': '99' },
		overrides: {
			'cache-type-k': 'q8_0',
			'cache-type-v': 'q8_0',
			'ctx-size': '32768',
			embeddings: 'false'
		}
	});

	it('names the file after the model id', () => {
		expect(settingsFileName(row.id)).toBe(
			'bartowski_Qwen2.5-7B-Instruct-GGUF_Q5_K_M.settings.json'
		);
		expect(settingsFileName('qwen2.5-chat')).toBe('qwen2.5-chat.settings.json');
		expect(settingsFileName('///')).toBe('model.settings.json');
	});

	it('writes only the custom settings by default, in the POST shape', () => {
		const out = buildSettingsExport(row, AdminExportMode.CUSTOM, NOW);

		expect(out).toMatchObject({
			exported_at: NOW.toISOString(),
			format: ADMIN_SETTINGS_FILE.FORMAT,
			version: 1
		});
		expect(out.models[0].id).toBe(row.id);
		expect(out.models[0].settings).toEqual({
			'cache-type-k': 'q8_0',
			'cache-type-v': 'q8_0',
			'ctx-size': 32768,
			embeddings: false
		});
		expect(out.models[0].about).toEqual({
			arch: 'llama',
			file: 'tinyA.gguf',
			n_ctx_train: 2048,
			quant: 'F32'
		});
	});

	it('writes every value the model gets with "all current values"', () => {
		const out = buildSettingsExport(row, AdminExportMode.ALL, NOW);

		expect(out.models[0].settings).toMatchObject({ 'ctx-size': 32768, 'n-gpu-layers': '99' });
		expect(out.models[0].settings).not.toHaveProperty('device');
	});

	it('round-trips through the parser', () => {
		const text = JSON.stringify(buildSettingsExport(row, AdminExportMode.CUSTOM, NOW));

		expect(parseSettingsFile(text).ok).toBe(true);
	});
});

describe('parse', () => {
	it('refuses broken JSON, another format, another version and empty files', () => {
		expect(parseSettingsFile('{oops')).toEqual({ error: 'The file is not valid JSON.', ok: false });
		expect(parseSettingsFile('{"format":"something-else","version":1}')).toMatchObject({
			ok: false
		});
		expect(
			parseSettingsFile(
				JSON.stringify({ format: ADMIN_SETTINGS_FILE.FORMAT, models: [], version: 2 })
			)
		).toMatchObject({ error: expect.stringContaining('version 2'), ok: false });
		expect(
			parseSettingsFile(
				JSON.stringify({ format: ADMIN_SETTINGS_FILE.FORMAT, models: [], version: 1 })
			)
		).toMatchObject({ error: 'The file holds no model settings.', ok: false });
		expect(parseSettingsFile('x'.repeat(ADMIN_SETTINGS_FILE.MAX_BYTES + 1))).toMatchObject({
			ok: false
		});
		expect(parseSettingsFile('[1,2]')).toMatchObject({ ok: false });
	});

	it('accepts a byte order mark and drops malformed entries', () => {
		const text =
			'\uFEFF' + file([{ about: {}, id: 'a', settings: { 'ctx-size': 4096 } }, { id: 5 } as never]);

		expect(parsed(text).models).toHaveLength(1);
	});

	it('keeps only text and a whole trained context in "about" (the file is untrusted)', () => {
		const text = JSON.stringify({
			format: ADMIN_SETTINGS_FILE.FORMAT,
			models: [
				{
					about: { arch: ['llama'], file: { name: 'x' }, n_ctx_train: '4096', quant: 5 },
					id: 'tinyA',
					settings: { 'ctx-size': 1024 }
				},
				{ about: 'text', id: 'tinyB', settings: {} },
				{
					about: { arch: 'llama', file: 'a.gguf', n_ctx_train: 2048, quant: 'Q8_0' },
					id: 'tinyC',
					settings: {}
				}
			],
			version: 1
		});
		const models = parsed(text).models;

		expect(models[0].about).toEqual({});
		expect(models[1].about).toEqual({});
		expect(models[2].about).toEqual({
			arch: 'llama',
			file: 'a.gguf',
			n_ctx_train: 2048,
			quant: 'Q8_0'
		});

		// such a file previews like any other instead of throwing
		const preview = buildImportPreview(parsed(text), makeRow({ id: 'tinyA' }), SCHEMA, DEVICES);

		expect(preview.differentModel).toBe(false);
		expect(preview.fields.map((field) => field.key)).toEqual(['ctx-size']);
	});
});

describe('import preview', () => {
	const row = makeRow({
		facts: makeFacts({ n_ctx_train: 32768 }),
		id: 'qwen2.5-chat',
		inherited: { 'ctx-size': '8192', device: 'CUDA1' }
	});

	it('previews current -> from file for the same model', () => {
		const preview = buildImportPreview(
			parsed(
				file([
					{
						about: { arch: 'llama', file: 'tinyA.gguf', quant: 'F32' },
						id: 'qwen2.5-chat',
						settings: { 'cache-type-k': 'q8_0', 'cache-type-v': 'q8_0', 'ctx-size': 16384 }
					}
				])
			),
			row,
			SCHEMA,
			DEVICES
		);

		expect(preview.differentModel).toBe(false);
		expect(
			preview.fields.map((field) => [field.key, field.current, field.value, field.selected])
		).toEqual([
			['cache-type-k', null, 'q8_0', true],
			['cache-type-v', null, 'q8_0', true],
			['ctx-size', '8192', '16384', true]
		]);
	});

	it('warns about a different model', () => {
		const preview = buildImportPreview(
			parsed(
				file([
					{ about: { arch: 'qwen2', quant: 'Q5_K_M' }, id: 'other', settings: { 'ctx-size': 4096 } }
				])
			),
			row,
			SCHEMA,
			DEVICES
		);

		expect(preview.differentModel).toBe(true);
		expect(preview.differences).toHaveLength(3);
		expect(preview.sourceId).toBe('other');
	});

	it('unticks an unknown GPU, skips locked and invalid values, lists unknown keys', () => {
		const locked = makeRow({
			facts: makeFacts({ n_ctx_train: 32768 }),
			id: 'qwen2.5-chat',
			inherited: { device: 'CUDA1' },
			locked: { 'flash-attn': { by: 'router_cli', value: 'on' } }
		});
		const preview = buildImportPreview(
			parsed(
				file([
					{
						about: {},
						id: 'qwen2.5-chat',
						settings: {
							'ctx-size': 65536,
							device: 'CUDA3',
							'flash-attn': 'off',
							model: '/etc/passwd',
							parallel: 0,
							threads: 8
						}
					}
				])
			),
			locked,
			SCHEMA,
			DEVICES
		);
		const byKey = Object.fromEntries(preview.fields.map((field) => [field.key, field]));

		expect(byKey.device).toMatchObject({
			issues: [AdminImportIssue.DEVICE_UNKNOWN],
			selected: false,
			skipped: false
		});
		expect(byKey['flash-attn']).toMatchObject({ issues: [AdminImportIssue.LOCKED], skipped: true });
		expect(byKey.parallel).toMatchObject({
			issues: [AdminImportIssue.INVALID_VALUE],
			skipped: true
		});
		expect(byKey['ctx-size']).toMatchObject({
			issues: [AdminImportIssue.CTX_ABOVE_TRAIN],
			selected: true
		});
		expect(preview.unknownKeys).toEqual(['model', 'threads']);
	});

	it('loads only the ticked fields into the draft and keeps K and V equal', () => {
		const preview = buildImportPreview(
			parsed(
				file([
					{
						about: {},
						id: 'qwen2.5-chat',
						settings: { 'cache-type-k': 'q4_0', 'ctx-size': 8192, device: 'cuda0' }
					}
				])
			),
			row,
			SCHEMA,
			DEVICES
		);
		const draft = applyImportToDraft(createModelSettingsDraft(row), row, preview, DEVICES);

		expect(draft.values['cache-type-k']).toBe('q4_0');
		expect(draft.values['cache-type-v']).toBe('q4_0');
		// equal to the inherited value: nothing to save
		expect(draft.values['ctx-size']).toBeNull();
		expect(draft.values.device).toBe('CUDA0');

		preview.fields.find((field) => field.key === 'device')!.selected = false;
		expect(
			applyImportToDraft(createModelSettingsDraft(row), row, preview, DEVICES).values.device
		).toBeNull();
	});
});

describe('GPU cards', () => {
	const GB = 1024 * 1024 * 1024;

	it('splits the bar into models, other and free', () => {
		const segments = gpuBarSegments(
			makeGpu('CUDA1', {
				free: 2 * GB,
				models: [
					{
						all_gpus: false,
						facts_status: 'ready',
						id: 'qwen',
						memory: { by: 'model', bytes: 6 * GB, source: 'reported' },
						pinned: true,
						status: 'loaded'
					},
					{
						all_gpus: false,
						facts_status: 'ready',
						id: 'idle',
						memory: null,
						pinned: false,
						status: 'unloaded'
					}
				],
				total: 11 * GB,
				used: 9 * GB
			})
		);

		expect(segments.map((segment) => [segment.kind, segment.bytes])).toEqual([
			['model', 6 * GB],
			['other', 3 * GB],
			['free', 2 * GB]
		]);
		expect(segments[0].source).toBe('reported');
	});

	it('labels unexplained use as pending while a running model has no figure', () => {
		const segments = gpuBarSegments(
			makeGpu('CUDA0', {
				models: [
					{
						all_gpus: false,
						facts_status: 'pending',
						id: 'gte',
						memory: null,
						pinned: true,
						status: 'loaded'
					}
				]
			})
		);

		expect(segments[0]).toMatchObject({ kind: 'pending', modelId: 'gte' });
		expect(segments[0].label).toContain('estimate pending');

		// facts read but still no figure: never blamed on "other processes"
		const ready = gpuBarSegments(
			makeGpu('CUDA0', {
				models: [
					{
						all_gpus: false,
						facts_status: 'ready',
						id: 'gte',
						memory: null,
						pinned: true,
						status: 'loaded'
					}
				]
			})
		);

		expect(ready[0].kind).toBe('pending');
		expect(ready[0].label).toContain('not broken down');
	});

	it('clamps other at zero and scales estimates above the used memory', () => {
		const segments = gpuBarSegments(
			makeGpu('CUDA0', {
				free: 9 * GB,
				models: [
					{
						all_gpus: true,
						facts_status: 'ready',
						id: 'big',
						memory: { by: 'formula', bytes: 4 * GB, source: 'estimate' },
						pinned: false,
						status: 'loaded'
					}
				],
				total: 11 * GB,
				used: 2 * GB
			})
		);

		expect(segments.map((segment) => segment.kind)).toEqual(['model', 'free']);
		expect(segments[0].fraction).toBeCloseTo(2 / 11, 6);
	});

	it('has no bar without a total', () => {
		expect(gpuBarSegments(makeGpu('Vulkan0', { free: null, total: null, used: null }))).toEqual([]);
	});

	it('says where memory comes from and how it was matched', () => {
		expect(describeGpuMemorySource(makeGpu('CUDA0'))).toBe('live (nvidia-smi)');
		expect(
			describeGpuMemorySource(
				makeGpu('ROCm0', { memory_age_ms: 120_000, memory_source: 'inventory', tool: null })
			)
		).toBe('measured 2 min ago, no live reading');
		expect(describeGpuMapping(makeGpu('CUDA0'))).toBeNull();
		expect(describeGpuMapping(makeGpu('CUDA0', { mapping: 'single' }))).toBe(
			'matched as the only GPU'
		);
		expect(describeGpuMapping(makeGpu('CUDA0', { mapping: 'unknown' }))).toContain('not matched');
	});

	it('tells a CPU-only build from a GPU backend without devices', () => {
		expect(
			describeGpuBackend(
				makeGpus([], {
					gpu_backend: { backends: [], libraries: [], message: '', state: 'cpu_only' }
				})
			)?.title
		).toBe('CPU only');
		expect(
			describeGpuBackend(
				makeGpus([], {
					gpu_backend: {
						backends: [],
						libraries: ['libggml-cuda.so'],
						message: '',
						state: 'no_devices'
					}
				})
			)?.text
		).toContain('/dev/kfd');
		expect(describeGpuBackend(makeGpus([makeGpu('CUDA0')]))).toBeNull();
	});

	it('explains a missing vendor tool with the Docker hint for NVIDIA', () => {
		const gpus = makeGpus([makeGpu('CUDA0')]);

		gpus.probe = { ...gpus.probe, error: 'not_found', ok: false };
		expect(describeProbeState(gpus)).toContain('NVIDIA_DRIVER_CAPABILITIES');

		const amd = makeGpus([makeGpu('ROCm0', { vendor: 'amd' })]);

		amd.probe = { ...amd.probe, error: 'not_found', ok: false };
		expect(describeProbeState(amd)).not.toContain('NVIDIA_DRIVER_CAPABILITIES');
		expect(describeProbeState(makeGpus([makeGpu('CUDA0')]))).toBeNull();
	});
});

describe('diagnostics report', () => {
	it('renders every item as text with its hint', () => {
		const diagnostics = {
			generated_at: '2026-10-06T10:00:00Z',
			items: [
				{
					hint: 'running directly on the host',
					id: 'deployment',
					label: 'Deployment',
					status: 'ok',
					value: 'host'
				},
				{
					hint: '',
					id: 'binary',
					label: 'Server binary',
					status: 'ok',
					value: { path: '/app/provision-server', version: 'b1' }
				},
				{ hint: 'not set', id: 'model_devices', label: 'MODEL_DEVICES', status: 'ok', value: null },
				{ hint: 'x', id: 'proxy', label: 'Proxy', status: 'info', value: false }
			]
		};
		const text = formatDiagnosticsReport(diagnostics, ['page: http://x']);

		expect(text).toContain('[OK] Deployment: host');
		expect(text).toContain('    running directly on the host');
		expect(text).toContain('[OK] Server binary: path: /app/provision-server; version: b1');
		expect(text).toContain('[OK] MODEL_DEVICES: —');
		expect(text).toContain('[INFO] Proxy: no');
		expect(text).toContain('page: http://x');
		expect(summarizeDiagnosticValue(diagnostics.items[1])).toBe('b1');
	});
});
