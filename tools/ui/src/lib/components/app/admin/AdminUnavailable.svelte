<script lang="ts">
	import { RefreshCw, ShieldOff } from '@lucide/svelte';
	import { Button } from '$lib/components/ui/button';
	import * as Empty from '$lib/components/ui/empty';
	import { ADMIN_UNAVAILABLE_TITLE, ROUTES } from '$lib/constants';
	import { adminStore } from '$lib/stores';
</script>

<Empty.Root class="mx-auto max-w-xl border">
	<Empty.Header>
		<Empty.Media variant="icon">
			<ShieldOff />
		</Empty.Media>

		<Empty.Title>{ADMIN_UNAVAILABLE_TITLE}</Empty.Title>

		<Empty.Description>
			{#if adminStore.probeError}
				<span class="block">{adminStore.probeError}</span>
			{/if}

			<span class="mt-2 block">
				The admin panel exists only on a router-mode server started with PROVISION_ADMIN=1, and only
				for addresses that are allowed to reach /admin.
			</span>
		</Empty.Description>
	</Empty.Header>

	<Empty.Content>
		<div class="flex flex-wrap items-center justify-center gap-2">
			<Button onclick={() => adminStore.retry()} size="sm">
				<RefreshCw />

				Retry
			</Button>

			<Button href={ROUTES.START} size="sm" variant="ghost">Back to chat</Button>
		</div>
	</Empty.Content>
</Empty.Root>
