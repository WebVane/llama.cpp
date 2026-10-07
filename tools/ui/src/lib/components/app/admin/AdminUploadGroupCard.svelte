<script lang="ts">
	import AdminUploadPlacement from './AdminUploadPlacement.svelte';
	import {
		ChevronDown,
		CircleCheck,
		FileBox,
		Loader2,
		ScanEye,
		TriangleAlert
	} from '@lucide/svelte';
	import { Badge } from '$lib/components/ui/badge';
	import * as Collapsible from '$lib/components/ui/collapsible';
	import { Input } from '$lib/components/ui/input';
	import { Switch } from '$lib/components/ui/switch';
	import { AdminModelRole, AdminSettingKey } from '$lib/enums';
	import { AdminUploadFileRole } from '$lib/enums/admin.enums';
	import { AdminApiError } from '$lib/services/admin.service';
	import { adminUploadStore } from '$lib/stores/admin-upload.svelte';
	import type { ApiAdminGpu } from '$lib/types';
	import type {
		AdminUploadGroup,
		AdminUploadGroupState,
		AdminUploadPlacementValue,
		ApiAdminUploadCreateResult
	} from '$lib/types/admin-upload';
	import { formatBytes } from '$lib/utils/admin';
	import {
		buildUploadPlacement,
		describeModelNameProblem,
		describeNameTaken,
		describePickedRole,
		describeUploadFileProblem,
		describeUploadWarning,
		groupBytes,
		groupFiles,
		validateModelName
	} from '$lib/utils/admin-upload';
	import { onDestroy, untrack } from 'svelte';

	interface Props {
		group: AdminUploadGroup;
		/** 1-based number shown in the title when there are several models */
		number: number | null;
		devices: string[];
		gpus: ApiAdminGpu[];
		disabled?: boolean;
		onState: (key: string, state: AdminUploadGroupState) => void;
	}

	let { devices, disabled = false, gpus, group, number, onState }: Props = $props();

	const DRY_RUN_DELAY_MS = 400;
	const SHA_RE = /^[0-9a-fA-F]{64}$/;

	let name = $state('');
	let autoPublish = $state(true);
	let settingsMode = $state<'replace' | 'keep'>('replace');
	let placement = $state<AdminUploadPlacementValue>({
		ctxSize: '',
		device: '',
		parallel: '',
		role: AdminModelRole.CHAT
	});
	let placementOpen = $state(false);
	let shaOpen = $state(false);
	let sha = $state<Record<number, string>>({});
	let dry = $state<ApiAdminUploadCreateResult | null>(null);
	let dryError = $state<AdminApiError | null>(null);
	let dryRunning = $state(false);
	let dryFor = $state('');
	let run = 0;
	let timer: ReturnType<typeof setTimeout> | null = null;

	// the proposed id, set once per group (the user may edit it)
	let lastGroupKey = '';

	$effect(() => {
		if (group.key !== lastGroupKey) {
			lastGroupKey = group.key;
			name = group.proposedName;
		}
	});

	let files = $derived(groupFiles(group));
	let modelHeader = $derived(group.files[0]?.header ?? null);
	let settings = $derived(buildUploadPlacement(placement));
	let nameProblem = $derived(validateModelName(name.trim()));
	let shaProblems = $derived(
		Object.entries(sha)
			.filter(([, value]) => value.trim() && !SHA_RE.test(value.trim()))
			.map(([key]) => Number(key))
	);
	let requestKey = $derived(JSON.stringify([name.trim(), settings, settingsMode]));
	let fieldErrors = $derived.by(() => {
		const errors: Record<string, string> = {};

		if (dryError?.param && dryError.param !== 'name') errors[dryError.param] = dryError.message;

		return errors;
	});
	let nameError = $derived.by(() => {
		if (nameProblem) return describeModelNameProblem(nameProblem);

		if (!dryError || dryFor !== requestKey) return null;

		if (dryError.type === 'invalid_name') {
			return describeModelNameProblem(String(dryError.details?.reason ?? ''));
		}

		if (dryError.type === 'name_taken') {
			const holder = describeNameTaken(
				dryError.details?.by,
				String(dryError.details?.other ?? name),
				dryError.details?.case_variant === true
			);

			return `${holder} Choose another name.`;
		}

		return null;
	});
	let otherError = $derived(
		dryError && dryFor === requestKey && !nameError && !fieldErrors[dryError.param ?? '']
			? dryError.message
			: null
	);
	let existing = $derived(dry?.existing_settings ?? null);
	let blocking = $derived.by(() => {
		if (nameError) return nameError;

		if (group.problems.includes('split_incomplete')) {
			return `Parts ${group.missingShards.join(', ')} of the split model are missing.`;
		}

		if (group.problems.includes('split_mismatch')) return 'The parts are of different models.';

		if (shaProblems.length > 0) return 'A SHA-256 must be 64 hex digits.';

		if (dryError && dryFor === requestKey) return dryError.message;

		return null;
	});

	$effect(() => {
		const key = requestKey;
		const body = {
			files: files.map((file) => ({
				last_modified: file.file.lastModified,
				name: file.name,
				size: file.size
			})),
			name: name.trim(),
			settings,
			settings_mode: settingsMode
		};

		if (timer) clearTimeout(timer);

		if (nameProblem) {
			dryRunning = false;

			return;
		}

		dryRunning = true;
		timer = setTimeout(() => void dryRun(key, body), DRY_RUN_DELAY_MS);
	});

	// what the upload tab collects to queue the models
	$effect(() => {
		const ready = !blocking && !dryRunning && dryFor === requestKey && dry !== null;
		const state: AdminUploadGroupState = {
			blocking,
			checking: dryRunning,
			request: ready
				? {
						autoPublish,
						expectedSha256: Object.fromEntries(
							Object.entries(sha)
								.filter(([, value]) => value.trim())
								.map(([key, value]) => [Number(key), value.trim().toLowerCase()])
						),
						files,
						name: name.trim(),
						settings,
						settingsMode
					}
				: null,
			warnings: dry && dryFor === requestKey ? dry.warnings : []
		};
		const key = group.key;

		// the parent's own state must not become a dependency of this effect
		untrack(() => onState(key, state));
	});

	onDestroy(() => {
		if (timer) clearTimeout(timer);
	});

	async function dryRun(key: string, body: Parameters<typeof adminUploadStore.dryRun>[0]) {
		const mine = ++run;

		try {
			const result = await adminUploadStore.dryRun(body);

			if (mine !== run) return;

			dry = result;
			dryError = null;
		} catch (error) {
			if (mine !== run) return;

			dry = null;
			dryError = error instanceof AdminApiError ? error : new AdminApiError(String(error), 0, null);
		} finally {
			if (mine === run) {
				dryFor = key;
				dryRunning = false;
			}
		}
	}

	function headerLine(file: (typeof files)[number]): string {
		const header = file.header;

		if (!header || header.error) return '';

		const parts = [header.arch, header.quant].filter((part) => !!part);

		return parts.join(' · ');
	}
</script>

<section
	class="flex flex-col gap-4 rounded-lg border bg-card p-4"
	data-group={group.key}
	data-testid="upload-group"
>
	<div class="flex flex-wrap items-center gap-2">
		<FileBox class="h-4 w-4 shrink-0 text-muted-foreground" />

		<h3 class="text-sm font-semibold">
			{number !== null ? `Model ${number}` : 'Model'}
		</h3>

		<span class="text-xs text-muted-foreground tabular-nums">{formatBytes(groupBytes(group))}</span>

		{#if group.files.length > 1}
			<Badge class="font-normal" variant="outline">{group.files.length} parts</Badge>
		{/if}

		{#if group.projector}
			<Badge class="font-normal" variant="outline">
				<ScanEye />

				projector
			</Badge>
		{/if}
	</div>

	<ul class="flex flex-col divide-y rounded-md border text-sm" data-testid="upload-files">
		{#each files as file (file.key)}
			<li class="flex flex-col gap-0.5 px-3 py-2" data-file-role={file.role}>
				<div class="flex min-w-0 items-center gap-2">
					<span class="min-w-0 flex-1 truncate font-mono text-xs" title={file.name}
						>{file.name}</span
					>

					<span class="shrink-0 text-xs text-muted-foreground tabular-nums">
						{formatBytes(file.size)}
					</span>
				</div>

				<div class="flex flex-wrap items-center gap-x-2 gap-y-1 text-xs text-muted-foreground">
					<Badge
						class="font-normal"
						variant={file.role === AdminUploadFileRole.PROJECTOR ? 'secondary' : 'outline'}
					>
						{describePickedRole(file)}
					</Badge>

					{#if headerLine(file)}
						<span class="font-mono" data-testid="upload-header">{headerLine(file)}</span>
					{/if}

					{#if file.header?.contextLength && file.role !== AdminUploadFileRole.PROJECTOR}
						<span>trained for {file.header.contextLength.toLocaleString('en-US')} tokens</span>
					{/if}

					{#if file.role === AdminUploadFileRole.PROJECTOR && file.header && !file.header.error}
						<span>
							{[file.header.clipVision && 'images', file.header.clipAudio && 'audio']
								.filter(Boolean)
								.join(' + ') || 'no image or audio encoder found'}
						</span>
					{/if}
				</div>

				{#each file.problems as problem (problem)}
					<p class="flex items-start gap-1.5 text-xs text-amber-600 dark:text-amber-400">
						<TriangleAlert class="mt-0.5 h-3 w-3 shrink-0" />

						{describeUploadFileProblem(problem)}
					</p>
				{/each}
			</li>
		{/each}
	</ul>

	{#if group.problems.includes('split_incomplete')}
		<p class="flex items-start gap-2 text-sm text-destructive" data-testid="upload-missing-parts">
			<TriangleAlert class="mt-0.5 h-4 w-4 shrink-0" />

			Parts {group.missingShards.join(', ')} of {group.files[0]?.shard?.count ?? '?'} are missing: pick
			every part of the split model together.
		</p>
	{/if}

	<div class="flex flex-col gap-1.5">
		<label class="text-sm font-medium" for="upload-name-{group.key}">Name</label>

		<div class="flex items-center gap-2">
			<Input
				bind:value={name}
				aria-invalid={nameError ? 'true' : undefined}
				autocomplete="off"
				class="font-mono sm:max-w-md"
				{disabled}
				id="upload-name-{group.key}"
				spellcheck={false}
			/>

			{#if dryRunning}
				<Loader2 class="h-4 w-4 shrink-0 animate-spin text-muted-foreground" />
			{:else if !nameError && dry && dryFor === requestKey}
				<CircleCheck
					aria-label="name is free"
					class="h-4 w-4 shrink-0 text-green-600 dark:text-green-400"
				/>
			{/if}
		</div>

		{#if nameError}
			<p class="text-xs text-destructive" data-testid="upload-name-error" role="alert">
				{nameError}
			</p>
		{:else}
			<p class="text-xs text-muted-foreground">
				The model is listed under this name. It is proposed from the model file; the server checks
				it.
			</p>
		{/if}
	</div>

	{#if existing}
		<div
			class="flex flex-col gap-2 rounded-md border border-dashed p-3 text-sm"
			data-testid="upload-existing"
		>
			<p>
				There are saved settings from an earlier model with this name:
				<span class="font-mono text-xs">
					{Object.entries(existing)
						.map(([key, value]) => `${key} = ${value}`)
						.join(', ') || 'no values'}
				</span>
			</p>

			<div class="flex flex-wrap gap-4" role="radiogroup">
				<label class="flex items-center gap-2">
					<input
						checked={settingsMode === 'replace'}
						{disabled}
						name="upload-mode-{group.key}"
						onchange={() => (settingsMode = 'replace')}
						type="radio"
						value="replace"
					/>

					Start fresh
				</label>

				<label class="flex items-center gap-2">
					<input
						checked={settingsMode === 'keep'}
						{disabled}
						name="upload-mode-{group.key}"
						onchange={() => (settingsMode = 'keep')}
						type="radio"
						value="keep"
					/>

					Keep them
				</label>
			</div>

			<p class="text-xs text-muted-foreground">
				{settingsMode === 'keep'
					? 'The saved values apply to the new model; a placement chosen below replaces the same fields.'
					: 'The saved values are removed when the model is published.'}
			</p>
		</div>
	{/if}

	<Collapsible.Root bind:open={placementOpen}>
		<Collapsible.Trigger
			class="flex items-center gap-1.5 text-sm font-medium hover:text-foreground"
			data-testid="upload-placement-toggle"
		>
			<ChevronDown class={['h-4 w-4 transition-transform', placementOpen && 'rotate-180']} />

			Placement (optional)

			{#if Object.keys(settings).length > 0}
				<span class="text-xs font-normal text-muted-foreground">
					· {Object.entries(settings)
						.map(([key, value]) =>
							key === AdminSettingKey.DEVICE
								? value
								: key === AdminSettingKey.CTX_SIZE
									? `${value} tokens`
									: key === AdminSettingKey.PARALLEL
										? `${value} parallel`
										: key
						)
						.join(', ')}
				</span>
			{/if}
		</Collapsible.Trigger>

		<Collapsible.Content>
			<div class="mt-3">
				<AdminUploadPlacement
					{devices}
					{disabled}
					errors={fieldErrors}
					{gpus}
					idPrefix="upload-{group.key}"
					locks={dry?.locks ?? {}}
					nCtxTrain={modelHeader?.contextLength ?? null}
					onChange={(next) => (placement = next)}
					value={placement}
					weightsBytes={groupBytes(group)}
				/>
			</div>
		</Collapsible.Content>
	</Collapsible.Root>

	<Collapsible.Root bind:open={shaOpen}>
		<Collapsible.Trigger
			class="flex items-center gap-1.5 text-sm font-medium hover:text-foreground"
		>
			<ChevronDown class={['h-4 w-4 transition-transform', shaOpen && 'rotate-180']} />

			Verify with SHA-256 (optional)
		</Collapsible.Trigger>

		<Collapsible.Content>
			<div class="mt-3 flex flex-col gap-3">
				<p class="text-xs text-muted-foreground">
					Paste the SHA-256 shown on the file's Hugging Face page. A mismatch does not delete the
					upload: it waits for you to correct the value or publish anyway.
				</p>

				{#each files as file (file.key)}
					<div class="flex flex-col gap-1">
						<label class="truncate font-mono text-xs" for="upload-sha-{group.key}-{file.key}">
							{file.name}
						</label>

						<Input
							aria-invalid={shaProblems.includes(file.key) ? 'true' : undefined}
							autocomplete="off"
							class="font-mono text-xs"
							{disabled}
							id="upload-sha-{group.key}-{file.key}"
							oninput={(event) => (sha = { ...sha, [file.key]: event.currentTarget.value })}
							placeholder="64 hex digits"
							spellcheck={false}
							value={sha[file.key] ?? ''}
						/>
					</div>
				{/each}
			</div>
		</Collapsible.Content>
	</Collapsible.Root>

	<label class="flex items-center gap-3 text-sm">
		<Switch bind:checked={autoPublish} {disabled} />

		<span>
			Publish automatically when the checks pass
			<span class="block text-xs text-muted-foreground">
				Off: the upload waits for you to press Publish.
			</span>
		</span>
	</label>

	{#if otherError}
		<p class="flex items-start gap-2 text-sm text-destructive" role="alert">
			<TriangleAlert class="mt-0.5 h-4 w-4 shrink-0" />

			{otherError}
		</p>
	{/if}

	{#if dry && dryFor === requestKey && dry.warnings.length > 0}
		<ul class="flex flex-col gap-1.5 text-sm" data-testid="upload-warnings">
			{#each dry.warnings as code (code)}
				<li class="flex items-start gap-2 text-amber-600 dark:text-amber-400" data-warning={code}>
					<TriangleAlert class="mt-0.5 h-4 w-4 shrink-0" />

					<span>{describeUploadWarning(code)}</span>
				</li>
			{/each}
		</ul>
	{/if}

	{#if dry && dryFor === requestKey}
		<p class="text-xs text-muted-foreground tabular-nums">
			Free space in the models directory: {formatBytes(dry.disk.free_bytes)} of {formatBytes(
				dry.disk.total_bytes
			)}{dry.disk.other_uploads_bytes > 0
				? `; other unfinished uploads still need ${formatBytes(dry.disk.other_uploads_bytes)}`
				: ''}.
		</p>
	{/if}
</section>
