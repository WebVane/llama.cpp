<script lang="ts">
	import AdminAuditDetails from './AdminAuditDetails.svelte';
	import {
		ChevronDown,
		ChevronRight,
		Database,
		Download,
		Filter,
		Loader2,
		RefreshCw,
		TriangleAlert,
		X
	} from '@lucide/svelte';
	import * as Alert from '$lib/components/ui/alert';
	import { Badge } from '$lib/components/ui/badge';
	import { Button } from '$lib/components/ui/button';
	import * as Select from '$lib/components/ui/select';
	import * as Table from '$lib/components/ui/table';
	import {
		ADMIN_AUDIT,
		ADMIN_AUDIT_ACTION_GROUP_ORDER,
		ADMIN_AUDIT_ACTION_GROUPS,
		ADMIN_AUDIT_KIND_HINTS,
		ADMIN_AUDIT_KIND_LABELS,
		ADMIN_AUDIT_KIND_ORDER,
		ADMIN_AUDIT_RESULT_GROUP_ORDER,
		ADMIN_AUDIT_RESULT_GROUPS,
		ICON_CLASS_DEFAULT
	} from '$lib/constants';
	import {
		AdminAuditActionGroup,
		type AdminAuditKind,
		AdminAuditResultGroup
	} from '$lib/enums/admin.enums';
	import { deviceStore } from '$lib/stores';
	import { adminAuditStore } from '$lib/stores/admin-audit.svelte';
	import type { ApiAdminAuditEntry } from '$lib/types/admin-audit';
	import {
		type AdminAuditTone,
		auditEntryTime,
		auditResultTone,
		describeAuditAction,
		describeAuditActor,
		describeAuditNotPersisted,
		describeAuditResult,
		formatAuditUtc,
		summarizeAuditEntry,
		toggleAuditKind
	} from '$lib/utils/admin-audit';
	import { onDestroy, tick, untrack } from 'svelte';
	import { SvelteSet } from 'svelte/reactivity';
	import { toast } from 'svelte-sonner';

	const TONE_CLASSES: Record<AdminAuditTone, string> = {
		error: 'border-destructive/40 text-destructive',
		neutral: 'text-muted-foreground',
		success: 'border-green-600/30 text-green-700 dark:border-green-400/30 dark:text-green-400',
		warning: 'border-amber-500/40 text-amber-700 dark:text-amber-400'
	};

	interface Props {
		/** The tab is the one shown: the newest records are read every time it becomes visible */
		visible?: boolean;
	}

	let { visible = true }: Props = $props();

	let expanded = new SvelteSet<string>();

	let section = $state<HTMLElement>();
	/** Zero-height strip as wide as the panel: its size changes with the width only */
	let sizer = $state<HTMLElement>();
	let refreshButton = $state<HTMLElement | null>(null);
	/** Width of the panel in rem; 0 until measured */
	let panelRem = $state(0);

	let store = adminAuditStore;
	// cards when the panel is too narrow for the five columns: a tablet, or a laptop with the
	// sidebar open, is not a phone but leaves the Action column no room
	let isNarrow = $derived(
		deviceStore.isMobile || (panelRem > 0 && panelRem < ADMIN_AUDIT.TABLE_MIN_REM)
	);
	let isCompact = $derived(panelRem > 0 && panelRem < ADMIN_AUDIT.TABLE_WIDE_REM);
	let entries = $derived(store.entries);
	let filters = $derived(store.filters);
	let actionLabel = $derived(ADMIN_AUDIT_ACTION_GROUPS[filters.action]?.label ?? '');
	let resultLabel = $derived(ADMIN_AUDIT_RESULT_GROUPS[filters.result]?.label ?? '');
	let columns = $derived(isNarrow ? 1 : 5);

	// shown (also again after the key form, where the panel was gone): the newest records
	$effect(() => {
		if (visible) untrack(() => void store.load());
	});

	// the layout follows the width of the panel, not of the window (observing the panel itself
	// would see the height change that the switch of layout causes, a ResizeObserver loop)
	$effect(() => {
		const node = sizer;

		if (!node || typeof ResizeObserver === 'undefined') return;

		const observer = new ResizeObserver(() => {
			const width = node.clientWidth;

			// hidden behind another tab: keep the layout of the last width
			if (width <= 0) return;

			const rem = parseFloat(getComputedStyle(document.documentElement).fontSize) || 16;

			panelRem = width / rem;
		});

		observer.observe(node);

		return () => observer.disconnect();
	});

	// the records hold client addresses: they go when the panel goes (key form, other page)
	onDestroy(() => store.clear());

	function toggle(entry: ApiAdminAuditEntry) {
		if (expanded.has(entry.id)) expanded.delete(entry.id);
		else expanded.add(entry.id);
	}

	function setKinds(kinds: AdminAuditKind[]) {
		expanded.clear();
		store.setFilters({ kinds });
	}

	function setAction(value: string) {
		expanded.clear();
		store.setFilters({ action: value as AdminAuditActionGroup });
	}

	function setResult(value: string) {
		expanded.clear();
		store.setFilters({ result: value as AdminAuditResultGroup });
	}

	/** The keyboard focus is in the panel (on the control being pressed). */
	function hasFocus(): boolean {
		return !!section && section.contains(document.activeElement);
	}

	/**
	 * A control that went away with its own press (the last "Load more",
	 * "Clear filters", "Try again") leaves the focus on the page: move it to
	 * `target` so a keyboard user goes on from there.
	 */
	async function keepFocus(target: () => HTMLElement | null | undefined) {
		await tick();

		const active = document.activeElement;

		if (active && active !== document.body && active.isConnected) return;

		target()?.focus();
	}

	function toggleOf(id: string | undefined): HTMLElement | null {
		if (!id || !section) return null;

		return section.querySelector<HTMLElement>(
			`[data-audit-id="${CSS.escape(id)}"] [data-testid="audit-toggle"]`
		);
	}

	function resetFilters() {
		const focused = hasFocus();

		expanded.clear();
		store.resetFilters();

		if (focused) void keepFocus(() => section?.querySelector<HTMLElement>('[data-kind="all"]'));
	}

	// aria-disabled, not disabled, while reading: a disabled button would drop the focus
	function refresh() {
		if (store.loading) return;

		expanded.clear();
		void store.load();
	}

	async function retry() {
		if (store.loading) return;

		const focused = hasFocus();

		expanded.clear();
		await store.load();

		if (focused) await keepFocus(() => refreshButton);
	}

	async function loadMore() {
		if (store.loading || store.loadingMore) return;

		const focused = hasFocus();
		const first = entries.length;

		await store.loadMore();

		// the last page takes the button away: on to the first record it added
		if (focused) await keepFocus(() => toggleOf(store.entries[first]?.id));
	}

	async function download() {
		if (store.downloading) return;

		try {
			const file = await store.download();

			if (!file) return;

			const url = URL.createObjectURL(file.blob);
			const link = document.createElement('a');

			link.href = url;
			link.download = file.filename;
			document.body.appendChild(link);
			link.click();
			document.body.removeChild(link);
			setTimeout(() => URL.revokeObjectURL(url), 0);
		} catch (error) {
			toast.error(
				`The audit log could not be downloaded: ${error instanceof Error ? error.message : String(error)}`
			);
		}
	}
</script>

{#snippet resultBadge(entry: ApiAdminAuditEntry)}
	<Badge
		class={['font-normal', TONE_CLASSES[auditResultTone(entry.result)]]}
		data-result={entry.result}
		variant="outline"
	>
		{describeAuditResult(entry.result)}
	</Badge>
{/snippet}

{#snippet actorCell(entry: ApiAdminAuditEntry)}
	{@const actor = describeAuditActor(entry.actor, entry.details)}

	<div class="flex min-w-0 flex-col" title={actor.title}>
		<span
			class={['text-xs', actor.address ? 'truncate font-mono' : 'break-words whitespace-normal']}
			data-testid="audit-actor">{actor.primary}</span
		>

		{#if actor.secondary || actor.unverified}
			<span class="flex min-w-0 flex-wrap items-center gap-1 text-[11px] text-muted-foreground">
				{#if actor.secondary}
					<span class="truncate">{actor.secondary}</span>
				{/if}

				{#if actor.unverified}
					<Badge
						class="border-amber-500/40 px-1 py-0 text-[10px] font-normal text-amber-700 dark:text-amber-400"
						data-testid="audit-unverified"
						variant="outline"
					>
						via proxy, unverified
					</Badge>
				{/if}
			</span>
		{/if}
	</div>
{/snippet}

{#snippet actionCell(entry: ApiAdminAuditEntry)}
	{@const summary = summarizeAuditEntry(entry)}

	<div class="flex min-w-0 flex-col gap-0.5">
		<span class="flex flex-wrap items-center gap-1.5">
			<span class="font-medium" title={entry.action}>
				{describeAuditAction(entry.action, entry.result)}
			</span>

			{#if entry.open}
				<Badge
					class="px-1 py-0 text-[10px] font-normal"
					title="This minute is still being counted: refresh for the final count"
					variant="secondary"
				>
					counting
				</Badge>
			{/if}

			{#if !entry.persisted}
				<Badge
					class="gap-1 border-amber-500/40 px-1 py-0 text-[10px] font-normal text-amber-700 dark:text-amber-400"
					data-testid="audit-memory-only"
					title="Kept in memory only: lost when the server restarts"
					variant="outline"
				>
					<Database />

					memory only
				</Badge>
			{/if}
		</span>

		{#if summary}
			<span
				class="text-xs break-words whitespace-normal text-muted-foreground"
				data-testid="audit-summary"
			>
				{summary}
			</span>
		{/if}
	</div>
{/snippet}

{#snippet targetText(entry: ApiAdminAuditEntry)}
	{#if entry.target}
		<span class="font-mono text-xs break-all" title={entry.target}>{entry.target}</span>
	{:else}
		<span class="text-xs text-muted-foreground">—</span>
	{/if}
{/snippet}

{#snippet toggleButton(entry: ApiAdminAuditEntry, label: string)}
	<!-- the name starts with the visible time (voice control); aria-expanded says open or closed -->
	<button
		aria-controls="audit-details-{entry.id}"
		aria-expanded={expanded.has(entry.id)}
		aria-label="{label}, details of record {entry.seq}"
		class="inline-flex items-center gap-1.5 rounded text-left text-xs tabular-nums hover:text-foreground focus-visible:ring-2 focus-visible:ring-ring focus-visible:outline-none"
		data-testid="audit-toggle"
		onclick={() => toggle(entry)}
		type="button"
	>
		{#if expanded.has(entry.id)}
			<ChevronDown class="h-3.5 w-3.5 shrink-0" />
		{:else}
			<ChevronRight class="h-3.5 w-3.5 shrink-0" />
		{/if}

		<span title={formatAuditUtc(entry.time) || undefined}>{label}</span>
	</button>
{/snippet}

<section bind:this={section} class="relative flex flex-col gap-4" data-testid="admin-activity">
	<div
		bind:this={sizer}
		aria-hidden="true"
		class="pointer-events-none absolute inset-x-0 top-0 h-0"
	></div>

	{#if !store.persisted}
		<Alert.Root data-testid="audit-memory-banner" variant="destructive">
			<TriangleAlert class={ICON_CLASS_DEFAULT} />

			<Alert.Title>The audit log is kept in memory only</Alert.Title>

			<Alert.Description>{describeAuditNotPersisted(store.statusInfo)}</Alert.Description>
		</Alert.Root>
	{/if}

	<div class="flex flex-wrap items-center gap-2">
		<div
			aria-label="Kinds of records"
			class="inline-flex flex-wrap items-center gap-1 rounded-lg border bg-muted/40 p-0.5"
			data-testid="audit-kind-filter"
			role="group"
		>
			<button
				aria-pressed={filters.kinds.length === 0}
				class={[
					'rounded-md px-2.5 py-1 text-xs font-medium transition-colors',
					filters.kinds.length === 0
						? 'bg-background text-foreground shadow-sm'
						: 'text-muted-foreground hover:text-foreground'
				]}
				data-kind="all"
				onclick={() => setKinds([])}
				type="button"
			>
				All
			</button>

			{#each ADMIN_AUDIT_KIND_ORDER as kind (kind)}
				{@const on = filters.kinds.includes(kind)}

				<button
					aria-pressed={on}
					class={[
						'rounded-md px-2.5 py-1 text-xs font-medium transition-colors',
						on
							? 'bg-background text-foreground shadow-sm'
							: 'text-muted-foreground hover:text-foreground'
					]}
					data-kind={kind}
					onclick={() => setKinds(toggleAuditKind(filters.kinds, kind))}
					title={ADMIN_AUDIT_KIND_HINTS[kind]}
					type="button"
				>
					{ADMIN_AUDIT_KIND_LABELS[kind]}
				</button>
			{/each}
		</div>

		<!-- a phone: both selects share one line -->
		<div class="flex w-full gap-2 sm:w-auto">
			<Select.Root onValueChange={setAction} type="single" value={filters.action}>
				<Select.Trigger
					aria-label="Action"
					class="h-8 min-w-0 flex-1 text-xs sm:w-44 sm:flex-none"
					data-testid="audit-action-filter"
				>
					<span class="flex items-center gap-1.5 truncate">
						<Filter class="h-3.5 w-3.5 text-muted-foreground" />

						{actionLabel}
					</span>
				</Select.Trigger>

				<Select.Content>
					{#each ADMIN_AUDIT_ACTION_GROUP_ORDER as group (group)}
						<Select.Item label={ADMIN_AUDIT_ACTION_GROUPS[group].label} value={group} />
					{/each}
				</Select.Content>
			</Select.Root>

			<Select.Root onValueChange={setResult} type="single" value={filters.result}>
				<Select.Trigger
					aria-label="Result"
					class="h-8 min-w-0 flex-1 text-xs sm:w-44 sm:flex-none"
					data-testid="audit-result-filter"
				>
					<span class="truncate">{resultLabel}</span>
				</Select.Trigger>

				<Select.Content>
					{#each ADMIN_AUDIT_RESULT_GROUP_ORDER as group (group)}
						<Select.Item label={ADMIN_AUDIT_RESULT_GROUPS[group].label} value={group} />
					{/each}
				</Select.Content>
			</Select.Root>
		</div>

		{#if store.isFiltered}
			<Button data-testid="audit-reset-filters" onclick={resetFilters} size="sm" variant="ghost">
				<X />

				Clear filters
			</Button>
		{/if}

		<div class="ml-auto flex items-center gap-1">
			<Button
				bind:ref={refreshButton}
				aria-disabled={store.loading}
				aria-label="Refresh"
				data-testid="audit-refresh"
				onclick={refresh}
				size="sm"
				title="Read the newest records again"
				variant="ghost"
			>
				<RefreshCw class={store.loading ? 'animate-spin' : ''} />

				<span class="hidden sm:inline">Refresh</span>
			</Button>

			<Button
				aria-disabled={store.downloading}
				data-testid="audit-download"
				onclick={download}
				size="sm"
				title={store.isFiltered
					? 'Every record that matches the filters, as JSON lines, oldest first'
					: 'The whole audit log as JSON lines, oldest first'}
				variant="outline"
			>
				{#if store.downloading}
					<Loader2 class="animate-spin" />
				{:else}
					<Download />
				{/if}

				Download JSONL
			</Button>
		</div>
	</div>

	{#if store.error}
		<Alert.Root data-testid="audit-error" variant="destructive">
			<TriangleAlert class={ICON_CLASS_DEFAULT} />

			<Alert.Title>The audit log could not be read</Alert.Title>

			<Alert.Description>
				{store.error}

				<Button
					aria-disabled={store.loading}
					class="mt-2 w-fit"
					onclick={retry}
					size="sm"
					variant="outline"
				>
					Try again
				</Button>
			</Alert.Description>
		</Alert.Root>
	{/if}

	{#if !store.loaded && store.loading}
		<div class="flex items-center justify-center gap-2 py-12 text-sm text-muted-foreground">
			<Loader2 class="{ICON_CLASS_DEFAULT} animate-spin" />

			Reading the audit log...
		</div>
	{:else if store.loaded && entries.length === 0}
		<div
			class="rounded-lg border bg-card px-4 py-10 text-center text-sm text-muted-foreground"
			data-testid="audit-empty"
		>
			{store.isFiltered ? 'No records match these filters.' : 'Nothing has been recorded yet.'}
		</div>
	{:else if entries.length > 0}
		<p class="text-xs text-muted-foreground" data-testid="audit-count">
			{entries.length === store.total
				? `${store.total} record${store.total === 1 ? '' : 's'}`
				: `Showing ${entries.length} of ${store.total} records`}{store.isFiltered
				? ' matching the filters'
				: ''}, newest first. Times are local.
		</p>

		<div class="overflow-hidden rounded-lg border bg-card">
			<!-- min-width (ADMIN_AUDIT.TABLE_MIN_REM): scroll rather than squeeze the Action column -->
			<Table.Root
				class={isNarrow ? 'table-fixed' : 'min-w-[56rem] table-fixed'}
				data-testid="audit-table"
			>
				{#if !isNarrow}
					<Table.Header>
						<Table.Row>
							<Table.Head class="w-44 bg-muted/40 pl-4 text-muted-foreground">Time</Table.Head>

							<Table.Head
								class={['bg-muted/40 text-muted-foreground', isCompact ? 'w-44' : 'w-52']}
							>
								Actor
							</Table.Head>

							<Table.Head class="bg-muted/40 text-muted-foreground" data-testid="audit-action-head">
								Action
							</Table.Head>

							<Table.Head
								class={['bg-muted/40 text-muted-foreground', isCompact ? 'w-44' : 'w-56']}
							>
								Target
							</Table.Head>

							<Table.Head class="w-24 bg-muted/40 pr-4 text-muted-foreground">Result</Table.Head>
						</Table.Row>
					</Table.Header>
				{/if}

				<Table.Body>
					{#each entries as entry (entry.id)}
						{@const time = auditEntryTime(entry)}

						<Table.Row
							class={expanded.has(entry.id) ? 'border-b-0 bg-muted/30' : 'last:border-b-0'}
							data-action={entry.action}
							data-audit-id={entry.id}
							data-kind={entry.kind}
						>
							{#if isNarrow}
								<Table.Cell class="px-3 py-2.5 whitespace-normal">
									<div class="flex min-w-0 flex-col gap-1.5">
										<div class="flex items-start justify-between gap-2">
											{@render actionCell(entry)}

											{@render resultBadge(entry)}
										</div>

										{#if entry.target}
											<div class="min-w-0">{@render targetText(entry)}</div>
										{/if}

										<div class="flex min-w-0 items-start justify-between gap-2">
											{@render actorCell(entry)}

											<span class="shrink-0 text-muted-foreground">
												{@render toggleButton(entry, time)}
											</span>
										</div>
									</div>
								</Table.Cell>
							{:else}
								<Table.Cell class="pl-4 align-top">
									<span class="text-muted-foreground">{@render toggleButton(entry, time)}</span>
								</Table.Cell>

								<Table.Cell class="align-top">{@render actorCell(entry)}</Table.Cell>

								<Table.Cell class="align-top whitespace-normal">
									{@render actionCell(entry)}
								</Table.Cell>

								<Table.Cell class="align-top whitespace-normal">
									{@render targetText(entry)}
								</Table.Cell>

								<Table.Cell class="pr-4 align-top">{@render resultBadge(entry)}</Table.Cell>
							{/if}
						</Table.Row>

						{#if expanded.has(entry.id)}
							<Table.Row class="bg-muted/30 last:border-b-0 hover:bg-muted/30">
								<Table.Cell
									class="px-4 pt-1 pb-4 whitespace-normal"
									colspan={columns}
									id="audit-details-{entry.id}"
								>
									<AdminAuditDetails {entry} />
								</Table.Cell>
							</Table.Row>
						{/if}
					{/each}
				</Table.Body>
			</Table.Root>
		</div>

		{#if store.moreError}
			<p class="text-xs text-destructive" role="alert">
				More records could not be read: {store.moreError}
			</p>
		{/if}

		{#if store.nextBefore}
			<Button
				aria-disabled={store.loading || store.loadingMore}
				class="w-fit self-center"
				data-testid="audit-load-more"
				onclick={loadMore}
				size="sm"
				variant="outline"
			>
				{#if store.loadingMore}
					<Loader2 class="animate-spin" />
				{/if}

				Load more ({store.remaining} older)
			</Button>
		{/if}
	{/if}

	<p class="text-[11px] text-muted-foreground">
		Changes made through /admin with the admin key, refused /admin requests (merged per minute),
		what the server did by itself, and effects of the public model API seen on its event feed.
		Without user accounts, "who" is the address that presented the key; an X-Forwarded-For address
		is only verified when it comes from a trusted proxy. The admin key, other tokens and request
		bodies are never recorded (of a settings change only the names of the fields sent), nor are
		paths outside the models directory.
	</p>
</section>
