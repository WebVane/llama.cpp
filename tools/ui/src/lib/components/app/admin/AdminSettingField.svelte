<script lang="ts">
	import { Lock, TriangleAlert, X } from '@lucide/svelte';
	import { Badge } from '$lib/components/ui/badge';
	import { ADMIN_LOCK_TEXTS } from '$lib/constants';
	import { AdminLockSource, AdminSettingOrigin } from '$lib/enums';
	import type { ApiAdminSettingField } from '$lib/types';
	import { describeSettingOrigin } from '$lib/utils';
	import type { Snippet } from 'svelte';

	interface Props {
		/** The control (select, input...) */
		children: Snippet;
		/** The field as the server reports it */
		field: ApiAdminSettingField;
		/** Element id of the control, for the label */
		forId: string;
		/** Where the value with the draft comes from */
		origin: string;
		label: string;
		/** Validation or server message for this field */
		error?: string | null;
		help?: string | null;
		/** Text of the value the model gets without a value set here */
		inheritedText?: string;
		/** The draft sets a value here (shows the clear button) */
		isSet?: boolean;
		/** Formatted saved value that is not applied */
		inactiveText?: string | null;
		onClear?: () => void;
		/** Remove a saved value the server does not apply (locked / missing device) */
		onRemoveInactive?: () => void;
		/** Extra content under the control (quick picks, notes) */
		extra?: Snippet;
	}

	let {
		children,
		error = null,
		extra,
		field,
		forId,
		help = null,
		inactiveText = null,
		inheritedText = '',
		isSet = false,
		label,
		onClear,
		onRemoveInactive,
		origin
	}: Props = $props();

	let lockText = $derived(
		field.locked
			? (ADMIN_LOCK_TEXTS[field.locked.by as AdminLockSource] ?? `Fixed by ${field.locked.by}.`)
			: null
	);
	let originText = $derived(
		origin === AdminSettingOrigin.ADMIN ? 'custom' : `from ${describeSettingOrigin(origin)}`
	);
</script>

<div class="flex flex-col gap-1.5" data-field={forId}>
	<div class="flex flex-wrap items-center gap-x-2 gap-y-1">
		<label class="text-sm font-medium" for={forId}>{label}</label>

		<Badge
			class={[
				'font-normal',
				origin === AdminSettingOrigin.ADMIN && 'border-primary/40 text-primary',
				field.locked && 'gap-1'
			]}
			data-origin={origin}
			variant="outline"
		>
			{#if field.locked}
				<Lock class="h-3 w-3" />
			{/if}

			{originText}
		</Badge>

		{#if isSet && !field.locked && onClear}
			<button
				aria-label="Use the inherited value for {label}"
				class="inline-flex items-center gap-1 rounded px-1 text-xs text-muted-foreground hover:text-foreground"
				onclick={onClear}
				title="Back to {inheritedText}"
				type="button"
			>
				<X class="h-3 w-3" />

				<span class="hidden sm:inline">back to {inheritedText}</span>
			</button>
		{/if}
	</div>

	{@render children()}

	{#if extra}
		{@render extra()}
	{/if}

	{#if lockText}
		<p class="flex items-start gap-1.5 text-xs text-muted-foreground" data-testid="field-lock">
			<Lock class="mt-0.5 h-3 w-3 shrink-0" />

			<span>{lockText}</span>
		</p>
	{/if}

	{#if inactiveText}
		<p class="flex flex-wrap items-start gap-1.5 text-xs text-amber-600 dark:text-amber-400">
			<TriangleAlert class="mt-0.5 h-3 w-3 shrink-0" />

			<span class="min-w-0 flex-1">{inactiveText}</span>

			{#if onRemoveInactive}
				<button class="underline underline-offset-2" onclick={onRemoveInactive} type="button">
					Remove it
				</button>
			{/if}
		</p>
	{/if}

	{#if error}
		<p class="text-xs text-destructive" role="alert">{error}</p>
	{/if}

	{#if help}
		<p class="text-xs text-muted-foreground">{help}</p>
	{/if}
</div>
