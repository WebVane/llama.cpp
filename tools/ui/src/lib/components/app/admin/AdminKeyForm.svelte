<script lang="ts">
	import { KeyRound, Loader2, TriangleAlert } from '@lucide/svelte';
	import * as Alert from '$lib/components/ui/alert';
	import { Button } from '$lib/components/ui/button';
	import { Checkbox } from '$lib/components/ui/checkbox';
	import { Input } from '$lib/components/ui/input';
	import { Label } from '$lib/components/ui/label';
	import { ICON_CLASS_DEFAULT } from '$lib/constants';
	import { adminStore } from '$lib/stores';
	import { onMount, tick } from 'svelte';

	interface Props {
		/** The key was accepted; the form is about to be replaced by the panel */
		onUnlocked?: () => void;
	}

	let { onUnlocked }: Props = $props();

	let key = $state('');
	let remember = $state(adminStore.isKeyRemembered());
	let keyInput = $state<HTMLInputElement | null>(null);

	let canSubmit = $derived(key.trim().length > 0 && !adminStore.checkingKey);

	async function handleSubmit(event: SubmitEvent) {
		event.preventDefault();

		if (!canSubmit) return;

		if (await adminStore.submitKey(key, remember)) {
			key = '';
			onUnlocked?.();

			return;
		}

		// the input was disabled while the key was checked, which dropped the focus
		await tick();
		keyInput?.focus();
	}

	// the form replaces the whole panel (page opened, Lock, key rejected): the key is the only thing to do here
	onMount(() => {
		keyInput?.focus();
	});
</script>

<div class="mx-auto w-full max-w-md rounded-xl border bg-card p-6 shadow-sm">
	<div class="mb-4 flex items-center gap-3">
		<div class="flex size-9 shrink-0 items-center justify-center rounded-lg bg-muted">
			<KeyRound class={ICON_CLASS_DEFAULT} />
		</div>

		<div>
			<h2 class="text-base font-semibold">Admin key required</h2>

			<p class="text-sm text-muted-foreground">The settings of this server are protected.</p>
		</div>
	</div>

	{#if !adminStore.isKeyConfigured}
		<Alert.Root variant="destructive">
			<TriangleAlert class={ICON_CLASS_DEFAULT} />

			<Alert.Title>The server has no admin key</Alert.Title>

			<Alert.Description>
				It could not read or create one (its admin state directory is not writable). Set
				PROVISION_ADMIN_KEY or fix the state directory, restart the server and try again.
			</Alert.Description>
		</Alert.Root>

		<Button class="mt-4" onclick={() => adminStore.retry()} size="sm" variant="secondary">
			Check again
		</Button>
	{:else}
		<form class="space-y-4" onsubmit={handleSubmit}>
			<div class="space-y-2">
				<Label for="admin-key">Admin key</Label>

				<Input
					bind:ref={keyInput}
					bind:value={key}
					aria-describedby="admin-key-help"
					aria-invalid={adminStore.keyError ? 'true' : undefined}
					autocomplete="off"
					class="font-mono"
					disabled={adminStore.checkingKey}
					id="admin-key"
					placeholder="Paste the admin key"
					spellcheck={false}
					type="password"
				/>

				{#if adminStore.keyError}
					<p class="text-xs text-destructive" role="alert">{adminStore.keyError}</p>
				{/if}

				<p class="text-xs text-muted-foreground" id="admin-key-help">
					The key comes from PROVISION_ADMIN_KEY or PROVISION_ADMIN_KEY_FILE; without them the
					server generates one into admin.key in its admin state directory.
				</p>
			</div>

			<div class="flex items-start gap-2">
				<Checkbox
					bind:checked={remember}
					class="mt-0.5"
					disabled={adminStore.checkingKey}
					id="admin-key-remember"
				/>

				<div class="space-y-1">
					<Label class="font-normal" for="admin-key-remember">Remember on this device</Label>

					<p class="text-xs text-muted-foreground">
						Off: the key is kept for this browser tab only. On: it is also saved in this browser's
						local storage.
					</p>
				</div>
			</div>

			<Button class="w-full" disabled={!canSubmit} type="submit">
				{#if adminStore.checkingKey}
					<Loader2 class="{ICON_CLASS_DEFAULT} animate-spin" />
					Checking...
				{:else}
					<KeyRound class={ICON_CLASS_DEFAULT} />
					Unlock
				{/if}
			</Button>
		</form>
	{/if}
</div>
