<script lang="ts">
	import { CircleAlert, Info, Loader2, ScanEye, Trash2, TriangleAlert } from '@lucide/svelte';
	import { Badge } from '$lib/components/ui/badge';
	import { Button } from '$lib/components/ui/button';
	import { Checkbox } from '$lib/components/ui/checkbox';
	import * as Dialog from '$lib/components/ui/dialog';
	import { Input } from '$lib/components/ui/input';
	import { AdminNoticeLevel } from '$lib/enums';
	import { AdminApiError, AdminService } from '$lib/services/admin.service';
	import { adminStore } from '$lib/stores/admin.svelte';
	import type { ApiAdminDeletePreview, ApiAdminDeleteResult } from '$lib/types/admin-upload';
	import { formatBytes } from '$lib/utils/admin';
	import { keepFocusOffDialogClose } from '$lib/utils/admin-dialog-focus';
	import { describeDeleteRefusal } from '$lib/utils/admin-upload';

	interface Props {
		open: boolean;
		/** The models_dir model */
		modelId: string | null;
		/** Start with "Remove projector" instead of deleting the whole model */
		projectorOnly?: boolean;
		onOpenChange?: (open: boolean) => void;
	}

	let { modelId, onOpenChange, open = $bindable(), projectorOnly = false }: Props = $props();

	const ROLE_LABELS: Record<string, string> = {
		draft: 'draft model',
		model: 'model',
		projector: 'projector',
		shard: 'part'
	};

	let mode = $state<'model' | 'projector'>('model');
	let preview = $state<ApiAdminDeletePreview | null>(null);
	let previewError = $state<string | null>(null);
	let loading = $state(false);
	let typed = $state('');
	let removeSettings = $state(true);
	let deleting = $state(false);
	let error = $state<string | null>(null);
	let note = $state<string | null>(null);
	let content = $state<HTMLElement | null>(null);
	let run = 0;
	let loadedFor = '';

	let hasProjector = $derived(
		mode === 'model' && !!preview?.files.some((file) => file.role === 'projector')
	);
	let confirmed = $derived(mode === 'projector' || typed.trim() === modelId);
	let keptHidden = $derived(preview ? Math.max(0, preview.n_kept - preview.kept.length) : 0);
	/** Every kept entry is a projector or draft set for all models (none from the model's folder) */
	let sharedOnly = $derived(
		!!preview && preview.kept.length > 0 && preview.kept.every((entry) => entry.shared)
	);

	$effect(() => {
		const key = open && modelId ? `${modelId}|${projectorOnly}` : '';

		if (!key || key === loadedFor) return;

		loadedFor = key;
		mode = projectorOnly ? 'projector' : 'model';
		typed = '';
		removeSettings = true;
		error = null;
		note = null;
		void loadPreview();
	});

	async function loadPreview() {
		if (!modelId) return;

		const mine = ++run;
		const model = modelId;

		loading = true;
		previewError = null;
		preview = null;

		try {
			const answer = (await adminStore.runWithKey((key) =>
				AdminService.deleteModelFiles(key, {
					dry_run: true,
					model,
					projector_only: mode === 'projector'
				})
			)) as ApiAdminDeletePreview;

			if (mine !== run) return;

			preview = answer;
		} catch (failure) {
			if (mine !== run) return;

			previewError =
				failure instanceof AdminApiError && failure.type === 'not_deletable'
					? describeDeleteRefusal(failure.details?.reason as string | undefined)
					: failure instanceof Error
						? failure.message
						: String(failure);
		} finally {
			if (mine === run) loading = false;
		}
	}

	function handleOpenChange(value: boolean) {
		if (!value) {
			run++;
			loadedFor = '';
			preview = null;
			previewError = null;
			deleting = false;
		}

		open = value;
		onOpenChange?.(value);
	}

	function switchMode(next: 'model' | 'projector') {
		mode = next;
		typed = '';
		error = null;
		note = null;
		void loadPreview();
	}

	async function confirmDelete() {
		if (!modelId || !preview || !confirmed || deleting) return;

		const model = modelId;
		const removing = mode === 'projector';

		deleting = true;
		error = null;

		try {
			const result = (await adminStore.runWithKey((key) =>
				AdminService.deleteModelFiles(key, {
					model,
					projector_only: removing,
					remove_settings: removing ? undefined : preview?.has_settings ? removeSettings : true,
					stop: preview?.running ?? false
				})
			)) as ApiAdminDeleteResult;
			const freed = formatBytes(result.freed_bytes);

			adminStore.notice(
				AdminNoticeLevel.SUCCESS,
				removing
					? `Removed the projector of ${model} (${freed} freed)`
					: `Deleted ${model}${result.settings_removed ? ' and its saved settings' : ''} (${freed} freed)`
			);

			if (result.reload_error) {
				adminStore.notice(
					AdminNoticeLevel.WARNING,
					`The files were removed, but reloading the model list failed: ${result.reload_error}`
				);
			}

			if (result.trash_left) {
				adminStore.notice(
					AdminNoticeLevel.INFO,
					'Some files are still in use; the server removes them later.'
				);
			}

			void adminStore.refreshModels();
			void adminStore.loadModelSettings();
			void adminStore.loadStorage();
			handleOpenChange(false);
		} catch (failure) {
			if (failure instanceof AdminApiError && failure.type === 'model_running') {
				// it was started meanwhile: show it again, now with "it will be stopped"
				note = 'The model was started meanwhile: it will be stopped first.';
				await loadPreview();
			} else if (failure instanceof AdminApiError && failure.type === 'stop_timeout') {
				error = 'The model did not stop in time; nothing was deleted. Try again.';
			} else if (failure instanceof AdminApiError && failure.type === 'not_deletable') {
				error = describeDeleteRefusal(failure.details?.reason as string | undefined);
			} else {
				error = failure instanceof Error ? failure.message : String(failure);
			}
		} finally {
			deleting = false;
		}
	}
</script>

<Dialog.Root onOpenChange={handleOpenChange} {open}>
	<Dialog.Content
		bind:ref={content}
		class="flex flex-col gap-4 outline-none max-md:h-[100dvh]! max-md:w-screen! max-md:max-w-none! max-md:rounded-none! md:max-h-[90dvh]! md:w-[calc(100vw-4rem)]! md:max-w-2xl!"
		data-testid="admin-delete-model"
		onOpenAutoFocus={(event) => {
			// keep the focus ring off the close button: the dialog itself takes the focus
			event.preventDefault();
			content?.focus();
		}}
		onfocusin={(event) => keepFocusOffDialogClose(event, content)}
		tabindex={-1}
	>
		<Dialog.Header class="text-left">
			<Dialog.Title class="flex items-center gap-2 pr-6">
				{#if mode === 'projector'}
					<ScanEye class="h-5 w-5 shrink-0" />

					Remove the projector?
				{:else}
					<Trash2 class="h-5 w-5 shrink-0" />

					Delete model files?
				{/if}
			</Dialog.Title>

			<Dialog.Description>
				<span class="font-mono text-[13px] break-all text-foreground">{modelId}</span>
				{mode === 'projector'
					? '- only its projector (mmproj) file is removed; the model and its settings stay.'
					: '- only the files this model uses are removed from the models directory.'}
			</Dialog.Description>
		</Dialog.Header>

		<div class="-mx-6 min-h-0 flex-1 overflow-y-auto px-6">
			{#if loading}
				<p class="flex items-center gap-2 py-8 text-sm text-muted-foreground">
					<Loader2 class="h-4 w-4 animate-spin" />

					Listing the files...
				</p>
			{:else if previewError}
				<p
					class="flex items-start gap-2 py-6 text-sm text-destructive"
					data-testid="delete-refused"
					role="alert"
				>
					<CircleAlert class="mt-0.5 h-4 w-4 shrink-0" />

					{previewError}
				</p>
			{:else if preview}
				<div class="flex flex-col gap-4 text-sm">
					<div>
						<p
							class="mb-1 text-xs font-medium text-muted-foreground"
							data-testid="delete-files-heading"
						>
							{mode === 'projector' ? 'Will be removed' : 'Will be deleted'} ({preview.n_files} file{preview.n_files ===
							1
								? ''
								: 's'}, {formatBytes(preview.total_bytes)})
						</p>

						<ul class="flex flex-col divide-y rounded-md border" data-testid="delete-files">
							{#each preview.files as file (file.path)}
								<li class="flex items-center gap-2 px-3 py-1.5">
									<span class="min-w-0 flex-1 truncate font-mono text-xs" title={file.path}
										>{file.path}</span
									>

									<Badge class="font-normal" variant="outline"
										>{ROLE_LABELS[file.role] ?? file.role}</Badge
									>

									<span class="text-xs text-muted-foreground tabular-nums"
										>{formatBytes(file.bytes)}</span
									>
								</li>
							{/each}
						</ul>
					</div>

					{#if preview.n_kept > 0}
						<div data-testid="delete-kept">
							<p class="mb-1 text-xs font-medium text-muted-foreground">
								Kept: {sharedOnly ? 'used by every model' : 'not part of this model'} ({preview.n_kept}
								entr{preview.n_kept === 1 ? 'y' : 'ies'}{preview.kept_bytes
									? `, ${formatBytes(preview.kept_bytes)}`
									: ''})
							</p>

							<ul
								class="flex max-h-40 flex-col divide-y overflow-y-auto rounded-md border border-dashed"
							>
								{#each preview.kept as entry (entry.path)}
									<li
										class="flex items-center gap-2 px-3 py-1.5 text-muted-foreground"
										data-shared={entry.shared ? 'true' : undefined}
									>
										<span class="min-w-0 flex-1 truncate font-mono text-xs" title={entry.path}
											>{entry.path}</span
										>

										{#if entry.shared}
											<Badge
												class="font-normal"
												title="Set for every model (router command line or [*] of models.ini): not deleted from here"
												variant="outline">used by every model</Badge
											>
										{/if}

										<span class="text-xs"
											>{entry.kind === 'file' ? formatBytes(entry.bytes ?? 0) : entry.kind}</span
										>
									</li>
								{/each}
							</ul>

							{#if keptHidden > 0}
								<p class="mt-1 text-xs text-muted-foreground">... and {keptHidden} more.</p>
							{/if}
						</div>
					{/if}

					{#if preview.folder && !preview.folder_removed && preview.kept.some((entry) => !entry.shared)}
						<p class="text-xs text-muted-foreground">
							The folder <span class="font-mono">{preview.folder}</span> stays because it holds other
							files.
						</p>
					{:else if preview.folder && preview.folder_removed}
						<p class="text-xs text-muted-foreground">
							The folder <span class="font-mono">{preview.folder}</span> is removed too (nothing else
							is in it).
						</p>
					{/if}

					{#if preview.warnings.includes('folder_keeps_other_model')}
						<p class="flex items-start gap-2 text-amber-700 dark:text-amber-400">
							<TriangleAlert class="mt-0.5 h-4 w-4 shrink-0" />

							The folder holds files of another model; they stay.
						</p>
					{/if}

					{#if preview.running}
						<p
							class="flex items-start gap-2 text-amber-700 dark:text-amber-400"
							data-testid="delete-running"
						>
							<TriangleAlert class="mt-0.5 h-4 w-4 shrink-0" />

							The model is {preview.status === 'loading' ? 'loading' : 'running'}: it is stopped
							first, its requests fail.
						</p>
					{/if}

					{#if note}
						<p class="text-amber-700 dark:text-amber-400">{note}</p>
					{/if}

					{#if mode === 'model' && preview.pinned}
						<p class="flex items-start gap-2 text-muted-foreground">
							<Info class="mt-0.5 h-4 w-4 shrink-0" />

							It is pinned; the pin is removed.
						</p>
					{/if}

					{#if mode === 'model' && preview.has_settings}
						<label class="flex items-start gap-2" data-testid="delete-settings">
							<Checkbox
								checked={removeSettings}
								class="mt-0.5"
								onCheckedChange={(value) => (removeSettings = value === true)}
							/>

							<span>
								Also remove its saved settings
								<span class="block text-xs text-muted-foreground">
									Otherwise a later model with the same name gets its GPU and context settings back.
								</span>
							</span>
						</label>
					{/if}

					{#if hasProjector}
						<div class="flex flex-wrap items-center gap-2 rounded-md border border-dashed p-3">
							<span class="min-w-0 flex-1 text-xs text-muted-foreground">
								A projector that does not match the model makes every start fail. To keep the model,
								remove only the projector.
							</span>

							<Button
								data-testid="delete-projector-only"
								onclick={() => switchMode('projector')}
								size="sm"
								variant="outline"
							>
								<ScanEye />

								Remove only the projector
							</Button>
						</div>
					{/if}

					{#if mode === 'model'}
						<div class="flex flex-col gap-1.5">
							<label class="text-sm" for="admin-delete-confirm">
								Type <span class="font-mono font-medium">{modelId}</span> to confirm
							</label>

							<Input
								bind:value={typed}
								autocomplete="off"
								class="font-mono sm:max-w-md"
								data-testid="delete-confirm-input"
								id="admin-delete-confirm"
								spellcheck={false}
							/>
						</div>
					{/if}
				</div>
			{/if}

			{#if error}
				<p
					class="mt-3 flex items-start gap-2 text-sm text-destructive"
					data-testid="delete-error"
					role="alert"
				>
					<CircleAlert class="mt-0.5 h-4 w-4 shrink-0" />

					{error}
				</p>
			{/if}
		</div>

		<Dialog.Footer class="gap-2">
			{#if mode === 'projector' && !projectorOnly}
				<Button class="sm:mr-auto" onclick={() => switchMode('model')} size="sm" variant="ghost">
					Delete the whole model instead
				</Button>
			{/if}

			<Button onclick={() => handleOpenChange(false)} size="sm" variant="secondary">Cancel</Button>

			<Button
				data-testid="delete-go"
				disabled={!preview || !confirmed || deleting}
				onclick={confirmDelete}
				size="sm"
				variant="destructive"
			>
				{#if deleting}
					<Loader2 class="animate-spin" />
				{:else}
					<Trash2 />
				{/if}

				{mode === 'projector'
					? 'Remove the projector'
					: preview?.running
						? 'Stop and delete'
						: 'Delete'}
			</Button>
		</Dialog.Footer>
	</Dialog.Content>
</Dialog.Root>
