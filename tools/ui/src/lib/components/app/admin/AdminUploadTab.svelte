<script lang="ts">
	import AdminUploadGroupCard from './AdminUploadGroupCard.svelte';
	import AdminUploadJobCard from './AdminUploadJobCard.svelte';
	import AdminUploadsBanner from './AdminUploadsBanner.svelte';
	import { FolderOpen, Loader2, Plus, TriangleAlert, Upload } from '@lucide/svelte';
	import { Button } from '$lib/components/ui/button';
	import * as Select from '$lib/components/ui/select';
	import { adminStore } from '$lib/stores/admin.svelte';
	import { adminUploadStore } from '$lib/stores/admin-upload.svelte';
	import type {
		AdminGgufHeader,
		AdminUploadGroup,
		AdminUploadGrouping,
		AdminUploadGroupState
	} from '$lib/types/admin-upload';
	import { formatBytes } from '$lib/utils/admin';
	import { readGgufHeader } from '$lib/utils/admin-gguf-header';
	import {
		describeIgnoredFile,
		describeUploadUnavailable,
		describeUploadWarning,
		groupUploadFiles,
		suggestProjectorGroup
	} from '$lib/utils/admin-upload';
	import { untrack } from 'svelte';

	interface Props {
		/** Files dropped on the page or handed over by the dialog: grouped at once */
		files?: File[] | null;
		/** Open the settings dialog of a published model */
		onOpenSettings?: (model: string) => void;
		/** The files were taken over (the dialog clears its copy) */
		onFilesTaken?: () => void;
	}

	let { files = null, onFilesTaken, onOpenSettings }: Props = $props();

	const NONE = '__none__';

	let mode = $state<'pick' | 'review' | 'jobs'>(adminUploadStore.jobs.length > 0 ? 'jobs' : 'pick');
	let reading = $state(false);
	let readProgress = $state({ done: 0, total: 0 });
	let grouping = $state<AdminUploadGrouping | null>(null);
	let projectorChoice = $state<Record<number, string>>({});
	let groupStates = $state<Record<string, AdminUploadGroupState>>({});
	let confirmWarnings = $state(false);
	let dragOver = $state(false);
	let picker = $state<HTMLInputElement | null>(null);

	let available = $derived(adminUploadStore.available);
	let info = $derived(adminUploadStore.info);
	let unavailable = $derived(
		available === false
			? describeUploadUnavailable(
					info?.reason ?? adminStore.status?.upload?.reason ?? null,
					info?.message ?? adminStore.status?.upload?.message ?? null
				)
			: null
	);
	let devices = $derived(adminStore.buildDevices);
	let gpus = $derived(adminStore.gpus?.gpus ?? []);
	let effectiveGroups = $derived.by((): AdminUploadGroup[] => {
		if (!grouping) return [];

		return grouping.groups.map((group) => {
			if (group.projector) return group;

			const chosen = grouping?.unpairedProjectors.find(
				(projector) => projectorChoice[projector.key] === group.key
			);

			return chosen ? { ...group, projector: chosen } : group;
		});
	});
	let projectorConflict = $derived.by(() => {
		const chosen = Object.values(projectorChoice).filter((key) => key && key !== NONE);

		return new Set(chosen).size !== chosen.length;
	});
	let states = $derived(effectiveGroups.map((group) => groupStates[group.key] ?? null));
	let checking = $derived(states.some((state) => !state || state.checking));
	let blockers = $derived(
		effectiveGroups
			.map((group, index) => ({ index, text: groupStates[group.key]?.blocking ?? null }))
			.filter((item) => item.text)
	);
	let warnings = $derived([...new Set(states.flatMap((state) => state?.warnings ?? []))]);
	let canUpload = $derived(
		effectiveGroups.length > 0 &&
			!checking &&
			!projectorConflict &&
			blockers.length === 0 &&
			states.every((state) => state?.request)
	);

	// the files of the current review, and files dropped while their headers were being read
	let reviewFiles: File[] = [];
	let pendingFiles: File[] = [];

	// files handed over by the dialog (dropped on the Models tab or on the dialog)
	$effect(() => {
		const handed = files;

		if (!handed || handed.length === 0) return;

		untrack(() => {
			onFilesTaken?.();
			addFiles(handed);
		});
	});

	function sameFile(a: File, b: File): boolean {
		return a.name === b.name && a.size === b.size && a.lastModified === b.lastModified;
	}

	/**
	 * Files dropped on the dialog: during a review they join the files under
	 * review (a forgotten projector or part) and everything is grouped again;
	 * otherwise they start a new review.
	 */
	function addFiles(more: File[]) {
		if (reading) {
			pendingFiles = [...pendingFiles, ...more];

			return;
		}

		if (mode === 'review' && grouping) {
			const added = more.filter((file) => !reviewFiles.some((known) => sameFile(known, file)));

			if (added.length > 0) void processFiles([...reviewFiles, ...added]);

			return;
		}

		void processFiles(more);
	}

	async function processFiles(picked: File[]) {
		if (picked.length === 0 || reading) return;

		reviewFiles = picked;
		reading = true;
		mode = 'review';
		grouping = null;
		groupStates = {};
		projectorChoice = {};
		confirmWarnings = false;
		readProgress = { done: 0, total: picked.length };

		const inputs: { file: File; header: AdminGgufHeader | null }[] = [];

		// one file at a time: each read is small (the metadata only)
		for (const file of picked) {
			let header: AdminGgufHeader | null = null;

			try {
				header = await readGgufHeader(file);
			} catch {
				header = null;
			}

			inputs.push({ file, header });
			readProgress = { done: inputs.length, total: picked.length };
		}

		const result = groupUploadFiles(inputs);
		const choice: Record<number, string> = {};

		for (const projector of result.unpairedProjectors) {
			choice[projector.key] = suggestProjectorGroup(projector, result.groups) ?? NONE;
		}

		projectorChoice = choice;
		grouping = result;
		reading = false;

		if (pendingFiles.length > 0) {
			const more = pendingFiles;

			pendingFiles = [];
			addFiles(more);
		}
	}

	function onPicked(event: Event) {
		const input = event.currentTarget as HTMLInputElement;
		const picked = [...(input.files ?? [])];

		input.value = '';
		void processFiles(picked);
	}

	function onDrop(event: DragEvent) {
		event.preventDefault();
		dragOver = false;

		const dropped = [...(event.dataTransfer?.files ?? [])];

		if (dropped.length > 0) void processFiles(dropped);
	}

	function onDragOver(event: DragEvent) {
		if (!event.dataTransfer?.types.includes('Files')) return;

		event.preventDefault();
		dragOver = true;
	}

	function setGroupState(key: string, state: AdminUploadGroupState) {
		groupStates = { ...groupStates, [key]: state };
	}

	function startUpload() {
		if (!canUpload) return;

		if (warnings.length > 0 && !confirmWarnings) {
			confirmWarnings = true;

			return;
		}

		const requests = states.map((state) => state?.request).filter((request) => !!request);

		adminUploadStore.enqueue(requests as NonNullable<AdminUploadGroupState['request']>[]);
		grouping = null;
		groupStates = {};
		confirmWarnings = false;
		mode = 'jobs';
	}

	function reset() {
		grouping = null;
		groupStates = {};
		confirmWarnings = false;
		mode = adminUploadStore.jobs.length > 0 ? 'jobs' : 'pick';
	}

	function projectorOptions() {
		return [
			{ label: 'Do not upload it', value: NONE },
			...(grouping?.groups ?? []).map((group, index) => ({
				label: `Model ${index + 1}: ${group.proposedName}`,
				value: group.key
			}))
		];
	}
</script>

<div class="flex flex-col gap-4" data-testid="upload-tab">
	{#if unavailable}
		<div
			class="flex flex-col gap-2 rounded-lg border border-amber-500/40 bg-amber-500/10 p-4 text-sm text-amber-800 dark:text-amber-300"
			data-testid="upload-unavailable"
		>
			<p class="flex items-start gap-2 font-medium">
				<TriangleAlert class="mt-0.5 h-4 w-4 shrink-0" />

				{unavailable.text}
			</p>

			<p class="text-amber-800/90 dark:text-amber-300/90">{unavailable.fix}</p>
		</div>
	{:else if mode === 'pick'}
		<div
			aria-label="Drop GGUF files here"
			class={[
				'flex flex-col items-center gap-3 rounded-lg border-2 border-dashed px-4 py-10 text-center transition-colors',
				dragOver ? 'border-primary bg-primary/5' : 'border-border'
			]}
			data-testid="upload-dropzone"
			ondragleave={() => (dragOver = false)}
			ondragover={onDragOver}
			ondrop={onDrop}
			role="region"
		>
			<Upload class="h-8 w-8 text-muted-foreground" />

			<p class="text-sm font-medium">Drop GGUF files here</p>

			<Button onclick={() => picker?.click()} size="sm" variant="secondary">
				<FolderOpen />

				Choose files
			</Button>

			<p class="max-w-md text-xs text-muted-foreground">
				A model is one .gguf file or all parts of a split model (name-00001-of-00003.gguf ...), plus
				a projector (mmproj) for images or audio. Several models are uploaded one after another. An
				interrupted upload continues where it stopped.
			</p>
		</div>

		<AdminUploadsBanner compact />
	{:else if mode === 'review'}
		{#if reading}
			<div class="flex items-center justify-center gap-2 py-10 text-sm text-muted-foreground">
				<Loader2 class="h-4 w-4 animate-spin" />

				Reading the file headers ({readProgress.done} / {readProgress.total})...
			</div>
		{:else if grouping}
			{#if grouping.ignored.length > 0}
				<div class="rounded-md border border-dashed p-3 text-sm" data-testid="upload-ignored">
					<p class="mb-1 font-medium">Not uploaded:</p>

					<ul class="flex flex-col gap-1 text-xs text-muted-foreground">
						{#each grouping.ignored as item (item.name + item.reason)}
							<li data-reason={item.reason}>
								<span class="font-mono text-foreground">{item.name}</span>: {describeIgnoredFile(
									item.reason
								)}
							</li>
						{/each}
					</ul>
				</div>
			{/if}

			{#if grouping.groups.length === 0}
				<p class="py-6 text-center text-sm text-muted-foreground">
					None of these files can be uploaded as a model.
				</p>
			{/if}

			{#each grouping.unpairedProjectors as projector (projector.key)}
				{@const options = projectorOptions()}

				<div
					class="flex flex-col gap-2 rounded-md border p-3 text-sm"
					data-testid="upload-projector-choice"
				>
					<p>
						Projector <span class="font-mono text-xs">{projector.name}</span>
						({formatBytes(projector.size)}): which model is it for?
					</p>

					<Select.Root
						onValueChange={(value) =>
							(projectorChoice = { ...projectorChoice, [projector.key]: value })}
						type="single"
						value={projectorChoice[projector.key] ?? NONE}
					>
						<Select.Trigger class="w-full sm:w-96">
							<span class="truncate">
								{options.find((option) => option.value === (projectorChoice[projector.key] ?? NONE))
									?.label}
							</span>
						</Select.Trigger>

						<Select.Content>
							{#each options as option (option.value)}
								<Select.Item label={option.label} value={option.value} />
							{/each}
						</Select.Content>
					</Select.Root>
				</div>
			{/each}

			{#if projectorConflict}
				<p class="text-sm text-destructive">A model can have one projector only.</p>
			{/if}

			{#each effectiveGroups as group, index (group.key)}
				<AdminUploadGroupCard
					{devices}
					{gpus}
					{group}
					number={effectiveGroups.length > 1 ? index + 1 : null}
					onState={setGroupState}
				/>
			{/each}

			{#if confirmWarnings && warnings.length > 0}
				<div
					class="flex flex-col gap-2 rounded-md border border-amber-500/40 bg-amber-500/10 p-3 text-sm text-amber-800 dark:text-amber-300"
					data-testid="upload-confirm-warnings"
				>
					<p class="font-medium">Upload anyway?</p>

					<ul class="flex list-disc flex-col gap-1 pl-5">
						{#each warnings as code (code)}
							<li>{describeUploadWarning(code)}</li>
						{/each}
					</ul>
				</div>
			{/if}

			<div class="flex flex-wrap items-center gap-2 border-t pt-4">
				<Button onclick={reset} size="sm" variant="ghost">
					{adminUploadStore.jobs.length > 0 ? 'Back to the uploads' : 'Choose other files'}
				</Button>

				{#if blockers.length > 0}
					<span class="text-xs text-destructive">
						{blockers.length === 1 && effectiveGroups.length === 1
							? blockers[0].text
							: `Fix ${blockers.length} model${blockers.length === 1 ? '' : 's'} first.`}
					</span>
				{/if}

				<Button
					class="ml-auto"
					data-testid="upload-start"
					disabled={!canUpload}
					onclick={startUpload}
					size="sm"
				>
					{#if checking}
						<Loader2 class="animate-spin" />
					{:else}
						<Upload />
					{/if}

					{confirmWarnings && warnings.length > 0
						? 'Upload anyway'
						: effectiveGroups.length > 1
							? `Upload ${effectiveGroups.length} models`
							: 'Upload'}
				</Button>
			</div>
		{/if}
	{:else}
		<div class="flex flex-col gap-3" data-testid="upload-jobs">
			{#each adminUploadStore.jobs as job (job.key)}
				<AdminUploadJobCard {job} {onOpenSettings} />
			{/each}

			{#if adminUploadStore.jobs.length === 0}
				<p class="py-6 text-center text-sm text-muted-foreground">
					No uploads in this browser tab.
				</p>
			{/if}

			<AdminUploadsBanner compact />

			<div class="flex border-t pt-4">
				<Button onclick={() => (mode = 'pick')} size="sm" variant="secondary">
					<Plus />

					Upload more models
				</Button>
			</div>
		</div>
	{/if}

	<input
		bind:this={picker}
		accept=".gguf"
		class="hidden"
		data-testid="upload-input"
		multiple
		onchange={onPicked}
		type="file"
	/>
</div>
