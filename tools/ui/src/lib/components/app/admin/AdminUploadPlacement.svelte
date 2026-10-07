<script lang="ts">
	import AdminNumberInput from './AdminNumberInput.svelte';
	import { ChevronDown, Lock } from '@lucide/svelte';
	import * as Collapsible from '$lib/components/ui/collapsible';
	import * as Select from '$lib/components/ui/select';
	import {
		ADMIN_CONTEXT_QUICK_PICKS,
		ADMIN_DEVICE_NONE,
		ADMIN_LOCK_TEXTS,
		ADMIN_MODEL_ROLE_LABELS,
		ADMIN_PARALLEL_CHOICES
	} from '$lib/constants';
	import { AdminLockSource, AdminModelRole, AdminSettingKey } from '$lib/enums';
	import type { ApiAdminGpu, ApiAdminSettingLock } from '$lib/types';
	import type { AdminUploadPlacementValue } from '$lib/types/admin-upload';
	import { formatBytes } from '$lib/utils/admin';
	import { formatTokensShort } from '$lib/utils/admin-model-settings';

	interface Props {
		value: AdminUploadPlacementValue;
		/** GPU devices of the server (empty on a CPU-only build) */
		devices: string[];
		/** GPU cards with their free memory, for the estimate */
		gpus: ApiAdminGpu[];
		/** Fields fixed by MODEL_DEVICES or the router command line */
		locks: Record<string, ApiAdminSettingLock>;
		/** Bytes of the model files: the weights-only estimate */
		weightsBytes: number;
		/** Trained context of the model, from its header */
		nCtxTrain: number | null;
		/** Field errors of the server (by setting key) */
		errors?: Record<string, string>;
		disabled?: boolean;
		idPrefix: string;
		onChange: (value: AdminUploadPlacementValue) => void;
	}

	let {
		devices,
		disabled = false,
		errors = {},
		gpus,
		idPrefix,
		locks,
		nCtxTrain,
		onChange,
		value,
		weightsBytes
	}: Props = $props();

	const AUTO = '__auto__';

	let moreOpen = $state(false);
	let deviceLock = $derived(locks[AdminSettingKey.DEVICE] ?? null);
	let ctxLock = $derived(locks[AdminSettingKey.CTX_SIZE] ?? null);
	let parallelLock = $derived(locks[AdminSettingKey.PARALLEL] ?? null);
	let freeByDevice = $derived(
		Object.fromEntries(gpus.map((gpu) => [gpu.device, gpu.free])) as Record<string, number | null>
	);
	let deviceOptions = $derived([
		{ label: 'Automatic (inherited)', value: AUTO },
		...(devices.length > 0 ? [{ label: 'CPU only', value: ADMIN_DEVICE_NONE }] : []),
		...devices.map((device) => {
			const free = freeByDevice[device];

			return {
				label:
					free !== null && free !== undefined ? `${device} (${formatBytes(free)} free)` : device,
				value: device
			};
		})
	]);
	let parallelOptions = $derived([
		{ label: 'Automatic', value: AUTO },
		...ADMIN_PARALLEL_CHOICES.map((count) => ({ label: String(count), value: String(count) }))
	]);
	let roleOptions = $derived(
		Object.values(AdminModelRole).map((role) => ({
			label: ADMIN_MODEL_ROLE_LABELS[role],
			value: role
		}))
	);
	let quickPicks = $derived(
		[...ADMIN_CONTEXT_QUICK_PICKS, ...(nCtxTrain ? [nCtxTrain] : [])].filter(
			(tokens, index, all) => all.indexOf(tokens) === index && (!nCtxTrain || tokens <= nCtxTrain)
		)
	);
	// weights only: the context and the compute buffers come on top (checked by llama.cpp after the upload)
	let estimate = $derived.by(() => {
		if (weightsBytes <= 0) return null;

		const device = deviceLock?.value ?? value.device;

		if (!device || device === ADMIN_DEVICE_NONE || devices.length === 0) {
			return `The weights alone need about ${formatBytes(weightsBytes)} of memory.`;
		}

		const names = device.split(/[,;]/).filter((name) => name);
		const free = names.reduce((sum, name) => sum + (freeByDevice[name] ?? 0), 0);

		if (free <= 0) return `The weights alone need about ${formatBytes(weightsBytes)}.`;

		return weightsBytes > free
			? `The weights alone need about ${formatBytes(weightsBytes)}, more than the ${formatBytes(free)} free on ${names.join(' + ')}.`
			: `The weights alone need about ${formatBytes(weightsBytes)}; ${names.join(' + ')} has ${formatBytes(free)} free. The context comes on top.`;
	});

	function labelOf(options: { label: string; value: string }[], current: string): string {
		return options.find((option) => option.value === current)?.label ?? current;
	}

	function lockText(lock: ApiAdminSettingLock): string {
		return ADMIN_LOCK_TEXTS[lock.by as AdminLockSource] ?? `Fixed by ${lock.by}.`;
	}

	function set(patch: Partial<AdminUploadPlacementValue>) {
		onChange({ ...value, ...patch });
	}
</script>

{#snippet lockNote(lock: ApiAdminSettingLock)}
	<p class="flex items-start gap-1.5 text-xs text-muted-foreground" data-testid="upload-lock">
		<Lock class="mt-0.5 h-3 w-3 shrink-0" />

		<span>
			{lock.value ? `${lock.value}: ` : ''}{lockText(lock)}
		</span>
	</p>
{/snippet}

<div class="flex flex-col gap-4" data-testid="upload-placement">
	<div class="flex flex-col gap-1.5">
		<label class="text-sm font-medium" for="{idPrefix}-device">GPU</label>

		<Select.Root
			disabled={disabled || deviceLock !== null}
			onValueChange={(next) => set({ device: next === AUTO ? '' : next })}
			type="single"
			value={deviceLock ? (deviceLock.value ?? AUTO) : value.device || AUTO}
		>
			<Select.Trigger class="w-full sm:w-80" id="{idPrefix}-device">
				<span class="truncate">
					{deviceLock
						? (deviceLock.value ?? 'locked')
						: labelOf(deviceOptions, value.device || AUTO)}
				</span>
			</Select.Trigger>

			<Select.Content>
				{#each deviceOptions as option (option.value)}
					<Select.Item label={option.label} value={option.value} />
				{/each}
			</Select.Content>
		</Select.Root>

		{#if deviceLock}
			{@render lockNote(deviceLock)}
		{:else if devices.length === 0}
			<p class="text-xs text-muted-foreground">
				This server has no GPU: the model runs on the CPU.
			</p>
		{/if}

		{#if errors[AdminSettingKey.DEVICE]}
			<p class="text-xs text-destructive">{errors[AdminSettingKey.DEVICE]}</p>
		{/if}
	</div>

	<div class="flex flex-col gap-1.5">
		<label class="text-sm font-medium" for="{idPrefix}-ctx">Context size</label>

		<div class="flex flex-wrap items-center gap-2">
			<AdminNumberInput
				disabled={disabled || ctxLock !== null}
				id="{idPrefix}-ctx"
				invalid={!!errors[AdminSettingKey.CTX_SIZE]}
				min={0}
				onValueChange={(text) => set({ ctxSize: text })}
				placeholder={nCtxTrain ? String(nCtxTrain) : 'trained'}
				step={256}
				value={ctxLock ? ctxLock.value : value.ctxSize || null}
			/>

			<span class="text-xs text-muted-foreground">tokens</span>

			{#each quickPicks as tokens (tokens)}
				<button
					class={[
						'rounded border px-1.5 py-0.5 text-xs hover:bg-muted disabled:opacity-50',
						value.ctxSize === String(tokens) && 'border-primary bg-primary/10'
					]}
					disabled={disabled || ctxLock !== null}
					onclick={() => set({ ctxSize: String(tokens) })}
					type="button"
				>
					{tokens === nCtxTrain
						? `trained (${formatTokensShort(tokens)})`
						: formatTokensShort(tokens)}
				</button>
			{/each}
		</div>

		{#if ctxLock}
			{@render lockNote(ctxLock)}
		{:else}
			<p class="text-xs text-muted-foreground">
				Empty: the model uses its trained context{nCtxTrain
					? ` (${nCtxTrain.toLocaleString('en-US')} tokens)`
					: ''}, shared by the parallel requests.
			</p>
		{/if}

		{#if errors[AdminSettingKey.CTX_SIZE]}
			<p class="text-xs text-destructive">{errors[AdminSettingKey.CTX_SIZE]}</p>
		{/if}
	</div>

	<div class="flex flex-col gap-1.5">
		<label class="text-sm font-medium" for="{idPrefix}-parallel">Parallel requests</label>

		<Select.Root
			disabled={disabled || parallelLock !== null}
			onValueChange={(next) => set({ parallel: next === AUTO ? '' : next })}
			type="single"
			value={value.parallel || AUTO}
		>
			<Select.Trigger class="w-full sm:w-80" id="{idPrefix}-parallel">
				<span class="truncate">
					{parallelLock
						? (parallelLock.value ?? 'locked')
						: labelOf(parallelOptions, value.parallel || AUTO)}
				</span>
			</Select.Trigger>

			<Select.Content>
				{#each parallelOptions as option (option.value)}
					<Select.Item label={option.label} value={option.value} />
				{/each}
			</Select.Content>
		</Select.Root>

		{#if parallelLock}
			{@render lockNote(parallelLock)}
		{/if}

		{#if errors[AdminSettingKey.PARALLEL]}
			<p class="text-xs text-destructive">{errors[AdminSettingKey.PARALLEL]}</p>
		{/if}
	</div>

	<Collapsible.Root bind:open={moreOpen}>
		<Collapsible.Trigger
			class="flex items-center gap-1.5 text-xs font-semibold tracking-wide text-muted-foreground uppercase hover:text-foreground"
		>
			<ChevronDown class={['h-3.5 w-3.5 transition-transform', moreOpen && 'rotate-180']} />

			More
		</Collapsible.Trigger>

		<Collapsible.Content>
			<div class="mt-3 flex flex-col gap-1.5">
				<label class="text-sm font-medium" for="{idPrefix}-role">Used for</label>

				<Select.Root
					{disabled}
					onValueChange={(next) => set({ role: next as AdminModelRole })}
					type="single"
					value={value.role}
				>
					<Select.Trigger class="w-full sm:w-80" id="{idPrefix}-role">
						<span class="truncate">{labelOf(roleOptions, value.role)}</span>
					</Select.Trigger>

					<Select.Content>
						{#each roleOptions as option (option.value)}
							<Select.Item label={option.label} value={option.value} />
						{/each}
					</Select.Content>
				</Select.Root>

				<p class="text-xs text-muted-foreground">
					Everything else (GPU layers, flash attention, KV cache precision...) is set in the model
					settings after the upload.
				</p>
			</div>
		</Collapsible.Content>
	</Collapsible.Root>

	{#if estimate}
		<p class="text-xs text-muted-foreground" data-testid="upload-estimate">
			{estimate} A rough estimate; llama.cpp checks the fit after the upload.
		</p>
	{/if}
</div>
