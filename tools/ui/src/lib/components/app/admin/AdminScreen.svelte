<script lang="ts">
	import AdminDiagnostics from './AdminDiagnostics.svelte';
	import AdminKeyForm from './AdminKeyForm.svelte';
	import AdminModelsTab from './AdminModelsTab.svelte';
	import AdminServerInfo from './AdminServerInfo.svelte';
	import AdminSettingsTab from './AdminSettingsTab.svelte';
	import AdminUnavailable from './AdminUnavailable.svelte';
	import AdminUploadIndicator from './AdminUploadIndicator.svelte';
	import { Loader2, Lock, ShieldCheck, TriangleAlert } from '@lucide/svelte';
	import { goto } from '$app/navigation';
	import * as Alert from '$lib/components/ui/alert';
	import { Badge } from '$lib/components/ui/badge';
	import { Button } from '$lib/components/ui/button';
	import { ADMIN_TABS, ICON_CLASS_DEFAULT, ROUTES } from '$lib/constants';
	import { AdminFeature, AdminProbeState, AdminTab, KeyboardKey } from '$lib/enums';
	import { adminStore, modelsStore } from '$lib/stores';
	import {
		countRunningModels,
		describeModelWarning,
		isDownloadPlaceholderStatus
	} from '$lib/utils';
	import { onDestroy, onMount, tick, untrack } from 'svelte';

	let activeTab = $state<AdminTab>(AdminTab.MODELS);
	let tabButtons = $state<Partial<Record<AdminTab, HTMLButtonElement>>>({});

	let isChecking = $derived(
		adminStore.probeState === AdminProbeState.IDLE ||
			adminStore.probeState === AdminProbeState.CHECKING
	);
	let runningCount = $derived(countRunningModels(modelsStore.routerModels));
	let modelCount = $derived(
		modelsStore.routerModels.filter((model) => !isDownloadPlaceholderStatus(model.status?.value))
			.length
	);
	let modelsMax = $derived(
		adminStore.settings?.models_max ?? adminStore.status?.models?.max ?? null
	);
	let stateInfo = $derived(adminStore.status?.state);
	let statusWarnings = $derived(adminStore.isAuthorized ? (adminStore.status?.warnings ?? []) : []);
	let showDiagnostics = $derived(adminStore.hasFeature(AdminFeature.DIAGNOSTICS));
	// a refused address (HTTP 403) without a saved key: nothing to explain, back to the chat
	let refused = $derived(
		!isChecking && !adminStore.isAvailable && adminStore.forbidden && !adminStore.hasSavedKey()
	);

	$effect(() => {
		if (refused) untrack(() => void goto(ROUTES.START, { replaceState: true }));
	});

	// the GPU cards refresh on a timer only while the Models tab is the one shown
	$effect(() => {
		const visible = adminStore.isAuthorized && activeTab === AdminTab.MODELS;

		untrack(() => adminStore.setModelsTabVisible(visible));
	});

	// what the server detected about itself: read when the Settings tab is shown
	$effect(() => {
		if (activeTab === AdminTab.SETTINGS && showDiagnostics && !adminStore.diagnostics) {
			untrack(() => void adminStore.loadDiagnostics());
		}
	});

	function selectTab(tab: AdminTab) {
		if (tab === activeTab) return;

		activeTab = tab;

		// the model list may have changed while the other tab was open; the form
		// stays mounted, so unsaved edits survive and only untouched fields follow
		if (tab === AdminTab.SETTINGS) {
			void adminStore.loadSettings();
			void adminStore.loadDiagnostics();
		}
	}

	/** Arrow keys, Home and End move between the tabs (WAI-ARIA tabs pattern). */
	function handleTabKeydown(event: KeyboardEvent, index: number) {
		const count = ADMIN_TABS.length;

		let next: number;

		switch (event.key) {
			case KeyboardKey.ARROW_LEFT:
				next = (index - 1 + count) % count;

				break;
			case KeyboardKey.ARROW_RIGHT:
				next = (index + 1) % count;

				break;
			case KeyboardKey.HOME:
				next = 0;

				break;
			case KeyboardKey.END:
				next = count - 1;

				break;
			default:
				return;
		}

		event.preventDefault();

		const tab = ADMIN_TABS[next].id;

		selectTab(tab);
		tabButtons[tab]?.focus();
	}

	/** The key form was replaced by the panel: keep the keyboard focus on the page. */
	async function focusActiveTab() {
		await tick();
		tabButtons[activeTab]?.focus();
	}

	onMount(() => {
		void adminStore.enter();
	});

	onDestroy(() => adminStore.leave());

	/*
	 * A file dropped where the admin page does not take it (its header, the
	 * overlay of a dialog ...) would make the browser open the file and leave
	 * the page, uploads in progress included: such drags are refused. The
	 * drop areas of the page and its dialogs handle theirs first.
	 */
	function isUnhandledFileDrag(event: DragEvent): boolean {
		return !event.defaultPrevented && !!event.dataTransfer?.types.includes('Files');
	}

	function refuseFileDrag(event: DragEvent) {
		if (!isUnhandledFileDrag(event)) return;

		event.preventDefault();

		if (event.dataTransfer) event.dataTransfer.dropEffect = 'none';
	}

	function refuseFileDrop(event: DragEvent) {
		if (isUnhandledFileDrag(event)) event.preventDefault();
	}
</script>

<svelte:window ondragover={refuseFileDrag} ondrop={refuseFileDrop} />

<div class="mx-auto flex w-full max-w-6xl flex-col gap-4 px-4 pt-14 pb-10 md:pt-6">
	<header class="flex flex-wrap items-center justify-between gap-3">
		<div class="flex items-center gap-2.5">
			<ShieldCheck class="h-5 w-5" />

			<h1 class="text-xl font-semibold">Admin</h1>
		</div>

		{#if adminStore.isAuthorized}
			<div class="flex flex-wrap items-center gap-2">
				<Badge class="font-normal tabular-nums" data-testid="admin-loaded-count" variant="outline">
					Loaded {runningCount}{modelsMax !== null && modelsMax > 0 ? ` / ${modelsMax}` : ''}
				</Badge>

				<Badge class="font-normal tabular-nums" variant="outline">
					{modelCount} model{modelCount === 1 ? '' : 's'}
				</Badge>

				<AdminUploadIndicator />

				<Button
					onclick={() => adminStore.forgetKey()}
					size="sm"
					title="Forget the admin key in this browser"
					variant="ghost"
				>
					<Lock />

					Lock
				</Button>
			</div>
		{/if}
	</header>

	{#if isChecking || refused}
		<div class="flex items-center justify-center gap-2 py-16 text-sm text-muted-foreground">
			<Loader2 class="{ICON_CLASS_DEFAULT} animate-spin" />

			Checking the admin API...
		</div>
	{:else if !adminStore.isAvailable}
		<AdminUnavailable />
	{:else if !adminStore.isAuthorized}
		<AdminKeyForm onUnlocked={focusActiveTab} />
	{:else}
		{#if stateInfo?.error}
			<Alert.Root variant="destructive">
				<TriangleAlert class={ICON_CLASS_DEFAULT} />

				<Alert.Title>Saved admin settings could not be read</Alert.Title>

				<Alert.Description>
					{stateInfo.error} The server runs with its start-up defaults until settings are saved again.
				</Alert.Description>
			</Alert.Root>
		{/if}

		{#each statusWarnings as code (code)}
			<Alert.Root>
				<TriangleAlert class={ICON_CLASS_DEFAULT} />

				<Alert.Description data-warning={code}>{describeModelWarning(code)}</Alert.Description>
			</Alert.Root>
		{/each}

		{#if stateInfo && !stateInfo.writable}
			<Alert.Root variant="destructive">
				<TriangleAlert class={ICON_CLASS_DEFAULT} />

				<Alert.Title>The admin state directory is not writable</Alert.Title>

				<Alert.Description>
					Settings cannot be saved until the server can write to {stateInfo.dir}.
				</Alert.Description>
			</Alert.Root>
		{/if}

		<div
			aria-label="Admin sections"
			class="inline-flex w-fit rounded-lg border bg-muted/40 p-0.5"
			role="tablist"
		>
			{#each ADMIN_TABS as tab, index (tab.id)}
				<button
					bind:this={tabButtons[tab.id]}
					aria-controls="admin-panel-{tab.id}"
					aria-selected={activeTab === tab.id}
					class={[
						'rounded-md px-4 py-1.5 text-sm font-medium transition-colors',
						activeTab === tab.id
							? 'bg-background text-foreground shadow-sm'
							: 'text-muted-foreground hover:text-foreground'
					]}
					id="admin-tab-{tab.id}"
					onclick={() => selectTab(tab.id)}
					onkeydown={(event) => handleTabKeydown(event, index)}
					role="tab"
					tabindex={activeTab === tab.id ? 0 : -1}
					type="button"
				>
					{tab.label}
				</button>
			{/each}
		</div>

		<!-- both panels stay mounted: switching tabs must not throw away unsaved settings -->
		<div
			aria-labelledby="admin-tab-{AdminTab.MODELS}"
			hidden={activeTab !== AdminTab.MODELS}
			id="admin-panel-{AdminTab.MODELS}"
			role="tabpanel"
		>
			<AdminModelsTab />
		</div>

		<div
			aria-labelledby="admin-tab-{AdminTab.SETTINGS}"
			hidden={activeTab !== AdminTab.SETTINGS}
			id="admin-panel-{AdminTab.SETTINGS}"
			role="tabpanel"
		>
			<div class="grid gap-6 lg:grid-cols-[minmax(0,1fr)_20rem]">
				<AdminSettingsTab />

				<div class="flex flex-col gap-4">
					<AdminServerInfo />

					{#if showDiagnostics}
						<AdminDiagnostics />
					{/if}
				</div>
			</div>
		</div>
	{/if}
</div>
