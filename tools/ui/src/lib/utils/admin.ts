/**
 * Pure helpers of the admin panel: size formatting, the Hugging Face model
 * spec, pre-flight warning texts, download bookkeeping, the model table rows
 * and the settings form rules. No state and no I/O, covered by unit tests.
 */

import {
	ADMIN_DEVICE_NAME_RE,
	ADMIN_DEVICE_NONE,
	ADMIN_HF_MODEL_RE,
	ADMIN_HF_URL_PREFIX_RE
} from '$lib/constants';
import {
	AdminDownloadOutcome,
	AdminDownloadWarning,
	AdminNoticeLevel,
	ServerModelSource,
	ServerModelStatus
} from '$lib/enums';
import type {
	AdminDownload,
	AdminDownloadResult,
	AdminDownloadTotals,
	AdminDownloadWarningInfo,
	AdminModelRow,
	AdminSettingsDraft,
	AdminSettingsValidation,
	ApiAdminDownloadCheck,
	ApiAdminSettings,
	ApiAdminSettingsUpdate
} from '$lib/types/admin';
import type { ApiModelDataEntry, ApiModelsSseDownloadFile } from '$lib/types/api';

const BYTE_UNITS = ['B', 'KB', 'MB', 'GB', 'TB', 'PB'];
const BYTES_PER_UNIT = 1024;

/**
 * Human readable size with binary multiples: `1.13 MB`, `40.9 GB`, `466 GB`.
 * Three significant digits; anything that is not a finite, non-negative
 * number renders as a dash.
 */
export function formatBytes(bytes: unknown): string {
	if (typeof bytes !== 'number' || !Number.isFinite(bytes) || bytes < 0) return '—';

	let value = bytes;
	let unit = 0;

	while (value >= BYTES_PER_UNIT && unit < BYTE_UNITS.length - 1) {
		value /= BYTES_PER_UNIT;
		unit++;
	}

	if (unit === 0) return `${Math.round(value)} B`;

	// 1023.99 MB would print as "1024 MB": move on to the next unit instead
	if (value >= BYTES_PER_UNIT - 0.5 && unit < BYTE_UNITS.length - 1) {
		value /= BYTES_PER_UNIT;
		unit++;
	}

	const digits = value >= 99.95 ? 0 : value >= 9.995 ? 1 : 2;

	return `${value.toFixed(digits)} ${BYTE_UNITS[unit]}`;
}

/**
 * Turn what the user typed into the `user/repo[:QUANT]` form the server
 * expects: trims, accepts a pasted Hugging Face page URL, and upper-cases the
 * quantization part (cache tags are upper-case, so the download row keeps the
 * name the finished model will have).
 */
export function normalizeHfModelSpec(input: string): string {
	let spec = input.trim();

	if (ADMIN_HF_URL_PREFIX_RE.test(spec)) {
		// https://huggingface.co/user/repo/tree/main?x#y -> user/repo
		spec = spec
			.replace(ADMIN_HF_URL_PREFIX_RE, '')
			.split(/[?#]/)[0]
			.split('/')
			.slice(0, 2)
			.join('/');
	}

	const colon = spec.indexOf(':');

	if (colon === -1) return spec;

	return spec.slice(0, colon) + ':' + spec.slice(colon + 1).toUpperCase();
}

export function isValidHfModelSpec(spec: string): boolean {
	return ADMIN_HF_MODEL_RE.test(spec);
}

/**
 * Sentences for the warning codes of the pre-flight check, in the order the
 * server reported them. Unknown codes are passed through so a newer server
 * is never silently ignored.
 */
export function describeDownloadWarnings(check: ApiAdminDownloadCheck): AdminDownloadWarningInfo[] {
	return check.warnings.map((code) => {
		switch (code) {
			case AdminDownloadWarning.ALREADY_PRESENT:
				return {
					code,
					level: AdminNoticeLevel.INFO,
					text: 'This model is already in the cache; nothing new will be downloaded.'
				};
			case AdminDownloadWarning.HF_UNREACHABLE:
				return {
					code,
					level: AdminNoticeLevel.WARNING,
					text: 'The server could not reach Hugging Face. The download will probably fail.'
				};
			case AdminDownloadWarning.NO_GGUF_FOR_QUANT:
				return {
					code,
					level: AdminNoticeLevel.WARNING,
					text: check.quant
						? `The repository has no GGUF file for quantization ${check.quant}.`
						: 'The repository has no GGUF file to download.'
				};
			case AdminDownloadWarning.NOT_ENOUGH_SPACE:
				return {
					code,
					level: AdminNoticeLevel.WARNING,
					text: `Not enough free space: ${formatBytes(check.total_bytes)} needed, ${formatBytes(check.free_bytes)} available.`
				};
			case AdminDownloadWarning.OFFLINE_MODE:
				return {
					code,
					level: AdminNoticeLevel.WARNING,
					text: 'The server runs in offline mode, so it cannot download models.'
				};
			case AdminDownloadWarning.PRESET_REPOSITORY:
				return {
					code,
					level: AdminNoticeLevel.WARNING,
					text: 'This repository holds a preset (preset.ini): only that file is downloaded, and no model will appear in the list.'
				};
			case AdminDownloadWarning.QUANT_NOT_SPECIFIED:
				return {
					code,
					level: AdminNoticeLevel.INFO,
					text: check.resolved_quant
						? `No quantization given: the server will pick ${check.resolved_quant}.`
						: 'No quantization given: the server will pick one itself.'
				};
			case AdminDownloadWarning.REPO_NOT_FOUND:
				return {
					code,
					level: AdminNoticeLevel.WARNING,
					text: 'Repository not found. It may be misspelled, private or gated without access for this server.'
				};
			case AdminDownloadWarning.SIZE_UNKNOWN:
				return {
					code,
					level: AdminNoticeLevel.WARNING,
					text: 'The download size could not be determined, so free space was not compared.'
				};
			default:
				return { code, level: AdminNoticeLevel.WARNING, text: `Warning: ${code}` };
		}
	});
}

/**
 * Sum the per-file counters of a download_progress record. A file whose
 * total is still unknown (0) keeps the fraction at null rather than showing
 * a misleading percentage.
 */
export function sumDownloadProgress(
	files: Record<string, ApiModelsSseDownloadFile> | null | undefined
): AdminDownloadTotals {
	const entries = Object.values(files ?? {});

	let done = 0;
	let total = 0;
	let known = true;

	for (const file of entries) {
		const fileDone = Number(file?.done) || 0;
		const fileTotal = Number(file?.total) || 0;

		done += fileDone;
		total += fileTotal;

		if (fileTotal <= 0) known = false;
	}

	if (entries.length === 0 || !known || total <= 0) {
		return { done, fraction: null, total };
	}

	return { done, fraction: Math.min(1, done / total), total };
}

/** Transient rows of the download surface: not models anybody can select or load. */
export function isDownloadPlaceholderStatus(status: string | null | undefined): boolean {
	return status === ServerModelStatus.DOWNLOADING || status === ServerModelStatus.DOWNLOADED;
}

/** Ids of the real cache models in a list (download placeholders excluded). */
export function cacheModelIds(models: ApiModelDataEntry[]): string[] {
	return models
		.filter(
			(model) =>
				model.source === ServerModelSource.CACHE &&
				!isDownloadPlaceholderStatus(model.status?.value)
		)
		.map((model) => model.id);
}

/**
 * A cache model id (always `user/repo:TAG`) that a download of `name`
 * (`user/repo[:QUANT]`) can produce: same repository, compared without case
 * like Hugging Face does. The tag is not compared, the server may resolve a
 * quantization to a longer tag.
 */
export function isCacheModelOfDownload(id: string, name: string): boolean {
	const repo = name.split(':')[0].toLowerCase();
	const model = id.toLowerCase();

	return model === repo || model.startsWith(repo + ':');
}

/**
 * Decide how a download ended from the model list fetched after the final
 * feed event. Success is reported only when a new cache model of the
 * download's own repository appeared: download_finished is also sent when
 * nothing was downloaded, another download (from this page or another
 * client) may have finished meanwhile, and a download_failed that follows
 * the user's own Cancel is not a failure.
 */
export function resolveDownloadResult(
	download: Pick<AdminDownload, 'cacheIdsBefore' | 'cancelRequested' | 'name'>,
	modelsAfter: ApiModelDataEntry[],
	failed: boolean
): AdminDownloadResult {
	const before = new Set(download.cacheIdsBefore);
	const newModelIds = cacheModelIds(modelsAfter).filter(
		(id) => !before.has(id) && isCacheModelOfDownload(id, download.name)
	);

	if (newModelIds.length > 0) return { newModelIds, outcome: AdminDownloadOutcome.SUCCESS };

	if (download.cancelRequested) return { newModelIds, outcome: AdminDownloadOutcome.CANCELLED };

	return {
		newModelIds,
		outcome: failed ? AdminDownloadOutcome.FAILED : AdminDownloadOutcome.NOTHING
	};
}

/**
 * Rows of the admin model table: the public model list merged with what only
 * the admin page knows (pins, exit codes seen on the feed, download progress).
 *
 * `exitCodes` holds the exit code of the last unload seen on the feed and
 * wins over the row, whose `failed` flag dates from the last list fetch.
 */
export function buildAdminModelRows(
	models: ApiModelDataEntry[],
	context: {
		pinned?: Iterable<string>;
		exitCodes?: ReadonlyMap<string, number>;
		downloads?: ReadonlyMap<string, AdminDownload>;
	} = {}
): AdminModelRow[] {
	const pinned = new Set(context.pinned ?? []);

	return models.map((model) => {
		const raw = model.status?.value ?? ServerModelStatus.UNLOADED;
		const source = model.source ?? null;

		let status = raw;
		let exitCode: number | null = null;

		if (raw === ServerModelStatus.UNLOADED) {
			const seen = context.exitCodes?.get(model.id);
			const code = seen ?? (model.status?.failed ? (model.status.exit_code ?? 1) : 0);

			if (code !== 0) {
				status = ServerModelStatus.FAILED;
				exitCode = code;
			}
		}

		return {
			aliases: model.aliases ?? [],
			canDelete: source === ServerModelSource.CACHE && !isDownloadPlaceholderStatus(raw),
			download:
				raw === ServerModelStatus.DOWNLOADING
					? sumDownloadProgress(context.downloads?.get(model.id)?.files)
					: null,
			exitCode,
			id: model.id,
			pinned: pinned.has(model.id),
			source,
			status,
			tags: model.tags ?? []
		};
	});
}

/** Number of models that hold a slot: loaded, loading or sleeping. */
export function countRunningModels(models: ApiModelDataEntry[]): number {
	return models.filter(
		(model) =>
			model.status?.value === ServerModelStatus.LOADED ||
			model.status?.value === ServerModelStatus.LOADING ||
			model.status?.value === ServerModelStatus.SLEEPING
	).length;
}

/** The form of a device list the server stores: trimmed, `;` turned into `,`. */
export function normalizeDeviceSpec(value: string): string {
	return value
		.trim()
		.split(/[,;]/)
		.map((name) => name.trim())
		.join(',');
}

/**
 * Empty (not set), `none`, or one or more device names such as `CUDA0`
 * separated by `,` or `;` — the rule POST /admin/settings applies.
 */
export function isValidDeviceSpec(value: string): boolean {
	const spec = normalizeDeviceSpec(value);

	if (spec === '' || spec === ADMIN_DEVICE_NONE) return true;

	return spec.split(',').every((name) => ADMIN_DEVICE_NAME_RE.test(name));
}

/** Largest number of models that may be pinned: one slot must stay evictable. */
export function maxPinnedModels(modelsMax: number): number {
	return Math.max(0, modelsMax - 1);
}

export function createSettingsDraft(settings: ApiAdminSettings): AdminSettingsDraft {
	return {
		defaultDevice: settings.default_device,
		modelsMax: String(settings.models_max),
		pinned: [...settings.pinned]
	};
}

/** Same set of pinned ids, whatever the order. */
function samePins(left: string[], right: string[]): boolean {
	const a = [...new Set(left)].sort();
	const b = [...new Set(right)].sort();

	return a.length === b.length && a.every((id, index) => id === b[index]);
}

/**
 * Carry a draft over to a newer settings snapshot. A field the user did not
 * touch (it still equals `previous`) follows the server; an edited field is
 * kept, so a background refresh neither throws away what is being typed nor
 * turns stale values into changes. Pins of models that are gone are dropped.
 */
export function rebaseSettingsDraft(
	draft: AdminSettingsDraft,
	previous: ApiAdminSettings,
	next: ApiAdminSettings
): AdminSettingsDraft {
	const modelsMaxUntouched = parseModelsMax(draft.modelsMax) === previous.models_max;
	const deviceUntouched =
		normalizeDeviceSpec(draft.defaultDevice) === normalizeDeviceSpec(previous.default_device);
	const known = new Set(next.models.map((model) => model.id));

	return {
		defaultDevice: deviceUntouched ? next.default_device : draft.defaultDevice,
		modelsMax: modelsMaxUntouched ? String(next.models_max) : draft.modelsMax,
		pinned: samePins(draft.pinned, previous.pinned)
			? [...next.pinned]
			: draft.pinned.filter((id) => known.has(id))
	};
}

/** Whole number typed into the "Max loaded models" input, or null. */
export function parseModelsMax(text: string): number | null {
	const trimmed = text.trim();

	if (!/^\d+$/.test(trimmed)) return null;

	const value = Number(trimmed);

	return Number.isSafeInteger(value) ? value : null;
}

/**
 * Client-side rules of the settings form, mirroring the server: models_max
 * within the limits, a well-formed device list, fewer pins than slots. A
 * value the user did not change is not held against them (the server may
 * report a models_max outside the limits, e.g. 0 for "no limit").
 */
export function validateSettingsDraft(
	draft: AdminSettingsDraft,
	settings: ApiAdminSettings
): AdminSettingsValidation {
	const { models_max_max: max, models_max_min: min } = settings.limits;
	const modelsMaxValue = parseModelsMax(draft.modelsMax);
	const outOfRange =
		modelsMaxValue !== null &&
		modelsMaxValue !== settings.models_max &&
		(modelsMaxValue < min || modelsMaxValue > max);

	let modelsMax: string | null = null;
	let defaultDevice: string | null = null;
	let pinned: string | null = null;

	if (modelsMaxValue === null || outOfRange) {
		modelsMax = `Enter a whole number from ${min} to ${max}.`;
	}

	if (!settings.default_device_locked && !isValidDeviceSpec(draft.defaultDevice)) {
		defaultDevice =
			'Use device names such as CUDA0 separated by commas, "none" for CPU only, or leave it empty.';
	}

	// 0 or less means "no limit" on the server: nothing to compare the pins against
	if (modelsMaxValue !== null && modelsMaxValue > 0 && draft.pinned.length >= modelsMaxValue) {
		const allowed = maxPinnedModels(modelsMaxValue);

		pinned =
			allowed === 0
				? 'At least one slot must stay evictable: with a single slot no model can be pinned.'
				: `At least one slot must stay evictable: pin at most ${allowed} model${allowed === 1 ? '' : 's'} or raise "Max loaded models".`;
	}

	return {
		defaultDevice,
		modelsMax,
		pinned,
		valid: modelsMax === null && defaultDevice === null && pinned === null
	};
}

/**
 * The POST /admin/settings body for a draft: only the fields that differ
 * from the saved settings. `default_device` is never sent while it is locked
 * by MODEL_DEVICES (any other value would be rejected with 409).
 */
export function buildSettingsUpdate(
	draft: AdminSettingsDraft,
	settings: ApiAdminSettings
): ApiAdminSettingsUpdate {
	const update: ApiAdminSettingsUpdate = {};
	const modelsMax = parseModelsMax(draft.modelsMax);

	if (modelsMax !== null && modelsMax !== settings.models_max) {
		update.models_max = modelsMax;
	}

	if (!settings.default_device_locked) {
		const device = normalizeDeviceSpec(draft.defaultDevice);

		if (device !== normalizeDeviceSpec(settings.default_device)) {
			update.default_device = device;
		}
	}

	if (!samePins(draft.pinned, settings.pinned)) {
		update.pinned = [...new Set(draft.pinned)].sort();
	}

	return update;
}
