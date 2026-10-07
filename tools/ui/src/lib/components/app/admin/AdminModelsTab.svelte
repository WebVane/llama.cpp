<script lang="ts">
	import AdminGpuPanel from './AdminGpuPanel.svelte';
	import AdminModelsTable from './AdminModelsTable.svelte';
	import AdminUploadsBanner from './AdminUploadsBanner.svelte';
	import {
		ChevronDown,
		CircleAlert,
		CircleCheck,
		HardDrive,
		Info,
		Loader2,
		Plus,
		RefreshCw,
		Trash2,
		TriangleAlert,
		Upload,
		X
	} from '@lucide/svelte';
	import {
		DialogAdminAddModel,
		DialogAdminModelLog,
		DialogAdminModelSettings,
		DialogConfirmation
	} from '$lib/components/app/dialogs';
	import DialogAdminDeleteModel from '$lib/components/app/dialogs/DialogAdminDeleteModel.svelte';
	import { SearchInput } from '$lib/components/app/forms';
	import { Button } from '$lib/components/ui/button';
	import { Checkbox } from '$lib/components/ui/checkbox';
	import * as Collapsible from '$lib/components/ui/collapsible';
	import { ADMIN_PUBLIC_API_NOTE, ICON_CLASS_DEFAULT } from '$lib/constants';
	import {
		AdminFeature,
		AdminNoticeLevel,
		AdminStorageDirKind,
		ServerModelStatus
	} from '$lib/enums';
	import { AdminAddModelTab } from '$lib/enums/admin.enums';
	import { adminStore, modelsStore } from '$lib/stores';
	import { adminUploadStore } from '$lib/stores/admin-upload.svelte';
	import type { AdminModelRow, ApiAdminUnattachedEntry } from '$lib/types';
	import { buildAdminModelRows, describeModelWarning, formatBytes } from '$lib/utils';
	import { type Component, onMount, untrack } from 'svelte';

	/** Longest part of an unreadable entry shown as it is in the file. */
	const RAW_ENTRY_MAX = 200;

	let filter = $state('');
	let showAddModel = $state(false);
	let addModelTab = $state<AdminAddModelTab>(AdminAddModelTab.HUGGING_FACE);
	// files dropped on this tab, handed to the Upload tab of the dialog
	let droppedFiles = $state<File[] | null>(null);
	let dragOver = $state(false);
	// "Delete files..." of a models_dir model (stage 3)
	let deleteFilesTarget = $state<string | null>(null);
	let deleteFilesOpen = $state(false);
	let deleteTarget = $state<AdminModelRow | null>(null);
	// "Also remove its saved settings" in the Delete confirmation (checked by default)
	let deleteSettings = $state(true);
	let settingsOpen = $state(false);
	let settingsTarget = $state<string | null>(null);
	let logOpen = $state(false);
	let logTarget = $state<string | null>(null);
	let unattachedOpen = $state(false);
	// a hidden entry still applies: removing it is confirmed first
	let removeTarget = $state<ApiAdminUnattachedEntry | null>(null);

	let showGpus = $derived(adminStore.hasFeature(AdminFeature.GPUS));
	let showUpload = $derived(adminStore.hasFeature(AdminFeature.UPLOAD));
	let showDeleteFiles = $derived(adminStore.hasFeature(AdminFeature.DELETE));
	let modelsDir = $derived(
		adminStore.storage?.dirs.find((dir) => dir.kind === AdminStorageDirKind.MODELS_DIR) ?? null
	);
	let anyDialogOpen = $derived(
		showAddModel ||
			settingsOpen ||
			logOpen ||
			deleteFilesOpen ||
			deleteTarget !== null ||
			removeTarget !== null
	);
	let showModelSettings = $derived(adminStore.hasFeature(AdminFeature.MODEL_SETTINGS));
	let unattached = $derived(showModelSettings ? (adminStore.modelSettings?.unattached ?? []) : []);
	let deleteHasSettings = $derived.by(() => {
		if (!deleteTarget || !showModelSettings) return false;

		const settings = adminStore.modelSettingsRow(deleteTarget.id);

		return !!settings && (Object.keys(settings.overrides).length > 0 || settings.phase !== null);
	});
	let fileState = $derived(adminStore.modelSettings?.state ?? null);
	// list warnings of the server, e.g. a MODEL_DEVICES entry that names no models.ini section
	let listWarnings = $derived(showModelSettings ? (adminStore.modelSettings?.warnings ?? []) : []);

	let rows = $derived(
		buildAdminModelRows(modelsStore.routerModels, {
			downloads: adminStore.downloads,
			exitCodes: adminStore.exitCodes,
			pinned: adminStore.pinnedIds
		})
	);
	let visibleRows = $derived.by(() => {
		const term = filter.trim().toLowerCase();

		if (!term) return rows;

		return rows.filter(
			(row) =>
				row.id.toLowerCase().includes(term) ||
				row.aliases.some((alias) => alias.toLowerCase().includes(term)) ||
				row.tags.some((tag) => tag.toLowerCase().includes(term))
		);
	});
	let cache = $derived(adminStore.cacheDir);
	let partialDownloads = $derived(adminStore.storage?.partial_downloads ?? []);
	let partialBytes = $derived(partialDownloads.reduce((sum, item) => sum + (item.bytes || 0), 0));
	let deleteDescription = $derived.by(() => {
		if (!deleteTarget) return '';

		const running =
			deleteTarget.status === ServerModelStatus.LOADED ||
			deleteTarget.status === ServerModelStatus.SLEEPING ||
			deleteTarget.status === ServerModelStatus.LOADING;

		return (
			`"${deleteTarget.id}" will be removed from the model cache on the server's disk. ` +
			(running ? 'It is in use and will be stopped first. ' : '') +
			'This cannot be undone.'
		);
	});

	const NOTICE_STYLES: Record<AdminNoticeLevel, { class: string; icon: Component }> = {
		[AdminNoticeLevel.ERROR]: {
			class: 'border-destructive/40 bg-destructive/10 text-destructive',
			icon: CircleAlert
		},
		[AdminNoticeLevel.INFO]: { class: 'border-border bg-muted/40', icon: Info },
		[AdminNoticeLevel.SUCCESS]: {
			class: 'border-green-500/40 bg-green-500/10 text-green-700 dark:text-green-400',
			icon: CircleCheck
		},
		[AdminNoticeLevel.WARNING]: {
			class: 'border-amber-500/40 bg-amber-500/10 text-amber-700 dark:text-amber-400',
			icon: TriangleAlert
		}
	};

	// the header indicator, the uploads banner or a drop asks for the dialog
	$effect(() => {
		const request = adminUploadStore.dialogRequest;

		if (!request) return;

		untrack(() => {
			adminUploadStore.dialogRequest = null;
			addModelTab = request.tab;

			if (request.files) droppedFiles = request.files;

			showAddModel = true;
		});
	});

	// a download (or an upload) finished: open the model's settings for its placement
	$effect(() => {
		const id = adminStore.settingsRequest;

		if (!id || anyDialogOpen || !showModelSettings) return;

		untrack(() => {
			const model = adminStore.takeSettingsRequest();

			if (model) openSettings(model);
		});
	});

	onMount(() => {
		adminUploadStore.enter();
	});

	function onDragOver(event: DragEvent) {
		if (!showUpload || !event.dataTransfer?.types.includes('Files')) return;

		event.preventDefault();
		dragOver = true;
	}

	function onDrop(event: DragEvent) {
		if (!showUpload) return;

		event.preventDefault();
		dragOver = false;

		const files = [...(event.dataTransfer?.files ?? [])];

		if (files.length > 0) adminUploadStore.openDialog(AdminAddModelTab.UPLOAD, files);
	}

	function openDeleteFiles(row: AdminModelRow) {
		deleteFilesTarget = row.id;
		deleteFilesOpen = true;
	}

	/** An upload card asked for the settings of the model it published. */
	function openSettingsFromUpload(model: string) {
		showAddModel = false;
		openSettings(model);
	}

	async function confirmDelete() {
		const target = deleteTarget;
		const removeSettings = deleteHasSettings && deleteSettings;

		deleteTarget = null;

		if (target) await adminStore.deleteModel(target.id, removeSettings);
	}

	function openDelete(row: AdminModelRow) {
		deleteSettings = true;
		deleteTarget = row;
	}

	function openSettings(id: string) {
		settingsTarget = id;
		settingsOpen = true;
	}

	function openLog(id: string) {
		logTarget = id;
		logOpen = true;
	}

	function removeUnattached(entry: ApiAdminUnattachedEntry) {
		if (entry.kind === 'hidden') {
			removeTarget = entry;

			return;
		}

		void adminStore.removeModelSettings(entry.id);
	}

	async function confirmRemoveUnattached() {
		const target = removeTarget;

		removeTarget = null;

		if (target) await adminStore.removeModelSettings(target.id);
	}

	function rawEntryText(raw: unknown): string {
		let text: string;

		try {
			text = JSON.stringify(raw) ?? String(raw);
		} catch {
			text = String(raw);
		}

		return text.length > RAW_ENTRY_MAX ? `${text.slice(0, RAW_ENTRY_MAX)}...` : text;
	}
</script>

<div
	aria-label="Models"
	class={[
		'relative flex flex-col gap-3',
		dragOver && 'rounded-lg ring-2 ring-primary ring-offset-4'
	]}
	data-testid="admin-models-tab"
	ondragleave={(event) => {
		if (event.currentTarget === event.target) dragOver = false;
	}}
	ondragover={onDragOver}
	ondrop={onDrop}
	role="region"
>
	{#if dragOver}
		<div
			class="pointer-events-none absolute inset-0 z-10 flex items-center justify-center rounded-lg bg-background/80 text-sm font-medium"
			data-testid="admin-drop-overlay"
		>
			<span class="flex items-center gap-2"
				><Upload class="h-5 w-5" /> Drop GGUF files to upload them</span
			>
		</div>
	{/if}

	<div class="flex flex-wrap items-center gap-2">
		<SearchInput
			bind:value={filter}
			class="min-w-40 flex-1 sm:max-w-xs"
			placeholder="Filter models..."
		/>

		<div class="ml-auto flex items-center gap-2">
			<Button
				disabled={adminStore.rescanning}
				onclick={() => adminStore.rescan()}
				size="sm"
				title="Re-scan the model cache, the models directory and the preset file"
				variant="outline"
			>
				<RefreshCw class={adminStore.rescanning ? 'animate-spin' : ''} />

				Rescan
			</Button>

			<Button data-testid="admin-add-model-button" onclick={() => (showAddModel = true)} size="sm">
				<Plus />

				Add model
			</Button>
		</div>
	</div>

	{#if showGpus}
		<AdminGpuPanel onOpenModel={openSettings} />
	{/if}

	{#if fileState && (fileState.read_only || fileState.backup_loaded || fileState.error)}
		<div
			class="flex items-start gap-2 rounded-md border border-amber-500/40 bg-amber-500/10 px-3 py-2 text-sm text-amber-700 dark:text-amber-400"
			data-testid="admin-model-settings-file"
			role="status"
		>
			<TriangleAlert class="mt-0.5 h-4 w-4 shrink-0" />

			<span class="min-w-0 break-words">
				{#if fileState.read_only}
					Per-model settings are read-only: the settings file of the server could not be read ({fileState.error ||
						'unknown version'}). Nothing can be saved until it is fixed or removed:
					<span class="font-mono">{fileState.file}</span>
				{:else if fileState.backup_loaded}
					The per-model settings file could not be read, so its backup copy is used ({fileState.error}).
				{:else}
					Some saved per-model settings were skipped: {fileState.error}
				{/if}
			</span>
		</div>
	{/if}

	{#each listWarnings as code (code)}
		<div
			class="flex items-start gap-2 rounded-md border border-amber-500/40 bg-amber-500/10 px-3 py-2 text-sm text-amber-700 dark:text-amber-400"
			data-list-warning={code}
			role="status"
		>
			<TriangleAlert class="mt-0.5 h-4 w-4 shrink-0" />

			<span class="min-w-0 break-words">{describeModelWarning(code)}</span>
		</div>
	{/each}

	<div
		class="flex flex-wrap items-center gap-x-2 gap-y-1 text-xs text-muted-foreground"
		data-testid="admin-storage"
	>
		<HardDrive class="h-3.5 w-3.5 shrink-0" />

		{#if cache}
			<span>
				Model cache:
				<span class="font-medium text-foreground tabular-nums">
					{formatBytes(cache.free_bytes)} free
				</span>
				of {formatBytes(cache.total_bytes)}
			</span>

			<span class="max-w-full truncate font-mono" title={cache.path}>{cache.path}</span>

			{#if !cache.exists}
				<span class="text-amber-600 dark:text-amber-400">(not created yet)</span>
			{:else if !cache.writable}
				<span class="text-destructive">(not writable: downloads will fail)</span>
			{/if}

			{#if partialDownloads.length > 0}
				<span>
					· {partialDownloads.length} unfinished download file{partialDownloads.length === 1
						? ''
						: 's'} ({formatBytes(partialBytes)})
				</span>
			{/if}
		{:else if adminStore.storageError}
			<span class="text-destructive"
				>Storage information unavailable: {adminStore.storageError}</span
			>
		{:else if adminStore.storage}
			<span>The server did not report a model cache directory.</span>
		{:else}
			<span>Reading storage information...</span>
		{/if}
	</div>

	{#if modelsDir && showUpload}
		<div
			class="-mt-2 flex flex-wrap items-center gap-x-2 gap-y-1 text-xs text-muted-foreground"
			data-testid="admin-storage-models-dir"
		>
			<HardDrive class="h-3.5 w-3.5 shrink-0" />

			<span>
				Models directory (uploads):
				<span class="font-medium text-foreground tabular-nums">
					{formatBytes(modelsDir.free_bytes)} free
				</span>
				of {formatBytes(modelsDir.total_bytes)}
			</span>

			<span class="max-w-full truncate font-mono" title={modelsDir.path}>{modelsDir.path}</span>

			{#if !modelsDir.writable}
				<span class="text-destructive">(not writable: uploads are not possible)</span>
			{/if}
		</div>
	{/if}

	{#if showUpload}
		<AdminUploadsBanner />
	{/if}

	{#each adminStore.notices as notice (notice.id)}
		{@const style = NOTICE_STYLES[notice.level]}
		{@const NoticeIcon = style.icon}

		<div
			class={['flex items-start gap-2 rounded-md border px-3 py-2 text-sm', style.class]}
			data-notice={notice.level}
			role="status"
		>
			<NoticeIcon class="mt-0.5 h-4 w-4 shrink-0" />

			<span class="min-w-0 flex-1 break-words">{notice.text}</span>

			<button
				aria-label="Dismiss"
				class="shrink-0 rounded p-0.5 opacity-70 hover:opacity-100"
				onclick={() => adminStore.dismissNotice(notice.id)}
				type="button"
			>
				<X class="h-3.5 w-3.5" />
			</button>
		</div>
	{/each}

	{#if rows.length === 0}
		<div class="rounded-lg border border-dashed p-8 text-center text-sm text-muted-foreground">
			{#if modelsStore.loading}
				<span class="inline-flex items-center gap-2">
					<Loader2 class="{ICON_CLASS_DEFAULT} animate-spin" />

					Loading models...
				</span>
			{:else}
				No models yet. Add one from Hugging Face, or put a GGUF file into the models directory and
				press Rescan.
			{/if}
		</div>
	{:else if visibleRows.length === 0}
		<div class="rounded-lg border border-dashed p-8 text-center text-sm text-muted-foreground">
			No model matches "{filter.trim()}".
		</div>
	{:else}
		<AdminModelsTable
			onDelete={openDelete}
			onDeleteFiles={showDeleteFiles ? openDeleteFiles : undefined}
			onSettings={showModelSettings ? (row) => openSettings(row.id) : undefined}
			onShowLog={(row) => openLog(row.id)}
			rows={visibleRows}
		/>
	{/if}

	{#if unattached.length > 0}
		<Collapsible.Root bind:open={unattachedOpen}>
			<div
				class="rounded-md border border-dashed px-3 py-2 text-sm text-muted-foreground"
				data-testid="admin-unattached"
			>
				<Collapsible.Trigger class="flex w-full items-center gap-2 text-left hover:text-foreground">
					<ChevronDown
						class={['h-4 w-4 shrink-0 transition-transform', unattachedOpen && 'rotate-180']}
					/>

					Saved settings for {unattached.length} model{unattached.length === 1 ? '' : 's'} that
					{unattached.length === 1 ? 'is' : 'are'} not in the list
				</Collapsible.Trigger>

				<Collapsible.Content>
					<ul class="mt-2 divide-y">
						{#each unattached as entry (entry.id)}
							<li class="flex flex-col gap-1 py-1.5" data-unattached-kind={entry.kind}>
								<div class="flex flex-wrap items-center gap-2">
									<span class="min-w-0 flex-1 font-mono text-xs break-all text-foreground">
										{entry.id}
									</span>

									{#if entry.kind !== 'unreadable'}
										<span class="text-xs">
											{Object.keys(entry.overrides ?? {})
												.map((key) => `${key} = ${entry.overrides[key]}`)
												.join(', ') || 'no values'}
										</span>

										<Button onclick={() => removeUnattached(entry)} size="sm" variant="ghost">
											<Trash2 />

											Remove
										</Button>
									{/if}
								</div>

								{#if entry.kind === 'hidden'}
									<p class="text-xs">
										Still in use: the model is a downloaded copy that the list hides because a
										models.ini section uses the same file. The settings apply when a request names
										this id.
									</p>
								{:else if entry.kind === 'unreadable'}
									<p class="text-xs">
										This entry of the settings file could not be read, so it is not applied; the
										server keeps it unchanged. Fix or delete it in
										<span class="font-mono break-all">{fileState?.file ?? 'admin-models.json'}</span
										>:
									</p>

									<code class="font-mono text-xs break-all text-foreground/80">
										{rawEntryText(entry.raw)}
									</code>
								{:else}
									<p class="text-xs">
										The model was deleted or renamed. The settings come back if it appears again
										(for example after a new download); Remove deletes them.
									</p>
								{/if}
							</li>
						{/each}
					</ul>
				</Collapsible.Content>
			</div>
		</Collapsible.Root>
	{/if}

	<p class="flex items-start gap-2 text-xs text-muted-foreground">
		<Info class="mt-0.5 h-3.5 w-3.5 shrink-0" />

		<span>{ADMIN_PUBLIC_API_NOTE}</span>
	</p>
</div>

<DialogAdminAddModel
	bind:files={droppedFiles}
	bind:open={showAddModel}
	bind:tab={addModelTab}
	onOpenSettings={showModelSettings ? openSettingsFromUpload : undefined}
/>

{#if showDeleteFiles}
	<DialogAdminDeleteModel bind:open={deleteFilesOpen} modelId={deleteFilesTarget} />
{/if}

<DialogConfirmation
	confirmText="Delete"
	description={deleteDescription}
	icon={Trash2}
	onCancel={() => (deleteTarget = null)}
	onConfirm={confirmDelete}
	open={deleteTarget !== null}
	title="Delete model?"
	variant="destructive"
>
	{#if deleteHasSettings}
		<label class="flex items-start gap-2 text-sm" data-testid="admin-delete-settings">
			<Checkbox
				checked={deleteSettings}
				class="mt-0.5"
				onCheckedChange={(value) => (deleteSettings = value === true)}
			/>

			<span>
				Also remove its saved settings
				<span class="block text-xs text-muted-foreground">
					Otherwise a later download of the same model gets its GPU and context settings back.
				</span>
			</span>
		</label>
	{/if}
</DialogConfirmation>

<DialogConfirmation
	confirmText="Remove"
	description={removeTarget
		? `The settings of "${removeTarget.id}" are still applied when a request names that model. Remove them anyway?`
		: ''}
	icon={Trash2}
	onCancel={() => (removeTarget = null)}
	onConfirm={confirmRemoveUnattached}
	open={removeTarget !== null}
	title="Remove saved settings?"
	variant="destructive"
/>

{#if showModelSettings}
	<DialogAdminModelSettings bind:open={settingsOpen} modelId={settingsTarget} />
{/if}

{#if adminStore.hasFeature(AdminFeature.MODEL_LOG)}
	<DialogAdminModelLog bind:open={logOpen} modelId={logTarget} onOpenSettings={openSettings} />
{/if}
