<script lang="ts">
	import { Pin, TriangleAlert } from '@lucide/svelte';
	import { Badge } from '$lib/components/ui/badge';
	import { AdminModelMemorySource, ServerModelStatus } from '$lib/enums';
	import type { AdminGpuBarSegment, ApiAdminGpu, ApiAdminGpuModel } from '$lib/types';
	import {
		describeGpuFree,
		describeGpuMapping,
		describeGpuMemorySource,
		describeModelMemorySource,
		formatBytes,
		gpuBarSegments,
		isRunningStatus
	} from '$lib/utils';

	interface Props {
		gpu: ApiAdminGpu;
		/** Open the settings of a model shown on the card */
		onOpenModel?: (id: string) => void;
		/** A vendor tool (nvidia-smi...) answered, so its rows could be matched */
		vendorRowsRead?: boolean;
	}

	let { gpu, onOpenModel, vendorRowsRead = true }: Props = $props();

	// model parts of the bar cycle through these
	const MODEL_COLORS = [
		'bg-sky-500',
		'bg-violet-500',
		'bg-emerald-500',
		'bg-rose-500',
		'bg-amber-500'
	];
	const HATCH =
		'background-image: repeating-linear-gradient(135deg, rgba(255,255,255,0.35) 0 3px, transparent 3px 6px);';
	const STATUS_DOTS: Record<string, string> = {
		[ServerModelStatus.LOADED]: 'bg-green-500',
		[ServerModelStatus.LOADING]: 'bg-blue-500 animate-pulse',
		[ServerModelStatus.SLEEPING]: 'bg-orange-400'
	};

	let segments = $derived(gpuBarSegments(gpu));
	let usedSegments = $derived(segments.filter((segment) => segment.kind !== 'free'));
	let freeText = $derived(describeGpuFree(gpu));
	let mappingText = $derived(describeGpuMapping(gpu, vendorRowsRead));
	// running models, plus models placed on this GPU by name (not "all GPUs") that are stopped
	let shownModels = $derived(
		gpu.models.filter((model) => isRunningStatus(model.status) || !model.all_gpus || model.pinned)
	);
	let hiddenCount = $derived(gpu.models.length - shownModels.length);
	// one colour per model part, the same on the bar and on its chip
	let colorOf = $derived(
		Object.fromEntries(
			usedSegments
				.filter((segment) => segment.kind === 'model' && segment.modelId)
				.map((segment, index) => [
					segment.modelId as string,
					MODEL_COLORS[index % MODEL_COLORS.length]
				])
		) as Record<string, string>
	);

	function segmentClass(segment: AdminGpuBarSegment): string {
		if (segment.kind === 'model') return colorOf[segment.modelId ?? ''] ?? MODEL_COLORS[0];

		if (segment.kind === 'pending') return 'bg-muted-foreground/30';

		return 'bg-muted-foreground/45';
	}

	function segmentStyle(segment: AdminGpuBarSegment): string {
		const hatched =
			segment.kind === 'pending' ||
			(segment.kind === 'model' && segment.source === AdminModelMemorySource.ESTIMATE);

		return `width: ${(segment.fraction * 100).toFixed(2)}%;${hatched ? ` ${HATCH}` : ''}`;
	}

	function memoryText(model: ApiAdminGpuModel): string | null {
		if (model.memory) {
			const prefix = model.memory.source === AdminModelMemorySource.ESTIMATE ? '≈ ' : '';

			return `${prefix}${formatBytes(model.memory.bytes)} ${describeModelMemorySource(model.memory.source)}`;
		}

		if (isRunningStatus(model.status)) {
			return model.facts_status === 'pending' ? 'estimate pending' : 'memory not known';
		}

		return null;
	}
</script>

<article
	class="flex min-w-0 flex-col gap-2 rounded-lg border bg-card p-3"
	data-gpu={gpu.device}
	data-memory-source={gpu.memory_source}
	data-settling={gpu.settling ? 'true' : 'false'}
>
	<header class="flex min-w-0 items-start justify-between gap-2">
		<div class="min-w-0">
			<div class="flex flex-wrap items-center gap-1.5">
				<span class="font-mono text-sm font-semibold">{gpu.device}</span>

				{#if gpu.backend}
					<Badge class="px-1.5 py-0 text-[10px] font-normal" variant="outline">{gpu.backend}</Badge>
				{/if}

				{#if gpu.settling}
					<Badge
						class="px-1.5 py-0 text-[10px] font-normal text-blue-600 dark:text-blue-400"
						title="A model on this GPU is loading or stopping: the figures are about to change"
						variant="outline"
					>
						settling
					</Badge>
				{/if}
			</div>

			<p class="truncate text-xs text-muted-foreground" title={gpu.description}>
				{gpu.description || gpu.name || 'GPU'}
			</p>
		</div>

		{#if mappingText}
			<Badge
				class="shrink-0 px-1.5 py-0 text-[10px] font-normal text-amber-600 dark:text-amber-400"
				title="The memory tool rows were not matched to this device by PCI bus id"
				variant="outline"
			>
				{mappingText}
			</Badge>
		{/if}
	</header>

	{#if usedSegments.length > 0 || freeText}
		<div
			aria-label="{gpu.device} memory: {freeText ?? 'unknown'}"
			aria-valuemax={gpu.total ?? 0}
			aria-valuemin={0}
			aria-valuenow={gpu.used ?? 0}
			class="flex h-2.5 w-full overflow-hidden rounded-full bg-muted"
			role="meter"
		>
			{#each usedSegments as segment (segment.key)}
				<div
					class={['h-full', segmentClass(segment)]}
					style={segmentStyle(segment)}
					title="{segment.label}: {formatBytes(segment.bytes)}"
				></div>
			{/each}
		</div>

		<div class="flex flex-wrap items-baseline justify-between gap-x-2 text-xs">
			<span class="font-medium tabular-nums">{freeText ?? 'free memory unknown'}</span>

			<span class="text-muted-foreground" data-testid="gpu-memory-source">
				{describeGpuMemorySource(gpu)}
			</span>
		</div>

		{#each usedSegments.filter((segment) => segment.kind !== 'model') as segment (segment.key)}
			<span class="text-[11px] text-muted-foreground tabular-nums">
				{formatBytes(segment.bytes)}
				{segment.label}
			</span>
		{/each}
	{:else}
		<p class="text-xs text-muted-foreground">
			Memory usage unavailable{gpu.unsupported
				? ': this GPU reports no memory figures (MIG / vGPU)'
				: ''}.
		</p>
	{/if}

	{#if gpu.same_gpu_as.length > 0}
		<p class="text-[11px] text-muted-foreground">
			Same physical GPU as {gpu.same_gpu_as.join(', ')}.
		</p>
	{/if}

	{#if shownModels.length > 0}
		<ul aria-label="Models on {gpu.device}" class="flex flex-wrap gap-1.5">
			{#each shownModels as model (model.id)}
				{@const running = isRunningStatus(model.status)}
				{@const memory = memoryText(model)}

				<li class="min-w-0">
					<button
						class={[
							'flex max-w-full min-w-0 items-center gap-1.5 rounded-md border px-2 py-1 text-left text-xs hover:bg-muted',
							!running && 'text-muted-foreground'
						]}
						data-gpu-model={model.id}
						disabled={!onOpenModel}
						onclick={() => onOpenModel?.(model.id)}
						title="{model.id}{model.all_gpus
							? ' (no GPU set: may use every GPU)'
							: ''} — open its settings"
						type="button"
					>
						<span
							aria-hidden="true"
							class={[
								'size-2 shrink-0 rounded-full',
								colorOf[model.id] ?? STATUS_DOTS[model.status] ?? 'bg-muted-foreground/40'
							]}
						></span>

						<span class="truncate font-mono">{model.id}</span>

						{#if memory}
							<span class="shrink-0 tabular-nums opacity-80">{memory}</span>
						{/if}

						{#if model.all_gpus}
							<span class="shrink-0 opacity-70">all GPUs</span>
						{/if}

						{#if model.pinned}
							<Pin class="h-3 w-3 shrink-0" />
						{/if}
					</button>
				</li>
			{/each}
		</ul>
	{/if}

	{#if hiddenCount > 0}
		<p class="text-[11px] text-muted-foreground">
			{hiddenCount} stopped model{hiddenCount === 1 ? '' : 's'} without a GPU setting may also use it.
		</p>
	{/if}

	{#if gpu.unsupported && usedSegments.length > 0}
		<p class="flex items-center gap-1 text-[11px] text-amber-600 dark:text-amber-400">
			<TriangleAlert class="h-3 w-3" />

			Partitioned GPU (MIG / vGPU): the figures may not be complete.
		</p>
	{/if}
</article>
