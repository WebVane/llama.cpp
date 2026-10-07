<script lang="ts">
	import AdminNumberInput from './AdminNumberInput.svelte';
	import AdminSettingField from './AdminSettingField.svelte';
	import { ChevronDown, TriangleAlert } from '@lucide/svelte';
	import { Checkbox } from '$lib/components/ui/checkbox';
	import * as Collapsible from '$lib/components/ui/collapsible';
	import * as Select from '$lib/components/ui/select';
	import {
		ADMIN_CONTEXT_QUICK_PICKS,
		ADMIN_DEVICE_NONE,
		ADMIN_FLASH_ATTN_LABELS,
		ADMIN_KV_PRECISION_HELP,
		ADMIN_KV_PRECISION_LABELS,
		ADMIN_MODEL_DEVICES_MAX,
		ADMIN_MODEL_ROLE_LABELS,
		ADMIN_PARALLEL_AUTO_SLOTS,
		ADMIN_PARALLEL_CHOICES,
		ADMIN_POOLING_LABELS,
		ADMIN_SETTING_LABELS
	} from '$lib/constants';
	import { AdminContextMode, AdminModelRole, AdminSettingKey } from '$lib/enums';
	import type {
		AdminModelSettingsDraft,
		ApiAdminModelSettingsRow,
		ApiAdminSettingSchema
	} from '$lib/types';
	import {
		contextModeOf,
		describeInheritedValue,
		describeSettingValue,
		draftContext,
		effectiveDraftSetting,
		formatTokenCount,
		formatTokensShort,
		modelRoleOf,
		normalizeDraftValue,
		padContext,
		settingField,
		settingSchema,
		splitDeviceList,
		ubatchHelp,
		withContextMode,
		withModelRole
	} from '$lib/utils';

	interface Props {
		row: ApiAdminModelSettingsRow;
		draft: AdminModelSettingsDraft;
		schema: ApiAdminSettingSchema[];
		/** GPU devices of the server */
		devices: string[];
		/** Description per device name, for the GPU list */
		deviceDescriptions?: Record<string, string>;
		errors?: Record<string, string>;
		disabled?: boolean;
		onDraftChange: (draft: AdminModelSettingsDraft) => void;
	}

	let {
		deviceDescriptions = {},
		devices,
		disabled = false,
		draft,
		errors = {},
		onDraftChange,
		row,
		schema
	}: Props = $props();

	const INHERIT = '__inherit__';
	const CUSTOM = '__custom__';
	const MULTI = '__multi__';
	const INACTIVE_REASONS: Record<string, string> = {
		device_missing: 'that GPU is not on this server',
		invalid: 'the value is not valid',
		model_devices: 'MODEL_DEVICES fixes this field',
		router_cli: 'the server command line fixes this field',
		unknown_key: 'the key is not known'
	};

	let hasGpus = $derived(devices.length > 0);
	let nLayer = $derived(row.facts?.n_layer ?? null);
	let nCtxTrain = $derived(row.facts?.n_ctx_train ?? row.context?.n_ctx_train ?? null);
	let context = $derived(draftContext(row, draft));
	let mode = $derived(contextModeOf(row, draft));
	let role = $derived(modelRoleOf(row, draft));
	let multiDevice = $state(false);
	let customParallel = $state(false);
	let customLayers = $state(false);
	let advancedOpen = $state(false);

	// open "Advanced" when one of its values is set here
	$effect(() => {
		const advanced = [
			AdminSettingKey.N_GPU_LAYERS,
			AdminSettingKey.FLASH_ATTN,
			AdminSettingKey.CACHE_TYPE_K,
			AdminSettingKey.UBATCH_SIZE
		];

		if (advanced.some((key) => row.overrides?.[key] !== undefined)) advancedOpen = true;
	});

	function eff(key: string) {
		return effectiveDraftSetting(row, draft, key);
	}

	function field(key: string) {
		return settingField(row, key);
	}

	function isSet(key: string): boolean {
		return draft.values[key] !== null && draft.values[key] !== undefined;
	}

	function inherited(key: string): string {
		return describeInheritedValue(row, key, { hasGpus, nLayer });
	}

	function inactiveText(key: string): string | null {
		const f = field(key);

		if (f.override === null || !f.inactive_reason) return null;

		return `The saved value ${describeSettingValue(key, f.override, { hasGpus, nLayer })} is not applied: ${INACTIVE_REASONS[f.inactive_reason] ?? f.inactive_reason}.`;
	}

	function update(values: Record<string, string | null>) {
		onDraftChange({ values: { ...draft.values, ...values } });
	}

	/**
	 * A value chosen or typed in a field: kept even when it equals the inherited
	 * value, which pins the model to it (a later change of models.ini or of "GPU
	 * for new models" no longer moves it). "Inherit" in a select and the "back to"
	 * button of a field go back to inheriting.
	 */
	function setValue(key: string, raw: string | number | boolean | null) {
		update({ [key]: raw === null ? null : normalizeDraftValue(row, key, raw, devices) });
	}

	/** Shown value of a select: inherit, the value itself, or "custom". */
	function choice(key: string, options: string[]): string {
		const value = draft.values[key];

		if (value === null || value === undefined) return INHERIT;

		return options.includes(value) ? value : CUSTOM;
	}

	// GPU

	let deviceChoice = $derived.by(() => {
		const value = draft.values[AdminSettingKey.DEVICE];

		if (value === null || value === undefined) return multiDevice ? MULTI : INHERIT;

		if (value === ADMIN_DEVICE_NONE) return ADMIN_DEVICE_NONE;

		const names = splitDeviceList(value);

		return names.length === 1 && !multiDevice ? names[0] : MULTI;
	});
	let chosenDevices = $derived(
		deviceChoice === MULTI ? splitDeviceList(eff(AdminSettingKey.DEVICE).value) : []
	);
	let deviceOptions = $derived([
		{ label: `Inherit: ${inherited(AdminSettingKey.DEVICE)}`, value: INHERIT },
		...devices.map((device) => ({
			label: deviceDescriptions[device] ? `${device} · ${deviceDescriptions[device]}` : device,
			value: device
		})),
		...(devices.length >= 2 ? [{ label: 'Several GPUs (split the model)', value: MULTI }] : []),
		{ label: 'CPU only (no GPU)', value: ADMIN_DEVICE_NONE }
	]);

	function handleDevice(value: string) {
		if (value === MULTI) {
			multiDevice = true;

			const current = splitDeviceList(eff(AdminSettingKey.DEVICE).value).filter((name) =>
				devices.includes(name)
			);
			const start = current.length >= 2 ? current : devices.slice(0, ADMIN_MODEL_DEVICES_MAX);

			setValue(AdminSettingKey.DEVICE, start.join(','));

			return;
		}

		multiDevice = false;
		setValue(AdminSettingKey.DEVICE, value === INHERIT ? null : value);
	}

	function toggleDevice(device: string, checked: boolean) {
		const current = chosenDevices.filter((name) => name !== device);
		const next = checked ? [...current, device] : current;
		const ordered = devices.filter((name) => next.includes(name));

		setValue(AdminSettingKey.DEVICE, ordered.length > 0 ? ordered.join(',') : null);
	}

	// context

	let perSlotInherited = $derived(field(AdminSettingKey.KV_UNIFIED_PER_SLOT).inherited !== null);
	let ctxValue = $derived(eff(AdminSettingKey.CTX_SIZE).value);
	let ctxRounded = $derived.by(() => {
		const tokens = Number(ctxValue);

		if (!ctxValue || !Number.isInteger(tokens) || tokens <= 0) return null;

		return padContext(tokens) !== tokens ? padContext(tokens) : null;
	});
	let quickPicks = $derived([
		...ADMIN_CONTEXT_QUICK_PICKS.filter((tokens) => !nCtxTrain || tokens <= nCtxTrain * 4).map(
			(tokens) => ({ label: formatTokensShort(tokens), tokens })
		),
		...(nCtxTrain
			? [{ label: `trained (${formatTokensShort(nCtxTrain)})`, tokens: nCtxTrain }]
			: [])
	]);

	function setMode(next: AdminContextMode) {
		if (next === mode) return;

		const perRequest =
			context.maxPerRequest ??
			context.perRequestAllBusy ??
			nCtxTrain ??
			ADMIN_CONTEXT_QUICK_PICKS[0];

		onDraftChange(withContextMode(draft, row, next, perRequest));
	}

	function setPerSlot(text: string) {
		const tokens = Number(text);

		if (text.trim() === '' || !Number.isFinite(tokens)) {
			update({ [AdminSettingKey.KV_UNIFIED_PER_SLOT]: text.trim() === '' ? null : text });

			return;
		}

		onDraftChange(withContextMode(draft, row, AdminContextMode.PER_SLOT, tokens));
	}

	// parallel

	let parallelOptions = $derived(['-1', ...ADMIN_PARALLEL_CHOICES.map(String)]);
	let parallelChoice = $derived(
		customParallel ? CUSTOM : choice(AdminSettingKey.PARALLEL, parallelOptions)
	);

	function handleParallel(value: string) {
		customParallel = value === CUSTOM;

		if (value === CUSTOM) return;

		applyParallel(value === INHERIT ? null : value);
	}

	/** Parallel changes the pool in "per request x slots" mode: keep N per request. */
	function applyParallel(raw: string | null) {
		const values = {
			...draft.values,
			[AdminSettingKey.PARALLEL]:
				raw === null ? null : normalizeDraftValue(row, AdminSettingKey.PARALLEL, raw)
		};

		if (mode === AdminContextMode.PER_SLOT && context.perSlotCap !== null) {
			onDraftChange(
				withContextMode({ values }, row, AdminContextMode.PER_SLOT, context.perSlotCap)
			);
		} else {
			onDraftChange({ values });
		}
	}

	// advanced

	let layerChoice = $derived(
		customLayers ? CUSTOM : choice(AdminSettingKey.N_GPU_LAYERS, ['all', 'auto'])
	);
	let cacheChoice = $derived(
		draft.values[AdminSettingKey.CACHE_TYPE_K] === null &&
			draft.values[AdminSettingKey.CACHE_TYPE_V] === null
			? INHERIT
			: (eff(AdminSettingKey.CACHE_TYPE_K).value ?? 'f16')
	);
	let cacheValues = $derived(
		settingSchema(schema, AdminSettingKey.CACHE_TYPE_K)?.values ?? ['f16', 'q8_0', 'q4_0']
	);
	let cacheLocked = $derived(
		field(AdminSettingKey.CACHE_TYPE_K).locked !== null ||
			field(AdminSettingKey.CACHE_TYPE_V).locked !== null
	);

	function handleLayers(value: string) {
		customLayers = value === CUSTOM;

		if (value === CUSTOM) return;

		setValue(AdminSettingKey.N_GPU_LAYERS, value === INHERIT ? null : value);
	}

	function handleCache(value: string) {
		if (value === INHERIT) {
			update({ [AdminSettingKey.CACHE_TYPE_K]: null, [AdminSettingKey.CACHE_TYPE_V]: null });

			return;
		}

		update({
			[AdminSettingKey.CACHE_TYPE_K]: normalizeDraftValue(row, AdminSettingKey.CACHE_TYPE_K, value),
			[AdminSettingKey.CACHE_TYPE_V]: normalizeDraftValue(row, AdminSettingKey.CACHE_TYPE_V, value)
		});
	}

	// role

	let pooling = $derived(eff(AdminSettingKey.POOLING).value);
	let poolingValues = $derived(
		settingSchema(schema, AdminSettingKey.POOLING)?.values ?? Object.keys(ADMIN_POOLING_LABELS)
	);
	let roleOrigin = $derived(
		role === AdminModelRole.RERANKING
			? eff(AdminSettingKey.RERANKING).origin
			: eff(AdminSettingKey.EMBEDDINGS).origin
	);
	let roleSet = $derived(isSet(AdminSettingKey.EMBEDDINGS) || isSet(AdminSettingKey.RERANKING));

	function handleRole(value: string) {
		onDraftChange(
			withModelRole(draft, row, value as AdminModelRole, draft.values[AdminSettingKey.POOLING])
		);
	}

	function labelOf(options: { label: string; value: string }[], value: string): string {
		return options.find((option) => option.value === value)?.label ?? value;
	}
</script>

{#snippet selectControl(
	id: string,
	options: { label: string; value: string }[],
	value: string,
	onChange: (value: string) => void,
	locked: boolean
)}
	<Select.Root disabled={disabled || locked} onValueChange={onChange} type="single" {value}>
		<Select.Trigger class="w-full sm:w-80" {id}>
			<span class="truncate">{labelOf(options, value)}</span>
		</Select.Trigger>

		<Select.Content>
			{#each options as option (option.value)}
				<Select.Item label={option.label} value={option.value} />
			{/each}
		</Select.Content>
	</Select.Root>
{/snippet}

<div class="flex flex-col gap-5" data-testid="admin-model-settings-form">
	<section aria-label="Placement" class="flex flex-col gap-4">
		<h3 class="text-xs font-semibold tracking-wide text-muted-foreground uppercase">Placement</h3>

		<AdminSettingField
			error={errors[AdminSettingKey.DEVICE]}
			field={field(AdminSettingKey.DEVICE)}
			forId="ms-device"
			help={hasGpus
				? 'Without a GPU setting the model may use every GPU, split by free memory.'
				: 'This server has no GPU: models run on the CPU.'}
			inactiveText={inactiveText(AdminSettingKey.DEVICE)}
			inheritedText={inherited(AdminSettingKey.DEVICE)}
			isSet={isSet(AdminSettingKey.DEVICE)}
			label={ADMIN_SETTING_LABELS[AdminSettingKey.DEVICE]}
			onClear={() => handleDevice(INHERIT)}
			onRemoveInactive={() => update({ [AdminSettingKey.DEVICE]: null })}
			origin={eff(AdminSettingKey.DEVICE).origin}
		>
			{@render selectControl(
				'ms-device',
				deviceOptions,
				deviceChoice,
				handleDevice,
				field(AdminSettingKey.DEVICE).locked !== null
			)}

			{#if deviceChoice === MULTI}
				<div class="flex flex-wrap gap-x-4 gap-y-1.5 pt-1" data-testid="ms-device-multi">
					{#each devices as device (device)}
						{@const checked = chosenDevices.includes(device)}

						<label class="flex items-center gap-2 text-sm">
							<Checkbox
								{checked}
								disabled={disabled || (!checked && chosenDevices.length >= ADMIN_MODEL_DEVICES_MAX)}
								onCheckedChange={(value) => toggleDevice(device, value === true)}
							/>

							<span class="font-mono">{device}</span>
						</label>
					{/each}
				</div>
			{/if}
		</AdminSettingField>

		<AdminSettingField
			error={errors[AdminSettingKey.CTX_SIZE] ?? errors[AdminSettingKey.KV_UNIFIED_PER_SLOT]}
			field={field(AdminSettingKey.CTX_SIZE)}
			forId="ms-ctx"
			inactiveText={inactiveText(AdminSettingKey.CTX_SIZE)}
			inheritedText={inherited(AdminSettingKey.CTX_SIZE)}
			isSet={isSet(AdminSettingKey.CTX_SIZE) || isSet(AdminSettingKey.KV_UNIFIED_PER_SLOT)}
			label={ADMIN_SETTING_LABELS[AdminSettingKey.CTX_SIZE]}
			onClear={() =>
				update({ [AdminSettingKey.CTX_SIZE]: null, [AdminSettingKey.KV_UNIFIED_PER_SLOT]: null })}
			origin={mode === AdminContextMode.PER_SLOT
				? eff(AdminSettingKey.KV_UNIFIED_PER_SLOT).origin
				: eff(AdminSettingKey.CTX_SIZE).origin}
		>
			<div
				aria-label="Context mode"
				class="inline-flex w-fit rounded-md border bg-muted/40 p-0.5 text-xs"
				role="radiogroup"
			>
				{#each [{ label: 'Shared pool', value: AdminContextMode.POOL }, { label: 'Per request × slots', value: AdminContextMode.PER_SLOT }] as option (option.value)}
					<button
						aria-checked={mode === option.value}
						class={[
							'rounded px-2.5 py-1 font-medium transition-colors',
							mode === option.value
								? 'bg-background text-foreground shadow-sm'
								: 'text-muted-foreground hover:text-foreground'
						]}
						data-context-mode={option.value}
						disabled={disabled ||
							field(AdminSettingKey.CTX_SIZE).locked !== null ||
							(option.value === AdminContextMode.POOL && perSlotInherited)}
						onclick={() => setMode(option.value)}
						role="radio"
						title={option.value === AdminContextMode.POOL && perSlotInherited
							? 'The per-request limit is set in models.ini'
							: ''}
						type="button"
					>
						{option.label}
					</button>
				{/each}
			</div>

			{#if mode === AdminContextMode.POOL}
				<div class="flex flex-wrap items-center gap-2">
					<AdminNumberInput
						disabled={disabled || field(AdminSettingKey.CTX_SIZE).locked !== null}
						id="ms-ctx"
						invalid={!!errors[AdminSettingKey.CTX_SIZE]}
						min={256}
						onValueChange={(text) => setValue(AdminSettingKey.CTX_SIZE, text === '' ? null : text)}
						placeholder={nCtxTrain ? String(nCtxTrain) : 'tokens'}
						step={256}
						value={ctxValue}
					/>

					<span class="text-xs text-muted-foreground">tokens in total</span>

					{#each quickPicks as pick (pick.label)}
						<button
							class={[
								'rounded border px-2 py-0.5 text-xs hover:bg-muted',
								Number(ctxValue) === pick.tokens && 'border-primary text-primary'
							]}
							disabled={disabled || field(AdminSettingKey.CTX_SIZE).locked !== null}
							onclick={() => setValue(AdminSettingKey.CTX_SIZE, pick.tokens)}
							type="button"
						>
							{pick.label}
						</button>
					{/each}
				</div>
			{:else}
				<div class="flex flex-wrap items-center gap-2">
					<AdminNumberInput
						disabled={disabled || field(AdminSettingKey.KV_UNIFIED_PER_SLOT).locked !== null}
						id="ms-ctx"
						invalid={!!errors[AdminSettingKey.KV_UNIFIED_PER_SLOT]}
						min={256}
						onValueChange={setPerSlot}
						placeholder="tokens"
						step={256}
						value={eff(AdminSettingKey.KV_UNIFIED_PER_SLOT).value}
					/>

					<span class="text-xs text-muted-foreground tabular-nums">
						tokens per request × {context.slots} slots = pool of
						{context.pool !== null ? formatTokenCount(context.pool) : '?'} tokens
					</span>
				</div>
			{/if}

			{#snippet extra()}
				<p class="text-xs text-muted-foreground" data-testid="ms-context-help">
					{#if context.pool === null}
						No context size set: the model uses its trained context{nCtxTrain
							? ` (${formatTokenCount(nCtxTrain)} tokens)`
							: ''}, shared by {context.slots} slots.
					{:else if mode === AdminContextMode.PER_SLOT}
						Every one of the {context.slots} slots can use {formatTokenCount(
							context.perSlotCap ?? 0
						)} tokens at the same time: the server keeps one pool of {formatTokenCount(
							context.pool
						)} tokens and caps each request at {formatTokenCount(context.maxPerRequest ?? 0)} tokens (the
						per-request value and the trained context).
					{:else}
						Up to {formatTokenCount(context.maxPerRequest ?? 0)} tokens per request; when all {context.slots}
						slots are busy about {formatTokenCount(context.perRequestAllBusy ?? 0)} each{context.unified
							? ' (they share the pool)'
							: ' (each slot has its own part)'}.
					{/if}

					{#if nCtxTrain}
						The model was trained for {formatTokenCount(nCtxTrain)} tokens.
					{/if}

					{#if ctxRounded && mode === AdminContextMode.POOL}
						Rounded up to {formatTokenCount(ctxRounded)}.
					{/if}
				</p>
			{/snippet}
		</AdminSettingField>

		<AdminSettingField
			error={errors[AdminSettingKey.PARALLEL]}
			field={field(AdminSettingKey.PARALLEL)}
			forId="ms-parallel"
			help="How many requests the model serves at the same time. The requests share the context."
			inactiveText={inactiveText(AdminSettingKey.PARALLEL)}
			inheritedText={inherited(AdminSettingKey.PARALLEL)}
			isSet={isSet(AdminSettingKey.PARALLEL)}
			label={ADMIN_SETTING_LABELS[AdminSettingKey.PARALLEL]}
			onClear={() => handleParallel(INHERIT)}
			origin={eff(AdminSettingKey.PARALLEL).origin}
		>
			<div class="flex flex-wrap items-center gap-2">
				{@render selectControl(
					'ms-parallel',
					[
						{ label: `Inherit: ${inherited(AdminSettingKey.PARALLEL)}`, value: INHERIT },
						{ label: `Auto (${ADMIN_PARALLEL_AUTO_SLOTS} slots)`, value: '-1' },
						...ADMIN_PARALLEL_CHOICES.map((count) => ({
							label: String(count),
							value: String(count)
						})),
						{ label: 'Other number...', value: CUSTOM }
					],
					parallelChoice,
					handleParallel,
					field(AdminSettingKey.PARALLEL).locked !== null
				)}

				{#if parallelChoice === CUSTOM}
					<AdminNumberInput
						class="w-24"
						{disabled}
						id="ms-parallel-custom"
						invalid={!!errors[AdminSettingKey.PARALLEL]}
						max={16}
						min={1}
						onValueChange={(text) => applyParallel(text === '' ? null : text)}
						value={eff(AdminSettingKey.PARALLEL).value}
					/>
				{/if}
			</div>
		</AdminSettingField>
	</section>

	<Collapsible.Root bind:open={advancedOpen}>
		<Collapsible.Trigger
			class="flex w-full items-center gap-2 text-xs font-semibold tracking-wide text-muted-foreground uppercase hover:text-foreground"
			data-testid="ms-advanced-toggle"
		>
			<ChevronDown class={['h-4 w-4 transition-transform', advancedOpen && 'rotate-180']} />

			Advanced
		</Collapsible.Trigger>

		<Collapsible.Content>
			<div class="flex flex-col gap-4 pt-4">
				<AdminSettingField
					error={errors[AdminSettingKey.N_GPU_LAYERS]}
					field={field(AdminSettingKey.N_GPU_LAYERS)}
					forId="ms-ngl"
					help="All puts every layer on the GPU (fastest). Auto lets llama.cpp keep layers on the CPU when the free memory is short. A number puts that many layers on the GPU; the output layer goes first."
					inactiveText={inactiveText(AdminSettingKey.N_GPU_LAYERS)}
					inheritedText={inherited(AdminSettingKey.N_GPU_LAYERS)}
					isSet={isSet(AdminSettingKey.N_GPU_LAYERS)}
					label={ADMIN_SETTING_LABELS[AdminSettingKey.N_GPU_LAYERS]}
					onClear={() => handleLayers(INHERIT)}
					origin={eff(AdminSettingKey.N_GPU_LAYERS).origin}
				>
					<div class="flex flex-wrap items-center gap-2">
						{@render selectControl(
							'ms-ngl',
							[
								{ label: `Inherit: ${inherited(AdminSettingKey.N_GPU_LAYERS)}`, value: INHERIT },
								{ label: 'All layers', value: 'all' },
								{ label: 'Auto (fit to free memory)', value: 'auto' },
								{
									label: `Number of layers${nLayer ? ` (the model has ${nLayer} + output)` : ''}...`,
									value: CUSTOM
								}
							],
							layerChoice,
							handleLayers,
							field(AdminSettingKey.N_GPU_LAYERS).locked !== null
						)}

						{#if layerChoice === CUSTOM}
							<AdminNumberInput
								class="w-24"
								{disabled}
								id="ms-ngl-custom"
								invalid={!!errors[AdminSettingKey.N_GPU_LAYERS]}
								max={999}
								min={0}
								onValueChange={(text) =>
									setValue(AdminSettingKey.N_GPU_LAYERS, text === '' ? null : text)}
								value={eff(AdminSettingKey.N_GPU_LAYERS).value}
							/>
						{/if}
					</div>
				</AdminSettingField>

				<AdminSettingField
					error={errors[AdminSettingKey.FLASH_ATTN]}
					field={field(AdminSettingKey.FLASH_ATTN)}
					forId="ms-fa"
					help="Faster attention with less memory where the GPU supports it. Needed for a quantized KV cache."
					inactiveText={inactiveText(AdminSettingKey.FLASH_ATTN)}
					inheritedText={inherited(AdminSettingKey.FLASH_ATTN)}
					isSet={isSet(AdminSettingKey.FLASH_ATTN)}
					label={ADMIN_SETTING_LABELS[AdminSettingKey.FLASH_ATTN]}
					onClear={() => setValue(AdminSettingKey.FLASH_ATTN, null)}
					origin={eff(AdminSettingKey.FLASH_ATTN).origin}
				>
					{@render selectControl(
						'ms-fa',
						[
							{ label: `Inherit: ${inherited(AdminSettingKey.FLASH_ATTN)}`, value: INHERIT },
							...['on', 'off', 'auto'].map((value) => ({
								label: ADMIN_FLASH_ATTN_LABELS[value],
								value
							}))
						],
						choice(AdminSettingKey.FLASH_ATTN, ['on', 'off', 'auto']),
						(value) => setValue(AdminSettingKey.FLASH_ATTN, value === INHERIT ? null : value),
						field(AdminSettingKey.FLASH_ATTN).locked !== null
					)}
				</AdminSettingField>

				<AdminSettingField
					error={errors[AdminSettingKey.CACHE_TYPE_V] ?? errors[AdminSettingKey.CACHE_TYPE_K]}
					field={field(AdminSettingKey.CACHE_TYPE_K)}
					forId="ms-kv"
					help={ADMIN_KV_PRECISION_HELP}
					inactiveText={inactiveText(AdminSettingKey.CACHE_TYPE_K)}
					inheritedText={inherited(AdminSettingKey.CACHE_TYPE_K)}
					isSet={isSet(AdminSettingKey.CACHE_TYPE_K) || isSet(AdminSettingKey.CACHE_TYPE_V)}
					label="KV cache precision"
					onClear={() => handleCache(INHERIT)}
					origin={eff(AdminSettingKey.CACHE_TYPE_K).origin}
				>
					{@render selectControl(
						'ms-kv',
						[
							{ label: `Inherit: ${inherited(AdminSettingKey.CACHE_TYPE_K)}`, value: INHERIT },
							...cacheValues.map((value) => ({
								label: ADMIN_KV_PRECISION_LABELS[value] ?? value,
								value
							}))
						],
						cacheChoice,
						handleCache,
						cacheLocked
					)}
				</AdminSettingField>

				<AdminSettingField
					error={errors[AdminSettingKey.UBATCH_SIZE]}
					field={field(AdminSettingKey.UBATCH_SIZE)}
					forId="ms-ubatch"
					help={ubatchHelp(pooling, role)}
					inactiveText={inactiveText(AdminSettingKey.UBATCH_SIZE)}
					inheritedText={inherited(AdminSettingKey.UBATCH_SIZE)}
					isSet={isSet(AdminSettingKey.UBATCH_SIZE)}
					label={ADMIN_SETTING_LABELS[AdminSettingKey.UBATCH_SIZE]}
					onClear={() => setValue(AdminSettingKey.UBATCH_SIZE, null)}
					origin={eff(AdminSettingKey.UBATCH_SIZE).origin}
				>
					<AdminNumberInput
						disabled={disabled || field(AdminSettingKey.UBATCH_SIZE).locked !== null}
						id="ms-ubatch"
						invalid={!!errors[AdminSettingKey.UBATCH_SIZE]}
						max={8192}
						min={32}
						onValueChange={(text) =>
							setValue(AdminSettingKey.UBATCH_SIZE, text === '' ? null : text)}
						value={eff(AdminSettingKey.UBATCH_SIZE).value}
					/>
				</AdminSettingField>
			</div>
		</Collapsible.Content>
	</Collapsible.Root>

	<section aria-label="Model role" class="flex flex-col gap-4">
		<h3 class="text-xs font-semibold tracking-wide text-muted-foreground uppercase">Model role</h3>

		<AdminSettingField
			error={errors[AdminSettingKey.EMBEDDINGS] ?? errors[AdminSettingKey.RERANKING]}
			field={field(
				role === AdminModelRole.RERANKING ? AdminSettingKey.RERANKING : AdminSettingKey.EMBEDDINGS
			)}
			forId="ms-role"
			inheritedText={describeSettingValue(
				AdminSettingKey.EMBEDDINGS,
				field(AdminSettingKey.EMBEDDINGS).inherited
			)}
			isSet={roleSet}
			label="Used for"
			onClear={() =>
				update({
					[AdminSettingKey.EMBEDDINGS]: null,
					[AdminSettingKey.POOLING]: null,
					[AdminSettingKey.RERANKING]: null
				})}
			origin={roleOrigin}
		>
			{@render selectControl(
				'ms-role',
				Object.values(AdminModelRole).map((value) => ({
					label: ADMIN_MODEL_ROLE_LABELS[value],
					value
				})),
				role,
				handleRole,
				field(AdminSettingKey.EMBEDDINGS).locked !== null ||
					field(AdminSettingKey.RERANKING).locked !== null
			)}

			{#if roleSet}
				<p class="flex items-start gap-1.5 text-xs text-amber-600 dark:text-amber-400">
					<TriangleAlert class="mt-0.5 h-3 w-3 shrink-0" />

					<span>
						The role decides which API endpoints work for this model: chat for /v1/chat/completions,
						embeddings for /v1/embeddings, reranking for /v1/rerank.
					</span>
				</p>
			{/if}
		</AdminSettingField>

		{#if role === AdminModelRole.EMBEDDINGS}
			<AdminSettingField
				error={errors[AdminSettingKey.POOLING]}
				field={field(AdminSettingKey.POOLING)}
				forId="ms-pooling"
				help="How the token vectors become one embedding. Without a value the model file decides."
				inactiveText={inactiveText(AdminSettingKey.POOLING)}
				inheritedText={inherited(AdminSettingKey.POOLING)}
				isSet={isSet(AdminSettingKey.POOLING)}
				label={ADMIN_SETTING_LABELS[AdminSettingKey.POOLING]}
				onClear={() => setValue(AdminSettingKey.POOLING, null)}
				origin={eff(AdminSettingKey.POOLING).origin}
			>
				{@render selectControl(
					'ms-pooling',
					[
						{ label: `Inherit: ${inherited(AdminSettingKey.POOLING)}`, value: INHERIT },
						...poolingValues.map((value) => ({
							label: ADMIN_POOLING_LABELS[value] ?? value,
							value
						}))
					],
					choice(AdminSettingKey.POOLING, poolingValues),
					(value) => setValue(AdminSettingKey.POOLING, value === INHERIT ? null : value),
					field(AdminSettingKey.POOLING).locked !== null
				)}
			</AdminSettingField>
		{/if}
	</section>
</div>
