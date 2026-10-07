<script lang="ts">
	import { Copy } from '@lucide/svelte';
	import { Button } from '$lib/components/ui/button';
	import type { AdminAuditDetailRow, ApiAdminAuditEntry } from '$lib/types/admin-audit';
	import {
		auditActorRows,
		auditDetailRows,
		auditRecordRows,
		formatAuditRaw
	} from '$lib/utils/admin-audit';
	import { copyToClipboard } from '$lib/utils/clipboard';

	interface Props {
		entry: ApiAdminAuditEntry;
	}

	let { entry }: Props = $props();

	let details = $derived(auditDetailRows(entry));
	let actor = $derived(auditActorRows(entry.actor));
	let record = $derived(auditRecordRows(entry));
	let raw = $derived(formatAuditRaw(entry));
</script>

<!-- the titles are plain text, not headings: a table row has no place in the page's outline -->
{#snippet rowList(title: string, rows: AdminAuditDetailRow[], testid: string)}
	<section class="min-w-0" data-testid={testid}>
		<p class="mb-1.5 text-xs font-medium text-muted-foreground">{title}</p>

		<dl class="grid grid-cols-[minmax(6rem,auto)_minmax(0,1fr)] gap-x-3 gap-y-1 text-xs">
			{#each rows as row, index (index)}
				<dt class="text-muted-foreground">{row.label}</dt>

				<dd class={['min-w-0 break-words', row.mono && 'font-mono']}>
					{#if row.list && row.list.length > 0}
						<ul class="space-y-0.5">
							{#each row.list as item, itemIndex (itemIndex)}
								<li class="break-all">{item}</li>
							{/each}
						</ul>
					{:else}
						{row.value}
					{/if}
				</dd>
			{/each}
		</dl>
	</section>
{/snippet}

<div class="flex flex-col gap-4 whitespace-normal" data-testid="audit-details">
	<div class="grid gap-4 md:grid-cols-3">
		{#if details.length > 0}
			{@render rowList('Details', details, 'audit-details-fields')}
		{:else}
			<section class="min-w-0" data-testid="audit-details-fields">
				<p class="mb-1.5 text-xs font-medium text-muted-foreground">Details</p>

				<p class="text-xs text-muted-foreground">No further details.</p>
			</section>
		{/if}

		{@render rowList('Who', actor, 'audit-details-actor')}

		{@render rowList('Record', record, 'audit-details-record')}
	</div>

	<details class="group text-xs">
		<summary
			class="w-fit cursor-pointer text-muted-foreground select-none hover:text-foreground"
			data-testid="audit-raw-toggle"
		>
			Raw record (JSON)
		</summary>

		<div class="mt-2 flex flex-col gap-2">
			<pre
				class="max-h-72 overflow-auto rounded-md border bg-muted/40 p-2 font-mono text-[11px] leading-relaxed whitespace-pre-wrap break-all"
				data-testid="audit-raw">{raw}</pre>

			<Button
				class="w-fit"
				onclick={() => void copyToClipboard(raw, 'Record copied')}
				size="sm"
				variant="outline"
			>
				<Copy />

				Copy record
			</Button>
		</div>
	</details>
</div>
