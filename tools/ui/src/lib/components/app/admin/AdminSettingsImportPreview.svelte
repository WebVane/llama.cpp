<script lang="ts">
	import { TriangleAlert } from '@lucide/svelte';
	import { Checkbox } from '$lib/components/ui/checkbox';
	import { ADMIN_SETTING_LABELS } from '$lib/constants';
	import { AdminImportIssue, AdminSettingKey } from '$lib/enums';
	import type { AdminImportPreview } from '$lib/types';
	import { describeImportIssue, describeSettingValue } from '$lib/utils';

	interface Props {
		preview: AdminImportPreview;
		modelId: string;
		/** The operator confirmed that the settings of another model may be used */
		confirmed: boolean;
		hasGpus: boolean;
		nCtxTrain: number | null;
		onConfirmedChange: (confirmed: boolean) => void;
		onToggle: (key: string, selected: boolean) => void;
	}

	let { confirmed, hasGpus, modelId, nCtxTrain, onConfirmedChange, onToggle, preview }: Props =
		$props();

	function label(key: string): string {
		return ADMIN_SETTING_LABELS[key as AdminSettingKey] ?? key;
	}
</script>

<div class="flex flex-col gap-3" data-testid="admin-import-preview">
	<p class="text-sm text-muted-foreground">
		Settings from the file{preview.exportedAt
			? `, exported ${new Date(preview.exportedAt).toLocaleString()}`
			: ''}. Nothing is saved yet: the ticked values are loaded into the form, and you save them
		there as usual.
	</p>

	{#if preview.differentModel}
		<div
			class="flex flex-col gap-2 rounded-md border border-amber-500/40 bg-amber-500/10 p-3 text-sm text-amber-700 dark:text-amber-400"
			data-testid="admin-import-different"
			role="alert"
		>
			<span class="flex items-start gap-2 font-medium">
				<TriangleAlert class="mt-0.5 h-4 w-4 shrink-0" />

				These settings were made for a different model:
			</span>

			<ul class="list-disc pl-8 text-xs">
				{#each preview.differences as difference (difference)}
					<li>{difference}</li>
				{/each}
			</ul>

			<label class="flex items-center gap-2 text-xs text-foreground">
				<Checkbox
					checked={confirmed}
					onCheckedChange={(value) => onConfirmedChange(value === true)}
				/>

				Use them for {modelId} anyway
			</label>
		</div>
	{/if}

	{#if preview.fields.length === 0}
		<p class="rounded-md border border-dashed p-3 text-center text-sm text-muted-foreground">
			The file sets no per-model setting.
		</p>
	{:else}
		<ul class="divide-y rounded-md border">
			{#each preview.fields as field (field.key)}
				<li class="flex items-start gap-3 px-3 py-2 text-sm" data-import-key={field.key}>
					<Checkbox
						aria-label="Import {label(field.key)}"
						checked={field.selected}
						class="mt-0.5"
						disabled={field.skipped}
						onCheckedChange={(value) => onToggle(field.key, value === true)}
					/>

					<div class="flex min-w-0 flex-1 flex-col gap-0.5">
						<span class="font-medium">{label(field.key)}</span>

						<span class="text-xs text-muted-foreground tabular-nums">
							{describeSettingValue(field.key, field.current, { hasGpus })}
							<span aria-hidden="true">→</span>

							<span class="font-medium text-foreground">
								{describeSettingValue(field.key, field.value, { hasGpus })}
							</span>
						</span>

						{#each field.issues as issue (issue)}
							<span
								class={[
									'text-xs',
									issue === AdminImportIssue.CTX_ABOVE_TRAIN
										? 'text-amber-600 dark:text-amber-400'
										: 'text-destructive'
								]}
							>
								{describeImportIssue(issue, nCtxTrain)}
							</span>
						{/each}
					</div>
				</li>
			{/each}
		</ul>
	{/if}

	{#if preview.unknownKeys.length > 0}
		<p class="text-xs text-muted-foreground">
			Ignored, not per-model settings: <span class="font-mono"
				>{preview.unknownKeys.join(', ')}</span
			>
		</p>
	{/if}
</div>
