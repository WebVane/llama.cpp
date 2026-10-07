/**
 * Export / import of the settings of one model (admin panel, stage 2).
 *
 * File `<id>.settings.json`:
 *   { "format": "provision-llm-model-settings", "version": 1, "exported_at": "...",
 *     "models": [ { "id": "...", "about": { arch, file, quant, n_ctx_train },
 *                   "settings": { "ctx-size": 32768, "cache-type-k": "q8_0", ... } } ] }
 *
 * `settings` has exactly the shape of the POST /admin/models/settings body,
 * so a file can also be applied with curl. An imported file is untrusted:
 * only whitelisted keys are read, every value is validated, and nothing is
 * saved until the operator presses Save in the dialog.
 */

import {
	ADMIN_SETTING_BOOL_KEYS,
	ADMIN_SETTING_INT_KEYS,
	ADMIN_SETTING_KEYS,
	ADMIN_SETTINGS_FILE
} from '$lib/constants';
import { AdminExportMode, AdminImportIssue, AdminSettingKey } from '$lib/enums';
import type {
	AdminImportField,
	AdminImportPreview,
	AdminModelSettingsDraft,
	AdminSettingsExportFile,
	ApiAdminModelSettingsRow,
	ApiAdminSettingSchema
} from '$lib/types/admin';
import {
	canonicalSettingValue,
	draftValueUnlessInherited,
	isAdminSettingKey,
	isSettingLocked,
	settingField,
	settingSchema,
	splitDeviceList,
	validateSettingValue
} from '$lib/utils/admin-model-settings';

/** A value of the file in the JSON type it is written with. */
function typedValue(key: string, value: string): string | number | boolean {
	if ((ADMIN_SETTING_INT_KEYS as string[]).includes(key) && /^-?\d+$/.test(value)) {
		return Number(value);
	}

	if (
		(ADMIN_SETTING_BOOL_KEYS as string[]).includes(key) &&
		(value === 'true' || value === 'false')
	) {
		return value === 'true';
	}

	return value;
}

/** File name for a model id: characters that are not safe in file names become `_`. */
export function settingsFileName(id: string): string {
	const safe = id
		.replace(/[\\/:*?"<>|\s]+/g, '_')
		.replace(/_+/g, '_')
		.replace(/^[_.]+|_+$/g, '');

	return `${safe || 'model'}${ADMIN_SETTINGS_FILE.EXTENSION}`;
}

/**
 * The export file of a model: its saved values ("only my custom
 * settings"), or the value of every whitelisted key the model gets now
 * ("all current values"; keys that are not set at all are left out).
 */
export function buildSettingsExport(
	row: ApiAdminModelSettingsRow,
	mode: AdminExportMode,
	now: Date = new Date()
): AdminSettingsExportFile {
	const settings: Record<string, string | number | boolean> = {};

	for (const key of ADMIN_SETTING_KEYS) {
		const value =
			mode === AdminExportMode.CUSTOM
				? (row.overrides?.[key] ?? null)
				: settingField(row, key).effective;

		if (value !== null && value !== undefined) settings[key] = typedValue(key, value);
	}

	const facts = row.facts;
	const about: AdminSettingsExportFile['models'][number]['about'] = {};

	if (facts?.arch) about.arch = facts.arch;

	if (facts?.files?.[0]?.name) about.file = facts.files[0].name;

	if (facts?.quant) about.quant = facts.quant;

	if (facts?.n_ctx_train) about.n_ctx_train = facts.n_ctx_train;

	return {
		exported_at: now.toISOString(),
		format: ADMIN_SETTINGS_FILE.FORMAT,
		models: [{ about, id: row.id, settings }],
		version: ADMIN_SETTINGS_FILE.VERSION
	};
}

/** Result of reading an import file. */
export type AdminSettingsFileParse =
	| { ok: true; file: AdminSettingsExportFile }
	| { ok: false; error: string };

function isRecord(value: unknown): value is Record<string, unknown> {
	return typeof value === 'object' && value !== null && !Array.isArray(value);
}

/** Longest text of an `about` field that is kept; longer ones are cut. */
const ABOUT_TEXT_MAX = 256;

/**
 * The `about` block of an untrusted file: only text values of arch / file /
 * quant and a positive whole n_ctx_train are kept, anything else is dropped.
 */
function sanitizeAbout(value: unknown): AdminSettingsExportFile['models'][number]['about'] {
	const about: AdminSettingsExportFile['models'][number]['about'] = {};

	if (!isRecord(value)) return about;

	for (const key of ['arch', 'file', 'quant'] as const) {
		const text = value[key];

		if (typeof text === 'string' && text.trim() !== '') about[key] = text.slice(0, ABOUT_TEXT_MAX);
	}

	const train = value.n_ctx_train;

	if (typeof train === 'number' && Number.isSafeInteger(train) && train > 0) {
		about.n_ctx_train = train;
	}

	return about;
}

/**
 * Read and check an import file. Refuses broken JSON, another format, an
 * unknown version and files without model settings; never throws.
 */
export function parseSettingsFile(text: string): AdminSettingsFileParse {
	if (text.length > ADMIN_SETTINGS_FILE.MAX_BYTES) {
		return {
			error: `The file is larger than ${ADMIN_SETTINGS_FILE.MAX_BYTES / 1024} KiB.`,
			ok: false
		};
	}

	let data: unknown;

	try {
		data = JSON.parse(text.replace(/^\uFEFF/, ''));
	} catch {
		return { error: 'The file is not valid JSON.', ok: false };
	}

	if (!isRecord(data) || data.format !== ADMIN_SETTINGS_FILE.FORMAT) {
		return {
			error: `This is not a model settings file (expected format "${ADMIN_SETTINGS_FILE.FORMAT}").`,
			ok: false
		};
	}

	if (data.version !== ADMIN_SETTINGS_FILE.VERSION) {
		return {
			error: `The file has version ${JSON.stringify(data.version)}; this panel reads version ${ADMIN_SETTINGS_FILE.VERSION}.`,
			ok: false
		};
	}

	const models = Array.isArray(data.models) ? data.models : [];
	const entries = models.filter(
		(model): model is AdminSettingsExportFile['models'][number] =>
			isRecord(model) && typeof model.id === 'string' && isRecord(model.settings)
	);

	if (entries.length === 0) return { error: 'The file holds no model settings.', ok: false };

	return {
		file: {
			exported_at: typeof data.exported_at === 'string' ? data.exported_at : '',
			format: ADMIN_SETTINGS_FILE.FORMAT,
			models: entries.map((entry) => ({
				about: sanitizeAbout(entry.about),
				id: entry.id,
				settings: entry.settings
			})),
			version: ADMIN_SETTINGS_FILE.VERSION
		},
		ok: true
	};
}

/** Differences between the model a file was exported for and the model in the dialog. */
function modelDifferences(
	entry: AdminSettingsExportFile['models'][number],
	row: ApiAdminModelSettingsRow
): string[] {
	const differences: string[] = [];
	const facts = row.facts;
	const about = entry.about ?? {};

	if (entry.id !== row.id) differences.push(`exported for "${entry.id}", not "${row.id}"`);

	if (facts) {
		if (about.arch && facts.arch && about.arch !== facts.arch) {
			differences.push(`architecture ${about.arch} instead of ${facts.arch}`);
		}

		if (about.quant && facts.quant && about.quant.toLowerCase() !== facts.quant.toLowerCase()) {
			differences.push(`quantization ${about.quant} instead of ${facts.quant}`);
		}

		const file = facts.files?.[0]?.name;

		if (about.file && file && about.file !== file)
			differences.push(`file ${about.file} instead of ${file}`);
	}

	return differences;
}

/**
 * What an import would change for the model in the dialog. The entry for
 * the same id is used, else the first one (then it is "a different
 * model"). Unknown keys are listed and ignored; invalid and locked values
 * are skipped; a GPU this server does not have starts unticked.
 */
export function buildImportPreview(
	file: AdminSettingsExportFile,
	row: ApiAdminModelSettingsRow,
	schema: ApiAdminSettingSchema[],
	devices: string[]
): AdminImportPreview {
	const entry = file.models.find((model) => model.id === row.id) ?? file.models[0];
	const differences = modelDifferences(entry, row);
	const fields: AdminImportField[] = [];
	const unknownKeys: string[] = [];
	const nCtxTrain = row.facts?.n_ctx_train ?? row.context?.n_ctx_train ?? null;

	for (const [key, raw] of Object.entries(entry.settings ?? {})) {
		if (!isAdminSettingKey(key)) {
			unknownKeys.push(key);

			continue;
		}

		if (raw !== null && typeof raw === 'object') {
			fields.push({
				current: settingField(row, key).effective,
				issues: [AdminImportIssue.INVALID_VALUE],
				key,
				selected: false,
				skipped: true,
				value: JSON.stringify(raw)
			});

			continue;
		}

		const value = canonicalSettingValue(key, raw as string | number | boolean | null, devices);
		const issues: AdminImportIssue[] = [];

		let skipped = false;
		let selected = true;

		if (value === null) continue;

		if (isSettingLocked(row, key)) {
			issues.push(AdminImportIssue.LOCKED);
			skipped = true;
		} else if (key === AdminSettingKey.DEVICE && value !== 'none') {
			const unknown = splitDeviceList(value).some(
				(name) => !devices.some((device) => device.toLowerCase() === name.toLowerCase())
			);

			if (unknown) {
				issues.push(AdminImportIssue.DEVICE_UNKNOWN);
				selected = false;
			} else if (validateSettingValue(key, value, settingSchema(schema, key), devices)) {
				issues.push(AdminImportIssue.INVALID_VALUE);
				skipped = true;
			}
		} else if (validateSettingValue(key, value, settingSchema(schema, key), devices)) {
			issues.push(AdminImportIssue.INVALID_VALUE);
			skipped = true;
		}

		if (
			!skipped &&
			(key === AdminSettingKey.CTX_SIZE || key === AdminSettingKey.KV_UNIFIED_PER_SLOT) &&
			nCtxTrain !== null &&
			Number(value) > nCtxTrain
		) {
			issues.push(AdminImportIssue.CTX_ABOVE_TRAIN);
		}

		fields.push({
			current: settingField(row, key).effective,
			issues,
			key,
			selected: selected && !skipped,
			skipped,
			value
		});
	}

	return {
		differences,
		differentModel: differences.length > 0,
		exportedAt: file.exported_at || null,
		fields,
		sourceId: entry.id,
		unknownKeys
	};
}

/** Sentence for an import issue of a field. */
export function describeImportIssue(
	issue: AdminImportIssue,
	nCtxTrain: number | null = null
): string {
	switch (issue) {
		case AdminImportIssue.CTX_ABOVE_TRAIN:
			return nCtxTrain
				? `above the trained context of this model (${nCtxTrain.toLocaleString('en-US')})`
				: 'above the trained context of this model';
		case AdminImportIssue.DEVICE_UNKNOWN:
			return 'this GPU is not on this server';
		case AdminImportIssue.INVALID_VALUE:
			return 'not a valid value: skipped';
		case AdminImportIssue.LOCKED:
			return 'fixed on this server (MODEL_DEVICES or command line): skipped';
		default:
			return issue;
	}
}

/**
 * Load the ticked fields of a preview into a draft (nothing is saved). The
 * two cache types are kept equal: a file that sets only one sets both. A
 * value the model gets without a per-model value anyway is not loaded as
 * one: importing "all current values" back changes nothing.
 */
export function applyImportToDraft(
	draft: AdminModelSettingsDraft,
	row: ApiAdminModelSettingsRow,
	preview: AdminImportPreview,
	devices: string[] = []
): AdminModelSettingsDraft {
	const values = { ...draft.values };
	const chosen = preview.fields.filter((field) => field.selected && !field.skipped);

	for (const field of chosen) {
		values[field.key] = draftValueUnlessInherited(row, field.key, field.value, devices);
	}

	const typeK = chosen.find((field) => field.key === AdminSettingKey.CACHE_TYPE_K);
	const typeV = chosen.find((field) => field.key === AdminSettingKey.CACHE_TYPE_V);

	if (typeK && !typeV && !isSettingLocked(row, AdminSettingKey.CACHE_TYPE_V)) {
		values[AdminSettingKey.CACHE_TYPE_V] = draftValueUnlessInherited(
			row,
			AdminSettingKey.CACHE_TYPE_V,
			typeK.value
		);
	}

	if (typeV && !typeK && !isSettingLocked(row, AdminSettingKey.CACHE_TYPE_K)) {
		values[AdminSettingKey.CACHE_TYPE_K] = draftValueUnlessInherited(
			row,
			AdminSettingKey.CACHE_TYPE_K,
			typeV.value
		);
	}

	return { values };
}
