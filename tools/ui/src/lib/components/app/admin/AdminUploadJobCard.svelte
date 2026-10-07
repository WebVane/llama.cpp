<script lang="ts">
	import {
		Check,
		ChevronDown,
		CircleAlert,
		CircleCheck,
		Clock,
		Copy,
		FolderOpen,
		Loader2,
		Pause,
		Play,
		Power,
		RotateCcw,
		SlidersHorizontal,
		Trash2,
		TriangleAlert,
		X
	} from '@lucide/svelte';
	import { DialogConfirmation } from '$lib/components/app/dialogs';
	import { Badge } from '$lib/components/ui/badge';
	import { Button } from '$lib/components/ui/button';
	import * as Collapsible from '$lib/components/ui/collapsible';
	import { Input } from '$lib/components/ui/input';
	import { ADMIN_FIT_VERDICT_LABELS, ADMIN_UPLOAD_NGINX_SNIPPET } from '$lib/constants';
	import { AdminErrorType, AdminFeature, AdminFitVerdict } from '$lib/enums';
	import {
		AdminUploadBlock,
		AdminUploadJobPhase,
		AdminUploadSendState
	} from '$lib/enums/admin.enums';
	import { adminStore } from '$lib/stores/admin.svelte';
	import { adminUploadStore } from '$lib/stores/admin-upload.svelte';
	import type { AdminUploadJob } from '$lib/types/admin-upload';
	import { formatBytes } from '$lib/utils/admin';
	import { toFitVerdict, worstFitVerdict } from '$lib/utils/admin-fit';
	import {
		describeExpiry,
		describeModelNameProblem,
		describeSendState,
		describeUploadBlock,
		describeUploadRejection,
		formatDuration,
		uploadNameLimit,
		validateModelName
	} from '$lib/utils/admin-upload';
	import { onDestroy } from 'svelte';

	interface Props {
		job: AdminUploadJob;
		/** Open the settings dialog of a published model */
		onOpenSettings?: (model: string) => void;
	}

	let { job, onOpenSettings }: Props = $props();

	const PHASE_LABELS: Record<AdminUploadJobPhase, string> = {
		[AdminUploadJobPhase.CANCELLED]: 'Cancelled',
		[AdminUploadJobPhase.CHECKING]: 'Checking',
		[AdminUploadJobPhase.CREATING]: 'Starting',
		[AdminUploadJobPhase.EXPIRED]: 'Expired',
		[AdminUploadJobPhase.FAILED]: 'Not started',
		[AdminUploadJobPhase.PUBLISHED]: 'Published',
		[AdminUploadJobPhase.PUBLISHING]: 'Publishing',
		[AdminUploadJobPhase.QUEUED]: 'Queued',
		[AdminUploadJobPhase.READY]: 'Ready',
		[AdminUploadJobPhase.REJECTED]: 'Rejected',
		[AdminUploadJobPhase.UPLOADING]: 'Uploading'
	};
	const SUBSTATE_LABELS: Record<string, string> = {
		classifying: 'Reading the files...',
		hashing: 'Computing the SHA-256...',
		load_check: 'Loading the model metadata with llama.cpp...',
		queued: 'Waiting for another check to finish...'
	};

	let now = $state(Date.now());
	let ticker = setInterval(() => (now = Date.now()), 1000);
	let filesOpen = $state(false);
	let snippetOpen = $state(false);
	let confirmCancel = $state(false);
	let busy = $state(false);
	let actionError = $state<string | null>(null);
	let renameValue = $state('');
	let shaEdits = $state<Record<number, string>>({});
	let copied = $state<string | null>(null);
	let picker = $state<HTMLInputElement | null>(null);

	let session = $derived(job.session);
	let send = $derived(job.send);
	let total = $derived(job.files.reduce((sum, file) => sum + file.size, 0));
	let confirmed = $derived(
		send?.confirmed ?? job.files.reduce((sum, file) => sum + Math.min(file.offset, file.size), 0)
	);
	let fraction = $derived(total > 0 ? Math.min(1, (confirmed + (send?.inFlight ?? 0)) / total) : 0);
	let needsFiles = $derived(
		adminUploadStore.needsFiles(job) ||
			send?.state === AdminUploadSendState.NOT_SAME_FILE ||
			send?.state === AdminUploadSendState.NEEDS_FILE
	);
	let paused = $derived(send?.state === AdminUploadSendState.PAUSED);
	let sending = $derived(
		job.phase === AdminUploadJobPhase.UPLOADING &&
			!!send &&
			(send.state === AdminUploadSendState.UPLOADING ||
				send.state === AdminUploadSendState.RETRYING ||
				send.state === AdminUploadSendState.SERVER_RESTARTING ||
				send.state === AdminUploadSendState.UNREACHABLE)
	);
	let block = $derived(
		job.phase === AdminUploadJobPhase.READY
			? describeUploadBlock(session?.blocked_by ?? null, session?.blocked ?? null)
			: null
	);
	/**
	 * Files the server refused while the upload is open: from the session (the
	 * header check may answer after the last byte was sent) and from the chunk
	 * answer that stopped the sending. Each one can be dropped.
	 */
	let refusedFiles = $derived.by(() => {
		if (job.phase !== AdminUploadJobPhase.UPLOADING) return [];

		const list: { index: number; name: string; code: string; message: string | null }[] = [];

		for (const remote of session?.files ?? []) {
			if (remote.error && !remote.dropped) {
				list.push({
					code: remote.error.code,
					index: remote.index,
					message: remote.error.message ?? null,
					name: remote.name
				});
			}
		}

		const stopped = send?.state === AdminUploadSendState.FILE_REJECTED ? send.problem : null;

		if (stopped && !list.some((item) => item.index === stopped.file)) {
			const file = job.files.find((item) => item.index === stopped.file);

			if (file) {
				list.push({
					code: stopped.code,
					index: file.index,
					message: stopped.message ?? null,
					name: file.name
				});
			}
		}

		return list;
	});
	let mismatches = $derived(
		(session?.files ?? []).filter(
			(file) => file.sha256_expected && file.sha256 && file.sha256_expected !== file.sha256
		)
	);
	let renameProblem = $derived(renameValue ? validateModelName(renameValue.trim()) : 'empty');
	/** The checks wait for a shorter name (Windows, split model): its longest length */
	let nameLimit = $derived(
		job.phase === AdminUploadJobPhase.READY &&
			session?.blocked_by === AdminUploadBlock.CHECK_FAILED
			? uploadNameLimit(session.blocked)
			: null
	);
	let limitedRenameProblem = $derived(
		renameProblem ??
			(nameLimit !== null && renameValue.trim().length > nameLimit ? 'too_long_here' : null)
	);
	let model = $derived(session?.result?.model ?? job.name);
	let fitVerdict = $derived.by(() => {
		const fit = job.fit;

		if (!fit) return null;

		if (!fit.ok) return null;

		const verdicts = (fit.devices ?? []).map((device) => toFitVerdict(device.verdict));

		return verdicts.length > 0 ? worstFitVerdict(verdicts) : AdminFitVerdict.CPU_ONLY;
	});
	let progressText = $derived.by(() => {
		const parts = [`${formatBytes(confirmed)} of ${formatBytes(total)}`];

		if (sending && send?.speed) parts.push(`${formatBytes(send.speed)}/s`);

		if (sending && send?.eta !== null && send?.eta !== undefined && send.eta > 0) {
			parts.push(`${formatDuration(send.eta)} left`);
		}

		return parts.join(' · ');
	});

	onDestroy(() => clearInterval(ticker));

	async function act(run: () => Promise<void> | void) {
		if (busy) return;

		busy = true;
		actionError = null;

		try {
			await run();
		} catch (error) {
			actionError = error instanceof Error ? error.message : String(error);
		} finally {
			busy = false;
		}
	}

	async function onPicked(event: Event) {
		const input = event.currentTarget as HTMLInputElement;
		const files = [...(input.files ?? [])];

		input.value = '';

		if (files.length === 0) return;

		await act(async () => {
			const report = await adminUploadStore.attachFiles(job.key, files);

			if (report.mismatched.length > 0) {
				throw new Error(
					`Not the same file: ${report.mismatched.join(', ')} differs from the part already uploaded.`
				);
			}

			if (report.missing.length > 0) {
				throw new Error(`Still missing: ${report.missing.join(', ')} (same name and size).`);
			}
		});
	}

	async function copy(text: string) {
		try {
			await navigator.clipboard.writeText(text);
			copied = text;
			setTimeout(() => (copied = null), 1500);
		} catch {
			// clipboard refused (plain HTTP): the hash stays selectable
		}
	}
</script>

<article
	class="flex flex-col gap-3 rounded-lg border bg-card p-4"
	data-job={job.key}
	data-phase={job.phase}
	data-send-state={send?.state ?? ''}
	data-testid="upload-job"
>
	<header class="flex flex-wrap items-center gap-2">
		<span class="min-w-0 truncate font-mono text-sm font-medium" title={job.name}>{job.name}</span>

		<Badge
			class={[
				'font-normal',
				job.phase === AdminUploadJobPhase.PUBLISHED &&
					'border-green-500/40 text-green-700 dark:text-green-400',
				(job.phase === AdminUploadJobPhase.REJECTED || job.phase === AdminUploadJobPhase.FAILED) &&
					'border-destructive/40 text-destructive'
			]}
			data-testid="upload-phase"
			variant="outline"
		>
			{PHASE_LABELS[job.phase]}
		</Badge>

		{#if session?.expires_at && (job.phase === AdminUploadJobPhase.UPLOADING || job.phase === AdminUploadJobPhase.READY)}
			<span class="text-xs text-muted-foreground">{describeExpiry(session.expires_at, now)}</span>
		{/if}

		<span class="ml-auto text-xs text-muted-foreground tabular-nums">{formatBytes(total)}</span>
	</header>

	{#if job.phase === AdminUploadJobPhase.UPLOADING || job.phase === AdminUploadJobPhase.QUEUED || job.phase === AdminUploadJobPhase.CREATING}
		<div class="flex flex-col gap-1.5">
			<div
				aria-label="Upload progress"
				aria-valuemax={100}
				aria-valuemin={0}
				aria-valuenow={Math.round(fraction * 100)}
				class="h-1.5 w-full overflow-hidden rounded-full bg-muted"
				role="progressbar"
			>
				<div
					class={[
						'h-full rounded-full transition-[width] duration-300 ease-out',
						paused || needsFiles ? 'bg-muted-foreground/50' : 'bg-primary'
					]}
					style="width: {Math.round(fraction * 1000) / 10}%"
				></div>
			</div>

			<div
				class="flex flex-wrap items-center gap-x-3 gap-y-1 text-xs text-muted-foreground tabular-nums"
			>
				<span data-testid="upload-progress-text">{progressText}</span>

				{#if job.phase === AdminUploadJobPhase.QUEUED}
					<span class="flex items-center gap-1" data-testid="upload-queued" role="status">
						<Clock class="h-3 w-3 shrink-0" />

						{job.errorType === AdminErrorType.TOO_MANY_UPLOADS && job.error
							? `waiting: ${job.error}`
							: 'waiting for its turn'}
					</span>
				{:else if job.phase === AdminUploadJobPhase.CREATING}
					<span class="flex items-center gap-1">
						<Loader2 class="h-3 w-3 animate-spin" /> starting...
					</span>
				{/if}
			</div>

			{#if refusedFiles.length > 0}
				<div
					class="flex flex-col gap-2 rounded-md border border-destructive/40 bg-destructive/5 p-3 text-sm"
					data-testid="upload-refused"
					role="alert"
				>
					<p class="text-destructive">
						The server refused {refusedFiles.length === 1 ? 'a file' : 'some files'} of this upload. Drop
						{refusedFiles.length === 1 ? 'it' : 'them'} to go on with the rest, or cancel the upload.
					</p>

					<ul class="flex flex-col gap-2">
						{#each refusedFiles as file (file.index)}
							<li class="flex flex-wrap items-center gap-2" data-refused-file={file.index}>
								<span class="min-w-0 flex-1">
									<span class="font-mono text-xs">{file.name}</span>:
									{describeUploadRejection(file.code, file.message)}
								</span>

								<Button
									disabled={busy}
									onclick={() => act(() => adminUploadStore.dropFile(job.key, file.index))}
									size="sm"
									variant="outline"
								>
									Drop this file
								</Button>
							</li>
						{/each}
					</ul>
				</div>
			{:else if send && send.state !== AdminUploadSendState.UPLOADING && send.state !== AdminUploadSendState.IDLE && send.state !== AdminUploadSendState.DONE && send.state !== AdminUploadSendState.FILE_REJECTED}
				<p
					aria-live="polite"
					class={[
						'flex items-start gap-2 text-sm',
						send.state === AdminUploadSendState.PAUSED
							? 'text-muted-foreground'
							: 'text-amber-700 dark:text-amber-400'
					]}
					data-testid="upload-send-state"
					role="status"
				>
					<TriangleAlert class="mt-0.5 h-4 w-4 shrink-0" />

					<span>{describeSendState(send, now)}</span>
				</p>
			{:else if needsFiles}
				<p
					aria-live="polite"
					class="flex items-start gap-2 text-sm text-amber-700 dark:text-amber-400"
					data-testid="upload-needs-files"
					role="status"
				>
					<FolderOpen class="mt-0.5 h-4 w-4 shrink-0" />

					<span>
						Pick {job.files.filter((file) => !file.complete && !file.file).length === 1
							? 'the file'
							: 'the files'} again to continue:
						<span class="font-mono text-xs">
							{job.files
								.filter((file) => !file.complete && !file.file)
								.map((file) => file.name)
								.join(', ')}
						</span>
					</span>
				</p>
			{/if}
		</div>
	{:else if job.phase === AdminUploadJobPhase.CHECKING || job.phase === AdminUploadJobPhase.PUBLISHING}
		<div class="flex flex-col gap-1.5 text-sm" data-testid="upload-checking">
			<span class="flex items-center gap-2 text-muted-foreground">
				<Loader2 class="h-4 w-4 animate-spin" />

				{job.phase === AdminUploadJobPhase.PUBLISHING
					? 'Publishing into the models directory...'
					: (SUBSTATE_LABELS[session?.substate ?? 'queued'] ?? 'Checking...')}
			</span>

			{#if session?.progress && session.progress.total_bytes > 0}
				{@const percent = Math.min(
					100,
					Math.round((session.progress.done_bytes / session.progress.total_bytes) * 100)
				)}

				<div
					aria-label={job.phase === AdminUploadJobPhase.PUBLISHING
						? 'Publishing progress'
						: 'Check progress'}
					aria-valuemax={100}
					aria-valuemin={0}
					aria-valuenow={percent}
					class="h-1.5 w-full overflow-hidden rounded-full bg-muted"
					role="progressbar"
				>
					<div
						class="h-full rounded-full bg-primary transition-[width] duration-300"
						style="width: {percent}%"
					></div>
				</div>
			{/if}
		</div>
	{/if}

	{#if send?.proxyLimit}
		<Collapsible.Root bind:open={snippetOpen}>
			<div
				class="rounded-md border border-dashed p-2 text-xs text-muted-foreground"
				data-testid="upload-proxy-note"
			>
				<Collapsible.Trigger
					class="flex w-full items-center gap-1.5 text-left hover:text-foreground"
				>
					<ChevronDown
						class={['h-3.5 w-3.5 shrink-0 transition-transform', snippetOpen && 'rotate-180']}
					/>

					A proxy limits the request size; the upload uses smaller pieces ({formatBytes(
						send.proxyLimit
					)}).
				</Collapsible.Trigger>

				<Collapsible.Content>
					<p class="mt-2">
						For full speed, raise the limit for /admin/ in the proxy, for example in nginx:
					</p>

					<pre
						class="mt-1 overflow-x-auto rounded bg-muted p-2 font-mono text-[11px]">{ADMIN_UPLOAD_NGINX_SNIPPET}</pre>
				</Collapsible.Content>
			</div>
		</Collapsible.Root>
	{/if}

	{#if job.phase === AdminUploadJobPhase.READY && block}
		<div
			class="flex flex-col gap-2 rounded-md border border-amber-500/40 bg-amber-500/10 p-3 text-sm"
			data-block={session?.blocked_by}
			data-block-code={session?.blocked?.code ?? ''}
			data-testid="upload-block"
			role={session?.blocked_by === AdminUploadBlock.MANUAL ? 'status' : 'alert'}
		>
			<p class="font-medium text-amber-800 dark:text-amber-300">{block.title}</p>

			<p class="text-amber-800/90 dark:text-amber-300/90">{block.text}</p>

			{#if session?.blocked_by === AdminUploadBlock.SHA256_MISMATCH}
				<ul class="flex flex-col gap-2">
					{#each mismatches as file (file.index)}
						<li class="flex flex-col gap-1 text-xs">
							<span class="font-mono">{file.name}</span>

							<span class="font-mono break-all">expected {file.sha256_expected}</span>

							<span class="font-mono break-all">uploaded {file.sha256}</span>

							<div class="flex flex-wrap items-center gap-2">
								<Input
									class="h-8 max-w-md font-mono text-xs"
									oninput={(event) =>
										(shaEdits = { ...shaEdits, [file.index]: event.currentTarget.value })}
									placeholder="corrected SHA-256 (empty: no check)"
									value={shaEdits[file.index] ?? ''}
								/>

								<Button
									disabled={busy}
									onclick={() =>
										act(async () => {
											await adminUploadStore.setExpectedSha(
												job.key,
												file.index,
												(shaEdits[file.index] ?? '').trim()
											);
											await adminUploadStore.publish(job.key);
										})}
									size="sm"
									variant="outline"
								>
									Save and check again
								</Button>
							</div>
						</li>
					{/each}
				</ul>
			{/if}

			{#if session?.blocked_by === AdminUploadBlock.NAME_TAKEN}
				<div class="flex flex-wrap items-center gap-2">
					<Input
						bind:value={renameValue}
						aria-invalid={renameValue && renameProblem ? 'true' : undefined}
						class="h-8 max-w-xs font-mono text-xs"
						data-testid="upload-rename"
						placeholder="new name"
						spellcheck={false}
					/>

					<Button
						disabled={busy || renameProblem !== null}
						onclick={() =>
							act(async () => {
								await adminUploadStore.rename(job.key, renameValue.trim());
								await adminUploadStore.publish(job.key);
							})}
						size="sm"
					>
						Rename and publish
					</Button>
				</div>

				{#if renameValue && renameProblem}
					<p class="text-xs text-destructive">{describeModelNameProblem(renameProblem)}</p>
				{/if}
			{/if}

			{#if nameLimit !== null && nameLimit > 0}
				<div class="flex flex-wrap items-center gap-2">
					<Input
						bind:value={renameValue}
						aria-invalid={renameValue && limitedRenameProblem ? 'true' : undefined}
						aria-label="New name"
						class="h-8 max-w-xs font-mono text-xs"
						data-testid="upload-rename"
						maxlength={nameLimit}
						placeholder="new name, at most {nameLimit} characters"
						spellcheck={false}
					/>

					<Button
						disabled={busy || limitedRenameProblem !== null}
						onclick={() =>
							act(async () => {
								await adminUploadStore.rename(job.key, renameValue.trim());
								// Retry: the checks run again with the shorter name
								await adminUploadStore.publish(job.key);
							})}
						size="sm"
					>
						Rename and check again
					</Button>
				</div>

				{#if renameValue && limitedRenameProblem}
					<p class="text-xs text-destructive">
						{limitedRenameProblem === 'too_long_here'
							? `The name may be at most ${nameLimit} characters long on this server.`
							: describeModelNameProblem(limitedRenameProblem)}
					</p>
				{/if}
			{/if}
		</div>
	{/if}

	{#if job.phase === AdminUploadJobPhase.REJECTED}
		<p class="flex items-start gap-2 text-sm text-destructive" data-testid="upload-rejected">
			<CircleAlert class="mt-0.5 h-4 w-4 shrink-0" />

			<span>
				{describeUploadRejection(session?.error?.code ?? '', session?.error?.message)}
				{#if session?.error?.file !== null && session?.error?.file !== undefined}
					<span class="font-mono text-xs">
						({job.files.find((file) => file.index === session?.error?.file)?.name ??
							`file ${session.error.file}`})
					</span>
				{/if}
			</span>
		</p>
	{/if}

	{#if job.phase === AdminUploadJobPhase.FAILED && job.error}
		<p class="flex items-start gap-2 text-sm text-destructive">
			<CircleAlert class="mt-0.5 h-4 w-4 shrink-0" />

			{job.error}
		</p>

		{#if job.errorType === 'name_taken' || job.errorType === 'invalid_name'}
			<div class="flex flex-wrap items-center gap-2">
				<Input
					bind:value={renameValue}
					class="h-8 max-w-xs font-mono text-xs"
					placeholder="new name"
					spellcheck={false}
				/>

				<Button
					disabled={renameProblem !== null}
					onclick={() => adminUploadStore.retryCreate(job.key, renameValue.trim())}
					size="sm"
				>
					Try again with this name
				</Button>
			</div>
		{/if}
	{/if}

	{#if job.phase === AdminUploadJobPhase.PUBLISHED && session?.result}
		<div class="flex flex-col gap-2 text-sm" data-testid="upload-result">
			<p class="flex items-start gap-2 text-green-700 dark:text-green-400">
				<CircleCheck class="mt-0.5 h-4 w-4 shrink-0" />

				<span>
					Published as <span class="font-mono">{model}</span>: it is in the model list, not loaded
					yet.
				</span>
			</p>

			<ul class="flex flex-col divide-y rounded-md border text-xs">
				{#each session.result.files as file (file.name)}
					<li class="flex flex-col gap-0.5 px-3 py-2">
						<div class="flex items-center gap-2">
							<span class="min-w-0 flex-1 truncate font-mono" title={file.name}>{file.name}</span>

							<Badge class="font-normal" variant="outline">{file.role}</Badge>

							<span class="text-muted-foreground tabular-nums">{formatBytes(file.size)}</span>
						</div>

						<div class="flex items-center gap-1 text-muted-foreground">
							<span class="min-w-0 flex-1 truncate font-mono select-all" title={file.sha256}>
								SHA-256 {file.sha256}
							</span>

							<button
								aria-label="Copy the SHA-256"
								class="rounded p-1 hover:bg-muted"
								onclick={() => copy(file.sha256)}
								type="button"
							>
								{#if copied === file.sha256}
									<Check class="h-3 w-3" />
								{:else}
									<Copy class="h-3 w-3" />
								{/if}
							</button>
						</div>
					</li>
				{/each}
			</ul>

			{#if job.placement}
				<p class="text-xs text-muted-foreground" data-testid="upload-fit">
					{#if job.fitRunning}
						Checking the fit of the placement with llama.cpp...
					{:else if fitVerdict}
						Fit of the placement (checked by llama.cpp): {ADMIN_FIT_VERDICT_LABELS[fitVerdict]}.
					{:else if job.fit && !job.fit.ok}
						The fit check refused the settings: {job.fit.error?.message ?? 'see the settings'}.
					{:else if job.fitError}
						The fit could not be checked: {job.fitError}
					{/if}
				</p>
			{/if}
		</div>
	{/if}

	{#if actionError}
		<p class="text-sm text-destructive" role="alert">{actionError}</p>
	{/if}

	<div class="flex flex-wrap items-center gap-2">
		{#if job.phase === AdminUploadJobPhase.UPLOADING}
			{#if needsFiles}
				<Button disabled={busy} onclick={() => picker?.click()} size="sm">
					<FolderOpen />

					Pick the file{job.files.filter((file) => !file.complete && !file.file).length === 1
						? ''
						: 's'}
				</Button>
			{:else if send?.state === AdminUploadSendState.OTHER_CLIENT}
				<Button onclick={() => adminUploadStore.takeover(job.key)} size="sm">Take over</Button>
			{:else if paused || send?.state === AdminUploadSendState.DISK_FULL || send?.state === AdminUploadSendState.FAILED}
				<Button onclick={() => adminUploadStore.resume(job.key)} size="sm">
					<Play />

					Resume
				</Button>
			{:else if send?.state === AdminUploadSendState.RETRYING || send?.state === AdminUploadSendState.UNREACHABLE || send?.state === AdminUploadSendState.SERVER_RESTARTING}
				<Button onclick={() => adminUploadStore.resume(job.key)} size="sm" variant="outline">
					<RotateCcw />

					Retry now
				</Button>
			{/if}

			{#if sending}
				<Button onclick={() => adminUploadStore.pause(job.key)} size="sm" variant="outline">
					<Pause />

					Pause
				</Button>
			{/if}
		{/if}

		{#if job.phase === AdminUploadJobPhase.READY}
			{#if session?.blocked_by === AdminUploadBlock.MANUAL || session?.blocked_by === AdminUploadBlock.PUBLISH_ERROR}
				<Button
					disabled={busy}
					onclick={() => act(() => adminUploadStore.publish(job.key))}
					size="sm"
				>
					Publish
				</Button>
			{:else if session?.blocked_by === AdminUploadBlock.CHECK_FAILED && nameLimit === null}
				<!-- a path_too_long waits for a rename (above): the same name would be refused again -->
				<Button
					disabled={busy}
					onclick={() => act(() => adminUploadStore.publish(job.key))}
					size="sm"
				>
					<RotateCcw />

					Retry the checks
				</Button>
			{:else if session?.blocked_by === AdminUploadBlock.SHA256_MISMATCH}
				<Button
					disabled={busy}
					onclick={() =>
						act(() => adminUploadStore.publish(job.key, { ignoreSha256Mismatch: true }))}
					size="sm"
					variant="outline"
				>
					Publish anyway
				</Button>
			{:else if session?.blocked_by === AdminUploadBlock.SETTINGS_NOT_SAVED || session?.blocked_by === AdminUploadBlock.SETTINGS_INVALID}
				<Button
					disabled={busy}
					onclick={() => act(() => adminUploadStore.publish(job.key, { withoutSettings: true }))}
					size="sm"
				>
					Publish without the placement
				</Button>

				{#if session?.blocked_by === AdminUploadBlock.SETTINGS_NOT_SAVED}
					<Button
						disabled={busy}
						onclick={() => act(() => adminUploadStore.publish(job.key))}
						size="sm"
						variant="outline"
					>
						Retry
					</Button>
				{/if}
			{/if}
		{/if}

		{#if job.phase === AdminUploadJobPhase.PUBLISHED}
			{#if onOpenSettings && adminStore.hasFeature(AdminFeature.MODEL_SETTINGS)}
				<Button onclick={() => onOpenSettings?.(model)} size="sm" variant="outline">
					<SlidersHorizontal />

					Open settings
				</Button>
			{/if}

			<Button onclick={() => adminStore.loadModel(model)} size="sm" variant="outline">
				<Power />

				Load now
			</Button>
		{/if}

		{#if job.phase === AdminUploadJobPhase.FAILED && job.errorType !== 'name_taken' && job.errorType !== 'invalid_name'}
			<Button onclick={() => adminUploadStore.retryCreate(job.key)} size="sm" variant="outline">
				<RotateCcw />

				Try again
			</Button>
		{/if}

		<span class="ml-auto"></span>

		{#if job.phase === AdminUploadJobPhase.UPLOADING || job.phase === AdminUploadJobPhase.READY || job.phase === AdminUploadJobPhase.QUEUED || job.phase === AdminUploadJobPhase.CHECKING}
			<Button
				class="text-muted-foreground hover:text-destructive"
				disabled={busy}
				onclick={() => (job.sessionId ? (confirmCancel = true) : adminUploadStore.cancel(job.key))}
				size="sm"
				variant="ghost"
			>
				<Trash2 />

				{job.sessionId ? 'Cancel' : 'Remove'}
			</Button>
		{:else if job.phase !== AdminUploadJobPhase.CREATING && job.phase !== AdminUploadJobPhase.PUBLISHING}
			<Button onclick={() => adminUploadStore.dismiss(job.key)} size="sm" variant="ghost">
				<X />

				Dismiss
			</Button>
		{/if}
	</div>

	{#if job.files.length > 0 && job.phase !== AdminUploadJobPhase.PUBLISHED}
		<Collapsible.Root bind:open={filesOpen}>
			<Collapsible.Trigger
				class="flex items-center gap-1.5 text-xs text-muted-foreground hover:text-foreground"
			>
				<ChevronDown class={['h-3.5 w-3.5 transition-transform', filesOpen && 'rotate-180']} />

				{job.files.length} file{job.files.length === 1 ? '' : 's'}
			</Collapsible.Trigger>

			<Collapsible.Content>
				<ul class="mt-2 flex flex-col gap-2 text-xs" data-testid="upload-job-files">
					{#each job.files as file (file.index)}
						{@const remote = session?.files?.find((item) => item.index === file.index)}

						<li class="flex flex-col gap-1">
							<div class="flex items-center gap-2">
								<span class="min-w-0 flex-1 truncate font-mono" title={file.name}>{file.name}</span>

								<span class="text-muted-foreground tabular-nums">
									{formatBytes(Math.min(file.offset, file.size))} / {formatBytes(file.size)}
								</span>
							</div>

							<div class="h-1 w-full overflow-hidden rounded-full bg-muted">
								<div
									class="h-full rounded-full bg-primary/70"
									style="width: {file.size > 0
										? Math.round((Math.min(file.offset, file.size) / file.size) * 100)
										: 0}%"
								></div>
							</div>

							{#if remote}
								<span class="text-muted-foreground" data-header-state={remote.header.state}>
									{#if remote.dropped}
										dropped from the upload
									{:else if remote.error}
										<span class="text-destructive"
											>{describeUploadRejection(remote.error.code, remote.error.message)}</span
										>
									{:else if remote.header.state === 'ok' || remote.header.state === 'warning'}
										header checked: {[remote.header.arch, remote.role]
											.filter(Boolean)
											.join(', ')}{remote.header.split
											? `, part ${remote.header.split.no + 1} of ${remote.header.split.count}`
											: ''}{remote.header.message ? ` (${remote.header.message})` : ''}
									{:else if remote.header.state === 'queued' || remote.header.state === 'scanning'}
										checking the header...
									{:else if remote.header.state === 'failed' || remote.header.state === 'error'}
										<span class="text-destructive"
											>{remote.header.message ?? 'the header check failed'}</span
										>
									{/if}
								</span>
							{/if}
						</li>
					{/each}
				</ul>
			</Collapsible.Content>
		</Collapsible.Root>
	{/if}

	<input
		bind:this={picker}
		accept=".gguf"
		class="hidden"
		multiple
		onchange={onPicked}
		type="file"
	/>
</article>

<DialogConfirmation
	cancelText="Keep uploading"
	confirmText="Cancel the upload"
	description={`The uploaded part of "${job.name}" (${formatBytes(confirmed)}) is deleted on the server. This cannot be undone.`}
	icon={Trash2}
	onCancel={() => (confirmCancel = false)}
	onConfirm={() => {
		confirmCancel = false;
		void adminUploadStore.cancel(job.key);
	}}
	open={confirmCancel}
	title="Cancel this upload?"
	variant="destructive"
/>
