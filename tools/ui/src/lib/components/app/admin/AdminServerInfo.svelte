<script lang="ts">
	import { adminStore } from '$lib/stores';

	const KEY_SOURCE_LABELS: Record<string, string> = {
		env: 'PROVISION_ADMIN_KEY',
		file: 'PROVISION_ADMIN_KEY_FILE',
		generated: 'admin.key in the state directory'
	};

	let status = $derived(adminStore.status);
	let rows = $derived.by(() => {
		if (!status) return [];

		const download = status.download;

		return [
			{
				label: 'Admin key from',
				mono: false,
				value: status.key_source ? (KEY_SOURCE_LABELS[status.key_source] ?? status.key_source) : ''
			},
			{ label: 'State directory', mono: true, value: status.state?.dir ?? '' },
			{ label: 'Model cache', mono: true, value: status.dirs?.cache ?? '' },
			{ label: 'Models directory', mono: true, value: status.dirs?.models_dir ?? '' },
			{ label: 'Preset file', mono: true, value: status.dirs?.preset ?? '' },
			{
				label: 'Downloads',
				mono: false,
				value: !download
					? ''
					: download.available
						? 'available'
						: download.offline
							? 'off (offline mode)'
							: 'off (no HTTPS support)'
			}
		].filter((row) => row.value !== '');
	});
</script>

{#if rows.length > 0}
	<aside aria-label="Server" class="h-fit rounded-lg border bg-card p-4">
		<h3 class="mb-3 text-sm font-medium">Server</h3>

		<dl class="space-y-2.5 text-xs">
			{#each rows as row (row.label)}
				<div>
					<dt class="text-muted-foreground">{row.label}</dt>

					<dd class={['break-all', row.mono && 'font-mono']}>{row.value}</dd>
				</div>
			{/each}
		</dl>
	</aside>
{/if}
