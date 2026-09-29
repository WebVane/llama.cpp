<script lang="ts">
	import { KeyRound, ShieldAlert } from '@lucide/svelte';
	import { DialogLicenseActivate } from '$lib/components/app';
	import * as Alert from '$lib/components/ui/alert';
	import { alertVariants } from '$lib/components/ui/alert';
	import { ICON_CLASS_DEFAULT } from '$lib/constants';
	import { licenseStore } from '$lib/stores';
	import { onMount } from 'svelte';

	let showDialog = $state(false);

	let status = $derived(licenseStore.status);
	let remaining = $derived(licenseStore.remaining);
	// Shown while the budget is limited (trial) or requests are refused; a
	// licensed server with a healthy license stays quiet.
	let visible = $derived(licenseStore.isEnabled && (licenseStore.isLocked || licenseStore.isTrial));

	onMount(() => licenseStore.startPolling());
</script>

{#snippet activateButton()}
	<button
		class={[
			'flex items-center gap-1.5 rounded-lg px-2 py-1 text-xs font-medium',
			licenseStore.isLocked
				? 'bg-destructive/20 hover:bg-destructive/30'
				: 'bg-foreground/10 hover:bg-foreground/20'
		]}
		onclick={() => (showDialog = true)}
		type="button"
	>
		<KeyRound class="h-3 w-3" />
		Activate
	</button>
{/snippet}

{#if visible && status}
	<div class="pointer-events-auto mx-auto mb-4 max-w-[48rem] px-1">
		{#if licenseStore.isLocked}
			<Alert.Root variant="destructive">
				<ShieldAlert class={ICON_CLASS_DEFAULT} />

				<Alert.Title class="flex items-center justify-between gap-2">
					<span>License required</span>

					{@render activateButton()}
				</Alert.Title>

				<Alert.Description>
					{#if status.mode === 'trial'}
						The trial budget of {status.limit} requests is used up. Activate a license to continue.
					{:else if status.status === 'expired'}
						The license has expired. Renew it and activate again.
					{:else}
						Requests are refused{status.reason ? ` (${status.reason})` : ''}. Activate a license to
						continue.
					{/if}
				</Alert.Description>
			</Alert.Root>
		{:else}
			<!-- informational, changes after every request: a plain region, not a
			     role="alert" live region that would re-announce each counter tick -->
			<div aria-live="off" class={alertVariants({ variant: 'default' })} role="status">
				<KeyRound class={ICON_CLASS_DEFAULT} />

				<Alert.Title class="flex items-center justify-between gap-2">
					<span>Trial: {status.used} of {status.limit} requests used</span>

					{@render activateButton()}
				</Alert.Title>

				<Alert.Description>
					{#if remaining !== null}
						{remaining} request{remaining === 1 ? '' : 's'} left before activation is required.
					{/if}
				</Alert.Description>
			</div>
		{/if}
	</div>
{/if}

<DialogLicenseActivate bind:open={showDialog} />
