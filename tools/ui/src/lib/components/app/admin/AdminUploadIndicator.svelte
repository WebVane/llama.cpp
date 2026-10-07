<script lang="ts">
	import { CircleAlert, CircleCheck, Loader2, Upload } from '@lucide/svelte';
	import { Button } from '$lib/components/ui/button';
	import {
		AdminAddModelTab,
		AdminUploadJobPhase,
		AdminUploadSendState
	} from '$lib/enums/admin.enums';
	import { adminUploadStore } from '$lib/stores/admin-upload.svelte';

	let active = $derived(adminUploadStore.activeJobs);
	let totals = $derived(adminUploadStore.totals);
	let attention = $derived(
		active.filter(
			(job) =>
				job.phase === AdminUploadJobPhase.READY ||
				(job.send !== null &&
					job.send.state !== AdminUploadSendState.UPLOADING &&
					job.send.state !== AdminUploadSendState.DONE &&
					job.send.state !== AdminUploadSendState.IDLE &&
					job.send.state !== AdminUploadSendState.RETRYING)
		).length
	);
	let checking = $derived(
		active.filter(
			(job) =>
				job.phase === AdminUploadJobPhase.CHECKING || job.phase === AdminUploadJobPhase.PUBLISHING
		).length
	);
	let label = $derived.by(() => {
		if (totals.sending > 0 && totals.total > 0) {
			return `Uploading ${Math.floor((totals.confirmed / totals.total) * 100)}%`;
		}

		if (checking > 0) return 'Checking upload';

		if (attention > 0) return `${attention} upload${attention === 1 ? '' : 's'} waiting`;

		return `${active.length} upload${active.length === 1 ? '' : 's'}`;
	});
</script>

{#if active.length > 0}
	<Button
		class="gap-1.5 font-normal tabular-nums"
		data-testid="upload-indicator"
		onclick={() => adminUploadStore.openDialog(AdminAddModelTab.UPLOAD)}
		size="sm"
		title="Show the uploads"
		variant="outline"
	>
		{#if attention > 0}
			<CircleAlert class="text-amber-600 dark:text-amber-400" />
		{:else if totals.sending > 0}
			<Upload />
		{:else if checking > 0}
			<Loader2 class="animate-spin" />
		{:else}
			<CircleCheck />
		{/if}

		{label}
	</Button>
{/if}
