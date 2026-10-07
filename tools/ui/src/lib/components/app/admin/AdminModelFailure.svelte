<script lang="ts">
	import { ChevronDown, CircleAlert, Copy } from '@lucide/svelte';
	import { Button } from '$lib/components/ui/button';
	import * as Collapsible from '$lib/components/ui/collapsible';
	import { ADMIN_RESTART_RISK_NOTE } from '$lib/constants';
	import { AdminFeature, ServerModelSource } from '$lib/enums';
	import { adminStore, modelsStore } from '$lib/stores';
	import type { ApiAdminFailureReason } from '$lib/types';
	import { copyToClipboard, describeFailureReason } from '$lib/utils';
	import type { Snippet } from 'svelte';

	interface Props {
		modelId: string;
		reason: ApiAdminFailureReason | null;
		/** Output tail of the failed instance, ANSI-free */
		lines: string[];
		truncated?: boolean;
		exitCode?: number | null;
		/** When the failure happened (ISO) */
		time?: string | null;
		/** Show the "requests keep retrying" note */
		showRisk?: boolean;
		/** Open the output tail right away */
		openLog?: boolean;
		/** Buttons under the panel */
		actions?: Snippet;
	}

	let {
		actions,
		exitCode = null,
		lines,
		modelId,
		openLog = false,
		reason,
		showRisk = true,
		time = null,
		truncated = false
	}: Props = $props();

	// a models_dir model: "Delete files..." can remove only its projector, which the hint names
	let projectorRemovable = $derived(
		adminStore.hasFeature(AdminFeature.DELETE) &&
			modelsStore.routerModels.some(
				(model) => model.id === modelId && model.source === ServerModelSource.MODELS_DIR
			)
	);
	let text = $derived(describeFailureReason(reason, { lines, projectorRemovable }));
	let logOpen = $state(false);

	$effect(() => {
		if (openLog) logOpen = true;
	});

	function copyLog() {
		void copyToClipboard(lines.join('\n'), 'Output copied');
	}
</script>

<div
	class="flex flex-col gap-3"
	data-reason={reason?.code ?? 'unknown'}
	data-testid="admin-failure"
>
	<div
		class="flex items-start gap-2 rounded-md border border-destructive/40 bg-destructive/10 p-3 text-sm text-destructive"
		role="alert"
	>
		<CircleAlert class="mt-0.5 h-4 w-4 shrink-0" />

		<div class="flex min-w-0 flex-col gap-1">
			<span class="font-medium">
				{modelId} did not start: {text.label}
			</span>

			<span class="break-words text-foreground/90">{text.message}</span>

			{#if text.hint}
				<span class="text-foreground/80">What to try: {text.hint}</span>
			{/if}

			{#if reason?.line}
				<code class="font-mono text-xs break-all text-foreground/70">{reason.line}</code>
			{/if}

			<span class="text-xs text-foreground/60">
				{#if exitCode !== null}Exit code {exitCode}.{/if}

				{#if time}At {new Date(time).toLocaleString()}.{/if}
			</span>
		</div>
	</div>

	{#if showRisk}
		<p class="text-xs text-muted-foreground">{ADMIN_RESTART_RISK_NOTE}</p>
	{/if}

	<Collapsible.Root bind:open={logOpen}>
		<div class="flex items-center gap-2">
			<Collapsible.Trigger
				class="inline-flex items-center gap-1 text-sm font-medium hover:underline"
				data-testid="admin-failure-log-toggle"
			>
				<ChevronDown class={['h-4 w-4 transition-transform', logOpen && 'rotate-180']} />

				Output of the model ({lines.length} line{lines.length === 1 ? '' : 's'}{truncated
					? ', last part'
					: ''})
			</Collapsible.Trigger>

			<Button
				aria-label="Copy the output"
				class="ml-auto"
				disabled={lines.length === 0}
				onclick={copyLog}
				size="sm"
				variant="ghost"
			>
				<Copy />

				Copy
			</Button>
		</div>

		<Collapsible.Content>
			<pre
				class="mt-2 max-h-72 overflow-auto rounded-md border bg-muted/40 p-2 font-mono text-[11px] leading-relaxed whitespace-pre-wrap break-all"
				data-testid="admin-failure-log">{lines.length > 0
					? lines.join('\n')
					: '(no output was captured)'}</pre>
		</Collapsible.Content>
	</Collapsible.Root>

	{#if actions}
		<div class="flex flex-wrap items-center gap-2">
			{@render actions()}
		</div>
	{/if}
</div>
