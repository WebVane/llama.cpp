import { makeFacts, makeRow, SCHEMA } from './admin-fixtures';
import {
	AdminContextMode,
	AdminFailureCode,
	AdminModelRole,
	AdminRestartPhase,
	AdminSettingOrigin,
	ServerModelStatus
} from '$lib/enums';
import {
	buildModelSettingsUpdate,
	canonicalSettingValue,
	contextModeOf,
	createModelSettingsDraft,
	createRestartTracker,
	describeFailureReason,
	describeModelWarning,
	describeResetChanges,
	describeSaveResult,
	describeSettingValue,
	draftChangesEffect,
	draftContext,
	draftWarnings,
	effectiveDraftSetting,
	formatTokensShort,
	isInstanceAfterFailure,
	modelRoleOf,
	normalizeDraftValue,
	placementSummary,
	rebaseModelSettingsDraft,
	restartPhase,
	revertPlan,
	trackRestartStatus,
	ubatchHelp,
	validateModelSettingsDraft,
	withContextMode,
	withFailedSettings,
	withModelRole
} from '$lib/utils/admin-model-settings';
import { describe, expect, it } from 'vitest';

const DEVICES = ['CUDA0', 'CUDA1'];

describe('canonical values', () => {
	it('spells numbers, words and devices the way the server stores them', () => {
		expect(canonicalSettingValue('ctx-size', 1024)).toBe('1024');
		expect(canonicalSettingValue('ctx-size', ' 01024 ')).toBe('1024');
		expect(canonicalSettingValue('ctx-size', '')).toBeNull();
		expect(canonicalSettingValue('parallel', 'auto')).toBe('-1');
		expect(canonicalSettingValue('n-gpu-layers', -1)).toBe('auto');
		expect(canonicalSettingValue('n-gpu-layers', '-5')).toBe('all');
		expect(canonicalSettingValue('n-gpu-layers', 'ALL')).toBe('all');
		expect(canonicalSettingValue('flash-attn', 'true')).toBe('on');
		expect(canonicalSettingValue('flash-attn', 'disabled')).toBe('off');
		expect(canonicalSettingValue('embeddings', true)).toBe('true');
		expect(canonicalSettingValue('reranking', 'off')).toBe('false');
		expect(canonicalSettingValue('cache-type-k', 'Q8_0')).toBe('q8_0');
		expect(canonicalSettingValue('device', 'cuda1', DEVICES)).toBe('CUDA1');
		expect(canonicalSettingValue('device', 'cuda0; cuda1', DEVICES)).toBe('CUDA0,CUDA1');
		expect(canonicalSettingValue('device', 'NONE', DEVICES)).toBe('none');
		expect(canonicalSettingValue('device', 'rocm0', ['ROCm0', 'Vulkan0'])).toBe('ROCm0');
	});
});

describe('draft', () => {
	const row = makeRow({
		inherited: { 'ctx-size': '8192', 'n-gpu-layers': '99' },
		overrides: { 'ctx-size': '32768', device: 'CUDA1' },
		source: undefined
	});

	it('starts from the saved values', () => {
		const draft = createModelSettingsDraft(row);

		expect(draft.values['ctx-size']).toBe('32768');
		expect(draft.values.device).toBe('CUDA1');
		expect(draft.values.parallel).toBeNull();
		expect(buildModelSettingsUpdate(draft, row)).toEqual({});
	});

	it('sends only what changed, null to remove a saved value', () => {
		const draft = createModelSettingsDraft(row);

		draft.values['ctx-size'] = '16384';
		draft.values.device = null;
		draft.values['flash-attn'] = 'on';

		expect(buildModelSettingsUpdate(draft, row)).toEqual({
			'ctx-size': '16384',
			device: null,
			'flash-attn': 'on'
		});
	});

	it('does not save the built-in default while nothing is inherited', () => {
		expect(normalizeDraftValue(row, 'ctx-size', '16384')).toBe('16384');
		expect(normalizeDraftValue(row, 'parallel', 'auto')).toBeNull();
		expect(normalizeDraftValue(row, 'parallel', '-1')).toBeNull();
		expect(normalizeDraftValue(row, 'cache-type-k', 'f16')).toBeNull();
		expect(normalizeDraftValue(row, 'cache-type-k', 'q8_0')).toBe('q8_0');
		expect(normalizeDraftValue(row, 'n-gpu-layers', 'auto')).toBe('auto');
		expect(normalizeDraftValue(row, 'flash-attn', 'auto')).toBeNull();
	});

	it('keeps a value equal to the inherited one: it pins the model to it', () => {
		const pinned = makeRow({
			inherited: { 'cache-type-k': 'q8_0', device: 'CUDA0', parallel: '2' }
		});

		pinned.fields.device.inherited_origin = AdminSettingOrigin.DEFAULT_DEVICE;

		// the GPU it gets from "GPU for new models" now, and values from models.ini
		expect(normalizeDraftValue(pinned, 'device', 'cuda0', DEVICES)).toBe('CUDA0');
		expect(normalizeDraftValue(row, 'ctx-size', 8192)).toBe('8192');
		expect(normalizeDraftValue(row, 'n-gpu-layers', '99')).toBe('99');
		// a built-in default is kept too when something else is inherited
		expect(normalizeDraftValue(pinned, 'cache-type-k', 'f16')).toBe('f16');
		expect(normalizeDraftValue(pinned, 'parallel', 'auto')).toBe('-1');
	});

	it('tells a pin from a change of what the model runs with', () => {
		const pinned = makeRow({ inherited: { 'ctx-size': '8192', device: 'CUDA0' } });
		const draft = createModelSettingsDraft(pinned);

		draft.values.device = 'CUDA0';
		draft.values['ctx-size'] = '8192';
		expect(buildModelSettingsUpdate(draft, pinned)).toEqual({
			'ctx-size': '8192',
			device: 'CUDA0'
		});
		expect(draftChangesEffect(draft, pinned)).toBe(false);

		draft.values.device = 'CUDA1';
		expect(draftChangesEffect(draft, pinned)).toBe(true);

		// removing a saved pin changes nothing either
		const saved = makeRow({ inherited: { device: 'CUDA0' }, overrides: { device: 'CUDA0' } });
		const unpin = createModelSettingsDraft(saved);

		unpin.values.device = null;
		expect(draftChangesEffect(unpin, saved)).toBe(false);

		// an explicit parallel couples a shared pool: a change when the slots do not share one now
		const split = makeRow({ inherited: { parallel: '4' } });

		split.context.unified = false;

		const parallel = createModelSettingsDraft(split);

		parallel.values.parallel = '4';
		expect(draftChangesEffect(parallel, split)).toBe(true);
	});

	it('never sets a locked key, but may remove a saved value of it', () => {
		const locked = makeRow({
			inherited: { device: 'CUDA1' },
			locked: { device: { by: 'model_devices', value: 'CUDA1' } },
			overrides: { device: 'CUDA0' }
		});
		const draft = createModelSettingsDraft(locked);

		draft.values.device = 'CUDA1';
		expect(buildModelSettingsUpdate(draft, locked)).toEqual({});

		draft.values.device = null;
		expect(buildModelSettingsUpdate(draft, locked)).toEqual({ device: null });
	});

	it('rebases: untouched keys follow the server, edited keys stay', () => {
		const draft = createModelSettingsDraft(row);

		draft.values['flash-attn'] = 'off';

		const next = makeRow({
			inherited: { 'ctx-size': '8192' },
			overrides: { 'ctx-size': '65536', device: 'CUDA1' }
		});
		const rebased = rebaseModelSettingsDraft(draft, row, next);

		expect(rebased.values['ctx-size']).toBe('65536');
		expect(rebased.values['flash-attn']).toBe('off');
	});

	it('takes a lock first, then the draft, then the inherited value, then the default', () => {
		const locked = makeRow({
			inherited: { 'ctx-size': '8192', device: 'CUDA1' },
			locked: { device: { by: 'router_cli', value: 'CUDA0' } }
		});
		const draft = createModelSettingsDraft(locked);

		expect(effectiveDraftSetting(locked, draft, 'device')).toEqual({
			locked: true,
			origin: 'router_cli',
			value: 'CUDA0'
		});
		expect(effectiveDraftSetting(locked, draft, 'ctx-size').origin).toBe(
			AdminSettingOrigin.MODELS_INI
		);

		draft.values['ctx-size'] = '4096';
		expect(effectiveDraftSetting(locked, draft, 'ctx-size')).toEqual({
			locked: false,
			origin: 'admin',
			value: '4096'
		});
		expect(effectiveDraftSetting(locked, draft, 'ubatch-size')).toEqual({
			locked: false,
			origin: 'default',
			value: '512'
		});
	});
});

describe('validation', () => {
	const row = makeRow();

	function errorsFor(values: Record<string, string | null>, devices = DEVICES) {
		const draft = createModelSettingsDraft(row);

		Object.assign(draft.values, values);

		return validateModelSettingsDraft(draft, row, SCHEMA, devices).errors;
	}

	it('checks ranges and words of the touched keys', () => {
		expect(errorsFor({ 'ctx-size': '100' })['ctx-size']).toContain('256');
		expect(errorsFor({ 'ctx-size': 'abc' })['ctx-size']).toBeDefined();
		expect(errorsFor({ parallel: '0' }).parallel).toBeDefined();
		expect(errorsFor({ parallel: '17' }).parallel).toBeDefined();
		expect(errorsFor({ parallel: '-1' }).parallel).toBeUndefined();
		expect(errorsFor({ 'n-gpu-layers': 'x' })['n-gpu-layers']).toBeDefined();
		expect(errorsFor({ 'ubatch-size': '16' })['ubatch-size']).toBeDefined();
		expect(errorsFor({ pooling: 'foo' }).pooling).toBeDefined();
		expect(errorsFor({ 'ctx-size': '4096', 'n-gpu-layers': '20' })).toEqual({});
	});

	it('accepts only GPUs of this server, at most four, each once', () => {
		expect(errorsFor({ device: 'CUDA0' })).toEqual({});
		expect(errorsFor({ device: 'none' })).toEqual({});
		expect(errorsFor({ device: 'CUDA0,CUDA1' })).toEqual({});
		expect(errorsFor({ device: 'CUDA7' }).device).toContain('not on this server');
		expect(errorsFor({ device: 'CUDA0,CUDA0' }).device).toContain('only once');
		expect(errorsFor({ device: 'none,CUDA0' }).device).toBeDefined();
		expect(errorsFor({ device: 'CUDA0' }, []).device).toContain('no GPU');
		expect(
			errorsFor({ device: 'A0,A1,A2,A3,A4' }, ['A0', 'A1', 'A2', 'A3', 'A4']).device
		).toContain('at most 4');
	});

	it('needs flash attention for a quantized cache and equal K / V types', () => {
		expect(
			errorsFor({ 'cache-type-k': 'q8_0', 'cache-type-v': 'q8_0', 'flash-attn': 'off' })[
				'flash-attn'
			]
		).toContain('flash attention');
		expect(errorsFor({ 'cache-type-k': 'q8_0', 'cache-type-v': 'q8_0' })).toEqual({});
		expect(errorsFor({ 'cache-type-k': 'q8_0' })['cache-type-v']).toContain('same');
	});

	it('does not hold an untouched saved value against the user', () => {
		const saved = makeRow({ overrides: { device: 'CUDA7' } });
		const draft = createModelSettingsDraft(saved);

		draft.values['ctx-size'] = '4096';
		expect(validateModelSettingsDraft(draft, saved, SCHEMA, DEVICES).valid).toBe(true);
	});
});

describe('context', () => {
	it('computes pool, per request and all busy like the server', () => {
		const row = makeRow({ inherited: { 'ctx-size': '8192' } });
		const context = draftContext(row, createModelSettingsDraft(row));

		expect(context.slots).toBe(4);
		expect(context.unified).toBe(true);
		expect(context.pool).toBe(8192);
		expect(context.maxPerRequest).toBe(2048);
		expect(context.perRequestAllBusy).toBe(2048);
	});

	it('rounds up the pool and caps a request with the per-slot value', () => {
		const row = makeRow({ facts: makeFacts({ n_ctx_train: 32768 }) });
		const draft = createModelSettingsDraft(row);

		draft.values['ctx-size'] = '1000';
		expect(draftContext(row, draft).pool).toBe(1024);

		const perSlot = withContextMode(draft, row, AdminContextMode.PER_SLOT, 4096);

		expect(perSlot.values['ctx-size']).toBe('16384');
		expect(perSlot.values['kv-unified-per-slot']).toBe('4096');
		expect(contextModeOf(row, perSlot)).toBe(AdminContextMode.PER_SLOT);
		expect(draftContext(row, perSlot).maxPerRequest).toBe(4096);
		expect(draftContext(row, perSlot).perRequestAllBusy).toBe(4096);

		const back = withContextMode(perSlot, row, AdminContextMode.POOL, 0);

		expect(back.values['kv-unified-per-slot']).toBeNull();
		expect(back.values['ctx-size']).toBe('16384');
		expect(contextModeOf(row, back)).toBe(AdminContextMode.POOL);
	});

	it('leaves no change behind when the mode is switched there and back', () => {
		const row = makeRow({ inherited: { 'ctx-size': '8192' } });
		const draft = createModelSettingsDraft(row);
		const perSlot = withContextMode(draft, row, AdminContextMode.PER_SLOT, 2048);

		// per request x slots writes both values, even a pool equal to the inherited one
		expect(perSlot.values).toMatchObject({ 'ctx-size': '8192', 'kv-unified-per-slot': '2048' });

		const back = withContextMode(perSlot, row, AdminContextMode.POOL, 0);

		expect(buildModelSettingsUpdate(back, row)).toEqual({});

		// a saved pool equal to the inherited one stays saved
		const pinned = makeRow({
			inherited: { 'ctx-size': '8192' },
			overrides: { 'ctx-size': '8192' }
		});
		const again = withContextMode(
			withContextMode(createModelSettingsDraft(pinned), pinned, AdminContextMode.PER_SLOT, 2048),
			pinned,
			AdminContextMode.POOL,
			0
		);

		expect(buildModelSettingsUpdate(again, pinned)).toEqual({});
	});

	it('uses the explicit slot count and sizes the pool from the per-slot value alone', () => {
		const row = makeRow({ facts: makeFacts({ n_ctx_train: 32768 }) });
		const draft = createModelSettingsDraft(row);

		draft.values.parallel = '2';
		draft.values['kv-unified-per-slot'] = '1000';

		const context = draftContext(row, draft);

		expect(context.slots).toBe(2);
		expect(context.pool).toBe(2048);
		expect(context.maxPerRequest).toBe(1000);
	});

	it('splits the pool per slot when the slots do not share it', () => {
		const row = makeRow({ inherited: { 'ctx-size': '4096', parallel: '4' } });

		row.context.unified = false;

		const context = draftContext(row, createModelSettingsDraft(row));

		expect(context.unified).toBe(false);
		expect(context.maxPerRequest).toBe(1024);
	});

	it('warns about an overcommitted pool, a context above training, a large micro-batch', () => {
		const row = makeRow({ facts: makeFacts({ n_ctx_train: 32768 }) });
		const draft = createModelSettingsDraft(row);

		draft.values['ctx-size'] = '8192';
		expect(draftWarnings(row, draft)).toContain('pool_overcommit');

		draft.values['ctx-size'] = '262144';
		expect(draftWarnings(row, draft)).toContain('ctx_above_train');

		draft.values['ctx-size'] = '256';
		draft.values['ubatch-size'] = '1024';
		expect(draftWarnings(row, draft)).toContain('ubatch_above_ctx');
	});
});

describe('model role', () => {
	const row = makeRow();

	it('sets and reads the role keys', () => {
		const draft = createModelSettingsDraft(row);

		expect(modelRoleOf(row, draft)).toBe(AdminModelRole.CHAT);

		const embed = withModelRole(draft, row, AdminModelRole.EMBEDDINGS, 'mean');

		expect(embed.values.embeddings).toBe('true');
		expect(embed.values.pooling).toBe('mean');
		expect(modelRoleOf(row, embed)).toBe(AdminModelRole.EMBEDDINGS);

		const rank = withModelRole(embed, row, AdminModelRole.RERANKING);

		expect(rank.values.reranking).toBe('true');
		expect(rank.values.embeddings).toBeNull();
		expect(modelRoleOf(row, rank)).toBe(AdminModelRole.RERANKING);

		const chat = withModelRole(rank, row, AdminModelRole.CHAT);

		expect(chat.values).toMatchObject({ embeddings: null, pooling: null, reranking: null });
	});

	it('turns an inherited embeddings role off with an explicit false', () => {
		const embedRow = makeRow({ inherited: { embeddings: 'true', pooling: 'mean' } });
		const chat = withModelRole(createModelSettingsDraft(embedRow), embedRow, AdminModelRole.CHAT);

		expect(chat.values.embeddings).toBe('false');
		expect(modelRoleOf(embedRow, chat)).toBe(AdminModelRole.CHAT);

		// choosing the inherited role again removes the role values (the select has no "inherit")
		const again = withModelRole(chat, embedRow, AdminModelRole.EMBEDDINGS, null);

		expect(again.values).toMatchObject({ embeddings: null, pooling: null, reranking: null });
		expect(buildModelSettingsUpdate(again, embedRow)).toEqual({});
	});

	it('explains the micro-batch by pooling', () => {
		expect(ubatchHelp('last', AdminModelRole.EMBEDDINGS)).toContain(
			'speed and compute memory only'
		);
		expect(ubatchHelp('mean', AdminModelRole.EMBEDDINGS)).toContain('longest input');
		expect(ubatchHelp(null, AdminModelRole.CHAT)).toContain('prompt');
	});
});

describe('texts', () => {
	it('describes values for people', () => {
		expect(describeSettingValue('parallel', null)).toBe('Auto (4 slots)');
		expect(describeSettingValue('device', null, { hasGpus: true })).toBe('all GPUs');
		expect(describeSettingValue('device', 'none')).toBe('CPU only');
		expect(describeSettingValue('device', 'CUDA0,CUDA1')).toBe('CUDA0 + CUDA1');
		expect(describeSettingValue('n-gpu-layers', '99', { nLayer: 28 })).toBe('99 (all)');
		expect(describeSettingValue('ctx-size', '32768')).toBe('32,768 tokens');
		expect(formatTokensShort(32768)).toBe('32k');
		expect(formatTokensShort(1536)).toBe('1.5k');
		expect(formatTokensShort(512)).toBe('512');
	});

	it('lists what "Remove custom settings" puts back', () => {
		const row = makeRow({
			inherited: { 'ctx-size': '8192' },
			overrides: { 'cache-type-k': 'q8_0', 'ctx-size': '32768' }
		});

		expect(describeResetChanges(row)).toEqual([
			'Context size: 32,768 tokens → 8,192 tokens (models.ini)',
			'KV cache precision (K): q8_0 → f16 (built-in default)'
		]);
	});

	it('summarises placement for the table', () => {
		const row = makeRow({ overrides: { device: 'CUDA1' }, pool: 32768 });

		row.fields.device.effective = 'CUDA1';
		expect(placementSummary(row, true)).toMatchObject({
			context: '32k',
			custom: true,
			device: 'CUDA1'
		});
		expect(placementSummary(makeRow(), true)).toMatchObject({
			context: null,
			custom: false,
			device: 'all GPUs'
		});
		expect(placementSummary(makeRow(), false).device).toBe('CPU');
	});

	it('words failure reasons and save results', () => {
		expect(describeFailureReason(null).label).toBe('Failed to start');
		expect(
			describeFailureReason({
				code: AdminFailureCode.OUT_OF_MEMORY,
				hint: 'lower the context',
				hints: ['memory_tight'],
				line: 'cudaMalloc failed',
				message: 'Out of GPU memory'
			})
		).toMatchObject({ label: 'Out of memory', message: 'Out of GPU memory' });

		expect(
			describeSaveResult({
				model: makeRow(),
				previous: {},
				result: {
					changed: true,
					load_error: 'model limit reached, try again later',
					not_restarted: [{ error: 'x', id: 'b' }],
					reload_error: '',
					reloaded: true,
					restarted: false,
					restarted_also: ['a']
				},
				warnings: []
			})
		).toHaveLength(3);
	});
});

describe('failure and revert', () => {
	const failure = {
		before_loaded: true,
		exit_code: 1,
		failures: 1,
		lines: ['invalid device: CUDA1'],
		port: 51001,
		reason: { code: 'invalid_device', hint: '', hints: [], line: '', message: '' },
		settings_reverted: false,
		time: '2026-10-06T10:00:05Z',
		time_ms: Date.parse('2026-10-06T10:00:05Z') + 400,
		truncated: false
	};
	const instance = (started: string, port = 51001, running = false) => ({
		lines_total: 3,
		port,
		running,
		started
	});

	it('tells whether the instance of the log is a later start than the failure', () => {
		expect(isInstanceAfterFailure(null, failure)).toBe(false);
		expect(isInstanceAfterFailure(instance('2026-10-06T10:00:01Z'), null)).toBe(true);
		// the failed instance itself: started before the failure, or in its second (a start
		// that fails at once, e.g. an invalid device)
		expect(isInstanceAfterFailure(instance('2026-10-06T10:00:03Z'), failure)).toBe(false);
		expect(isInstanceAfterFailure(instance('2026-10-06T10:00:05Z'), failure)).toBe(false);
		// a stopped instance that started in a later second is a later start
		expect(isInstanceAfterFailure(instance('2026-10-06T10:00:06Z'), failure)).toBe(true);
		// a retry that runs (or loads) is shown even when it started in the second of the
		// failure, and so is an instance on another port, whatever the clock says
		expect(isInstanceAfterFailure(instance('2026-10-06T10:00:05Z', 51001, true), failure)).toBe(
			true
		);
		expect(isInstanceAfterFailure(instance('2026-10-06T10:00:03Z', 51002), failure)).toBe(true);
		expect(isInstanceAfterFailure(instance('2026-10-06T10:00:03Z', 51001, true), failure)).toBe(
			true
		);
	});

	it('compares the exact start time when the server sends it', () => {
		const exact = (startedMs: number) => ({
			...instance(new Date(Math.floor(startedMs / 1000) * 1000).toISOString()),
			started_ms: startedMs
		});

		// the failed instance itself: started before the failure, in the same second
		expect(isInstanceAfterFailure(exact(failure.time_ms - 300), failure)).toBe(false);
		expect(isInstanceAfterFailure(exact(failure.time_ms), failure)).toBe(false);
		// a retry that started after the failure in the same second (whole seconds cannot tell)
		expect(isInstanceAfterFailure(exact(failure.time_ms + 100), failure)).toBe(true);
		expect(isInstanceAfterFailure(exact(failure.time_ms + 5000), failure)).toBe(true);
	});

	it('remembers each failed set of values once', () => {
		let failed = withFailedSettings([], { 'ctx-size': '8192' });

		failed = withFailedSettings(failed, { 'ctx-size': '8192' });
		expect(failed).toEqual([{ 'ctx-size': '8192' }]);

		failed = withFailedSettings(failed, { 'ctx-size': '8192', device: 'CUDA1' });
		expect(failed).toHaveLength(2);

		// A (ctx 16384) had failed before the dialog opened (the row said so); B saved on top
		// of it fails too: previous is A, which Revert skips for the settings that last worked
		const row = makeRow({ overrides: { 'ctx-size': '32768' } });

		row.phase = 'failed';
		row.last_good = { 'ctx-size': '4096' };

		const known = withFailedSettings(withFailedSettings([], { 'ctx-size': '16384' }), {
			'ctx-size': '32768'
		});

		expect(revertPlan(row, { 'ctx-size': '16384' }, known).settings).toEqual({
			'ctx-size': '4096'
		});
	});

	it('undoes the last save, but never back to settings that already failed', () => {
		// committed S0 (ctx 4096); save A (ctx 8192) fails; save B (ctx 16384 on CUDA1) fails
		const row = makeRow({ overrides: { 'ctx-size': '16384', device: 'CUDA1' } });

		row.phase = 'failed';
		row.last_good = { 'ctx-size': '4096' };

		// first failure: undo the save (previous = the values before it)
		expect(revertPlan(row, { 'ctx-size': '4096', device: null }, [{ 'ctx-size': '8192' }])).toEqual(
			{
				label: 'Revert to previous settings and restart',
				settings: { 'ctx-size': '4096', device: null }
			}
		);

		// second failure in a row: previous is A, which failed: the settings that last worked
		const failed: Record<string, string>[] = [
			{ 'ctx-size': '8192' },
			{ 'ctx-size': '16384', device: 'CUDA1' }
		];
		const plan = revertPlan(row, { 'ctx-size': '8192', device: null }, failed);

		expect(plan.settings).toEqual({ 'ctx-size': '4096', device: null });
		expect(plan.label).toContain('last worked');

		// nothing ever worked: the per-model values are removed
		row.last_good = null;
		expect(revertPlan(row, { 'ctx-size': '8192', device: null }, failed)).toMatchObject({
			label: 'Remove the custom settings and restart',
			settings: { 'ctx-size': null, device: null }
		});
	});

	it('keeps saved values that were never tried when only the last change failed', () => {
		// ctx 2048 saved for a stopped model (pending), then a GPU added that fails
		const row = makeRow({ overrides: { 'ctx-size': '2048', device: 'CUDA1' } });

		row.phase = 'failed';

		expect(
			revertPlan(row, { 'ctx-size': '2048', device: null }, [
				{ 'ctx-size': '2048', device: 'CUDA1' }
			]).settings
		).toEqual({ 'ctx-size': '2048', device: null });
	});

	it('goes back to the settings that last worked after a failure from an earlier visit', () => {
		const row = makeRow({ overrides: { 'ctx-size': '16384' } });

		row.phase = 'failed';
		row.last_good = { 'ctx-size': '4096', device: 'CUDA0' };

		expect(revertPlan(row, null).settings).toEqual({ 'ctx-size': '4096', device: 'CUDA0' });
	});

	it('reverts a failed reset to the settings before it', () => {
		const row = makeRow();

		expect(revertPlan(row, { 'ctx-size': '8192' }, [{}])).toEqual({
			label: 'Revert to previous settings and restart',
			settings: { 'ctx-size': '8192' }
		});
	});

	it('words a MODEL_DEVICES entry that names no models.ini section', () => {
		expect(describeModelWarning('model_devices_unknown_section:embd-b')).toBe(
			'MODEL_DEVICES names "embd-b", which is not a section of models.ini; that entry is ignored, so the model is not locked to a GPU.'
		);
	});
});

describe('restart progress', () => {
	it('goes saving -> starting -> loading -> ready', () => {
		let tracker = createRestartTracker();

		expect(restartPhase(tracker)).toBe(AdminRestartPhase.SAVING);

		// the stop of the old instance is not a failure
		tracker = trackRestartStatus(tracker, ServerModelStatus.UNLOADED, 0);
		tracker = { ...tracker, saving: false };
		expect(restartPhase(tracker)).toBe(AdminRestartPhase.STARTING);

		tracker = trackRestartStatus(tracker, ServerModelStatus.LOADING);
		expect(restartPhase(tracker)).toBe(AdminRestartPhase.LOADING);

		tracker = trackRestartStatus(tracker, ServerModelStatus.LOADED);
		expect(restartPhase(tracker)).toBe(AdminRestartPhase.READY);
	});

	it('ignores the old instance while saving, catches a failed start', () => {
		let tracker = createRestartTracker();

		tracker = trackRestartStatus(tracker, ServerModelStatus.LOADED);
		expect(tracker.ready).toBe(false);

		tracker = trackRestartStatus(tracker, ServerModelStatus.LOADING);
		tracker = trackRestartStatus(tracker, ServerModelStatus.UNLOADED, 1);
		tracker = { ...tracker, saving: false };
		expect(restartPhase(tracker)).toBe(AdminRestartPhase.FAILED);
		expect(tracker.exitCode).toBe(1);

		// terminal
		expect(trackRestartStatus(tracker, ServerModelStatus.LOADED)).toBe(tracker);
	});

	it('takes a loaded model after the save as ready', () => {
		const tracker = trackRestartStatus(
			{ ...createRestartTracker(), saving: false },
			ServerModelStatus.LOADED
		);

		expect(restartPhase(tracker)).toBe(AdminRestartPhase.READY);
	});
});
