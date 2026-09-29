<script lang="ts">
	import { KeyRound } from '@lucide/svelte';
	import { LicenseActivationForm } from '$lib/components/app';
	import * as Dialog from '$lib/components/ui/dialog';

	interface Props {
		open: boolean;
		onOpenChange?: (open: boolean) => void;
	}

	let { onOpenChange, open = $bindable() }: Props = $props();

	function handleOpenChange(newOpen: boolean) {
		open = newOpen;
		onOpenChange?.(newOpen);
	}
</script>

<Dialog.Root onOpenChange={handleOpenChange} {open}>
	<Dialog.Content class="max-w-lg">
		<Dialog.Header>
			<Dialog.Title class="flex items-center gap-2">
				<KeyRound class="h-5 w-5" />
				Activate license
			</Dialog.Title>

			<Dialog.Description>
				Unlock the server with the activation code from your license. Status and activation are also
				available under Settings → License.
			</Dialog.Description>
		</Dialog.Header>

		<LicenseActivationForm onActivated={() => handleOpenChange(false)} />
	</Dialog.Content>
</Dialog.Root>
