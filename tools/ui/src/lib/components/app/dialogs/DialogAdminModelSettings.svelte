<script lang="ts">
	import {
		ArrowLeftRight,
		CircleCheck,
		Download,
		Loader2,
		RefreshCw,
		RotateCcw,
		SlidersHorizontal,
		TriangleAlert,
		Upload
	} from '@lucide/svelte';
	// direct imports, not the admin barrel: the admin components import the dialogs barrel
	import AdminFitIndicator from '$lib/components/app/admin/AdminFitIndicator.svelte';
	import AdminModelFailure from '$lib/components/app/admin/AdminModelFailure.svelte';
	import AdminModelSettingsForm from '$lib/components/app/admin/AdminModelSettingsForm.svelte';
	import AdminSettingsImportPreview from '$lib/components/app/admin/AdminSettingsImportPreview.svelte';
	import { Badge } from '$lib/components/ui/badge';
	import { Button, buttonVariants } from '$lib/components/ui/button';
	import * as Dialog from '$lib/components/ui/dialog';
	import * as DropdownMenu from '$lib/components/ui/dropdown-menu';
	import { cn } from '$lib/components/ui/utils';
	import {
		ADMIN_FIT_CHECK_BUSY_RETRY,
		ADMIN_FIT_CHECK_DEBOUNCE_MS,
		ADMIN_GPU_REFRESH,
		ADMIN_MODEL_SOURCE_LABELS,
		ADMIN_MODEL_STATUS_LABELS,
		ADMIN_RESTART_RISK_NOTE,
		ADMIN_RESTART_WHEN_IDLE,
		ADMIN_SETTINGS_FILE,
		ADMIN_SLOW_LOAD_MS
	} from '$lib/constants';
	import {
		AdminErrorType,
		AdminExportMode,
		AdminFeature,
		AdminFitErrorKind,
		AdminFitVerdict,
		AdminNoticeLevel,
		AdminRestartPhase,
		ServerModelStatus
	} from '$lib/enums';
	import { AdminApiError } from '$lib/services';
	import { adminStore, modelsStore } from '$lib/stores';
	import type {
		AdminImportPreview,
		AdminModelSettingsDraft,
		AdminSlotsBusy,
		ApiAdminFailureReason,
		ApiAdminFitCheck,
		ApiAdminModelLog,
		ApiAdminModelSettingsRow,
		ApiAdminModelSettingsUpdate,
		ApiModelsSseEvent
	} from '$lib/types';
	import type { AdminRestartTracker } from '$lib/utils';
	import {
		applyImportToDraft,
		buildImportPreview,
		buildModelSettingsUpdate,
		buildSettingsExport,
		createModelSettingsDraft,
		createRestartTracker,
		describeFit,
		describeModelWarning,
		describeResetChanges,
		describeSaveResult,
		draftChangesEffect,
		draftContext,
		draftSettingValue,
		draftWarnings,
		estimateFitByFormula,
		fitCheckErrorKind,
		fitFromCheck,
		fitGpuStates,
		formatBytes,
		formatTokenCount,
		isInstanceAfterFailure,
		isRunningStatus,
		modelLoadFraction,
		modelLoadProgressText,
		parseSettingsFile,
		rebaseModelSettingsDraft,
		restartPhase,
		revertPlan,
		settingsFileName,
		trackRestartStatus,
		validateModelSettingsDraft,
		withFailedSettings
	} from '$lib/utils';
	import { keepFocusOffDialogClose } from '$lib/utils/admin-dialog-focus';
	import { onDestroy, onMount, untrack } from 'svelte';
	import { toast } from 'svelte-sonner';

	interface Props {
		open: boolean;
		modelId: string | null;
		onOpenChange?: (open: boolean) => void;
	}

	let { modelId, onOpenChange, open = $bindable() }: Props = $props();

	// the dialog element: it takes the focus on open (see onOpenAutoFocus)
	let contentNode = $state<HTMLElement | null>(null);

	type View = 'edit' | 'confirm' | 'waiting' | 'progress' | 'failure' | 'import' | 'reset';

	/** What the next POST sends; set before a confirmation. */
	interface PendingSave {
		settings?: Record<string, string | null>;
		reset?: boolean;
		restart: boolean;
		/** Putting back the settings before a failed save */
		revert?: boolean;
	}

	interface FailureInfo {
		reason: ApiAdminFailureReason | null;
		lines: string[];
		truncated: boolean;
		exitCode: number | null;
		time: string | null;
		reverted: boolean;
	}

	let row = $state<ApiAdminModelSettingsRow | null>(null);
	let draft = $state<AdminModelSettingsDraft | null>(null);
	let loading = $state(false);
	let loadError = $state<string | null>(null);
	let view = $state<View>('edit');
	let saving = $state(false);
	let saveError = $state<string | null>(null);
	let serverErrors = $state<Record<string, string>>({});
	let pending = $state<PendingSave | null>(null);
	let slots = $state<AdminSlotsBusy | null>(null);
	let slotsLoading = $state(false);
	let idleMessage = $state<string | null>(null);
	let tracker = $state<AdminRestartTracker | null>(null);
	let progressSince = $state(0);
	// the save stops a running instance first
	let stopsRunning = $state(false);
	let now = $state(Date.now());
	let previous = $state<Record<string, string | null> | null>(null);
	// saved values the model failed to start with in this dialog: Revert never goes back to them
	let failedSettings = $state<Record<string, string>[]>([]);
	let failure = $state<FailureInfo | null>(null);
	let check = $state<{ key: string; result: ApiAdminFitCheck } | null>(null);
	let checkRunning = $state(false);
	let checkError = $state<string | null>(null);
	let importPreview = $state<AdminImportPreview | null>(null);
	let importError = $state<string | null>(null);
	let importConfirmed = $state(false);
	// output of the instance that is starting, shown in the progress view ("Show output")
	let output = $state<ApiAdminModelLog | null>(null);
	let outputOpen = $state(false);
	let outputLoading = $state(false);
	let outputError = $state<string | null>(null);
	// the view "Restart when idle" was chosen from, and goes back to on Cancel
	let waitingFrom = $state<View>('confirm');

	// the row the draft was last rebased on, and run counters against stale answers
	let syncedFrom: ApiAdminModelSettingsRow | null = null;
	let openRun = 0;
	let checkController: AbortController | null = null;
	// the debounced check after an edit; cleared whenever the inputs of the check change
	let checkTimer: ReturnType<typeof setTimeout> | null = null;
	// a repeat of the same check (memory was settling, another check was running): it must
	// survive list refreshes, which re-run the check effect without changing what is checked
	let recheckTimer: ReturnType<typeof setTimeout> | null = null;
	// the inputs whose check is scheduled, running or waiting for its repeat: a re-run of the
	// check effect with the same inputs (a list refresh) neither restarts nor drops it
	let checkPendingKey: string | null = null;
	let outputRun = 0;
	// saved values of the last save that started the model (what a failure failed with)
	let attempted: Record<string, string> | null = null;
	let idleTimer: ReturnType<typeof setTimeout> | null = null;
	let clockTimer: ReturnType<typeof setInterval> | null = null;
	let feedStop: (() => void) | null = null;
	let fileInput = $state<HTMLInputElement | null>(null);

	let schema = $derived(adminStore.modelSettings?.schema ?? []);
	let devices = $derived(adminStore.buildDevices);
	let hasGpus = $derived(devices.length > 0);
	let deviceDescriptions = $derived(
		Object.fromEntries((adminStore.gpus?.gpus ?? []).map((gpu) => [gpu.device, gpu.description]))
	);
	let liveStatus = $derived(
		modelId ? (modelsStore.getModelStatus(modelId) ?? row?.status ?? null) : null
	);
	let running = $derived(isRunningStatus(liveStatus));
	let failedNow = $derived.by(() => {
		if (!modelId || liveStatus !== ServerModelStatus.UNLOADED) return false;

		const seen = adminStore.exitCodes.get(modelId);

		return seen !== undefined ? seen !== 0 : (row?.last_run?.failed ?? false);
	});
	let update = $derived(draft && row ? buildModelSettingsUpdate(draft, row) : {});
	let dirty = $derived(Object.keys(update).length > 0);
	// the change only pins values the model gets already: saved without a restart or a load
	let pinOnly = $derived(
		dirty && draft !== null && row !== null && !draftChangesEffect(draft, row)
	);
	let validation = $derived(
		draft && row
			? validateModelSettingsDraft(draft, row, schema, devices)
			: { errors: {}, valid: true }
	);
	let errors = $derived({ ...validation.errors, ...serverErrors });
	let warnings = $derived(draft && row ? draftWarnings(row, draft) : []);
	let readOnly = $derived(adminStore.modelSettings?.state?.read_only ?? false);
	let hasCustom = $derived(Object.keys(row?.overrides ?? {}).length > 0);
	let canSave = $derived(dirty && validation.valid && !saving && !readOnly);
	let fitCheckAvailable = $derived(adminStore.hasFeature(AdminFeature.FIT_CHECK));
	let checkKey = $derived(row ? `${row.id}|${JSON.stringify(update)}` : '');
	let formula = $derived.by(() => {
		if (!row || !draft) return null;

		const current = row;
		const currentDraft = draft;

		return estimateFitByFormula({
			context: draftContext(current, currentDraft),
			devices,
			facts: current.facts,
			gpus: fitGpuStates(adminStore.gpus, current.id),
			margin: adminStore.gpus?.margin,
			value: (key) => draftSettingValue(current, currentDraft, key)
		});
	});
	let checkResult = $derived(check && check.key === checkKey ? fitFromCheck(check.result) : null);
	// the check could not run (timeout, helper failed): it judged nothing, the estimate stands in
	let checkNotRun = $derived(
		!!checkResult?.error &&
			fitCheckErrorKind(checkResult.error.code) === AdminFitErrorKind.NOT_CHECKED
	);
	let fit = $derived(checkNotRun ? (formula ?? checkResult) : (checkResult ?? formula));
	let checkNote = $derived.by(() => {
		if (!fitCheckAvailable) {
			return 'This server has no exact fit check: the figures are a rough estimate from the model file.';
		}

		if (!validation.valid) return 'Fix the highlighted fields to check the fit.';

		if (checkNotRun && formula && checkResult?.error) {
			return `The exact check could not run (${checkResult.error.message}); showing a rough estimate.`;
		}

		if (checkError && !checkResult)
			return `The exact check failed (${checkError}); showing a rough estimate.`;

		return null;
	});
	let revertAction = $derived(row ? revertPlan(row, previous, failedSettings) : null);
	// which instance "Show output" shows: the one that is starting, or still the one before it
	let outputCaption = $derived.by(() => {
		const instance = output?.instance;

		if (!instance) return null;

		const started = new Date(instance.started).toLocaleTimeString();
		const dropped = output?.truncated ? ' · the first lines were dropped' : '';

		return `${instance.running ? 'Instance that is starting' : 'Last instance (not running)'}, started ${started}${dropped}`;
	});
	let fitIsNo = $derived(fit?.verdict === AdminFitVerdict.NO);
	let phase = $derived(tracker ? restartPhase(tracker) : null);
	let loadProgress = $derived(modelId ? modelsStore.status.getLoadProgress(modelId) : null);
	let slow = $derived(
		(phase === AdminRestartPhase.STARTING || phase === AdminRestartPhase.LOADING) &&
			now - progressSince > ADMIN_SLOW_LOAD_MS
	);
	let resetLines = $derived(
		row ? describeResetChanges(row, { hasGpus, nLayer: row.facts?.n_layer }) : []
	);
	let factsLine = $derived.by(() => {
		const facts = row?.facts;

		if (!facts) {
			return row?.facts_status === 'pending' || loading ? 'Reading the model file...' : null;
		}

		return [
			facts.arch,
			`${facts.n_layer} layers`,
			`trained for ${formatTokenCount(facts.n_ctx_train)} tokens`,
			formatBytes(facts.file_bytes),
			facts.quant
		]
			.filter(Boolean)
			.join(' · ');
	});

	// --- opening and following the server ---------------------------------------------------

	$effect(() => {
		const id = modelId;

		if (!open || !id) return;

		untrack(() => void openFor(id));
	});

	// background refreshes of the list rebase the draft: untouched keys follow the server
	$effect(() => {
		const id = modelId;
		const listRow = id ? adminStore.modelSettingsRow(id) : null;

		if (!open || !listRow) return;

		// a rebase onto the same values changes nothing, so every list change is taken
		untrack(() => {
			if (loading) return;

			adopt({ ...listRow, facts: listRow.facts ?? row?.facts ?? null });
		});
	});

	// the exact check runs after the last edit, and at once when the dialog opens
	$effect(() => {
		const key = checkKey;
		const valid = validation.valid;
		const settings = update;
		// also while the restart is being confirmed: its "won't fit" line uses the result
		const active =
			open && !!row && (view === 'edit' || view === 'confirm') && fitCheckAvailable && valid;

		untrack(() => {
			if (!active) {
				// nothing is checked while another view is shown; coming back schedules it again
				cancelScheduledCheck();

				return;
			}

			// the same inputs: a final answer is there, or the check is on its way (the
			// effect re-runs on every list refresh, which must not restart or drop it)
			const answered = check?.key === key && !isSettling(check.result);

			if (answered || checkPendingKey === key) return;

			cancelScheduledCheck();
			checkPendingKey = key;
			checkTimer = setTimeout(
				() => {
					checkTimer = null;
					void runCheck(key, settings, 0);
				},
				check === null && !checkRunning ? 0 : ADMIN_FIT_CHECK_DEBOUNCE_MS
			);
		});
	});

	$effect(() => {
		if (phase === AdminRestartPhase.READY) untrack(() => void onReady());
		else if (phase === AdminRestartPhase.FAILED) untrack(() => void onFailed());
	});

	onMount(() => {
		feedStop = modelsStore.status.onEvent((event) => handleFeed(event));
		clockTimer = setInterval(() => (now = Date.now()), 5_000);
	});

	onDestroy(() => {
		feedStop?.();
		stopTimers();
	});

	function stopTimers() {
		if (clockTimer) clearInterval(clockTimer);

		cancelScheduledCheck();

		if (idleTimer) clearTimeout(idleTimer);

		checkController?.abort();
	}

	function clearRecheck() {
		if (recheckTimer) clearTimeout(recheckTimer);

		recheckTimer = null;
	}

	/** Drop a scheduled check or repeat (a running request ends on its own). */
	function cancelScheduledCheck() {
		if (checkTimer) clearTimeout(checkTimer);

		checkTimer = null;
		clearRecheck();
		checkPendingKey = null;
	}

	/** Memory of a GPU of the answer was settling: the verdict is not final yet. */
	function isSettling(result: ApiAdminFitCheck): boolean {
		return result.ok && (result.devices ?? []).some((device) => device.settling);
	}

	function resetState() {
		row = null;
		draft = null;
		syncedFrom = null;
		loadError = null;
		view = 'edit';
		saving = false;
		saveError = null;
		serverErrors = {};
		pending = null;
		slots = null;
		idleMessage = null;
		tracker = null;
		previous = null;
		failedSettings = [];
		attempted = null;
		failure = null;
		check = null;
		checkError = null;
		checkRunning = false;
		importPreview = null;
		importError = null;
		importConfirmed = false;
		closeOutput();
		checkController?.abort();
		cancelScheduledCheck();

		if (idleTimer) clearTimeout(idleTimer);
	}

	async function openFor(id: string) {
		const run = ++openRun;

		resetState();

		const listed = adminStore.modelSettingsRow(id);

		if (listed) adopt(listed);

		loading = true;

		try {
			const fresh = await adminStore.loadModelSettingsRow(id);

			if (run !== openRun) return;

			if (fresh?.exists) adopt(fresh);
			else loadError = 'This model is no longer in the list.';
		} catch (error) {
			if (run === openRun && !row)
				loadError = error instanceof Error ? error.message : String(error);
		} finally {
			if (run === openRun) loading = false;
		}
	}

	/** Take a newer row: a fresh draft the first time, a rebased one afterwards. */
	function adopt(next: ApiAdminModelSettingsRow) {
		if (!draft || !syncedFrom) {
			draft = createModelSettingsDraft(next);
		} else {
			draft = rebaseModelSettingsDraft(draft, syncedFrom, next);
		}

		// the server saw the model fail with its saved values (e.g. before this dialog was
		// opened): Revert never goes back to them
		if (next.exists && next.phase === 'failed') {
			failedSettings = withFailedSettings(failedSettings, { ...(next.overrides ?? {}) });
		}

		syncedFrom = next;
		row = next;
	}

	function handleOpenChange(value: boolean) {
		if (!value) {
			openRun++;
			resetState();
		}

		open = value;
		onOpenChange?.(value);
	}

	function handleFeed(event: ApiModelsSseEvent) {
		if (!tracker || event.model !== modelId || !event.data?.status) return;

		tracker = trackRestartStatus(tracker, event.data.status, event.data.exit_code ?? null);
	}

	function setDraft(next: AdminModelSettingsDraft) {
		draft = next;
		serverErrors = {};
		saveError = null;
	}

	// --- exact fit check ----------------------------------------------------------------------

	async function runCheck(key: string, settings: Record<string, string | null>, attempt: number) {
		if (!row) return;

		checkController?.abort();

		const controller = new AbortController();

		checkController = controller;
		checkPendingKey = key;
		checkRunning = true;
		checkError = null;

		// a repeat of this check is scheduled: it stays pending
		let again = false;

		try {
			const result = await adminStore.checkModelSettings(row.id, settings, controller.signal);

			if (controller !== checkController) return;

			check = { key, result };

			// memory was settling: ask again once it has settled (the effect drops the repeat
			// when the inputs change or another view is shown, and schedules it again on return)
			if (isSettling(result)) {
				again = true;
				scheduleRecheck(() => {
					if (checkPendingKey === key) void runCheck(key, settings, 0);
				}, ADMIN_GPU_REFRESH.AFTER_STATUS_MS + 1_000);
			}
		} catch (error) {
			if (controller !== checkController) return;

			if (error instanceof AdminApiError && error.isCancelled) return;

			if (
				error instanceof AdminApiError &&
				error.type === AdminErrorType.CHECK_BUSY &&
				attempt < ADMIN_FIT_CHECK_BUSY_RETRY.MAX
			) {
				again = true;
				scheduleRecheck(() => {
					if (checkPendingKey === key) void runCheck(key, settings, attempt + 1);
				}, ADMIN_FIT_CHECK_BUSY_RETRY.DELAY_MS);

				return;
			}

			checkError = error instanceof Error ? error.message : String(error);
		} finally {
			if (controller === checkController) {
				checkRunning = false;

				// done: a later re-run of the effect with these inputs may check again
				// (after an error) or keeps the answer
				if (!again && checkPendingKey === key) checkPendingKey = null;
			}
		}
	}

	function scheduleRecheck(run: () => void, delay: number) {
		clearRecheck();
		recheckTimer = setTimeout(() => {
			recheckTimer = null;
			run();
		}, delay);
	}

	// --- saving --------------------------------------------------------------------------------

	/** Save (stopped model), Save and load (stopped / failed), Save and restart (running). */
	async function requestSave(restart: boolean) {
		if (!row || !draft || !validation.valid || saving) return;

		// pinning what the model gets already changes nothing now: no restart, no load
		pending = { restart: restart && !pinOnly, settings: { ...update } };

		if (!pending.restart) {
			await send();

			return;
		}

		if (restart && running) {
			await openRestartConfirm();

			return;
		}

		if (restart && fitIsNo) {
			view = 'confirm';

			return;
		}

		await send();
	}

	async function openRestartConfirm() {
		view = 'confirm';
		idleMessage = null;
		slots = null;
		slotsLoading = true;

		try {
			slots = modelId ? await adminStore.getSlotsBusy(modelId) : null;
		} finally {
			slotsLoading = false;
		}
	}

	/**
	 * "Restart when idle": wait until no slot is busy (at most two minutes), then save.
	 * Only an answer with no busy slot restarts: when the slots cannot be read (timeout,
	 * the server busy), it keeps asking, and after the deadline it asks the operator again.
	 */
	function restartWhenIdle() {
		const id = modelId;
		const deadline = Date.now() + ADMIN_RESTART_WHEN_IDLE.MAX_MS;

		if (!id) return;

		waitingFrom = view === 'reset' ? 'reset' : 'confirm';
		view = 'waiting';
		idleMessage = null;

		const poll = async () => {
			if (view !== 'waiting') return;

			slots = await adminStore.getSlotsBusy(id);

			if (view !== 'waiting') return;

			// no busy slot, or the model stopped meanwhile (nothing left to interrupt)
			if ((slots && slots.busy === 0) || !running) {
				await send();

				return;
			}

			if (Date.now() >= deadline) {
				view = waitingFrom;
				idleMessage = slots
					? `Still ${slots.busy} of ${slots.total} slots busy after two minutes.`
					: 'The busy slots could not be read for two minutes, so the model was not restarted.';

				return;
			}

			idleTimer = setTimeout(() => void poll(), ADMIN_RESTART_WHEN_IDLE.POLL_MS);
		};

		void poll();
	}

	function cancelWaiting() {
		if (idleTimer) clearTimeout(idleTimer);

		view = waitingFrom === 'reset' ? 'reset' : 'edit';
	}

	async function send() {
		const target = row;
		const save = pending;

		if (!target || !save) return;

		const body: ApiAdminModelSettingsUpdate = save.reset
			? { model: target.id, reset: true, restart: save.restart }
			: { model: target.id, restart: save.restart, settings: save.settings ?? {} };

		saving = true;
		saveError = null;
		serverErrors = {};
		failure = null;

		if (save.restart) {
			closeOutput(); // the output of an earlier start in this dialog
			stopsRunning = running;
			tracker = createRestartTracker();
			progressSince = Date.now();
			now = Date.now();
			view = 'progress';
		}

		try {
			const result = await adminStore.saveModelSettings(body);

			if (!save.revert) previous = result.previous;

			attempted = result.model?.exists ? { ...result.model.overrides } : null;

			if (result.model?.exists) {
				syncedFrom = null;
				draft = null;
				adopt(result.model);
			}

			for (const line of describeSaveResult(result)) {
				adminStore.notice(
					result.result.load_error || result.result.not_restarted.length > 0
						? AdminNoticeLevel.WARNING
						: AdminNoticeLevel.INFO,
					`${target.id}: ${line}`
				);
			}

			if (save.restart && result.result.restarted) {
				await settleAfterRestart();

				return;
			}

			tracker = null;
			view = 'edit';

			if (!result.result.changed && !save.reset) {
				const stored = !sameOverrides(target.overrides, result.model?.overrides);

				if (!stored) {
					toast.info('Nothing to change: the model already gets these values');

					return;
				}

				toast.success(
					`Settings of ${target.id} saved. The model already gets these values, so nothing restarts.`
				);
				handleOpenChange(false);

				return;
			}

			toast.success(
				save.reset ? `Custom settings of ${target.id} removed` : `Settings of ${target.id} saved`
			);

			if (!result.result.load_error) handleOpenChange(false);
		} catch (error) {
			tracker = null;
			handleSaveError(error);
		} finally {
			saving = false;
		}
	}

	/**
	 * The POST returned with the model started. Its first statuses may have
	 * come before the answer: read the list once to know where it is now.
	 */
	async function settleAfterRestart() {
		if (!tracker || !modelId) return;

		tracker = { ...tracker, saving: false };

		await adminStore.refreshModels();

		if (!tracker || tracker.ready || tracker.failed) return;

		const status = modelsStore.getModelStatus(modelId);

		if (status === ServerModelStatus.LOADED || status === ServerModelStatus.SLEEPING) {
			tracker = { ...tracker, ready: true };
		} else if (status === ServerModelStatus.LOADING) {
			tracker = { ...tracker, sawStart: true };
		} else if (
			status === ServerModelStatus.UNLOADED &&
			(adminStore.exitCodes.get(modelId) ?? 0) !== 0
		) {
			tracker = { ...tracker, exitCode: adminStore.exitCodes.get(modelId) ?? null, failed: true };
		}
	}

	function handleSaveError(error: unknown) {
		view = 'edit';

		if (!(error instanceof AdminApiError)) {
			saveError = error instanceof Error ? error.message : String(error);

			return;
		}

		switch (error.type) {
			case AdminErrorType.RESTART_REQUIRED:
				// it started meanwhile: the change needs a restart now
				if (pending) pending = { ...pending, restart: true };

				void openRestartConfirm();

				return;
			case AdminErrorType.FIELD_LOCKED:
				serverErrors = { [error.param ?? 'form']: error.message };

				if (modelId)
					void adminStore
						.loadModelSettingsRow(modelId)
						.then(adopt)
						.catch(() => {});

				return;
			case AdminErrorType.STOP_TIMEOUT:
				saveError = `${error.message} Nothing was saved. If the GPU has failed, restart the server or the host.`;

				return;
			case AdminErrorType.MODEL_SETTINGS_READ_ONLY:
				saveError = error.message;

				return;
		}

		if (error.status === 400 && error.param) {
			serverErrors = { [error.param]: error.message };
			saveError = error.message;

			return;
		}

		if (error.status === 0) {
			saveError = `${error.message}: saving took longer than expected. Check the model status in the list.`;
			void adminStore.refreshModels();
			void adminStore.loadModelSettings();

			return;
		}

		saveError = error.message;
	}

	async function onReady() {
		if (!modelId) return;

		toast.success(`${modelId} is ready with the new settings`);
		void adminStore.refreshModels();
		void adminStore.loadGpus(true);

		try {
			adopt(await adminStore.loadModelSettingsRow(modelId));
		} catch {
			// the list refresh catches up
		}
	}

	async function onFailed() {
		const id = modelId;

		if (!id) return;

		const exitCode = tracker?.exitCode ?? null;

		failure = { exitCode, lines: [], reason: null, reverted: false, time: null, truncated: false };
		view = 'failure';

		// the values of the save that just failed
		if (attempted) failedSettings = withFailedSettings(failedSettings, attempted);

		try {
			const log = await adminStore.loadModelLog(id);
			// a snapshot from before this start (kept across retries) is not this failure:
			// then the output of the instance that just stopped tells what happened
			const last = isInstanceAfterFailure(log.instance, log.last_failure) ? null : log.last_failure;

			failure = {
				exitCode: last?.exit_code ?? exitCode,
				lines: last?.lines ?? log.lines,
				reason: last?.reason ?? log.reason,
				reverted: last?.settings_reverted ?? false,
				time: last?.time ?? null,
				truncated: last?.truncated ?? log.truncated
			};
		} catch {
			// the panel still shows the exit code
		}

		try {
			adopt(await adminStore.loadModelSettingsRow(id));
		} catch {
			// keep the row we have
		}
	}

	/** Put back the settings before the failed save, or those that last worked (see revertPlan). */
	async function revert() {
		if (!row) return;

		pending = {
			restart: true,
			revert: true,
			settings: revertPlan(row, previous, failedSettings).settings
		};
		await send();
	}

	function backToEdit() {
		tracker = null;
		failure = null;
		view = 'edit';
		check = null;
	}

	function sameOverrides(
		left: Record<string, string> | undefined,
		right: Record<string, string> | undefined
	): boolean {
		const a = left ?? {};
		const b = right ?? {};
		const keys = Object.keys(a);

		return keys.length === Object.keys(b).length && keys.every((key) => a[key] === b[key]);
	}

	// --- output while a start takes long --------------------------------------------------------

	/** Show the output of the starting instance in the progress view (it stays there). */
	async function toggleOutput() {
		if (outputOpen) {
			closeOutput();

			return;
		}

		outputOpen = true;
		await loadOutput();
	}

	async function loadOutput() {
		const id = modelId;

		if (!id) return;

		const run = ++outputRun;

		outputLoading = true;
		outputError = null;

		try {
			const log = await adminStore.loadModelLog(id);

			if (run === outputRun) output = log;
		} catch (error) {
			if (run === outputRun) outputError = error instanceof Error ? error.message : String(error);
		} finally {
			if (run === outputRun) outputLoading = false;
		}
	}

	function closeOutput() {
		outputRun++;
		outputOpen = false;
		outputLoading = false;
		outputError = null;
		output = null;
	}

	function requestReset() {
		view = 'reset';
		pending = { reset: true, restart: running };
		idleMessage = null;
		slots = null;

		if (running) void openRestartSlots();
	}

	async function openRestartSlots() {
		slotsLoading = true;

		try {
			slots = modelId ? await adminStore.getSlotsBusy(modelId) : null;
		} finally {
			slotsLoading = false;
		}
	}

	// --- export / import -----------------------------------------------------------------------

	function exportSettings(mode: AdminExportMode) {
		if (!row) return;

		const text = JSON.stringify(buildSettingsExport(row, mode), null, 2) + '\n';
		const blob = new Blob([text], { type: 'application/json' });
		const url = URL.createObjectURL(blob);
		const link = document.createElement('a');

		link.href = url;
		link.download = settingsFileName(row.id);
		document.body.appendChild(link);
		link.click();
		document.body.removeChild(link);
		URL.revokeObjectURL(url);
	}

	async function handleImportFile(event: Event) {
		const input = event.currentTarget as HTMLInputElement;
		const file = input.files?.[0];

		input.value = '';

		if (!file || !row) return;

		importError = null;

		if (file.size > ADMIN_SETTINGS_FILE.MAX_BYTES) {
			importError = `The file is larger than ${ADMIN_SETTINGS_FILE.MAX_BYTES / 1024} KiB: it is not a model settings file.`;
			view = 'import';
			importPreview = null;

			return;
		}

		const parsed = parseSettingsFile(await file.text());

		view = 'import';

		if (!parsed.ok) {
			importPreview = null;
			importError = parsed.error;

			return;
		}

		importConfirmed = false;

		try {
			importPreview = buildImportPreview(parsed.file, row, schema, devices);
		} catch (error) {
			// the file is untrusted: whatever it holds, it is refused with a message
			importPreview = null;
			importError = `the file could not be read (${error instanceof Error ? error.message : String(error)})`;
		}
	}

	function toggleImportField(key: string, selected: boolean) {
		if (!importPreview) return;

		importPreview = {
			...importPreview,
			fields: importPreview.fields.map((field) =>
				field.key === key ? { ...field, selected } : field
			)
		};
	}

	function applyImport() {
		if (!importPreview || !draft || !row) return;

		setDraft(applyImportToDraft(draft, row, importPreview, devices));
		importPreview = null;
		view = 'edit';
		toast.info('Loaded into the form. Nothing is saved until you press Save.');
	}

	function statusLabel(status: string | null): string {
		return status ? (ADMIN_MODEL_STATUS_LABELS[status as ServerModelStatus] ?? status) : '';
	}
</script>

{#snippet progressStep(label: string, state: 'done' | 'active' | 'todo')}
	<li
		class={[
			'flex items-center gap-2 text-sm',
			state === 'todo' && 'text-muted-foreground',
			state === 'active' && 'font-medium'
		]}
		data-step-state={state}
	>
		{#if state === 'done'}
			<CircleCheck class="h-4 w-4 text-green-600 dark:text-green-400" />
		{:else if state === 'active'}
			<Loader2 class="h-4 w-4 animate-spin" />
		{:else}
			<span class="h-4 w-4 rounded-full border"></span>
		{/if}

		{label}
	</li>
{/snippet}

<Dialog.Root onOpenChange={handleOpenChange} {open}>
	<!-- the dialog itself takes the focus on open: otherwise the close button of the header
	     (the first focusable element) gets it and opens with its focus ring drawn around it -->
	<Dialog.Content
		bind:ref={contentNode}
		class="flex flex-col gap-4 outline-none max-md:h-[100dvh]! max-md:w-screen! max-md:max-w-none! max-md:rounded-none! md:max-h-[90dvh]! md:w-[calc(100vw-4rem)]! md:max-w-3xl!"
		data-testid="admin-model-settings"
		onOpenAutoFocus={(event) => {
			event.preventDefault();
			contentNode?.focus();
		}}
		onfocusin={(event) => keepFocusOffDialogClose(event, contentNode)}
		tabindex={-1}
	>
		<Dialog.Header class="gap-1 text-left">
			<Dialog.Title class="flex items-center gap-2 pr-6">
				<SlidersHorizontal class="h-5 w-5 shrink-0" />

				<span>Model settings</span>
			</Dialog.Title>

			<Dialog.Description class="flex flex-wrap items-center gap-2">
				<span class="font-mono text-[13px] break-all text-foreground">{modelId}</span>

				{#if row?.source}
					<Badge class="font-normal" variant="outline">
						{ADMIN_MODEL_SOURCE_LABELS[row.source] ?? row.source}
					</Badge>
				{/if}

				{#if liveStatus}
					<Badge class="font-normal" data-testid="ms-status" variant="outline">
						{failedNow ? 'Failed' : statusLabel(liveStatus)}
					</Badge>
				{/if}

				{#if hasCustom}
					<Badge class="font-normal" variant="secondary">custom settings</Badge>
				{/if}

				{#if row?.phase && row.phase !== 'committed'}
					<Badge class="font-normal" title="State of the saved settings" variant="outline">
						{row.phase === 'pending'
							? 'not tried yet'
							: row.phase === 'trial'
								? 'starting with them'
								: 'last start failed'}
					</Badge>
				{/if}
			</Dialog.Description>

			{#if factsLine}
				<p class="text-xs text-muted-foreground" data-testid="ms-facts">{factsLine}</p>
			{/if}
		</Dialog.Header>

		<div class="-mx-6 min-h-0 flex-1 overflow-y-auto px-6">
			{#if !row}
				<div class="flex items-center justify-center gap-2 py-12 text-sm text-muted-foreground">
					{#if loadError}
						<span class="text-destructive">The settings could not be read: {loadError}</span>
					{:else}
						<Loader2 class="h-4 w-4 animate-spin" />

						Reading the settings...
					{/if}
				</div>
			{:else if view === 'progress'}
				{@const fraction = loadProgress ? modelLoadFraction(loadProgress) : null}

				<div class="flex flex-col gap-4 py-2" data-phase={phase} data-testid="ms-progress">
					<ol class="flex flex-col gap-2">
						{@render progressStep(
							stopsRunning ? 'Saving and stopping the running instance' : 'Saving',
							phase === AdminRestartPhase.SAVING ? 'active' : 'done'
						)}

						{@render progressStep(
							'Starting',
							phase === AdminRestartPhase.STARTING
								? 'active'
								: phase === AdminRestartPhase.SAVING
									? 'todo'
									: 'done'
						)}

						{@render progressStep(
							`Loading${fraction !== null && phase === AdminRestartPhase.LOADING ? ` ${Math.round(fraction * 100)}%` : ''}`,
							phase === AdminRestartPhase.LOADING
								? 'active'
								: phase === AdminRestartPhase.READY
									? 'done'
									: 'todo'
						)}

						{@render progressStep('Ready', phase === AdminRestartPhase.READY ? 'done' : 'todo')}
					</ol>

					{#if phase === AdminRestartPhase.LOADING && loadProgress}
						<p class="text-xs text-muted-foreground">{modelLoadProgressText(loadProgress)}</p>
					{/if}

					{#if phase === AdminRestartPhase.READY}
						<p
							class="flex items-center gap-2 rounded-md border border-green-500/40 bg-green-500/10 p-3 text-sm text-green-700 dark:text-green-400"
							data-testid="ms-ready"
						>
							<CircleCheck class="h-4 w-4" />

							{modelId} is running with the new settings.
						</p>
					{/if}

					{#if slow}
						<p class="text-sm text-amber-600 dark:text-amber-400">
							Still loading... Large models can take minutes. The output of the model shows what it
							is doing.
						</p>

						<div class="flex flex-wrap items-center gap-2">
							<Button
								aria-expanded={outputOpen}
								data-testid="ms-show-output"
								onclick={() => void toggleOutput()}
								size="sm"
								variant="outline"
							>
								{outputOpen ? 'Hide output' : 'Show output'}
							</Button>

							{#if outputOpen}
								<Button
									disabled={outputLoading}
									onclick={() => void loadOutput()}
									size="sm"
									variant="ghost"
								>
									<RefreshCw class={outputLoading ? 'animate-spin' : ''} />

									Refresh
								</Button>
							{/if}
						</div>
					{/if}

					{#if outputOpen}
						<!-- the output of the instance that is starting; the start goes on -->
						{#if outputError}
							<p class="text-sm text-destructive">The output could not be read: {outputError}</p>
						{:else if output}
							{#if outputCaption}
								<p class="text-xs text-muted-foreground" data-testid="ms-output-instance">
									{outputCaption}
								</p>
							{/if}

							<pre
								class="max-h-72 overflow-auto rounded-md border bg-muted/40 p-2 font-mono text-[11px] leading-relaxed break-all whitespace-pre-wrap"
								data-testid="ms-output">{output.instance && output.lines.length > 0
									? output.lines.join('\n')
									: '(no output yet)'}</pre>
						{:else}
							<p class="flex items-center gap-2 text-sm text-muted-foreground">
								<Loader2 class="h-4 w-4 animate-spin" />

								Reading the output...
							</p>
						{/if}
					{/if}
				</div>
			{:else if view === 'failure' && failure}
				<AdminModelFailure
					exitCode={failure.exitCode}
					lines={failure.lines}
					modelId={modelId ?? ''}
					openLog
					reason={failure.reason}
					showRisk={!row.reverted}
					time={failure.time}
					truncated={failure.truncated}
				/>

				{#if row.reverted}
					<p class="mt-3 text-sm text-amber-600 dark:text-amber-400" data-testid="ms-reverted">
						After {row.reverted.failures} failed starts the server put back the settings that worked before.
					</p>
				{/if}
			{:else if view === 'import'}
				{#if importError}
					<div
						class="rounded-md border border-destructive/40 bg-destructive/10 p-3 text-sm text-destructive"
						data-testid="admin-import-error"
						role="alert"
					>
						The file cannot be imported: {importError}
					</div>
				{:else if importPreview}
					<AdminSettingsImportPreview
						confirmed={importConfirmed}
						{hasGpus}
						modelId={row.id}
						nCtxTrain={row.facts?.n_ctx_train ?? null}
						onConfirmedChange={(value) => (importConfirmed = value)}
						onToggle={toggleImportField}
						preview={importPreview}
					/>
				{/if}
			{:else if view === 'reset'}
				<div class="flex flex-col gap-3" data-testid="ms-reset">
					<p class="text-sm">
						Remove the custom settings of {row.id}? It goes back to the values from models.ini or
						inherited from the server:
					</p>

					<ul class="list-disc space-y-1 pl-6 text-sm">
						{#each resetLines as line (line)}
							<li>{line}</li>
						{/each}
					</ul>

					{#if running}
						<p class="text-sm text-amber-600 dark:text-amber-400" data-testid="ms-reset-slots">
							The model is running: it is stopped (requests in progress are interrupted) and started
							with these values.
							{#if slotsLoading}
								Checking the busy slots...
							{:else if slots}
								{slots.busy} of {slots.total} slot{slots.total === 1 ? '' : 's'} busy right now.
							{:else}
								Busy slots unknown.
							{/if}
						</p>
					{/if}

					{#if idleMessage}
						<p class="text-sm text-amber-600 dark:text-amber-400">{idleMessage}</p>
					{/if}
				</div>
			{:else}
				<div class="flex flex-col gap-5 pb-2">
					{#if readOnly}
						<div
							class="rounded-md border border-destructive/40 bg-destructive/10 p-3 text-sm text-destructive"
							role="alert"
						>
							Settings cannot be saved: the settings file of the server could not be read ({adminStore
								.modelSettings?.state.error || 'read-only'}). Fix or remove it on the server.
						</div>
					{/if}

					{#if row.reverted}
						<div
							class="flex items-start gap-2 rounded-md border border-amber-500/40 bg-amber-500/10 p-3 text-sm text-amber-700 dark:text-amber-400"
							data-testid="ms-reverted"
						>
							<RotateCcw class="mt-0.5 h-4 w-4 shrink-0" />

							<span>
								{row.reverted_at_startup
									? 'When the server started, the settings saved last had not started successfully, so the server put back the settings that worked before.'
									: `The settings saved last failed to start ${row.reverted.failures} times, so the server put back the settings that worked before.`}
								{#if row.reverted.message}
									Reason: {row.reverted.message}.
								{/if}

								{#if row.reverted.time}
									({new Date(row.reverted.time).toLocaleString()})
								{/if}
							</span>
						</div>
					{/if}

					{#if row.last_failure && failedNow && view === 'edit'}
						<AdminModelFailure
							exitCode={row.last_failure.exit_code}
							lines={row.last_failure.lines}
							modelId={row.id}
							reason={row.last_failure.reason}
							time={row.last_failure.time}
							truncated={row.last_failure.truncated}
						/>
					{/if}

					{#each row.warnings.filter( (code) => ['device_missing', 'device_moved', 'model_devices_not_applied', 'apply_error', 'default_device_not_present'].includes(code) ) as code (code)}
						<p
							class="flex items-start gap-2 text-sm text-amber-600 dark:text-amber-400"
							data-warning={code}
						>
							<TriangleAlert class="mt-0.5 h-4 w-4 shrink-0" />

							<span>{describeModelWarning(code, row)}</span>
						</p>
					{/each}

					{#if draft}
						<AdminModelSettingsForm
							{deviceDescriptions}
							{devices}
							disabled={saving || readOnly}
							{draft}
							{errors}
							onDraftChange={setDraft}
							{row}
							{schema}
						/>
					{/if}

					<AdminFitIndicator {checkNote} checking={checkRunning} result={fit} />

					{#each warnings as code (code)}
						<p
							class="flex items-start gap-2 text-xs text-amber-600 dark:text-amber-400"
							data-warning={code}
						>
							<TriangleAlert class="mt-0.5 h-3.5 w-3.5 shrink-0" />

							<span>{describeModelWarning(code, row)}</span>
						</p>
					{/each}
				</div>
			{/if}
		</div>

		{#if row}
			<div class="flex flex-col gap-2 border-t pt-3">
				{#if saveError}
					<p
						class="flex items-start gap-2 text-sm text-destructive"
						data-testid="ms-save-error"
						role="alert"
					>
						<TriangleAlert class="mt-0.5 h-4 w-4 shrink-0" />

						<span>{saveError}</span>
					</p>
				{/if}

				{#if errors.form}
					<p class="text-sm text-destructive" role="alert">{errors.form}</p>
				{/if}

				{#if view === 'confirm' || view === 'waiting'}
					<div class="flex flex-col gap-2 text-sm" data-testid="ms-confirm">
						{#if pending?.restart && running}
							<p>
								<span class="font-medium">{row.id}</span> will be stopped (requests in progress are
								interrupted) and started {pending.reset
									? 'without its custom settings'
									: 'with the new settings'}.
							</p>

							<p class="text-muted-foreground" data-testid="ms-slots">
								{#if slotsLoading}
									Checking the busy slots...
								{:else if slots}
									{slots.busy} of {slots.total} slot{slots.total === 1 ? '' : 's'} busy right now.
								{:else}
									Busy slots unknown.
								{/if}
							</p>
						{:else}
							<p>{row.id} will be saved and started.</p>
						{/if}

						{#if fitIsNo && fit}
							<p class="text-destructive" data-testid="ms-confirm-nofit">
								{describeFit(fit).title}.
							</p>
						{/if}

						<p class="text-xs text-muted-foreground">{ADMIN_RESTART_RISK_NOTE}</p>

						{#if idleMessage && view === 'confirm'}
							<p class="text-amber-600 dark:text-amber-400">{idleMessage}</p>
						{/if}

						{#if view === 'waiting'}
							<p class="flex items-center gap-2 text-muted-foreground" data-testid="ms-waiting">
								<Loader2 class="h-4 w-4 animate-spin" />

								Waiting until no request is running{slots
									? ` (${slots.busy} of ${slots.total} busy)`
									: ' (busy slots unknown right now)'}...
							</p>
						{/if}
					</div>
				{/if}

				<div class="flex flex-wrap items-center gap-2">
					<!-- always mounted: destroying the menu while it closes leaves it reading a dead trigger -->
					<div class={view === 'edit' ? 'contents' : 'hidden'}>
						<DropdownMenu.Root>
							<DropdownMenu.Trigger
								class={cn(buttonVariants({ size: 'sm', variant: 'ghost' }), 'gap-1.5')}
								data-testid="ms-transfer"
							>
								<ArrowLeftRight class="h-4 w-4" />

								Export / import
							</DropdownMenu.Trigger>

							<DropdownMenu.Content align="start" class="min-w-[16rem]">
								<DropdownMenu.Item onclick={() => exportSettings(AdminExportMode.CUSTOM)}>
									<Download class="h-4 w-4" />

									Export only my custom settings
								</DropdownMenu.Item>

								<DropdownMenu.Item onclick={() => exportSettings(AdminExportMode.ALL)}>
									<Download class="h-4 w-4" />

									Export all current values
								</DropdownMenu.Item>

								<DropdownMenu.Item disabled={readOnly} onclick={() => fileInput?.click()}>
									<Upload class="h-4 w-4" />

									Import from a file...
								</DropdownMenu.Item>
							</DropdownMenu.Content>
						</DropdownMenu.Root>
					</div>

					{#if view === 'edit'}
						{#if hasCustom}
							<Button
								disabled={saving || readOnly}
								onclick={requestReset}
								size="sm"
								title="Back to the models.ini / inherited values"
								variant="ghost"
							>
								<RotateCcw />

								Remove custom settings
							</Button>
						{/if}

						<div class="ml-auto flex flex-wrap items-center gap-2">
							{#if !dirty}
								<span class="text-xs text-muted-foreground">No changes</span>
							{/if}

							{#if pinOnly}
								<!-- the values are what the model gets already: saved for it, nothing restarts or loads -->
								<span class="text-xs text-muted-foreground" data-testid="ms-pin-only">
									Nothing restarts: the model gets these values already
								</span>

								<Button
									data-testid="ms-save"
									disabled={!canSave}
									onclick={() => requestSave(false)}
								>
									{#if saving}<Loader2 class="animate-spin" />{/if}

									Save
								</Button>
							{:else if running}
								<Button
									data-testid="ms-save-restart"
									disabled={!canSave}
									onclick={() => requestSave(true)}
								>
									{#if saving}<Loader2 class="animate-spin" />{/if}

									Save and restart
								</Button>
							{:else if failedNow}
								<Button disabled={!canSave} onclick={() => requestSave(false)} variant="outline">
									Save
								</Button>

								<Button
									data-testid="ms-save-load"
									disabled={!canSave}
									onclick={() => requestSave(true)}
								>
									{#if saving}<Loader2 class="animate-spin" />{/if}

									Save and load
								</Button>
							{:else}
								<Button disabled={!canSave} onclick={() => requestSave(true)} variant="outline">
									Save and load
								</Button>

								<Button
									data-testid="ms-save"
									disabled={!canSave}
									onclick={() => requestSave(false)}
								>
									{#if saving}<Loader2 class="animate-spin" />{/if}

									Save
								</Button>
							{/if}
						</div>
					{:else if view === 'confirm'}
						<Button onclick={() => (view = 'edit')} variant="ghost">Back</Button>

						<div class="ml-auto flex flex-wrap items-center gap-2">
							{#if pending?.restart && running && !slotsLoading && (!slots || slots.busy > 0)}
								<Button data-testid="ms-restart-idle" onclick={restartWhenIdle} variant="outline">
									Restart when idle
								</Button>
							{/if}

							<Button
								data-testid="ms-confirm-go"
								onclick={() => void send()}
								variant={fitIsNo ? 'destructive' : 'default'}
							>
								{#if pending?.restart && running}
									{fitIsNo ? 'Restart anyway' : 'Restart now'}
								{:else}
									{fitIsNo ? 'Save and load anyway' : 'Save and load'}
								{/if}
							</Button>
						</div>
					{:else if view === 'waiting'}
						<Button onclick={cancelWaiting} variant="ghost">Cancel</Button>

						<Button class="ml-auto" onclick={() => void send()} variant="outline">
							{pending?.reset ? 'Remove and restart now' : 'Restart now'}
						</Button>
					{:else if view === 'progress'}
						<Button
							class="ml-auto"
							data-testid="ms-close"
							onclick={() => handleOpenChange(false)}
							variant={phase === AdminRestartPhase.READY ? 'default' : 'outline'}
						>
							Close
						</Button>
					{:else if view === 'failure'}
						<Button onclick={() => handleOpenChange(false)} variant="ghost">Close</Button>

						<div class="ml-auto flex flex-wrap items-center gap-2">
							<Button onclick={backToEdit} variant="outline">Edit settings</Button>

							{#if !row.reverted}
								<Button data-testid="ms-revert" disabled={saving} onclick={() => void revert()}>
									<RotateCcw />

									{revertAction?.label ?? 'Revert to previous settings and restart'}
								</Button>
							{/if}
						</div>
					{:else if view === 'import'}
						<Button onclick={() => (view = 'edit')} variant="ghost">Cancel</Button>

						{#if importPreview}
							<Button
								class="ml-auto"
								data-testid="ms-import-apply"
								disabled={(importPreview.differentModel && !importConfirmed) ||
									!importPreview.fields.some((field) => field.selected && !field.skipped)}
								onclick={applyImport}
							>
								Load into the form
							</Button>
						{/if}
					{:else if view === 'reset'}
						<Button onclick={() => (view = 'edit')} variant="ghost">Cancel</Button>

						<div class="ml-auto flex flex-wrap items-center gap-2">
							{#if running && !slotsLoading && (!slots || slots.busy > 0)}
								<Button data-testid="ms-reset-idle" onclick={restartWhenIdle} variant="outline">
									Remove when idle
								</Button>
							{/if}

							<Button
								data-testid="ms-reset-go"
								disabled={saving}
								onclick={() => void send()}
								variant="destructive"
							>
								{running ? 'Remove and restart' : 'Remove custom settings'}
							</Button>
						</div>
					{/if}
				</div>
			</div>
		{/if}

		<input
			bind:this={fileInput}
			accept={`${ADMIN_SETTINGS_FILE.EXTENSION},.json,application/json`}
			class="hidden"
			data-testid="ms-import-file"
			onchange={handleImportFile}
			type="file"
		/>
	</Dialog.Content>
</Dialog.Root>
