import {
	AdminDownloadOutcome,
	AdminDownloadWarning,
	AdminNoticeLevel,
	ServerModelSource,
	ServerModelStatus
} from '$lib/enums';
import type { AdminDownload, ApiAdminDownloadCheck, ApiAdminSettings } from '$lib/types/admin';
import type { ApiModelDataEntry } from '$lib/types/api';
import {
	buildAdminModelRows,
	buildSettingsUpdate,
	cacheModelIds,
	countRunningModels,
	createSettingsDraft,
	describeDownloadWarnings,
	formatBytes,
	isCacheModelOfDownload,
	isDownloadPlaceholderStatus,
	isValidDeviceSpec,
	isValidHfModelSpec,
	maxPinnedModels,
	normalizeDeviceSpec,
	normalizeHfModelSpec,
	parseModelsMax,
	rebaseSettingsDraft,
	resolveDownloadResult,
	sumDownloadProgress,
	validateSettingsDraft
} from '$lib/utils/admin';
import { describe, expect, it } from 'vitest';

const KB = 1024;
const MB = KB * 1024;
const GB = MB * 1024;

function model(
	id: string,
	status: ServerModelStatus = ServerModelStatus.UNLOADED,
	source: ServerModelSource = ServerModelSource.MODELS_DIR,
	extra: Partial<ApiModelDataEntry> = {}
): ApiModelDataEntry {
	return {
		created: 0,
		id,
		in_cache: false,
		object: 'model',
		owned_by: 'llamacpp',
		path: '',
		source,
		status: { value: status },
		...extra
	} as ApiModelDataEntry;
}

function settings(overrides: Partial<ApiAdminSettings> = {}): ApiAdminSettings {
	return {
		default_device: '',
		default_device_locked: false,
		known_devices: ['CUDA0', 'CUDA1'],
		limits: { models_max_max: 16, models_max_min: 1 },
		models: [
			{
				id: 'a',
				pinned: false,
				source: ServerModelSource.PRESET,
				status: ServerModelStatus.UNLOADED
			},
			{
				id: 'b',
				pinned: false,
				source: ServerModelSource.CACHE,
				status: ServerModelStatus.UNLOADED
			},
			{
				id: 'c',
				pinned: false,
				source: ServerModelSource.MODELS_DIR,
				status: ServerModelStatus.UNLOADED
			}
		],
		models_max: 3,
		pinned: [],
		...overrides
	};
}

function check(overrides: Partial<ApiAdminDownloadCheck> = {}): ApiAdminDownloadCheck {
	return {
		already_present: false,
		enough_space: true,
		exists: true,
		files: [{ bytes: 2 * GB, name: 'model-Q4_K_M.gguf' }],
		free_bytes: 100 * GB,
		hf_reachable: true,
		model: 'user/repo:Q4_K_M',
		quant: 'Q4_K_M',
		repo: 'user/repo',
		total_bytes: 2 * GB,
		warnings: [],
		...overrides
	};
}

describe('formatBytes', () => {
	it('prints bytes without decimals', () => {
		expect(formatBytes(0)).toBe('0 B');
		expect(formatBytes(1)).toBe('1 B');
		expect(formatBytes(1023)).toBe('1023 B');
	});

	it('uses binary multiples with three significant digits', () => {
		expect(formatBytes(KB)).toBe('1.00 KB');
		expect(formatBytes(1536)).toBe('1.50 KB');
		expect(formatBytes(1185376)).toBe('1.13 MB');
		expect(formatBytes(47 * MB)).toBe('47.0 MB');
		expect(formatBytes(2497280256)).toBe('2.33 GB');
		expect(formatBytes(42611642368)).toBe('39.7 GB');
		expect(formatBytes(499904409600)).toBe('466 GB');
		expect(formatBytes(3 * 1024 * GB)).toBe('3.00 TB');
	});

	it('never prints 1024 of a unit or four digits after rounding', () => {
		expect(formatBytes(MB - 1)).toBe('1.00 MB');
		expect(formatBytes(GB - 1)).toBe('1.00 GB');
		expect(formatBytes(999.96 * MB)).toBe('1000 MB');
		expect(formatBytes(99.96 * MB)).toBe('100 MB');
		expect(formatBytes(9.996 * MB)).toBe('10.0 MB');
	});

	it('stops at the largest unit', () => {
		expect(formatBytes(5000 * 1024 * 1024 * GB)).toBe('5000 PB');
	});

	it('renders anything that is not a size as a dash', () => {
		expect(formatBytes(-1)).toBe('—');
		expect(formatBytes(Number.NaN)).toBe('—');
		expect(formatBytes(Number.POSITIVE_INFINITY)).toBe('—');
		expect(formatBytes(undefined)).toBe('—');
		expect(formatBytes(null)).toBe('—');
		expect(formatBytes('5')).toBe('—');
	});
});

describe('normalizeHfModelSpec', () => {
	it('trims and upper-cases only the quantization part', () => {
		expect(normalizeHfModelSpec('  Qwen/Qwen3-4B-GGUF:q4_k_m ')).toBe('Qwen/Qwen3-4B-GGUF:Q4_K_M');
		expect(normalizeHfModelSpec('ggml-org/test-model-stories260K:f32')).toBe(
			'ggml-org/test-model-stories260K:F32'
		);
	});

	it('leaves a spec without a quantization alone', () => {
		expect(normalizeHfModelSpec('ggml-org/tiny-llamas')).toBe('ggml-org/tiny-llamas');
		expect(normalizeHfModelSpec('')).toBe('');
	});

	it('accepts a pasted Hugging Face page URL', () => {
		expect(normalizeHfModelSpec('https://huggingface.co/Qwen/Qwen3-4B-GGUF')).toBe(
			'Qwen/Qwen3-4B-GGUF'
		);
		expect(
			normalizeHfModelSpec('https://huggingface.co/Qwen/Qwen3-4B-GGUF/tree/main?not-for-all=1#x')
		).toBe('Qwen/Qwen3-4B-GGUF');
		expect(normalizeHfModelSpec('http://hf.co/user/repo/')).toBe('user/repo');
	});
});

describe('isValidHfModelSpec', () => {
	it('accepts user/repo with an optional quantization', () => {
		expect(isValidHfModelSpec('user/repo')).toBe(true);
		expect(isValidHfModelSpec('Qwen/Qwen3-4B-GGUF:Q4_K_M')).toBe(true);
		expect(isValidHfModelSpec('ggml-org/models.v2:F32')).toBe(true);
	});

	it('rejects everything else', () => {
		for (const spec of [
			'',
			'user',
			'user/',
			'/repo',
			'user/repo:',
			'user/repo/extra',
			'user/re po',
			'user/repo:Q4 K',
			'-user/repo',
			'user/.repo',
			'user/repo:Q4:K',
			'https://huggingface.co/user/repo'
		]) {
			expect(isValidHfModelSpec(spec), spec).toBe(false);
		}
	});
});

describe('describeDownloadWarnings', () => {
	it('returns nothing for a clean check', () => {
		expect(describeDownloadWarnings(check())).toEqual([]);
	});

	it('reports missing space with both sizes', () => {
		const [warning] = describeDownloadWarnings(
			check({
				enough_space: false,
				free_bytes: 1 * GB,
				total_bytes: 5 * GB,
				warnings: [AdminDownloadWarning.NOT_ENOUGH_SPACE]
			})
		);

		expect(warning.code).toBe('not_enough_space');
		expect(warning.level).toBe(AdminNoticeLevel.WARNING);
		expect(warning.text).toBe('Not enough free space: 5.00 GB needed, 1.00 GB available.');
	});

	it('maps every known code to a sentence, in the order of the server', () => {
		const codes = [
			AdminDownloadWarning.HF_UNREACHABLE,
			AdminDownloadWarning.REPO_NOT_FOUND,
			AdminDownloadWarning.NO_GGUF_FOR_QUANT,
			AdminDownloadWarning.QUANT_NOT_SPECIFIED,
			AdminDownloadWarning.NOT_ENOUGH_SPACE,
			AdminDownloadWarning.ALREADY_PRESENT,
			AdminDownloadWarning.OFFLINE_MODE,
			AdminDownloadWarning.PRESET_REPOSITORY,
			AdminDownloadWarning.SIZE_UNKNOWN
		];
		const warnings = describeDownloadWarnings(check({ warnings: codes }));

		expect(warnings.map((warning) => warning.code)).toEqual(codes);

		for (const warning of warnings) {
			expect(warning.text.length, warning.code).toBeGreaterThan(20);
			expect(warning.text, warning.code).not.toContain(warning.code);
		}

		expect(warnings.find((w) => w.code === 'hf_unreachable')?.text).toContain('Hugging Face');
		expect(warnings.find((w) => w.code === 'repo_not_found')?.text).toContain(
			'Repository not found'
		);
		expect(warnings.find((w) => w.code === 'offline_mode')?.text).toContain('offline');
	});

	it('explains that a preset repository adds no model', () => {
		const [warning] = describeDownloadWarnings(
			check({
				files: [{ bytes: 120, name: 'preset.ini' }],
				total_bytes: 120,
				warnings: [AdminDownloadWarning.PRESET_REPOSITORY]
			})
		);

		expect(warning).toEqual({
			code: 'preset_repository',
			level: AdminNoticeLevel.WARNING,
			text: 'This repository holds a preset (preset.ini): only that file is downloaded, and no model will appear in the list.'
		});
	});

	it('marks informational codes as info and the rest as warnings', () => {
		const warnings = describeDownloadWarnings(
			check({
				warnings: [
					AdminDownloadWarning.ALREADY_PRESENT,
					AdminDownloadWarning.QUANT_NOT_SPECIFIED,
					AdminDownloadWarning.REPO_NOT_FOUND
				]
			})
		);

		expect(warnings.map((warning) => warning.level)).toEqual([
			AdminNoticeLevel.INFO,
			AdminNoticeLevel.INFO,
			AdminNoticeLevel.WARNING
		]);
	});

	it('names the quantization that is missing or that the server picks', () => {
		const [missing] = describeDownloadWarnings(
			check({ quant: 'Q9_X', warnings: [AdminDownloadWarning.NO_GGUF_FOR_QUANT] })
		);
		const [missingAny] = describeDownloadWarnings(
			check({ quant: '', warnings: [AdminDownloadWarning.NO_GGUF_FOR_QUANT] })
		);
		const [picked] = describeDownloadWarnings(
			check({
				quant: '',
				resolved_quant: 'Q4_K_M',
				warnings: [AdminDownloadWarning.QUANT_NOT_SPECIFIED]
			})
		);
		const [pickedUnknown] = describeDownloadWarnings(
			check({ quant: '', warnings: [AdminDownloadWarning.QUANT_NOT_SPECIFIED] })
		);

		expect(missing.text).toContain('Q9_X');
		expect(missingAny.text).toBe('The repository has no GGUF file to download.');
		expect(picked.text).toContain('Q4_K_M');
		expect(pickedUnknown.text).toContain('pick one itself');
	});

	it('passes an unknown code through instead of hiding it', () => {
		const [warning] = describeDownloadWarnings(check({ warnings: ['brand_new_code'] }));

		expect(warning).toEqual({
			code: 'brand_new_code',
			level: AdminNoticeLevel.WARNING,
			text: 'Warning: brand_new_code'
		});
	});
});

describe('sumDownloadProgress', () => {
	it('has no fraction before the first counters arrive', () => {
		expect(sumDownloadProgress(undefined)).toEqual({ done: 0, fraction: null, total: 0 });
		expect(sumDownloadProgress({})).toEqual({ done: 0, fraction: null, total: 0 });
	});

	it('sums all files of a download', () => {
		expect(
			sumDownloadProgress({
				'https://hf/a.gguf': { done: 50, total: 100 },
				'https://hf/mmproj.gguf': { done: 100, total: 100 }
			})
		).toEqual({ done: 150, fraction: 0.75, total: 200 });
	});

	it('keeps the fraction unknown while one file has no total yet', () => {
		expect(
			sumDownloadProgress({
				a: { done: 50, total: 100 },
				b: { done: 0, total: 0 }
			})
		).toEqual({ done: 50, fraction: null, total: 100 });
	});

	it('never exceeds 100%', () => {
		expect(sumDownloadProgress({ a: { done: 120, total: 100 } }).fraction).toBe(1);
	});
});

describe('resolveDownloadResult', () => {
	const before: Pick<AdminDownload, 'cacheIdsBefore' | 'cancelRequested' | 'name'> = {
		cacheIdsBefore: ['user/old:Q4_K_M'],
		cancelRequested: false,
		name: 'user/repo:Q4_K_M'
	};

	it('reports success only when a new cache model appeared', () => {
		const after = [
			model('user/old:Q4_K_M', ServerModelStatus.UNLOADED, ServerModelSource.CACHE),
			model('user/repo:Q4_K_M', ServerModelStatus.UNLOADED, ServerModelSource.CACHE),
			model('local', ServerModelStatus.LOADED, ServerModelSource.MODELS_DIR)
		];

		expect(resolveDownloadResult(before, after, false)).toEqual({
			newModelIds: ['user/repo:Q4_K_M'],
			outcome: AdminDownloadOutcome.SUCCESS
		});
	});

	it('reports "nothing" for download_finished without a new model', () => {
		const after = [model('user/old:Q4_K_M', ServerModelStatus.UNLOADED, ServerModelSource.CACHE)];

		expect(resolveDownloadResult(before, after, false).outcome).toBe(AdminDownloadOutcome.NOTHING);
	});

	it('reports a failure for download_failed', () => {
		expect(resolveDownloadResult(before, [], true).outcome).toBe(AdminDownloadOutcome.FAILED);
	});

	it('reports "cancelled" when the failure follows a Cancel of the user', () => {
		const cancelled = { ...before, cancelRequested: true };

		expect(resolveDownloadResult(cancelled, [], true).outcome).toBe(AdminDownloadOutcome.CANCELLED);
		expect(resolveDownloadResult(cancelled, [], false).outcome).toBe(
			AdminDownloadOutcome.CANCELLED
		);
	});

	it('does not count download placeholders or other sources as new models', () => {
		const after = [
			model('user/repo:Q4_K_M', ServerModelStatus.DOWNLOADING, ServerModelSource.CACHE),
			model('user/other', ServerModelStatus.DOWNLOADED, ServerModelSource.CACHE),
			model('dropped-in', ServerModelStatus.UNLOADED, ServerModelSource.MODELS_DIR)
		];

		expect(resolveDownloadResult(before, after, false).outcome).toBe(AdminDownloadOutcome.NOTHING);
	});

	it('still reports success when the model arrived despite a late Cancel', () => {
		const after = [model('user/repo:Q4_K_M', ServerModelStatus.UNLOADED, ServerModelSource.CACHE)];

		expect(
			resolveDownloadResult(
				{ cacheIdsBefore: [], cancelRequested: true, name: 'user/repo:Q4_K_M' },
				after,
				true
			).outcome
		).toBe(AdminDownloadOutcome.SUCCESS);
	});

	describe('with another download finishing meanwhile', () => {
		const big = 'Qwen/Qwen3-4B-GGUF:Q4_K_M';
		const small = 'ggml-org/test-model-stories260K:F32';
		// the list after the big download ended: the small one finished while it ran
		const after = [
			model('user/old:Q4_K_M', ServerModelStatus.UNLOADED, ServerModelSource.CACHE),
			model(small, ServerModelStatus.UNLOADED, ServerModelSource.CACHE)
		];
		const bigDownload = { cacheIdsBefore: ['user/old:Q4_K_M'], cancelRequested: false, name: big };

		it('reports a Cancel as cancelled, not as the other download', () => {
			expect(resolveDownloadResult({ ...bigDownload, cancelRequested: true }, after, true)).toEqual(
				{ newModelIds: [], outcome: AdminDownloadOutcome.CANCELLED }
			);
		});

		it('reports a failure as failed', () => {
			expect(resolveDownloadResult(bigDownload, after, true)).toEqual({
				newModelIds: [],
				outcome: AdminDownloadOutcome.FAILED
			});
		});

		it('reports "nothing" when only the other download produced a model', () => {
			expect(resolveDownloadResult(bigDownload, after, false).outcome).toBe(
				AdminDownloadOutcome.NOTHING
			);
		});

		it('gives each of two finished downloads its own model', () => {
			const both = [...after, model(big, ServerModelStatus.UNLOADED, ServerModelSource.CACHE)];
			const smallDownload = { ...bigDownload, name: small };

			expect(resolveDownloadResult(bigDownload, both, false).newModelIds).toEqual([big]);
			expect(resolveDownloadResult(smallDownload, both, false).newModelIds).toEqual([small]);
		});

		it('does not count a model reported by the other download of the same repository', () => {
			const q8 = 'Qwen/Qwen3-4B-GGUF:Q8_0';
			const withQ8 = [...after, model(q8, ServerModelStatus.UNLOADED, ServerModelSource.CACHE)];
			// what the store does once the other download reported q8 as its result
			const claimed = { ...bigDownload, cacheIdsBefore: [...bigDownload.cacheIdsBefore, q8] };

			expect(resolveDownloadResult(bigDownload, withQ8, false).newModelIds).toEqual([q8]);
			expect(resolveDownloadResult({ ...claimed, cancelRequested: true }, withQ8, true)).toEqual({
				newModelIds: [],
				outcome: AdminDownloadOutcome.CANCELLED
			});
		});
	});

	it('does not report the whole cache when the list before the download was empty', () => {
		const after = [
			model('other/repo:Q4_K_M', ServerModelStatus.UNLOADED, ServerModelSource.CACHE),
			model('user/repo-v2:Q4_K_M', ServerModelStatus.UNLOADED, ServerModelSource.CACHE),
			model('user/repo:Q8_0', ServerModelStatus.UNLOADED, ServerModelSource.CACHE)
		];

		expect(
			resolveDownloadResult(
				{ cacheIdsBefore: [], cancelRequested: false, name: 'user/repo' },
				after,
				false
			)
		).toEqual({ newModelIds: ['user/repo:Q8_0'], outcome: AdminDownloadOutcome.SUCCESS });
		expect(
			resolveDownloadResult(
				{ cacheIdsBefore: [], cancelRequested: true, name: 'user/missing:Q4_K_M' },
				after,
				true
			).outcome
		).toBe(AdminDownloadOutcome.CANCELLED);
	});
});

describe('isCacheModelOfDownload', () => {
	it('matches the repository of the download, whatever the tag', () => {
		expect(isCacheModelOfDownload('user/repo:Q4_K_M', 'user/repo:Q4_K_M')).toBe(true);
		expect(isCacheModelOfDownload('user/repo:Q4_K_M', 'user/repo')).toBe(true);
		expect(isCacheModelOfDownload('user/repo:Q4_K_M', 'user/repo:K_M')).toBe(true);
	});

	it('ignores case like Hugging Face does', () => {
		expect(isCacheModelOfDownload('Qwen/Qwen3-4B-GGUF:Q4_K_M', 'qwen/qwen3-4b-gguf:Q4_K_M')).toBe(
			true
		);
	});

	it('does not match another repository, also one whose name starts the same', () => {
		expect(isCacheModelOfDownload('user/repo-v2:Q4_K_M', 'user/repo')).toBe(false);
		expect(isCacheModelOfDownload('user/rep:Q4_K_M', 'user/repo')).toBe(false);
		expect(isCacheModelOfDownload('other/repo:Q4_K_M', 'user/repo:Q4_K_M')).toBe(false);
	});
});

describe('cacheModelIds / isDownloadPlaceholderStatus / countRunningModels', () => {
	const models = [
		model('cache-a', ServerModelStatus.LOADED, ServerModelSource.CACHE),
		model('cache-dl', ServerModelStatus.DOWNLOADING, ServerModelSource.CACHE),
		model('preset-a', ServerModelStatus.LOADING, ServerModelSource.PRESET),
		model('dir-a', ServerModelStatus.SLEEPING, ServerModelSource.MODELS_DIR),
		model('dir-b', ServerModelStatus.UNLOADED, ServerModelSource.MODELS_DIR)
	];

	it('lists real cache models only', () => {
		expect(cacheModelIds(models)).toEqual(['cache-a']);
	});

	it('knows the two transient download statuses', () => {
		expect(isDownloadPlaceholderStatus(ServerModelStatus.DOWNLOADING)).toBe(true);
		expect(isDownloadPlaceholderStatus(ServerModelStatus.DOWNLOADED)).toBe(true);
		expect(isDownloadPlaceholderStatus(ServerModelStatus.LOADED)).toBe(false);
		expect(isDownloadPlaceholderStatus(undefined)).toBe(false);
	});

	it('counts loaded, loading and sleeping models as running', () => {
		expect(countRunningModels(models)).toBe(3);
	});
});

describe('buildAdminModelRows', () => {
	it('carries name, aliases, tags, source and pins', () => {
		const rows = buildAdminModelRows(
			[
				model('a', ServerModelStatus.LOADED, ServerModelSource.PRESET, {
					aliases: ['alias-a'],
					tags: ['chat']
				}),
				model('b')
			],
			{ pinned: ['a'] }
		);

		expect(rows[0]).toMatchObject({
			aliases: ['alias-a'],
			canDelete: false,
			download: null,
			exitCode: null,
			id: 'a',
			pinned: true,
			source: ServerModelSource.PRESET,
			status: ServerModelStatus.LOADED,
			tags: ['chat']
		});
		expect(rows[1]).toMatchObject({ aliases: [], pinned: false, tags: [] });
	});

	it('derives "failed" with the exit code from the list', () => {
		const [row] = buildAdminModelRows([
			model('broken', ServerModelStatus.UNLOADED, ServerModelSource.PRESET, {
				status: { exit_code: 3, failed: true, value: ServerModelStatus.UNLOADED }
			})
		]);

		expect(row.status).toBe(ServerModelStatus.FAILED);
		expect(row.exitCode).toBe(3);
	});

	it('lets the exit code seen on the feed win over the list', () => {
		const failedInList = model('m', ServerModelStatus.UNLOADED, ServerModelSource.PRESET, {
			status: { exit_code: 1, failed: true, value: ServerModelStatus.UNLOADED }
		});
		const [recovered] = buildAdminModelRows([failedInList], { exitCodes: new Map([['m', 0]]) });
		const [crashed] = buildAdminModelRows([model('m')], { exitCodes: new Map([['m', 137]]) });

		expect(recovered.status).toBe(ServerModelStatus.UNLOADED);
		expect(recovered.exitCode).toBeNull();
		expect(crashed.status).toBe(ServerModelStatus.FAILED);
		expect(crashed.exitCode).toBe(137);
	});

	it('ignores exit codes of a model that is running again', () => {
		const [row] = buildAdminModelRows([model('m', ServerModelStatus.LOADING)], {
			exitCodes: new Map([['m', 1]])
		});

		expect(row.status).toBe(ServerModelStatus.LOADING);
		expect(row.exitCode).toBeNull();
	});

	it('allows deleting cache models only, and not while they download', () => {
		const rows = buildAdminModelRows([
			model('cache', ServerModelStatus.LOADED, ServerModelSource.CACHE),
			model('downloading', ServerModelStatus.DOWNLOADING, ServerModelSource.CACHE),
			model('downloaded', ServerModelStatus.DOWNLOADED, ServerModelSource.CACHE),
			model('preset', ServerModelStatus.UNLOADED, ServerModelSource.PRESET),
			model('dir', ServerModelStatus.UNLOADED, ServerModelSource.MODELS_DIR)
		]);

		expect(rows.map((row) => row.canDelete)).toEqual([true, false, false, false, false]);
	});

	it('attaches download totals to a downloading row', () => {
		const downloads = new Map<string, AdminDownload>([
			[
				'user/repo:Q8_0',
				{
					cacheIdsBefore: [],
					cancelRequested: false,
					confirmed: true,
					files: { url: { done: 25, total: 100 } },
					name: 'user/repo:Q8_0'
				}
			]
		]);
		const rows = buildAdminModelRows(
			[
				model('user/repo:Q8_0', ServerModelStatus.DOWNLOADING, ServerModelSource.CACHE),
				model('user/unseen', ServerModelStatus.DOWNLOADING, ServerModelSource.CACHE)
			],
			{ downloads }
		);

		expect(rows[0].download).toEqual({ done: 25, fraction: 0.25, total: 100 });
		expect(rows[1].download).toEqual({ done: 0, fraction: null, total: 0 });
	});
});

describe('device spec', () => {
	it('normalises to the comma separated form the server stores', () => {
		expect(normalizeDeviceSpec(' CUDA0 ; CUDA1 ')).toBe('CUDA0,CUDA1');
		expect(normalizeDeviceSpec('CUDA0')).toBe('CUDA0');
		expect(normalizeDeviceSpec('   ')).toBe('');
	});

	it('accepts empty, none and lists of device names', () => {
		for (const spec of [
			'',
			'  ',
			'none',
			'CUDA0',
			'CUDA0,CUDA1',
			'CUDA0;CUDA1',
			'Vulkan0',
			'SYCL_1'
		]) {
			expect(isValidDeviceSpec(spec), spec).toBe(true);
		}
	});

	it('rejects malformed device lists', () => {
		for (const spec of [
			'cuda',
			'0CUDA',
			'CUDA0,,CUDA1',
			'CUDA0,',
			'none,CUDA0',
			'CUDA 0',
			'GPU-0'
		]) {
			expect(isValidDeviceSpec(spec), spec).toBe(false);
		}
	});
});

describe('parseModelsMax / maxPinnedModels', () => {
	it('reads whole numbers only', () => {
		expect(parseModelsMax('3')).toBe(3);
		expect(parseModelsMax(' 04 ')).toBe(4);
		expect(parseModelsMax('0')).toBe(0);
		expect(parseModelsMax('')).toBeNull();
		expect(parseModelsMax('1.5')).toBeNull();
		expect(parseModelsMax('-1')).toBeNull();
		expect(parseModelsMax('1e3')).toBeNull();
		expect(parseModelsMax('abc')).toBeNull();
	});

	it('keeps one slot evictable', () => {
		expect(maxPinnedModels(3)).toBe(2);
		expect(maxPinnedModels(1)).toBe(0);
		expect(maxPinnedModels(0)).toBe(0);
	});
});

describe('validateSettingsDraft', () => {
	it('accepts the settings as loaded', () => {
		const current = settings({ default_device: 'CUDA0', pinned: ['a'] });

		expect(validateSettingsDraft(createSettingsDraft(current), current)).toEqual({
			defaultDevice: null,
			modelsMax: null,
			pinned: null,
			valid: true
		});
	});

	it('requires a whole number within the limits', () => {
		const current = settings();

		for (const modelsMax of ['', 'abc', '2.5', '0', '17']) {
			const result = validateSettingsDraft({ ...createSettingsDraft(current), modelsMax }, current);

			expect(result.modelsMax, modelsMax).toBe('Enter a whole number from 1 to 16.');
			expect(result.valid, modelsMax).toBe(false);
		}

		for (const modelsMax of ['1', '16']) {
			expect(
				validateSettingsDraft({ ...createSettingsDraft(current), modelsMax }, current).modelsMax
			).toBeNull();
		}
	});

	it('does not hold an untouched out-of-range server value against the user', () => {
		const current = settings({ models_max: 0 });
		const result = validateSettingsDraft(createSettingsDraft(current), current);

		expect(result.modelsMax).toBeNull();
		expect(result.valid).toBe(true);
	});

	it('rejects pinned >= max and says how many pins fit', () => {
		const current = settings({ pinned: ['a'] });
		const draft = { ...createSettingsDraft(current), pinned: ['a', 'b', 'c'] };
		const full = validateSettingsDraft(draft, current);
		const lowered = validateSettingsDraft({ ...draft, modelsMax: '1', pinned: ['a'] }, current);
		const raised = validateSettingsDraft({ ...draft, modelsMax: '4' }, current);
		const two = validateSettingsDraft({ ...draft, pinned: ['a', 'b'] }, current);

		expect(full.valid).toBe(false);
		expect(full.pinned).toContain('At least one slot must stay evictable');
		expect(full.pinned).toContain('pin at most 2 models');
		expect(lowered.pinned).toContain('with a single slot no model can be pinned');
		expect(raised.pinned).toBeNull();
		expect(two.pinned).toBeNull();
	});

	it('checks the device list unless the deployment locked it', () => {
		const open = settings();
		const locked = settings({ default_device: 'CUDA1', default_device_locked: true });
		const bad = { ...createSettingsDraft(open), defaultDevice: 'gpu one' };

		expect(validateSettingsDraft(bad, open).defaultDevice).toContain('CUDA0');
		expect(validateSettingsDraft(bad, open).valid).toBe(false);
		expect(validateSettingsDraft(bad, locked).defaultDevice).toBeNull();
		expect(validateSettingsDraft(bad, locked).valid).toBe(true);
	});
});

describe('buildSettingsUpdate', () => {
	it('sends nothing for an untouched form', () => {
		const current = settings({ default_device: 'CUDA0', pinned: ['a', 'b'] });

		expect(buildSettingsUpdate(createSettingsDraft(current), current)).toEqual({});
	});

	it('sends only the fields that changed', () => {
		const current = settings({ default_device: 'CUDA0', pinned: ['a'] });
		const draft = createSettingsDraft(current);

		expect(buildSettingsUpdate({ ...draft, modelsMax: '5' }, current)).toEqual({ models_max: 5 });
		expect(buildSettingsUpdate({ ...draft, defaultDevice: ' CUDA0 ; CUDA1 ' }, current)).toEqual({
			default_device: 'CUDA0,CUDA1'
		});
		expect(buildSettingsUpdate({ ...draft, defaultDevice: '' }, current)).toEqual({
			default_device: ''
		});
		expect(buildSettingsUpdate({ ...draft, pinned: [] }, current)).toEqual({ pinned: [] });
	});

	it('ignores the order of the pins and drops duplicates', () => {
		const current = settings({ pinned: ['a', 'b'] });
		const draft = createSettingsDraft(current);

		expect(buildSettingsUpdate({ ...draft, pinned: ['b', 'a'] }, current)).toEqual({});
		expect(buildSettingsUpdate({ ...draft, pinned: ['c', 'a', 'c'] }, current)).toEqual({
			pinned: ['a', 'c']
		});
	});

	it('never sends the device while it is locked by MODEL_DEVICES', () => {
		const current = settings({ default_device: 'CUDA1', default_device_locked: true });
		const draft = { ...createSettingsDraft(current), defaultDevice: 'CUDA0', modelsMax: '2' };

		expect(buildSettingsUpdate(draft, current)).toEqual({ models_max: 2 });
	});

	it('does not send a models_max that cannot be read', () => {
		const current = settings();
		const draft = { ...createSettingsDraft(current), modelsMax: 'x' };

		expect(buildSettingsUpdate(draft, current)).toEqual({});
	});
});

describe('rebaseSettingsDraft', () => {
	const previous = settings({ default_device: 'CUDA0', models_max: 3, pinned: ['a'] });

	it('follows the server on every field the user did not touch', () => {
		const next = settings({ default_device: 'CUDA1', models_max: 5, pinned: ['b', 'c'] });

		expect(rebaseSettingsDraft(createSettingsDraft(previous), previous, next)).toEqual(
			createSettingsDraft(next)
		);
	});

	it('keeps edited fields and still updates the untouched ones', () => {
		const next = settings({ default_device: 'CUDA1', models_max: 2, pinned: ['a'] });
		const draft = { ...createSettingsDraft(previous), pinned: ['a', 'b'] };
		const rebased = rebaseSettingsDraft(draft, previous, next);

		expect(rebased).toEqual({ defaultDevice: 'CUDA1', modelsMax: '2', pinned: ['a', 'b'] });
		// the stale models_max does not turn into a change the user never made
		expect(buildSettingsUpdate(rebased, next)).toEqual({ pinned: ['a', 'b'] });
		// and the form rule is now checked against the limit in effect
		expect(validateSettingsDraft(rebased, next).pinned).toContain('pin at most 1 model');
	});

	it('keeps a typed models_max and device', () => {
		const next = settings({ default_device: '', models_max: 8, pinned: [] });
		const draft = { defaultDevice: 'CUDA0;CUDA1', modelsMax: '4', pinned: ['a'] };

		expect(rebaseSettingsDraft(draft, previous, next)).toEqual({
			defaultDevice: 'CUDA0;CUDA1',
			modelsMax: '4',
			pinned: []
		});
	});

	it('drops edited pins of models that are gone', () => {
		const next = settings({
			models: [
				{
					id: 'a',
					pinned: true,
					source: ServerModelSource.PRESET,
					status: ServerModelStatus.UNLOADED
				}
			],
			pinned: ['a']
		});
		const draft = { ...createSettingsDraft(previous), pinned: ['a', 'c'] };

		expect(rebaseSettingsDraft(draft, previous, next).pinned).toEqual(['a']);
	});
});
