<script lang="ts">
	import AdminGpuCard from './AdminGpuCard.svelte';
	import { Cpu, Info, Loader2, RefreshCw, ScanSearch, TriangleAlert } from '@lucide/svelte';
	import { DialogConfirmation } from '$lib/components/app/dialogs';
	import { Button } from '$lib/components/ui/button';
	import { adminStore } from '$lib/stores';
	import { describeGpuBackend, describeProbeState, formatAge, formatBytes } from '$lib/utils';
	import { onMount } from 'svelte';

	interface Props {
		onOpenModel?: (id: string) => void;
	}

	let { onOpenModel }: Props = $props();

	let confirmRescan = $state(false);
	// ticks so "updated N s ago" stays true without new data
	let now = $state(Date.now());

	let gpus = $derived(adminStore.gpus);
	let backendState = $derived(gpus ? describeGpuBackend(gpus) : null);
	let probeText = $derived(gpus && gpus.gpus.length > 0 ? describeProbeState(gpus) : null);
	let updatedText = $derived(
		adminStore.gpusLoadedAt ? `updated ${formatAge(now - adminStore.gpusLoadedAt)} ago` : ''
	);
	let gridClass = $derived(
		(gpus?.gpus.length ?? 0) >= 3
			? 'sm:grid-cols-2 xl:grid-cols-4'
			: (gpus?.gpus.length ?? 0) === 2
				? 'sm:grid-cols-2'
				: 'sm:grid-cols-1 md:max-w-xl'
	);

	onMount(() => {
		const timer = setInterval(() => (now = Date.now()), 5_000);

		return () => clearInterval(timer);
	});

	async function rescan() {
		confirmRescan = false;
		await adminStore.rescanGpus();
	}
</script>

<section aria-label="GPUs" class="flex flex-col gap-2" data-testid="admin-gpus">
	<div class="flex flex-wrap items-center gap-x-3 gap-y-1">
		<h2 class="text-sm font-medium">GPUs</h2>

		{#if gpus}
			<span class="text-xs text-muted-foreground tabular-nums">
				{gpus.gpus.length === 0
					? 'no GPU devices'
					: `${gpus.gpus.length} device${gpus.gpus.length === 1 ? '' : 's'}`}{updatedText
					? ` · ${updatedText}`
					: ''}
			</span>
		{/if}

		<div class="ml-auto flex items-center gap-1">
			<Button
				disabled={adminStore.gpusLoading}
				onclick={() => adminStore.loadGpus(true)}
				size="sm"
				title="Read the GPU memory again"
				variant="ghost"
			>
				<RefreshCw class={adminStore.gpusLoading ? 'animate-spin' : ''} />

				<span class="hidden sm:inline">Refresh</span>
			</Button>

			<Button
				disabled={adminStore.gpusRescanning}
				onclick={() => (confirmRescan = true)}
				size="sm"
				title="Detect the GPUs of this server again"
				variant="ghost"
			>
				{#if adminStore.gpusRescanning}
					<Loader2 class="animate-spin" />
				{:else}
					<ScanSearch />
				{/if}

				Re-detect GPUs
			</Button>
		</div>
	</div>

	{#if !gpus}
		<div class="rounded-lg border border-dashed p-4 text-sm text-muted-foreground">
			{#if adminStore.gpusError}
				<span class="text-destructive">GPU information unavailable: {adminStore.gpusError}</span>
			{:else}
				<span class="inline-flex items-center gap-2">
					<Loader2 class="h-4 w-4 animate-spin" />

					Reading the GPUs...
				</span>
			{/if}
		</div>
	{:else}
		{#if backendState}
			<div
				class={[
					'flex items-start gap-2 rounded-md border px-3 py-2 text-sm',
					backendState.level === 'info'
						? 'border-border bg-muted/40'
						: 'border-amber-500/40 bg-amber-500/10 text-amber-700 dark:text-amber-400'
				]}
				data-gpu-state={gpus.gpu_backend?.state}
				role="status"
			>
				{#if backendState.level === 'info'}
					<Cpu class="mt-0.5 h-4 w-4 shrink-0" />
				{:else}
					<TriangleAlert class="mt-0.5 h-4 w-4 shrink-0" />
				{/if}

				<span class="min-w-0">
					<span class="font-medium">{backendState.title}.</span>

					{backendState.text}
				</span>
			</div>
		{/if}

		{#if probeText}
			<p
				class="flex items-start gap-2 text-xs text-muted-foreground"
				data-testid="admin-gpus-probe"
			>
				<Info class="mt-0.5 h-3.5 w-3.5 shrink-0" />

				<span>{probeText}</span>
			</p>
		{/if}

		{#if gpus.gpus.length > 0}
			<div class={['grid gap-3', gridClass]}>
				{#each gpus.gpus as gpu (gpu.device)}
					<AdminGpuCard {gpu} {onOpenModel} vendorRowsRead={gpus.probe?.ok ?? false} />
				{/each}
			</div>
		{/if}

		{#if gpus.unmatched.length > 0}
			<div class="rounded-md border border-dashed p-2 text-xs text-muted-foreground">
				<p class="mb-1 font-medium text-foreground">
					Memory readings not matched to a device of this server:
				</p>

				<ul class="space-y-0.5">
					{#each gpus.unmatched as row, index (`${row.tool}-${row.pci_bus_id ?? index}`)}
						<li class="tabular-nums">
							{row.tool} #{row.index ?? '?'} · {row.name ?? 'GPU'}{row.pci_bus_id
								? ` · PCI ${row.pci_bus_id}`
								: ''}{row.total
								? ` · ${formatBytes(row.free ?? 0)} free of ${formatBytes(row.total)}`
								: ''}
						</li>
					{/each}
				</ul>

				<p class="mt-1">
					A GPU hidden from the server (for example by CUDA_VISIBLE_DEVICES or the container's
					device list) shows up here.
				</p>
			</div>
		{/if}
	{/if}
</section>

<DialogConfirmation
	confirmText="Re-detect"
	description="The server starts a short helper that looks at every GPU again. It briefly needs a little memory on each GPU; running models are not affected."
	icon={ScanSearch}
	onCancel={() => (confirmRescan = false)}
	onConfirm={rescan}
	open={confirmRescan}
	title="Re-detect GPUs?"
/>
