<script lang="ts">
	import AdminModelSettingsForm from '$lib/components/app/admin/AdminModelSettingsForm.svelte';
	import type {
		AdminModelSettingsDraft,
		ApiAdminModelSettingsRow,
		ApiAdminSettingSchema
	} from '$lib/types';
	import { buildModelSettingsUpdate, createModelSettingsDraft } from '$lib/utils';
	import { untrack } from 'svelte';

	interface Props {
		devices: string[];
		row: ApiAdminModelSettingsRow;
		schema: ApiAdminSettingSchema[];
	}

	let { devices, row, schema }: Props = $props();

	let draft = $state<AdminModelSettingsDraft>(untrack(() => createModelSettingsDraft(row)));
	let update = $derived(buildModelSettingsUpdate(draft, row));
</script>

<!--
	Drives AdminModelSettingsForm with its own draft and shows the POST body the
	draft would produce, so the client test can read it back.
-->
<AdminModelSettingsForm {devices} {draft} onDraftChange={(next) => (draft = next)} {row} {schema} />

<pre data-testid="draft-update">{JSON.stringify(update)}</pre>
