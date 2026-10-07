<script lang="ts">
	import { Loader2, Lock, RotateCcw, Save, TriangleAlert } from '@lucide/svelte';
	import { Badge } from '$lib/components/ui/badge';
	import { Button } from '$lib/components/ui/button';
	import { Checkbox } from '$lib/components/ui/checkbox';
	import { Input } from '$lib/components/ui/input';
	import { Label } from '$lib/components/ui/label';
	import * as Select from '$lib/components/ui/select';
	import {
		ADMIN_DEVICE_NONE,
		ADMIN_MODEL_SOURCE_LABELS,
		ADMIN_MODEL_STATUS_LABELS,
		ICON_CLASS_DEFAULT
	} from '$lib/constants';
	import { adminStore, modelsStore } from '$lib/stores';
	import type { AdminSettingsDraft, ApiAdminSettings } from '$lib/types';
	import {
		buildSettingsUpdate,
		countRunningModels,
		createSettingsDraft,
		maxPinnedModels,
		normalizeDeviceSpec,
		parseModelsMax,
		rebaseSettingsDraft,
		validateSettingsDraft
	} from '$lib/utils';
	import { untrack } from 'svelte';
	import { toast } from 'svelte-sonner';

	// select values that are not device names
	const DEVICE_UNSET = '__unset__';
	const DEVICE_CUSTOM = '__custom__';

	let settings = $derived(adminStore.settings);

	let draft = $state<AdminSettingsDraft | null>(null);
	// the device is typed by hand instead of picked from the list
	let customDevice = $state(false);

	// settings snapshot the draft was last compared with
	let syncedFrom: ApiAdminSettings | null = null;

	// stage 2: the GPUs the server detected; an older server only has the hint list from the presets
	let detected = $derived(settings?.build_devices !== undefined);
	let deviceOptions = $derived.by(() => {
		const known = (
			detected ? (settings?.build_devices ?? []) : (settings?.known_devices ?? [])
		).filter((device) => device && device !== ADMIN_DEVICE_NONE);

		return [
			{ label: 'Not set (server default)', value: DEVICE_UNSET },
			...known.map((device) => ({ label: device, value: device })),
			{ label: 'none (CPU only)', value: ADMIN_DEVICE_NONE },
			{ label: 'Custom value...', value: DEVICE_CUSTOM }
		];
	});
	let deviceChoice = $derived.by(() => {
		if (customDevice) return DEVICE_CUSTOM;

		const device = normalizeDeviceSpec(draft?.defaultDevice ?? '');

		if (device === '') return DEVICE_UNSET;

		return deviceOptions.some((option) => option.value === device) ? device : DEVICE_CUSTOM;
	});
	let deviceLabel = $derived(
		deviceOptions.find((option) => option.value === deviceChoice)?.label ?? deviceChoice
	);

	let validation = $derived(draft && settings ? validateSettingsDraft(draft, settings) : null);
	let update = $derived(draft && settings ? buildSettingsUpdate(draft, settings) : {});
	let isDirty = $derived(Object.keys(update).length > 0);
	let canSave = $derived(isDirty && validation?.valid === true && !adminStore.savingSettings);

	let modelsMaxValue = $derived(draft ? parseModelsMax(draft.modelsMax) : null);
	// null = no limit to enforce in the form (models_max 0 means "unlimited" on the server)
	let pinLimit = $derived(
		modelsMaxValue !== null && modelsMaxValue > 0 ? maxPinnedModels(modelsMaxValue) : null
	);
	let pinLimitReached = $derived(
		pinLimit !== null && draft !== null && draft.pinned.length >= pinLimit
	);
	let runningCount = $derived(countRunningModels(modelsStore.routerModels));

	function isListedDevice(device: string, source: ApiAdminSettings): boolean {
		const value = normalizeDeviceSpec(device);
		const listed = source.build_devices ?? source.known_devices;

		return value === '' || value === ADMIN_DEVICE_NONE || listed.includes(value);
	}

	/** Replace the draft with the values of a settings snapshot. */
	function resetDraft(source: ApiAdminSettings) {
		draft = createSettingsDraft(source);
		customDevice = !isListedDevice(source.default_device, source);
		syncedFrom = source;
	}

	// Follow the settings snapshot without throwing away what the user is
	// typing: on a background refresh (the model list changed, a save was
	// refused) untouched fields take the server's values and edited fields
	// stay. A successful save resets the draft itself (handleSave).
	$effect(() => {
		const next = settings;

		if (!next) return;

		untrack(() => {
			const previous = syncedFrom;

			if (draft === null || previous === null) {
				resetDraft(next);

				return;
			}

			const device = normalizeDeviceSpec(draft.defaultDevice);

			// the device changed on the server under an untouched field: show it as a fresh form would
			if (
				device === normalizeDeviceSpec(previous.default_device) &&
				device !== normalizeDeviceSpec(next.default_device)
			) {
				customDevice = !isListedDevice(next.default_device, next);
			}

			draft = rebaseSettingsDraft(draft, previous, next);
			syncedFrom = next;
		});
	});

	/** The answer to the last save no longer describes the form once it is edited. */
	function clearSaveError() {
		adminStore.saveError = null;
	}

	function handleDeviceChoice(value: string) {
		if (!draft) return;

		clearSaveError();

		if (value === DEVICE_CUSTOM) {
			customDevice = true;

			return;
		}

		customDevice = false;
		draft.defaultDevice = value === DEVICE_UNSET ? '' : value;
	}

	function togglePinned(id: string, checked: boolean) {
		if (!draft) return;

		clearSaveError();
		draft.pinned = checked
			? [...new Set([...draft.pinned, id])]
			: draft.pinned.filter((pinnedId) => pinnedId !== id);
	}

	function handleReset() {
		if (!settings) return;

		resetDraft(settings);
		clearSaveError();
	}

	async function handleSave(event: SubmitEvent) {
		event.preventDefault();

		if (!canSave) return;

		const saved = await adminStore.saveSettings(update);

		if (!saved) return;

		// the form now shows what the server reports as in effect
		if (adminStore.settings) resetDraft(adminStore.settings);

		toast.success('Settings saved');
	}
</script>

{#if !settings || !draft}
	<div class="rounded-lg border border-dashed p-8 text-center text-sm text-muted-foreground">
		{#if adminStore.settingsError}
			<p class="text-destructive">Settings could not be loaded: {adminStore.settingsError}</p>

			<Button class="mt-3" onclick={() => adminStore.loadSettings()} size="sm" variant="secondary">
				Try again
			</Button>
		{:else}
			<span class="inline-flex items-center gap-2">
				<Loader2 class="{ICON_CLASS_DEFAULT} animate-spin" />

				Loading settings...
			</span>
		{/if}
	</div>
{:else}
	<form class="flex max-w-3xl flex-col gap-6" novalidate onsubmit={handleSave}>
		<section class="space-y-2">
			<Label for="admin-models-max">Max loaded models</Label>

			<div class="flex flex-wrap items-center gap-3">
				<!-- not bind:value: on a number input that yields a number or null, the draft keeps the text -->
				<Input
					aria-describedby="admin-models-max-help"
					aria-invalid={validation?.modelsMax ? 'true' : undefined}
					class="w-28"
					disabled={adminStore.savingSettings}
					id="admin-models-max"
					inputmode="numeric"
					max={settings.limits.models_max_max}
					min={settings.limits.models_max_min}
					oninput={(event) => {
						clearSaveError();

						if (draft) draft.modelsMax = event.currentTarget.value;
					}}
					type="number"
					value={draft.modelsMax}
				/>

				<span class="text-xs text-muted-foreground">
					Loaded right now: <span class="font-medium text-foreground">{runningCount}</span>
				</span>
			</div>

			{#if validation?.modelsMax}
				<p class="text-xs text-destructive" role="alert">{validation.modelsMax}</p>
			{/if}

			<p class="text-xs text-muted-foreground" id="admin-models-max-help">
				How many models may be loaded at the same time ({settings.limits.models_max_min} to
				{settings.limits.models_max_max}). When the limit is reached, the least recently used model
				that is not pinned is unloaded to make room. A lower value takes effect at once but does not
				unload anything by itself.
			</p>
		</section>

		<section class="space-y-2">
			<Label for="admin-default-device">GPU for new models</Label>

			{#if settings.default_device_locked}
				<div class="flex flex-wrap items-center gap-3">
					<Input
						class="w-56 font-mono"
						disabled
						id="admin-default-device"
						value={settings.default_device || 'not set'}
					/>

					<span class="flex items-center gap-1.5 text-xs text-muted-foreground">
						<Lock class="h-3.5 w-3.5" />

						Locked by MODEL_DEVICES
					</span>
				</div>

				<p class="text-xs text-muted-foreground">
					The deployment sets a device for all models through a <span class="font-mono">*=</span>
					entry of the MODEL_DEVICES environment variable, so this value cannot be changed here. Change
					MODEL_DEVICES and restart the server instead.
				</p>
			{:else}
				<div class="flex flex-wrap items-center gap-2">
					<Select.Root
						disabled={adminStore.savingSettings}
						onValueChange={handleDeviceChoice}
						type="single"
						value={deviceChoice}
					>
						<Select.Trigger class="w-56" id="admin-default-device">{deviceLabel}</Select.Trigger>

						<Select.Content>
							{#each deviceOptions as option (option.value)}
								<Select.Item label={option.label} value={option.value} />
							{/each}
						</Select.Content>
					</Select.Root>

					{#if deviceChoice === DEVICE_CUSTOM}
						<Input
							bind:value={draft.defaultDevice}
							aria-invalid={validation?.defaultDevice ? 'true' : undefined}
							aria-label="Custom device value"
							autocomplete="off"
							class="w-56 font-mono"
							disabled={adminStore.savingSettings}
							oninput={clearSaveError}
							placeholder="CUDA0 or CUDA0,CUDA1"
							spellcheck={false}
						/>
					{/if}
				</div>

				{#if validation?.defaultDevice}
					<p class="text-xs text-destructive" role="alert">{validation.defaultDevice}</p>
				{/if}

				{#if update.default_device !== undefined && !validation?.defaultDevice}
					<p class="flex items-start gap-2 text-xs text-amber-600 dark:text-amber-400">
						<TriangleAlert class="mt-0.5 h-3.5 w-3.5 shrink-0" />

						<span>
							Saving a different device makes the server reload its model list: running downloaded
							and models-directory models that get the new device are unloaded.
						</span>
					</p>
				{/if}

				{#if settings.warnings?.includes('default_device_not_present')}
					<p
						class="flex items-start gap-2 text-xs text-amber-600 dark:text-amber-400"
						data-warning="default_device_not_present"
					>
						<TriangleAlert class="mt-0.5 h-3.5 w-3.5 shrink-0" />

						<span>
							The saved value names a GPU this server does not have ({settings.default_device}); it
							is ignored until it is changed or the GPU is back.
						</span>
					</p>
				{/if}

				<p class="text-xs text-muted-foreground">
					Applies to downloaded and dropped-in models (Hugging Face cache, models directory) that
					have no device of their own. A GPU chosen in the settings of a model wins over this,
					models defined in the preset file keep their own device, and the MODEL_DEVICES environment
					variable takes precedence over all of them.
					{#if detected}
						The list shows the GPUs this server detected{(settings.build_devices ?? []).length === 0
							? ': none, so models run on the CPU'
							: ''}.
					{:else}
						The list shows devices already used by the model presets; the server does not probe the
						GPUs.
					{/if}
				</p>
			{/if}
		</section>

		<section class="space-y-2">
			<div class="flex flex-wrap items-baseline justify-between gap-2">
				<span class="text-sm leading-none font-medium" id="admin-pinned-label">Pinned models</span>

				<span class="text-xs text-muted-foreground tabular-nums" data-testid="admin-pinned-count">
					{draft.pinned.length} pinned{pinLimit !== null ? ` · at most ${pinLimit}` : ''}
				</span>
			</div>

			<p class="text-xs text-muted-foreground">
				A pinned model is never unloaded to make room for another one. At least one slot must stay
				evictable, so fewer models can be pinned than may be loaded.
			</p>

			{#if settings.models.length === 0}
				<p class="rounded-lg border border-dashed p-4 text-center text-sm text-muted-foreground">
					There are no models to pin.
				</p>
			{:else}
				<ul
					aria-labelledby="admin-pinned-label"
					class="max-h-80 divide-y overflow-y-auto rounded-lg border bg-card"
				>
					{#each settings.models as model (model.id)}
						{@const checked = draft.pinned.includes(model.id)}
						{@const status = modelsStore.getModelStatus(model.id) ?? model.status}

						<li class="flex items-center gap-3 px-3 py-2">
							<Checkbox
								{checked}
								disabled={adminStore.savingSettings || (!checked && pinLimitReached)}
								id="admin-pin-{model.id}"
								onCheckedChange={(value) => togglePinned(model.id, value === true)}
							/>

							<Label
								class="min-w-0 flex-1 cursor-pointer font-mono text-[13px] font-normal"
								for="admin-pin-{model.id}"
							>
								<span class="truncate" title={model.id}>{model.id}</span>
							</Label>

							<span class="hidden text-xs text-muted-foreground sm:inline">
								{ADMIN_MODEL_STATUS_LABELS[status] ?? status}
							</span>

							<Badge class="font-normal" variant="outline">
								{ADMIN_MODEL_SOURCE_LABELS[model.source] ?? model.source}
							</Badge>
						</li>
					{/each}
				</ul>
			{/if}

			{#if validation?.pinned}
				<p class="text-xs text-destructive" data-testid="admin-pinned-error" role="alert">
					{validation.pinned}
				</p>
			{/if}

			{#if (settings.pinned_ignored?.length ?? 0) > 0}
				<p class="text-xs text-amber-600 dark:text-amber-400">
					Not in effect because they exceed the limit: {settings.pinned_ignored?.join(', ')}
				</p>
			{/if}

			{#if (settings.pinned_unknown?.length ?? 0) > 0}
				<p class="text-xs text-amber-600 dark:text-amber-400">
					Configured on the server but not in the model list: {settings.pinned_unknown?.join(', ')}
				</p>
			{/if}
		</section>

		<div class="flex flex-col gap-3 border-t pt-4">
			{#if adminStore.saveError}
				<p
					class="flex items-start gap-2 text-sm text-destructive"
					data-testid="admin-save-error"
					role="alert"
				>
					<TriangleAlert class="mt-0.5 h-4 w-4 shrink-0" />

					<span>{adminStore.saveError}</span>
				</p>
			{/if}

			{#if settings.reload_error}
				<p class="flex items-start gap-2 text-sm text-amber-600 dark:text-amber-400">
					<TriangleAlert class="mt-0.5 h-4 w-4 shrink-0" />

					<span>
						The settings were saved, but reloading the model list failed: {settings.reload_error}
					</span>
				</p>
			{/if}

			<div class="flex flex-wrap items-center gap-2">
				<Button disabled={!canSave} type="submit">
					{#if adminStore.savingSettings}
						<Loader2 class="{ICON_CLASS_DEFAULT} animate-spin" />
						Saving...
					{:else}
						<Save class={ICON_CLASS_DEFAULT} />
						Save changes
					{/if}
				</Button>

				<Button
					disabled={!isDirty || adminStore.savingSettings}
					onclick={handleReset}
					variant="ghost"
				>
					<RotateCcw class={ICON_CLASS_DEFAULT} />

					Reset
				</Button>

				{#if !isDirty}
					<span class="text-xs text-muted-foreground">No unsaved changes</span>
				{/if}
			</div>
		</div>
	</form>
{/if}
