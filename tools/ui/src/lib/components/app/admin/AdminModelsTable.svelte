<script lang="ts">
	import {
		Loader2,
		Pin,
		Power,
		PowerOff,
		RotateCcw,
		SlidersHorizontal,
		Square,
		Trash2,
		TriangleAlert,
		X
	} from '@lucide/svelte';
	import { Badge } from '$lib/components/ui/badge';
	import { Button } from '$lib/components/ui/button';
	import * as Table from '$lib/components/ui/table';
	import {
		ADMIN_MODEL_SOURCE_HINTS,
		ADMIN_MODEL_SOURCE_LABELS,
		ADMIN_MODEL_STATUS_LABELS,
		ICON_CLASS_DEFAULT
	} from '$lib/constants';
	import {
		AdminFeature,
		AdminModelOperation,
		ServerModelSource,
		ServerModelStatus
	} from '$lib/enums';
	import { adminStore, deviceStore, modelsStore } from '$lib/stores';
	import type { AdminModelRow } from '$lib/types';
	import {
		describeFailureReason,
		formatBytes,
		modelLoadFraction,
		modelLoadProgressText,
		placementSummary
	} from '$lib/utils';

	interface Props {
		rows: AdminModelRow[];
		/** Ask for confirmation before a cache model is deleted. */
		onDelete: (row: AdminModelRow) => void;
		/** "Delete files..." of a models_dir model (stage 3: the files its preset uses) */
		onDeleteFiles?: (row: AdminModelRow) => void;
		/** Open the per-model settings (stage 2). */
		onSettings?: (row: AdminModelRow) => void;
		/** Show the output tail and failure reason of a model (stage 2). */
		onShowLog?: (row: AdminModelRow) => void;
	}

	let { onDelete, onDeleteFiles, onSettings, onShowLog, rows }: Props = $props();

	// narrow screens: the Actions column folds under the status
	let isNarrow = $derived(deviceStore.isMobile);
	// stage-2 parts only when the server has them (a cached newer page on an older server)
	let showSettings = $derived(adminStore.hasFeature(AdminFeature.MODEL_SETTINGS));
	let showLog = $derived(adminStore.hasFeature(AdminFeature.MODEL_LOG));
	let hasGpus = $derived(adminStore.buildDevices.length > 0);
	// only a server that lists its devices can tell that a model names a missing one
	let listedDevices = $derived(
		adminStore.modelSettings?.build_devices ? adminStore.buildDevices : null
	);

	/** Reason of the last failed start, from the per-model settings row. */
	function failureLabel(row: AdminModelRow): string | null {
		const settings = adminStore.modelSettingsRow(row.id);
		const reason = settings?.last_failure?.reason ?? settings?.last_run?.reason ?? null;

		return reason ? describeFailureReason(reason).label : null;
	}

	const STATUS_DOT_CLASSES: Record<ServerModelStatus, string> = {
		[ServerModelStatus.DOWNLOADED]: 'bg-blue-500',
		[ServerModelStatus.DOWNLOADING]: 'bg-blue-500 animate-pulse',
		[ServerModelStatus.FAILED]: 'bg-red-500',
		[ServerModelStatus.LOADED]: 'bg-green-500',
		[ServerModelStatus.LOADING]: 'bg-blue-500 animate-pulse',
		[ServerModelStatus.SLEEPING]: 'bg-orange-400',
		[ServerModelStatus.UNLOADED]: 'bg-muted-foreground/50'
	};
	const OPERATION_LABELS: Record<AdminModelOperation, string> = {
		[AdminModelOperation.CANCELLING]: 'Cancelling...',
		[AdminModelOperation.DELETING]: 'Deleting...',
		[AdminModelOperation.STOPPING]: 'Stopping...'
	};

	/** Fraction (0..1) of the running load or download of a row, null when unknown or idle. */
	function progressFraction(row: AdminModelRow): number | null {
		if (row.status === ServerModelStatus.LOADING) {
			const progress = modelsStore.status.getLoadProgress(row.id);

			return progress ? modelLoadFraction(progress) : null;
		}

		return row.download?.fraction ?? null;
	}

	function statusText(row: AdminModelRow): string {
		const label = ADMIN_MODEL_STATUS_LABELS[row.status] ?? row.status;

		if (row.status === ServerModelStatus.FAILED) {
			const reason = showSettings ? failureLabel(row) : null;

			if (reason) return `${label}: ${reason}`;

			if (row.exitCode !== null) return `${label} (exit code ${row.exitCode})`;
		}

		const fraction = progressFraction(row);

		return fraction === null ? label : `${label} ${Math.round(fraction * 100)}%`;
	}

	/** Second line under the status: load stage or downloaded bytes. */
	function statusDetail(row: AdminModelRow): string | null {
		if (row.status === ServerModelStatus.LOADING) {
			return modelLoadProgressText(modelsStore.status.getLoadProgress(row.id));
		}

		if (row.status === ServerModelStatus.DOWNLOADED) return 'Finishing...';

		if (!row.download) return null;

		if (row.download.total > 0) {
			return `${formatBytes(row.download.done)} of ${formatBytes(row.download.total)}`;
		}

		return row.download.done > 0 ? formatBytes(row.download.done) : 'Starting...';
	}

	function hasProgressBar(row: AdminModelRow): boolean {
		return row.status === ServerModelStatus.LOADING || row.status === ServerModelStatus.DOWNLOADING;
	}
</script>

{#snippet sourceBadge(row: AdminModelRow)}
	{#if row.source}
		<Badge class="font-normal" title={ADMIN_MODEL_SOURCE_HINTS[row.source]} variant="outline">
			{ADMIN_MODEL_SOURCE_LABELS[row.source] ?? row.source}
		</Badge>
	{/if}
{/snippet}

{#snippet placementCell(row: AdminModelRow)}
	{@const settings = adminStore.modelSettingsRow(row.id)}

	{#if settings}
		{@const placement = placementSummary(settings, hasGpus, listedDevices)}

		<button
			class="inline-flex max-w-full items-center gap-1.5 rounded px-1 py-0.5 text-left text-xs hover:bg-muted"
			data-placement={row.id}
			disabled={!onSettings}
			onclick={() => onSettings?.(row)}
			title="GPU and context size of this model{placement.custom
				? ' (custom settings)'
				: ''} — open its settings"
			type="button"
		>
			<span class="truncate font-mono">
				{placement.device}{placement.context ? ` · ${placement.context}` : ''}
			</span>

			{#if placement.custom}
				<span
					aria-label="custom settings"
					class={[
						'size-1.5 shrink-0 rounded-full',
						placement.problem ? 'bg-amber-500' : 'bg-primary'
					]}
				></span>
			{/if}

			{#if settings.reverted}
				<RotateCcw
					aria-label="settings reverted after failed starts"
					class="h-3 w-3 shrink-0 text-amber-600 dark:text-amber-400"
				/>
			{/if}

			{#if placement.unknownDevice}
				<TriangleAlert
					aria-label="this GPU is not on this server"
					class="h-3 w-3 shrink-0 text-amber-600 dark:text-amber-400"
				/>
			{/if}
		</button>
	{/if}
{/snippet}

{#snippet tagBadges(row: AdminModelRow)}
	{#each row.tags as tag (tag)}
		<Badge class="font-mono text-[11px] font-normal" variant="tertiary">{tag}</Badge>
	{/each}
{/snippet}

{#snippet statusCell(row: AdminModelRow)}
	{@const operation = adminStore.operations.get(row.id)}
	{@const fraction = progressFraction(row)}
	{@const detail = statusDetail(row)}

	<div class="flex min-w-40 flex-col gap-1" data-status={row.status}>
		{#if operation}
			<span class="flex items-center gap-2 text-muted-foreground">
				<Loader2 class="h-3.5 w-3.5 animate-spin" />

				{OPERATION_LABELS[operation]}
			</span>
		{:else}
			<span class="flex items-center gap-2">
				<span
					aria-hidden="true"
					class={['size-2 shrink-0 rounded-full', STATUS_DOT_CLASSES[row.status]]}
				></span>

				<span class={row.status === ServerModelStatus.FAILED ? 'text-destructive' : ''}>
					{statusText(row)}
				</span>
			</span>
		{/if}

		{#if hasProgressBar(row)}
			<div
				aria-label="{ADMIN_MODEL_STATUS_LABELS[row.status]} progress"
				aria-valuemax={100}
				aria-valuemin={0}
				aria-valuenow={fraction === null ? undefined : Math.round(fraction * 100)}
				class="h-1 w-full max-w-44 overflow-hidden rounded-full bg-muted"
				role="progressbar"
			>
				<div
					class={[
						'h-full rounded-full bg-primary transition-[width] duration-300 ease-out',
						fraction === null && 'w-1/3 animate-pulse'
					]}
					style={fraction === null ? undefined : `width: ${Math.round(fraction * 100)}%`}
				></div>
			</div>
		{/if}

		{#if detail && !operation}
			<span class="text-xs text-muted-foreground tabular-nums">{detail}</span>
		{/if}

		{#if row.status === ServerModelStatus.FAILED && showLog && onShowLog && !operation}
			<button
				class="w-fit text-xs text-muted-foreground underline underline-offset-2 hover:text-foreground"
				data-show-log={row.id}
				onclick={() => onShowLog?.(row)}
				type="button"
			>
				Show log
			</button>
		{/if}
	</div>
{/snippet}

{#snippet actionsCell(row: AdminModelRow, justify: string)}
	{@const operation = adminStore.operations.get(row.id)}
	{@const busy = modelsStore.status.isOperationInProgress(row.id)}

	<div class={['flex items-center gap-1', justify]}>
		{#if operation}
			<Loader2 class="{ICON_CLASS_DEFAULT} mr-2 animate-spin text-muted-foreground" />
		{:else if row.status === ServerModelStatus.DOWNLOADING}
			<Button onclick={() => adminStore.cancelDownload(row.id)} size="sm" variant="outline">
				<X />

				Cancel
			</Button>
		{:else if row.status !== ServerModelStatus.DOWNLOADED}
			{#if row.status === ServerModelStatus.LOADING}
				<Button onclick={() => adminStore.stopLoading(row.id)} size="sm" variant="outline">
					<Square />

					Stop
				</Button>
			{:else if row.status === ServerModelStatus.LOADED || row.status === ServerModelStatus.SLEEPING}
				<Button
					disabled={busy}
					onclick={() => adminStore.unloadModel(row.id)}
					size="sm"
					variant="outline"
				>
					{#if busy}
						<Loader2 class="animate-spin" />
					{:else}
						<PowerOff />
					{/if}

					Unload
				</Button>
			{:else}
				<Button
					disabled={busy}
					onclick={() => adminStore.loadModel(row.id)}
					size="sm"
					variant="outline"
				>
					{#if busy}
						<Loader2 class="animate-spin" />
					{:else}
						<Power />
					{/if}

					Load
				</Button>
			{/if}

			{#if showSettings && onSettings}
				<Button
					aria-label="Settings of {row.id}"
					class="text-muted-foreground hover:text-foreground"
					data-settings={row.id}
					onclick={() => onSettings?.(row)}
					size="sm"
					title="GPU, context size and other settings of this model"
					variant="ghost"
				>
					<SlidersHorizontal />

					<span class="hidden lg:inline">Settings</span>
				</Button>
			{/if}

			{#if row.canDelete}
				<Button
					aria-label="Delete {row.id}"
					class="text-muted-foreground hover:text-destructive"
					disabled={busy}
					onclick={() => onDelete(row)}
					size="sm"
					title="Delete from the model cache"
					variant="ghost"
				>
					<Trash2 />

					<span class="hidden lg:inline">Delete</span>
				</Button>
			{:else if onDeleteFiles && row.source === ServerModelSource.MODELS_DIR}
				<Button
					aria-label="Delete the files of {row.id}"
					class="text-muted-foreground hover:text-destructive"
					data-delete-files={row.id}
					onclick={() => onDeleteFiles?.(row)}
					size="sm"
					title="Delete the files of this model from the models directory, or only its projector"
					variant="ghost"
				>
					<Trash2 />

					<span class="hidden lg:inline">Delete files</span>
				</Button>
			{/if}
		{/if}
	</div>
{/snippet}

<div class="overflow-hidden rounded-lg border bg-card">
	<Table.Root>
		<Table.Header>
			<Table.Row>
				<Table.Head class="bg-muted/40 pl-4 text-muted-foreground">Model</Table.Head>

				<Table.Head class="hidden bg-muted/40 text-muted-foreground md:table-cell">Tags</Table.Head>

				<Table.Head class="hidden bg-muted/40 text-muted-foreground md:table-cell">
					Source
				</Table.Head>

				{#if showSettings}
					<Table.Head class="hidden bg-muted/40 text-muted-foreground md:table-cell">
						Placement
					</Table.Head>
				{/if}

				<Table.Head class={['bg-muted/40 text-muted-foreground', isNarrow && 'pr-4']}>
					Status
				</Table.Head>

				{#if !isNarrow}
					<Table.Head class="bg-muted/40 pr-4 text-right text-muted-foreground">Actions</Table.Head>
				{/if}
			</Table.Row>
		</Table.Header>

		<Table.Body>
			{#each rows as row (row.id)}
				<Table.Row class="last:border-b-0" data-model-id={row.id}>
					<Table.Cell
						class="max-w-[14rem] pl-4 whitespace-normal sm:max-w-[22rem] md:whitespace-nowrap lg:max-w-[30rem]"
					>
						<div class="flex min-w-0 flex-col gap-1">
							<span class="flex min-w-0 flex-wrap items-center gap-x-2 gap-y-1 md:flex-nowrap">
								<!-- narrow screens: a long name wraps instead of being cut off -->
								<span
									class="font-mono text-[13px] font-medium break-all md:truncate"
									title={row.id}
								>
									{row.id}
								</span>

								{#if row.pinned}
									<Badge
										class="gap-1 font-normal"
										title="Pinned: never unloaded to make room for another model"
										variant="secondary"
									>
										<Pin />

										pinned
									</Badge>
								{/if}
							</span>

							{#if row.aliases.length > 0}
								<span class="truncate text-xs text-muted-foreground" title={row.aliases.join(', ')}>
									also: {row.aliases.join(', ')}
								</span>
							{/if}

							<!-- narrow screens: the Tags, Source and Placement columns fold under the name -->
							<span class="flex flex-wrap items-center gap-1 md:hidden">
								{@render sourceBadge(row)}

								{@render tagBadges(row)}

								{#if showSettings}
									{@render placementCell(row)}
								{/if}
							</span>
						</div>
					</Table.Cell>

					<Table.Cell class="hidden md:table-cell">
						<div class="flex max-w-48 flex-wrap gap-1">
							{@render tagBadges(row)}
						</div>
					</Table.Cell>

					<Table.Cell class="hidden md:table-cell">
						{@render sourceBadge(row)}
					</Table.Cell>

					{#if showSettings}
						<Table.Cell class="hidden max-w-48 md:table-cell">
							{@render placementCell(row)}
						</Table.Cell>
					{/if}

					<Table.Cell class={isNarrow ? 'pr-4' : ''}>
						{@render statusCell(row)}

						{#if isNarrow}
							<div class="mt-2">
								{@render actionsCell(row, 'justify-start')}
							</div>
						{/if}
					</Table.Cell>

					{#if !isNarrow}
						<Table.Cell class="pr-4">
							{@render actionsCell(row, 'justify-end')}
						</Table.Cell>
					{/if}
				</Table.Row>
			{/each}
		</Table.Body>
	</Table.Root>
</div>
