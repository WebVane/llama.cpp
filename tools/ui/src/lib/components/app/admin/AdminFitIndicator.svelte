<script lang="ts">
	import {
		CircleAlert,
		CircleCheck,
		CircleHelp,
		Cpu,
		Loader2,
		TriangleAlert
	} from '@lucide/svelte';
	import { Badge } from '$lib/components/ui/badge';
	import { AdminFitErrorKind, AdminFitSource, AdminFitVerdict } from '$lib/enums';
	import type { AdminFitDevice, AdminFitResult } from '$lib/types';
	import { describeFit, fitCheckErrorKind, formatAge, formatBytes } from '$lib/utils';
	import type { Component } from 'svelte';

	interface Props {
		result: AdminFitResult | null;
		/** The exact check by llama.cpp is running */
		checking?: boolean;
		/** Why the exact check is not shown (failed request, not available on this server...) */
		checkNote?: string | null;
	}

	let { checking = false, checkNote = null, result }: Props = $props();

	const VERDICT_STYLES: Record<AdminFitVerdict, { class: string; icon: Component }> = {
		[AdminFitVerdict.CPU_ONLY]: { class: 'border-border bg-muted/40', icon: Cpu },
		[AdminFitVerdict.FITS]: {
			class: 'border-green-500/40 bg-green-500/10 text-green-700 dark:text-green-400',
			icon: CircleCheck
		},
		[AdminFitVerdict.NO]: {
			class: 'border-destructive/40 bg-destructive/10 text-destructive',
			icon: CircleAlert
		},
		[AdminFitVerdict.TIGHT]: {
			class: 'border-amber-500/40 bg-amber-500/10 text-amber-700 dark:text-amber-400',
			icon: TriangleAlert
		},
		[AdminFitVerdict.UNKNOWN]: { class: 'border-border bg-muted/40', icon: CircleHelp }
	};

	let text = $derived(result ? describeFit(result) : null);
	let errorKind = $derived(result?.error ? fitCheckErrorKind(result.error.code) : null);
	// a check that could not run judged nothing: neutral, not "rejected"
	let style = $derived(
		errorKind && errorKind !== AdminFitErrorKind.NOT_CHECKED
			? VERDICT_STYLES[AdminFitVerdict.NO]
			: VERDICT_STYLES[
					errorKind ? AdminFitVerdict.UNKNOWN : (result?.verdict ?? AdminFitVerdict.UNKNOWN)
				]
	);
	let isEstimate = $derived(result?.source !== AdminFitSource.CHECK);

	function percent(part: number, whole: number): string {
		if (whole <= 0) return '0%';

		return `${Math.max(0, Math.min(100, (part / whole) * 100)).toFixed(2)}%`;
	}

	/** others | this model | margin | free, as fractions of the card total. */
	function barParts(device: AdminFitDevice, margin: number) {
		const total = device.total ?? 0;
		const available = device.available ?? 0;
		const others = Math.max(0, total - available);
		const need = Math.min(device.need.total, available);
		const reserve = Math.min(margin, Math.max(0, available - device.need.total));
		const free = Math.max(0, available - need - reserve);

		return { free, need, others, overflow: device.need.total > available, reserve, total };
	}
</script>

<section
	aria-label="Will it fit"
	aria-live="polite"
	class={['flex flex-col gap-2 rounded-lg border p-3 text-sm', style.class]}
	data-source={result?.source ?? 'none'}
	data-testid="admin-fit"
	data-verdict={errorKind ?? result?.verdict ?? 'none'}
>
	<div class="flex flex-wrap items-start gap-2">
		{#if result}
			{@const VerdictIcon = style.icon}

			<VerdictIcon class="mt-0.5 h-4 w-4 shrink-0" />
		{:else}
			<Loader2 class="mt-0.5 h-4 w-4 shrink-0 animate-spin" />
		{/if}

		<span class="min-w-0 flex-1 font-medium">
			{text?.title ?? 'Checking whether it fits...'}
		</span>

		{#if checking}
			<Badge class="gap-1 font-normal text-foreground" variant="outline">
				<Loader2 class="h-3 w-3 animate-spin" />

				checking with llama.cpp
			</Badge>
		{/if}

		{#if result}
			<Badge
				class="font-normal text-foreground"
				data-testid="admin-fit-source"
				title={isEstimate
					? 'An instant guess from the model file; the exact check by llama.cpp replaces it'
					: 'llama.cpp planned the load on the server without allocating the weights'}
				variant="outline"
			>
				{isEstimate ? 'rough estimate' : 'checked by llama.cpp'}
			</Badge>
		{/if}
	</div>

	{#each text?.lines ?? [] as line (line)}
		<p class="text-xs break-words text-foreground/80">{line}</p>
	{/each}

	{#if result && !result.error}
		{#each result.devices as device (device.device)}
			{@const parts = barParts(device, result.margin)}

			<div class="flex flex-col gap-1 text-foreground" data-fit-device={device.device}>
				<div class="flex flex-wrap items-baseline justify-between gap-x-3 text-xs">
					<span>
						<span class="font-mono font-medium">{device.device}</span>

						{#if device.description}
							<span class="text-muted-foreground">· {device.description}</span>
						{/if}
					</span>

					<span class="text-muted-foreground tabular-nums">
						needs {formatBytes(device.need.total)}
						{#if device.free !== null}
							· {formatBytes(device.free)} free{device.ownUsage > 0
								? ` + ${formatBytes(device.ownUsage)} it uses now`
								: ''}
						{/if}
					</span>
				</div>

				{#if parts.total > 0 && device.available !== null}
					<div
						aria-label="{device.device}: memory needed against memory available"
						class="flex h-2 w-full overflow-hidden rounded-full bg-muted"
						role="img"
					>
						<div
							class="h-full bg-muted-foreground/40"
							style="width: {percent(parts.others, parts.total)}"
							title="used by others: {formatBytes(parts.others)}"
						></div>

						<div
							class={['h-full', parts.overflow ? 'bg-destructive' : 'bg-sky-500']}
							style="width: {percent(parts.need, parts.total)}"
							title="this model: {formatBytes(device.need.total)}"
						></div>

						<div
							class="h-full bg-amber-500/50"
							style="width: {percent(parts.reserve, parts.total)}"
							title="safety margin: {formatBytes(result.margin)}"
						></div>
					</div>
				{/if}

				<span class="text-xs text-muted-foreground tabular-nums">
					model {formatBytes(device.need.model)} · context {formatBytes(device.need.context)} · compute
					{formatBytes(device.need.compute)}
					{#if device.memoryAgeMs !== null}
						· free memory measured {formatAge(device.memoryAgeMs)} ago
					{/if}
				</span>
			</div>
		{/each}

		{#if result.host && result.host.total > 0}
			<span class="text-xs text-muted-foreground tabular-nums">
				CPU side: model {formatBytes(result.host.model)} · context {formatBytes(
					result.host.context
				)}
				· compute {formatBytes(result.host.compute)}
			</span>
		{/if}

		{#each result.notes as note (note)}
			<p class="text-xs text-muted-foreground">{note}</p>
		{/each}
	{/if}

	{#if checkNote}
		<p class="text-xs text-muted-foreground" data-testid="admin-fit-note">{checkNote}</p>
	{/if}

	<p class="text-[11px] text-muted-foreground">
		The check warns only; it never blocks a save.
		{#if result?.checkedAt && !isEstimate}
			Checked at {new Date(result.checkedAt).toLocaleTimeString()}.
		{/if}
	</p>
</section>
