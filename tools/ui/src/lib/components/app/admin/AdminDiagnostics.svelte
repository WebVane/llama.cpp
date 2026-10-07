<script lang="ts">
	import {
		CircleAlert,
		CircleCheck,
		Copy,
		Info,
		Loader2,
		RefreshCw,
		TriangleAlert
	} from '@lucide/svelte';
	import { Button } from '$lib/components/ui/button';
	import { AdminDiagnosticStatus } from '$lib/enums';
	import { adminStore } from '$lib/stores';
	import { copyToClipboard, formatDiagnosticsReport, summarizeDiagnosticValue } from '$lib/utils';
	import type { Component } from 'svelte';

	const STATUS_ICONS: Record<string, { class: string; icon: Component }> = {
		[AdminDiagnosticStatus.ERROR]: { class: 'text-destructive', icon: CircleAlert },
		[AdminDiagnosticStatus.INFO]: { class: 'text-muted-foreground', icon: Info },
		[AdminDiagnosticStatus.OK]: { class: 'text-green-600 dark:text-green-400', icon: CircleCheck },
		[AdminDiagnosticStatus.WARNING]: {
			class: 'text-amber-600 dark:text-amber-400',
			icon: TriangleAlert
		}
	};

	let diagnostics = $derived(adminStore.diagnostics);
	let problems = $derived(
		(diagnostics?.items ?? []).filter(
			(item) =>
				item.status === AdminDiagnosticStatus.ERROR || item.status === AdminDiagnosticStatus.WARNING
		).length
	);

	function copyReport() {
		if (!diagnostics) return;

		void copyToClipboard(
			formatDiagnosticsReport(diagnostics, [`page: ${location.origin}${location.pathname}`]),
			'Diagnostics report copied'
		);
	}
</script>

<aside
	aria-label="Diagnostics"
	class="h-fit rounded-lg border bg-card p-4"
	data-testid="admin-diagnostics"
>
	<div class="mb-3 flex items-center gap-2">
		<h3 class="text-sm font-medium">Diagnostics</h3>

		{#if diagnostics}
			<span class="text-xs text-muted-foreground">
				{problems === 0 ? 'no problems found' : `${problems} to look at`}
			</span>
		{/if}

		<Button
			aria-label="Check again"
			class="ml-auto"
			disabled={adminStore.diagnosticsLoading}
			onclick={() => adminStore.loadDiagnostics()}
			size="icon-sm"
			title="Check again"
			variant="ghost"
		>
			<RefreshCw class={adminStore.diagnosticsLoading ? 'animate-spin' : ''} />
		</Button>
	</div>

	{#if !diagnostics}
		<p class="text-xs text-muted-foreground">
			{#if adminStore.diagnosticsError}
				<span class="text-destructive">Diagnostics unavailable: {adminStore.diagnosticsError}</span>
			{:else}
				<span class="inline-flex items-center gap-2">
					<Loader2 class="h-3.5 w-3.5 animate-spin" />

					Checking the server...
				</span>
			{/if}
		</p>
	{:else}
		<ul class="space-y-2.5 text-xs">
			{#each diagnostics.items as item (item.id)}
				{@const style = STATUS_ICONS[item.status] ?? STATUS_ICONS[AdminDiagnosticStatus.INFO]}
				{@const StatusIcon = style.icon}
				{@const summary = summarizeDiagnosticValue(item)}

				<li class="flex items-start gap-2" data-diagnostic={item.id} data-status={item.status}>
					<StatusIcon class={['mt-0.5 h-3.5 w-3.5 shrink-0', style.class]} />

					<div class="min-w-0 flex-1">
						<div class="flex flex-wrap items-baseline gap-x-2">
							<span class="text-muted-foreground">{item.label}</span>

							<span class="break-all">{summary}</span>
						</div>

						{#if item.hint && item.hint !== summary}
							<p class="text-muted-foreground/80">{item.hint}</p>
						{/if}
					</div>
				</li>
			{/each}
		</ul>

		<Button class="mt-3 w-full" onclick={copyReport} size="sm" variant="outline">
			<Copy />

			Copy report
		</Button>

		<p class="mt-2 text-[11px] text-muted-foreground">
			What this server detected about itself at runtime, with a hint for each problem. The copied
			report is the same as text and holds no keys.
		</p>
	{/if}
</aside>
