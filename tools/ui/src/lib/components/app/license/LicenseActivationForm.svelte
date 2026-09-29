<script lang="ts">
	import { CircleCheck, KeyRound, Loader2, ShieldAlert } from '@lucide/svelte';
	import { Badge } from '$lib/components/ui/badge';
	import { Button } from '$lib/components/ui/button';
	import { Input } from '$lib/components/ui/input';
	import { Label } from '$lib/components/ui/label';
	import { ICON_CLASS_DEFAULT } from '$lib/constants';
	import { licenseStore } from '$lib/stores';
	import { onMount } from 'svelte';
	import { toast } from 'svelte-sonner';

	interface Props {
		/** Called after a successful activation (e.g. to close a dialog). */
		onActivated?: () => void;
	}

	let { onActivated }: Props = $props();

	let code = $state('');

	let status = $derived(licenseStore.status);
	let remaining = $derived(licenseStore.remaining);
	let canSubmit = $derived(code.trim().length > 0 && !licenseStore.activating);

	const modeLabel: Record<string, string> = {
		disabled: 'Unrestricted build',
		none: 'No license',
		offline: 'Licensed (offline)',
		online: 'Licensed (online)',
		trial: 'Trial'
	};

	function formatDate(iso?: string): string {
		if (!iso) return '';

		const date = new Date(iso);

		return Number.isNaN(date.getTime()) ? iso : date.toLocaleDateString();
	}

	async function handleActivate() {
		if (!canSubmit) return;

		const ok = await licenseStore.activate(code);

		if (ok) {
			code = '';
			toast.success('License activated');
			onActivated?.();
		} else {
			toast.error(licenseStore.error ?? 'Activation failed');
		}
	}

	function handleKeydown(event: KeyboardEvent) {
		if (event.key === 'Enter') {
			event.preventDefault();
			handleActivate();
		}
	}

	onMount(() => licenseStore.startPolling());
</script>

<div class="space-y-4">
	{#if status}
		<div
			class={[
				'rounded-lg border px-4 py-3 text-sm',
				licenseStore.isLocked
					? 'border-destructive/40 bg-destructive/10'
					: licenseStore.isTrial
						? 'border-amber-500/40 bg-amber-500/10'
						: 'border-border bg-muted/40'
			]}
		>
			<div class="flex items-center justify-between gap-2">
				<span class="flex items-center gap-2 font-medium">
					{#if licenseStore.isLocked}
						<ShieldAlert class="{ICON_CLASS_DEFAULT} text-destructive" />
					{:else if licenseStore.isValid && !licenseStore.isTrial}
						<CircleCheck class="{ICON_CLASS_DEFAULT} text-green-600 dark:text-green-400" />
					{:else}
						<KeyRound class={ICON_CLASS_DEFAULT} />
					{/if}

					{modeLabel[status.mode] ?? status.mode}
				</span>

				<Badge variant={licenseStore.isValid ? 'secondary' : 'destructive'}>
					{status.status}
				</Badge>
			</div>

			<div class="mt-2 space-y-1 text-xs text-muted-foreground">
				{#if status.limit > 0}
					<p>
						Requests used: <span class="font-medium text-foreground">{status.used}</span>
						of {status.limit}
						{#if remaining !== null}
							({remaining} left)
						{/if}
					</p>
				{:else if status.limit < 0 && licenseStore.isValid}
					<p>Unlimited requests</p>
				{/if}

				{#if status.product || status.tariff}
					<p>
						{status.product ?? ''}{status.product && status.tariff ? ' · ' : ''}{status.tariff ??
							''}
					</p>
				{/if}

				{#if status.expiresAt}
					<p>Expires: {formatDate(status.expiresAt)}</p>
				{/if}

				{#if status.windowResetsAt}
					<p>Budget resets: {formatDate(status.windowResetsAt)}</p>
				{/if}

				{#if licenseStore.isLocked && status.reason}
					<p class="text-destructive">Requests are refused: {status.reason}</p>
				{/if}
			</div>
		</div>
	{:else if licenseStore.disabled}
		<p class="text-sm text-muted-foreground">
			This server has no license subsystem — nothing to activate.
		</p>
	{:else if licenseStore.unavailable}
		<p class="flex items-center gap-2 text-sm text-muted-foreground">
			<Loader2 class="{ICON_CLASS_DEFAULT} animate-spin" />
			License status unavailable ({licenseStore.unavailable}), retrying...
		</p>
	{:else}
		<p class="flex items-center gap-2 text-sm text-muted-foreground">
			<Loader2 class="{ICON_CLASS_DEFAULT} animate-spin" />
			Checking license status...
		</p>
	{/if}

	{#if licenseStore.isEnabled}
		<div class="space-y-2">
			<p class="text-sm text-muted-foreground" id="license-activation-help">
				Enter the activation code issued for this product. The server contacts the license server
				once and stores the signed license locally.
			</p>

			<Label class="sr-only" for="license-activation-code">Activation code</Label>

			<div class="flex gap-2">
				<Input
					bind:value={code}
					aria-describedby="license-activation-help"
					autocomplete="off"
					class="font-mono"
					disabled={licenseStore.activating}
					id="license-activation-code"
					onkeydown={handleKeydown}
					placeholder="XXXX-XXXX-XXXX"
					spellcheck={false}
				/>

				<Button disabled={!canSubmit} onclick={handleActivate}>
					{#if licenseStore.activating}
						<Loader2 class="{ICON_CLASS_DEFAULT} animate-spin" />
						Activating...
					{:else}
						<KeyRound class={ICON_CLASS_DEFAULT} />
						Activate
					{/if}
				</Button>
			</div>

			{#if licenseStore.error}
				<p class="text-xs text-destructive">{licenseStore.error}</p>
			{/if}
		</div>
	{/if}
</div>
