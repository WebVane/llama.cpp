<script lang="ts">
	import {
		CircleCheck,
		Download,
		Info,
		Loader2,
		Plus,
		Search,
		TriangleAlert,
		Upload
	} from '@lucide/svelte';
	import AdminUploadTab from '$lib/components/app/admin/AdminUploadTab.svelte';
	import { Button } from '$lib/components/ui/button';
	import * as Dialog from '$lib/components/ui/dialog';
	import { Input } from '$lib/components/ui/input';
	import { Label } from '$lib/components/ui/label';
	import { ICON_CLASS_DEFAULT } from '$lib/constants';
	import { AdminNoticeLevel, KeyboardKey } from '$lib/enums';
	import { AdminAddModelTab } from '$lib/enums/admin.enums';
	import { adminStore } from '$lib/stores';
	import { adminUploadStore } from '$lib/stores/admin-upload.svelte';
	import type { ApiAdminDownloadCheck } from '$lib/types';
	import {
		describeDownloadWarnings,
		formatBytes,
		isValidHfModelSpec,
		normalizeHfModelSpec
	} from '$lib/utils';
	import { toast } from 'svelte-sonner';

	interface Props {
		open: boolean;
		/** Tab shown (Hugging Face download or GGUF upload) */
		tab?: AdminAddModelTab;
		/** Files dropped on the Models tab: the Upload tab groups them at once */
		files?: File[] | null;
		onOpenChange?: (open: boolean) => void;
		/** Open the settings dialog of a model published by an upload */
		onOpenSettings?: (model: string) => void;
	}

	let {
		files = $bindable(null),
		onOpenChange,
		onOpenSettings,
		open = $bindable(),
		tab = $bindable(AdminAddModelTab.HUGGING_FACE)
	}: Props = $props();

	const TABS = [
		{ icon: Download, id: AdminAddModelTab.HUGGING_FACE, label: 'Hugging Face' },
		{ icon: Upload, id: AdminAddModelTab.UPLOAD, label: 'Upload GGUF' }
	];
	/** One panel for both tabs: it shows the content of the selected one */
	const TAB_PANEL_ID = 'admin-add-model-panel';

	function tabId(id: AdminAddModelTab): string {
		return `admin-add-model-tab-${id}`;
	}

	let input = $state('');
	let check = $state<ApiAdminDownloadCheck | null>(null);
	let checkedSpec = $state<string | null>(null);
	let checkError = $state<string | null>(null);
	let checking = $state(false);
	let downloadError = $state<string | null>(null);
	let starting = $state(false);

	let checkRun = 0;
	// bumped whenever the dialog is closed: a request started before belongs to a closed dialog
	let openRun = 0;
	let repoInput = $state<HTMLInputElement | null>(null);
	let content = $state<HTMLElement | null>(null);
	// the upload tab exists only when the server has the upload routes
	let uploadEnabled = $derived(adminUploadStore.enabled);
	let shownTab = $derived(uploadEnabled ? tab : AdminAddModelTab.HUGGING_FACE);

	// what is sent to the server: trimmed, URL prefix dropped, quantization upper-cased
	let spec = $derived(normalizeHfModelSpec(input));
	let isValid = $derived(isValidHfModelSpec(spec));
	let showInvalid = $derived(input.trim().length > 0 && !isValid);
	// a result belongs to the spec it was made for; editing the input hides it
	let result = $derived(check && checkedSpec === spec ? check : null);
	let warnings = $derived(result ? describeDownloadWarnings(result) : []);
	let downloadsUnavailable = $derived(adminStore.status?.download?.available === false);

	function reset() {
		checkRun++;
		openRun++;
		input = '';
		check = null;
		checkedSpec = null;
		checkError = null;
		checking = false;
		downloadError = null;
		starting = false;
	}

	function handleOpenChange(value: boolean) {
		if (!value) reset();

		open = value;
		onOpenChange?.(value);
	}

	async function handleCheck() {
		if (!isValid || checking) return;

		const run = ++checkRun;
		const requested = spec;

		checking = true;
		checkError = null;
		downloadError = null;

		try {
			const answer = await adminStore.checkDownload(requested);

			if (run !== checkRun) return;

			check = answer;
			checkedSpec = requested;
		} catch (error) {
			if (run !== checkRun) return;

			check = null;
			checkedSpec = null;
			checkError = error instanceof Error ? error.message : String(error);
		} finally {
			if (run === checkRun) checking = false;
		}
	}

	// Warnings never block this: the pre-flight check is advisory, and it is
	// not required before a download either. The dialog may be closed while
	// the server validates the repository; a refusal is then reported by the
	// store instead of being left on the closed dialog.
	async function handleDownload() {
		if (!isValid || starting) return;

		const run = openRun;
		const requested = spec;

		starting = true;
		downloadError = null;

		try {
			await adminStore.startDownload(requested);
			toast.success(`Download started: ${requested}`);

			if (run === openRun) handleOpenChange(false);
		} catch (error) {
			const message = error instanceof Error ? error.message : String(error);

			if (run === openRun) {
				downloadError = message;
			} else {
				adminStore.reportDownloadNotStarted(requested, message);
			}
		} finally {
			if (run === openRun) starting = false;
		}
	}

	function handleKeydown(event: KeyboardEvent) {
		if (event.key === KeyboardKey.ENTER) {
			event.preventDefault();
			handleCheck();
		}
	}

	function selectTab(next: AdminAddModelTab) {
		tab = next;

		if (next === AdminAddModelTab.UPLOAD) void adminUploadStore.loadInfo();
	}

	/** Arrow keys move between the two tabs (WAI-ARIA tabs pattern). */
	function handleTabKeydown(event: KeyboardEvent) {
		if (event.key !== KeyboardKey.ARROW_LEFT && event.key !== KeyboardKey.ARROW_RIGHT) return;

		event.preventDefault();
		selectTab(
			shownTab === AdminAddModelTab.UPLOAD ? AdminAddModelTab.HUGGING_FACE : AdminAddModelTab.UPLOAD
		);
	}

	/**
	 * The first focusable element would be the close button of the header, which
	 * then shows its focus ring on open: focus the repository field instead (the
	 * dialog itself on the Upload tab).
	 */
	function handleOpenAutoFocus(event: Event) {
		event.preventDefault();

		if (shownTab === AdminAddModelTab.HUGGING_FACE && repoInput) repoInput.focus();
		else content?.focus();
	}

	function isFileDrag(event: DragEvent): boolean {
		return !!event.dataTransfer?.types.includes('Files');
	}

	/**
	 * Files dragged anywhere over the dialog: the Upload tab takes them (the
	 * browser would otherwise open the file and the dialog would be lost).
	 * Without the upload routes nothing is accepted.
	 */
	function handleDragOver(event: DragEvent) {
		// the dropzone of the Upload tab handled it already
		if (event.defaultPrevented || !isFileDrag(event)) return;

		event.preventDefault();

		if (event.dataTransfer) event.dataTransfer.dropEffect = uploadEnabled ? 'copy' : 'none';
	}

	function handleDrop(event: DragEvent) {
		if (event.defaultPrevented || !isFileDrag(event)) return;

		event.preventDefault();

		const dropped = [...(event.dataTransfer?.files ?? [])];

		if (!uploadEnabled || dropped.length === 0) return;

		// the Upload tab groups them: added to the files under review, or a new review
		tab = AdminAddModelTab.UPLOAD;
		void adminUploadStore.loadInfo();
		files = dropped;
	}
</script>

<Dialog.Root onOpenChange={handleOpenChange} {open}>
	<!-- explicit width: the dialog is centred with left 50%, so "auto" would shrink it to half a phone screen -->
	<Dialog.Content
		bind:ref={content}
		class={[
			'flex flex-col outline-none',
			shownTab === AdminAddModelTab.UPLOAD
				? 'max-md:h-[100dvh]! max-md:w-screen! max-md:max-w-none! max-md:rounded-none! md:max-h-[90dvh]! md:w-[calc(100vw-4rem)]! md:max-w-3xl!'
				: 'w-[calc(100%-2rem)]! max-w-xl!'
		]}
		data-testid="admin-add-model"
		ondragover={handleDragOver}
		ondrop={handleDrop}
		onOpenAutoFocus={handleOpenAutoFocus}
		tabindex={-1}
	>
		<Dialog.Header class="text-left">
			<Dialog.Title class="flex items-center gap-2 pr-6">
				{#if uploadEnabled}
					<Plus class="h-5 w-5 shrink-0" />

					Add model
				{:else}
					<Download class="h-5 w-5 shrink-0" />

					Add model from Hugging Face
				{/if}
			</Dialog.Title>

			<Dialog.Description>
				{shownTab === AdminAddModelTab.UPLOAD
					? 'Uploads GGUF files from this computer into the models directory of the server. The model appears in the list once the server has checked that it can load it.'
					: 'Downloads a GGUF model into the model cache of the server. Check first to see the files, their size and the free space.'}
			</Dialog.Description>
		</Dialog.Header>

		{#if uploadEnabled}
			<div
				aria-label="Where the model comes from"
				class="inline-flex w-fit rounded-lg border bg-muted/40 p-0.5"
				role="tablist"
			>
				{#each TABS as item (item.id)}
					{@const TabIcon = item.icon}

					<button
						aria-controls={TAB_PANEL_ID}
						aria-selected={shownTab === item.id}
						class={[
							'flex items-center gap-1.5 rounded-md px-3 py-1.5 text-sm font-medium transition-colors',
							shownTab === item.id
								? 'bg-background text-foreground shadow-sm'
								: 'text-muted-foreground hover:text-foreground'
						]}
						data-tab={item.id}
						id={tabId(item.id)}
						onclick={() => selectTab(item.id)}
						onkeydown={handleTabKeydown}
						role="tab"
						tabindex={shownTab === item.id ? 0 : -1}
						type="button"
					>
						<TabIcon class="h-4 w-4" />

						{item.label}
					</button>
				{/each}
			</div>
		{/if}

		{#if shownTab === AdminAddModelTab.UPLOAD}
			<div
				aria-labelledby={tabId(AdminAddModelTab.UPLOAD)}
				class="-mx-6 min-h-0 flex-1 overflow-y-auto px-6 pb-1"
				id={TAB_PANEL_ID}
				role="tabpanel"
			>
				<AdminUploadTab {files} onFilesTaken={() => (files = null)} {onOpenSettings} />
			</div>
		{:else}
			<div
				aria-labelledby={uploadEnabled ? tabId(AdminAddModelTab.HUGGING_FACE) : undefined}
				class="space-y-4"
				id={uploadEnabled ? TAB_PANEL_ID : undefined}
				role={uploadEnabled ? 'tabpanel' : undefined}
			>
				<div class="space-y-2">
					<Label for="admin-add-model">Repository</Label>

					<div class="flex gap-2">
						<Input
							bind:ref={repoInput}
							bind:value={input}
							aria-describedby="admin-add-model-help"
							aria-invalid={showInvalid ? 'true' : undefined}
							autocomplete="off"
							class="font-mono"
							disabled={starting}
							id="admin-add-model"
							onkeydown={handleKeydown}
							placeholder="user/repo:QUANT"
							spellcheck={false}
						/>

						<Button
							disabled={!isValid || checking || starting}
							onclick={handleCheck}
							variant="secondary"
						>
							{#if checking}
								<Loader2 class="{ICON_CLASS_DEFAULT} animate-spin" />
							{:else}
								<Search class={ICON_CLASS_DEFAULT} />
							{/if}

							Check
						</Button>
					</div>

					{#if showInvalid}
						<p class="text-xs text-destructive" role="alert">
							Enter the repository as user/repo, optionally followed by :QUANT.
						</p>
					{:else if isValid && spec !== input.trim()}
						<p class="text-xs text-muted-foreground">
							Will be requested as <span class="font-mono text-foreground">{spec}</span>
						</p>
					{/if}

					<p class="text-xs text-muted-foreground" id="admin-add-model-help">
						Format: user/repo or user/repo:QUANT, for example
						<span class="font-mono">Qwen/Qwen3-4B-GGUF:Q4_K_M</span>. Without a quantization the
						server picks one itself.
					</p>
				</div>

				{#if downloadsUnavailable}
					<p class="flex items-start gap-2 text-sm text-amber-600 dark:text-amber-400">
						<TriangleAlert class="mt-0.5 h-4 w-4 shrink-0" />

						<span>
							This server reports that it cannot download models
							{adminStore.status?.download?.offline
								? '(offline mode)'
								: '(built without HTTPS support)'}.
						</span>
					</p>
				{/if}

				{#if checkError}
					<p class="flex items-start gap-2 text-sm text-destructive" role="alert">
						<TriangleAlert class="mt-0.5 h-4 w-4 shrink-0" />

						<span>Check failed: {checkError}</span>
					</p>
				{/if}

				{#if result}
					<div
						class="space-y-3 rounded-lg border bg-muted/30 p-3 text-sm"
						data-testid="download-check"
					>
						{#if result.files.length > 0}
							<div>
								<p class="mb-1 text-xs font-medium text-muted-foreground">
									Files to download ({result.files.length})
								</p>

								<ul class="max-h-40 space-y-1 overflow-y-auto">
									{#each result.files as file (file.name)}
										<li class="flex items-center justify-between gap-3">
											<span class="truncate font-mono text-xs" title={file.name}>{file.name}</span>

											<span class="shrink-0 text-xs text-muted-foreground tabular-nums">
												{formatBytes(file.bytes)}
											</span>
										</li>
									{/each}
								</ul>
							</div>
						{/if}

						<dl class="grid grid-cols-2 gap-x-4 gap-y-1 text-xs">
							<dt class="text-muted-foreground">Total size</dt>

							<dd class="text-right font-medium tabular-nums">
								{result.files.length > 0 ? formatBytes(result.total_bytes) : '—'}
							</dd>

							<dt class="text-muted-foreground">Free space in the model cache</dt>

							<dd class="text-right font-medium tabular-nums">{formatBytes(result.free_bytes)}</dd>
						</dl>

						{#if warnings.length > 0}
							<ul class="space-y-1.5 border-t pt-3">
								{#each warnings as warning (warning.code)}
									<li
										class={[
											'flex items-start gap-2',
											warning.level === AdminNoticeLevel.WARNING
												? 'text-amber-600 dark:text-amber-400'
												: 'text-muted-foreground'
										]}
										data-warning={warning.code}
									>
										{#if warning.level === AdminNoticeLevel.WARNING}
											<TriangleAlert class="mt-0.5 h-4 w-4 shrink-0" />
										{:else}
											<Info class="mt-0.5 h-4 w-4 shrink-0" />
										{/if}

										<span>{warning.text}</span>
									</li>
								{/each}
							</ul>
						{:else}
							<p
								class="flex items-center gap-2 border-t pt-3 text-green-600 dark:text-green-400"
								data-warning="none"
							>
								<CircleCheck class="h-4 w-4 shrink-0" />

								No problems found.
							</p>
						{/if}
					</div>
				{/if}

				{#if downloadError}
					<p class="flex items-start gap-2 text-sm text-destructive" role="alert">
						<TriangleAlert class="mt-0.5 h-4 w-4 shrink-0" />

						<span>The server did not start the download: {downloadError}</span>
					</p>
				{/if}
			</div>

			<Dialog.Footer>
				<Button onclick={() => handleOpenChange(false)} size="sm" variant="secondary">Cancel</Button
				>

				<Button disabled={!isValid || starting} onclick={handleDownload} size="sm">
					{#if starting}
						<Loader2 class="{ICON_CLASS_DEFAULT} animate-spin" />
					{:else}
						<Download class={ICON_CLASS_DEFAULT} />
					{/if}

					Download
				</Button>
			</Dialog.Footer>
		{/if}
	</Dialog.Content>
</Dialog.Root>
