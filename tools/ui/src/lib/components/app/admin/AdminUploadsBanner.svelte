<script lang="ts">
	import { CircleAlert, Eye, FolderOpen, Trash2, Upload } from '@lucide/svelte';
	import { DialogConfirmation } from '$lib/components/app/dialogs';
	import { Button } from '$lib/components/ui/button';
	import {
		AdminAddModelTab,
		AdminUploadJobPhase,
		AdminUploadSendState,
		AdminUploadState
	} from '$lib/enums/admin.enums';
	import { adminUploadStore } from '$lib/stores/admin-upload.svelte';
	import type { ApiAdminUploadSummary } from '$lib/types/admin-upload';
	import { formatBytes } from '$lib/utils/admin';
	import { describeExpiry, describeUploadBlock } from '$lib/utils/admin-upload';

	interface Props {
		/** Inside the upload dialog: only the sessions, no summary of the running jobs */
		compact?: boolean;
	}

	let { compact = false }: Props = $props();

	let picker = $state<HTMLInputElement | null>(null);
	let resumeId = $state<string | null>(null);
	let cancelTarget = $state<ApiAdminUploadSummary | null>(null);
	let message = $state<{ error: boolean; text: string } | null>(null);
	let now = $state(Date.now());

	let sessions = $derived(adminUploadStore.orphanSessions);
	let running = $derived(
		adminUploadStore.jobs.filter(
			(job) =>
				job.phase === AdminUploadJobPhase.UPLOADING ||
				job.phase === AdminUploadJobPhase.QUEUED ||
				job.phase === AdminUploadJobPhase.CHECKING ||
				job.phase === AdminUploadJobPhase.PUBLISHING ||
				job.phase === AdminUploadJobPhase.READY
		)
	);
	let diskFull = $derived(
		running.filter((job) => job.send?.state === AdminUploadSendState.DISK_FULL)
	);

	$effect(() => {
		const timer = setInterval(() => (now = Date.now()), 30_000);

		return () => clearInterval(timer);
	});

	function stateText(session: ApiAdminUploadSummary): string {
		switch (session.state) {
			case AdminUploadState.RECEIVING:
				return `${formatBytes(session.received_bytes)} of ${formatBytes(session.total_bytes)} uploaded`;
			case AdminUploadState.CHECKING:
				return 'being checked';
			case AdminUploadState.PUBLISHING:
				return 'being published';
			case AdminUploadState.READY:
				return describeUploadBlock(session.blocked_by, session.blocked).title;
			default:
				return session.state;
		}
	}

	function pickFor(session: ApiAdminUploadSummary) {
		resumeId = session.id;
		message = null;
		picker?.click();
	}

	async function onPicked(event: Event) {
		const input = event.currentTarget as HTMLInputElement;
		const files = [...(input.files ?? [])];
		const id = resumeId;

		input.value = '';
		resumeId = null;

		if (!id || files.length === 0) return;

		const report = await adminUploadStore.resumeSession(id, files);

		if (!report) return;

		if (report.mismatched.length > 0) {
			message = {
				error: true,
				text: `This is not the same file: ${report.mismatched.join(', ')} differs from the part already uploaded. Pick the file that was uploaded before.`
			};
		} else if (report.matched.length === 0) {
			message = {
				error: true,
				text: 'None of the picked files belongs to this upload (the name and the size must be the same).'
			};
		} else {
			message = {
				error: false,
				text: report.missing.length
					? `Continuing; still missing: ${report.missing.join(', ')}.`
					: 'Continuing the upload.'
			};

			if (!compact) adminUploadStore.openDialog(AdminAddModelTab.UPLOAD);
		}
	}

	async function show(session: ApiAdminUploadSummary) {
		const key = await adminUploadStore.follow(session.id);

		if (key && !compact) adminUploadStore.openDialog(AdminAddModelTab.UPLOAD);
	}
</script>

{#if sessions.length > 0 || (!compact && (running.length > 0 || diskFull.length > 0)) || message}
	<div class="flex flex-col gap-2" data-testid="upload-banner">
		{#if !compact && diskFull.length > 0}
			<div
				class="flex flex-wrap items-center gap-2 rounded-md border border-destructive/40 bg-destructive/10 px-3 py-2 text-sm text-destructive"
				data-testid="upload-disk-full"
			>
				<CircleAlert class="h-4 w-4 shrink-0" />

				<span class="min-w-0 flex-1">
					Upload of {diskFull.map((job) => job.name).join(', ')} paused: the disk of the models directory
					is full. Free space, then resume; cancelling frees the uploaded part.
				</span>

				<Button
					onclick={() => adminUploadStore.openDialog(AdminAddModelTab.UPLOAD)}
					size="sm"
					variant="outline"
				>
					Show
				</Button>
			</div>
		{/if}

		{#if !compact && running.length > 0}
			<div
				class="flex flex-wrap items-center gap-2 rounded-md border bg-muted/40 px-3 py-2 text-sm"
				data-testid="upload-running"
			>
				<Upload class="h-4 w-4 shrink-0 text-muted-foreground" />

				<span class="min-w-0 flex-1">
					{running.length === 1 ? `Upload: ${running[0].name}` : `${running.length} uploads`}
					{#if adminUploadStore.totals.total > 0}
						<span class="text-muted-foreground tabular-nums">
							· {Math.floor(
								(adminUploadStore.totals.confirmed / adminUploadStore.totals.total) * 100
							)}%
						</span>
					{/if}
				</span>

				<Button
					onclick={() => adminUploadStore.openDialog(AdminAddModelTab.UPLOAD)}
					size="sm"
					variant="outline"
				>
					<Eye />

					Show
				</Button>
			</div>
		{/if}

		{#if sessions.length > 0}
			<div
				class="flex flex-col gap-2 rounded-md border border-amber-500/40 bg-amber-500/10 px-3 py-2 text-sm"
				data-testid="upload-unfinished"
			>
				<p class="font-medium text-amber-800 dark:text-amber-300">
					{sessions.length} unfinished upload{sessions.length === 1 ? '' : 's'}
				</p>

				<ul class="flex flex-col gap-2">
					{#each sessions as session (session.id)}
						<li class="flex flex-wrap items-center gap-x-3 gap-y-1" data-session={session.id}>
							<span class="min-w-0 font-mono text-xs break-all">{session.name}</span>

							<span class="text-xs text-muted-foreground tabular-nums">
								{stateText(session)}{session.expires_at
									? ` · ${describeExpiry(session.expires_at, now)}`
									: ''}
							</span>

							<span class="ml-auto flex flex-wrap gap-1">
								{#if session.state === AdminUploadState.RECEIVING}
									<Button onclick={() => pickFor(session)} size="sm" variant="outline">
										<FolderOpen />

										Resume (pick the same files)
									</Button>
								{:else}
									<Button onclick={() => show(session)} size="sm" variant="outline">
										<Eye />

										Show
									</Button>
								{/if}

								{#if session.state !== AdminUploadState.PUBLISHING}
									<Button
										class="text-muted-foreground hover:text-destructive"
										onclick={() => (cancelTarget = session)}
										size="sm"
										variant="ghost"
									>
										<Trash2 />

										Cancel
									</Button>
								{/if}
							</span>
						</li>
					{/each}
				</ul>
			</div>
		{/if}

		{#if message}
			<p
				class={['text-sm', message.error ? 'text-destructive' : 'text-muted-foreground']}
				data-testid="upload-resume-message"
				role="status"
			>
				{message.text}
			</p>
		{/if}
	</div>
{/if}

<input
	bind:this={picker}
	accept=".gguf"
	class="hidden"
	data-testid="upload-resume-input"
	multiple
	onchange={onPicked}
	type="file"
/>

<DialogConfirmation
	cancelText="Keep it"
	confirmText="Cancel the upload"
	description={cancelTarget
		? `The uploaded part of "${cancelTarget.name}" (${formatBytes(cancelTarget.received_bytes)}) is deleted on the server.`
		: ''}
	icon={Trash2}
	onCancel={() => (cancelTarget = null)}
	onConfirm={() => {
		const target = cancelTarget;

		cancelTarget = null;

		if (target) void adminUploadStore.cancelSession(target.id);
	}}
	open={cancelTarget !== null}
	title="Cancel this upload?"
	variant="destructive"
/>
