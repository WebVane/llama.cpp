<script lang="ts">
	import { Loader2, RefreshCw, ScrollText } from '@lucide/svelte';
	// direct import, not the admin barrel: the admin components import the dialogs barrel
	import AdminModelFailure from '$lib/components/app/admin/AdminModelFailure.svelte';
	import { Button } from '$lib/components/ui/button';
	import * as Dialog from '$lib/components/ui/dialog';
	import { AdminFeature } from '$lib/enums';
	import { adminStore } from '$lib/stores';
	import type { ApiAdminModelLog } from '$lib/types';
	import { isInstanceAfterFailure } from '$lib/utils';
	import { untrack } from 'svelte';

	interface Props {
		open: boolean;
		modelId: string | null;
		onOpenChange?: (open: boolean) => void;
		/** Open the settings of the model (e.g. to change what made it fail) */
		onOpenSettings?: (id: string) => void;
	}

	let { modelId, onOpenChange, onOpenSettings, open = $bindable() }: Props = $props();

	// the dialog element: it takes the focus on open (see onOpenAutoFocus)
	let contentNode = $state<HTMLElement | null>(null);

	let log = $state<ApiAdminModelLog | null>(null);
	let loading = $state(false);
	let error = $state<string | null>(null);
	let run = 0;

	let failure = $derived(log?.last_failure ?? null);
	// the current instance shows its own output when it is a later start than the failure
	// (a running healthy one has no reason); a start in the same second is shown as well
	let current = $derived(log && isInstanceAfterFailure(log.instance, failure) ? log : null);

	$effect(() => {
		const id = modelId;

		if (!open || !id) return;

		untrack(() => void load(id));
	});

	async function load(id: string) {
		const mine = ++run;

		loading = true;
		error = null;

		try {
			const answer = await adminStore.loadModelLog(id);

			if (mine === run) log = answer;
		} catch (err) {
			if (mine === run) error = err instanceof Error ? err.message : String(err);
		} finally {
			if (mine === run) loading = false;
		}
	}

	function handleOpenChange(value: boolean) {
		if (!value) {
			run++;
			log = null;
			error = null;
		}

		open = value;
		onOpenChange?.(value);
	}
</script>

<Dialog.Root onOpenChange={handleOpenChange} {open}>
	<!-- the dialog itself takes the focus on open, not the close button (no focus ring on it) -->
	<Dialog.Content
		bind:ref={contentNode}
		class="flex flex-col gap-4 outline-none max-md:h-[100dvh]! max-md:w-screen! max-md:max-w-none! max-md:rounded-none! md:max-h-[90dvh]! md:w-[calc(100vw-4rem)]! md:max-w-3xl!"
		data-testid="admin-model-log"
		onOpenAutoFocus={(event) => {
			event.preventDefault();
			contentNode?.focus();
		}}
		tabindex={-1}
	>
		<Dialog.Header class="text-left">
			<Dialog.Title class="flex items-center gap-2 pr-6">
				<ScrollText class="h-5 w-5" />

				Output of {modelId}
			</Dialog.Title>

			<Dialog.Description>
				The last lines the model printed, kept in memory by the server (admin only).
			</Dialog.Description>
		</Dialog.Header>

		<div class="-mx-6 min-h-0 flex-1 overflow-y-auto px-6">
			{#if loading && !log}
				<p class="flex items-center gap-2 py-8 text-sm text-muted-foreground">
					<Loader2 class="h-4 w-4 animate-spin" />

					Reading the output...
				</p>
			{:else if error}
				<p class="py-8 text-sm text-destructive">The output could not be read: {error}</p>
			{:else if log}
				<div class="flex flex-col gap-5">
					{#if failure}
						<section aria-label="Last failed start" class="flex flex-col gap-2">
							<h3 class="text-sm font-medium">Last failed start</h3>

							<AdminModelFailure
								exitCode={failure.exit_code}
								lines={failure.lines}
								modelId={log.model}
								openLog
								reason={failure.reason}
								showRisk={false}
								time={failure.time}
								truncated={failure.truncated}
							/>

							{#if failure.settings_reverted}
								<p class="text-sm text-amber-600 dark:text-amber-400">
									The server put back the settings that worked before.
								</p>
							{/if}
						</section>
					{/if}

					{#if current?.instance}
						<section aria-label="Current instance" class="flex flex-col gap-2">
							<h3 class="text-sm font-medium">
								{current.instance.running ? 'Running instance' : 'Last instance'} (started
								{new Date(current.instance.started).toLocaleString()}, {current.instance
									.lines_total} lines)
							</h3>

							<pre
								class="max-h-72 overflow-auto rounded-md border bg-muted/40 p-2 font-mono text-[11px] leading-relaxed whitespace-pre-wrap break-all"
								data-testid="admin-model-log-lines">{current.lines.length > 0
									? current.lines.join('\n')
									: '(no output yet)'}</pre>
						</section>
					{:else if !failure}
						<p class="py-6 text-center text-sm text-muted-foreground">
							This model has not run since the server started.
						</p>
					{/if}
				</div>
			{/if}
		</div>

		<div class="flex flex-wrap items-center gap-2 border-t pt-3">
			<Button
				disabled={loading || !modelId}
				onclick={() => modelId && load(modelId)}
				size="sm"
				variant="ghost"
			>
				<RefreshCw class={loading ? 'animate-spin' : ''} />

				Refresh
			</Button>

			{#if onOpenSettings && modelId && adminStore.hasFeature(AdminFeature.MODEL_SETTINGS)}
				<Button
					class="ml-auto"
					onclick={() => {
						const id = modelId;

						handleOpenChange(false);

						if (id) onOpenSettings?.(id);
					}}
					size="sm"
					variant="outline"
				>
					Open settings
				</Button>
			{/if}
		</div>
	</Dialog.Content>
</Dialog.Root>
