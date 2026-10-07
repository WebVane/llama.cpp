/**
 * GGUF upload (admin panel, stage 3): pure helpers of the upload tab.
 *
 * - grouping a pick of files into models (a lone file, a complete split set,
 *   plus at most one projector), one upload session per model;
 * - the id proposed for a model (from the model file, never the projector)
 *   and the server's id rules, so a name the server refuses is flagged while
 *   it is typed;
 * - the plain-word texts of the server's codes (rejections, blocks,
 *   warnings, delete refusals) and of the scheduler states.
 *
 * Everything here is a hint for the page: the server checks the files, the
 * name and the settings itself.
 */

import {
	ADMIN_UPLOAD,
	ADMIN_UPLOAD_DRAFT_PREFIXES,
	ADMIN_UPLOAD_NAME_MAX,
	ADMIN_UPLOAD_RESERVED_NAMES,
	ADMIN_UPLOAD_SHARD_RE
} from '$lib/constants';
import {
	AdminModelRole,
	AdminSettingKey,
	AdminUploadBlock,
	AdminUploadFileRole,
	AdminUploadJobPhase,
	AdminUploadSendState,
	AdminUploadState
} from '$lib/enums/admin.enums';
import type {
	AdminGgufHeader,
	AdminUploadGroup,
	AdminUploadGrouping,
	AdminUploadPickedFile,
	AdminUploadSendSnapshot,
	ApiAdminUploadSummary
} from '$lib/types/admin-upload';
import { formatBytes } from '$lib/utils/admin';
import { describeModelWarning } from '$lib/utils/admin-model-settings';

/** A picked file before grouping: the File and what the header reader found. */
export interface AdminUploadPickInput {
	file: File;
	header: AdminGgufHeader | null;
}

/** `<prefix>-NNNNN-of-NNNNN.gguf` split into its parts, null for any other name. */
export function parseShardName(name: string): { prefix: string; no: number; count: number } | null {
	const match = ADMIN_UPLOAD_SHARD_RE.exec(name);

	if (!match) return null;

	const no = Number(match[2]);
	const count = Number(match[3]);

	if (no < 1 || count < 1 || no > count) return null;

	return { count, no, prefix: match[1] };
}

/** The scanner takes such a file as a draft model; drafts cannot be uploaded yet. */
export function isDraftFileName(name: string): boolean {
	const lower = name.toLowerCase();

	return ADMIN_UPLOAD_DRAFT_PREFIXES.some((prefix) => lower.startsWith(prefix));
}

/** The name looks like a projector (mmproj) file. */
export function isProjectorFileName(name: string): boolean {
	return name.toLowerCase().includes('mmproj');
}

/** Why the server would refuse this model name (its own rules), null when allowed. */
export function validateModelName(name: string): string | null {
	if (!name) return 'empty';

	if (name.length > ADMIN_UPLOAD_NAME_MAX) return 'too_long';

	if (!/^[A-Za-z0-9][A-Za-z0-9._-]*$/.test(name)) return 'charset';

	const lower = name.toLowerCase();

	if (lower.includes('mmproj')) return 'mmproj';

	if (lower.includes('.gguf')) return 'gguf';

	if (isDraftFileName(lower)) return 'draft_prefix';

	if (/-\d{5}-of-\d{5}/.test(lower)) return 'shard_suffix';

	if (name.endsWith('.')) return 'trailing_dot';

	const base = lower.split('.')[0];

	if (ADMIN_UPLOAD_RESERVED_NAMES.includes(base)) return 'reserved';

	return null;
}

/** The server's reason codes of `invalid_name`, as a sentence. */
export function describeModelNameProblem(reason: string): string {
	switch (reason) {
		case 'empty':
			return 'Enter a name.';
		case 'too_long':
			return `The name may be at most ${ADMIN_UPLOAD_NAME_MAX} characters long.`;
		case 'charset':
			return "Use letters, digits, '.', '_' and '-' only, starting with a letter or digit.";
		case 'mmproj':
			return "The name must not contain 'mmproj': such files are taken as projectors.";
		case 'gguf':
			return "The name must not contain '.gguf'.";
		case 'draft_prefix':
			return 'The name must not start with mtp-, dspark-, dflash- or eagle3-: such files are taken as draft models.';
		case 'shard_suffix':
			return 'The name must not contain a shard suffix like -00001-of-00003.';
		case 'trailing_dot':
			return "The name must not end with '.'.";
		case 'reserved':
			return 'This name is reserved on Windows.';
		default:
			return 'The server does not accept this name.';
	}
}

/**
 * The id proposed for a model: the model file's name without `.gguf` and
 * without the shard suffix, with every character the server refuses
 * replaced by '-'. Never taken from a projector.
 */
export function proposeModelName(fileName: string): string {
	let name = fileName.replace(/\.gguf$/i, '');

	const shard = /^(.*)-\d{5}-of-\d{5}$/i.exec(name);

	if (shard) name = shard[1];

	name = name
		.replace(/\.gguf/gi, '')
		.replace(/mmproj/gi, '')
		.replace(/-\d{5}-of-\d{5}/gi, '')
		.replace(/[^A-Za-z0-9._-]+/g, '-')
		.replace(/-{2,}/g, '-')
		.replace(/^[^A-Za-z0-9]+/, '');

	for (const prefix of ADMIN_UPLOAD_DRAFT_PREFIXES) {
		if (name.toLowerCase().startsWith(prefix)) name = name.slice(prefix.length);
	}

	name = name
		.replace(/^[^A-Za-z0-9]+/, '')
		.slice(0, ADMIN_UPLOAD_NAME_MAX)
		.replace(/[._-]+$/, '');

	if (!name) return 'model';

	if (validateModelName(name) === 'reserved') {
		// the part before the first '.' is what Windows reserves: extend that part
		const dot = name.indexOf('.');
		const base = dot < 0 ? name : name.slice(0, dot);
		const rest = dot < 0 ? '' : name.slice(dot);

		name = `${base}-model${rest}`.slice(0, ADMIN_UPLOAD_NAME_MAX).replace(/[._-]+$/, '');
	}

	return name;
}

function roleOf(name: string, header: AdminGgufHeader | null): AdminUploadFileRole {
	if (header && !header.error) {
		if (header.arch === 'clip' || header.type === 'mmproj') return AdminUploadFileRole.PROJECTOR;

		return AdminUploadFileRole.MODEL;
	}

	return isProjectorFileName(name) ? AdminUploadFileRole.PROJECTOR : AdminUploadFileRole.MODEL;
}

/** Why a file cannot be uploaded at all, null when it can. */
function rejectionOf(name: string, header: AdminGgufHeader | null): string | null {
	if (isDraftFileName(name)) return 'draft';

	if (!header) return null;

	if (header.error === 'not_gguf' || header.error === 'unsupported_version') return header.error;

	if (header.error) return null;

	if (header.type === 'adapter' || header.adapterType) return 'adapter';

	if (header.type === 'imatrix') return 'imatrix';

	return null;
}

/** A projector stem to match against model names: `mmproj-foo-f16.gguf` -> `foo`. */
function projectorStem(name: string): string {
	return name
		.toLowerCase()
		.replace(/\.gguf$/, '')
		.replace(/mmproj/g, '')
		.replace(/[-_.]?(f16|f32|bf16|q8_0|model)$/g, '')
		.replace(/^[-_.]+|[-_.]+$/g, '');
}

/**
 * The model a projector most likely belongs to, by name: the only group whose
 * proposed id contains the projector's stem. Null when none or several match.
 */
export function suggestProjectorGroup(
	projector: { name: string },
	groups: AdminUploadGroup[]
): string | null {
	const stem = projectorStem(projector.name);

	if (stem.length < 3) return null;

	const matches = groups.filter((group) => group.proposedName.toLowerCase().includes(stem));

	return matches.length === 1 ? matches[0].key : null;
}

/**
 * Group picked files into models: the shards of one split set together, a
 * lone model file on its own, and one projector paired with the model when
 * exactly one model and one projector were picked. Any other projector is
 * left for the user to pair. Drafts, adapters, imatrix and non-GGUF files
 * are listed as ignored with the reason.
 */
export function groupUploadFiles(inputs: AdminUploadPickInput[]): AdminUploadGrouping {
	const ignored: AdminUploadGrouping['ignored'] = [];
	const seen = new Set<string>();
	const picked: AdminUploadPickedFile[] = [];

	inputs.forEach((input, key) => {
		const name = input.file.name;

		if (seen.has(name)) {
			ignored.push({ name, reason: 'duplicate' });

			return;
		}

		seen.add(name);

		const rejected = rejectionOf(name, input.header);

		if (rejected) {
			ignored.push({ name, reason: rejected });

			return;
		}

		const role = roleOf(name, input.header);
		const fromName = parseShardName(name);
		const header = input.header && !input.header.error ? input.header : null;
		const problems: string[] = [];

		let shard = fromName ? { count: fromName.count, no: fromName.no } : null;

		if (input.header?.error) problems.push('header_unreadable');

		if (header && header.splitCount !== null && header.splitCount > 1) {
			const fromHeader = { count: header.splitCount, no: (header.splitNo ?? 0) + 1 };

			if (shard && (shard.count !== fromHeader.count || shard.no !== fromHeader.no)) {
				problems.push('split_name_mismatch');
			}

			shard ??= fromHeader;
		}

		picked.push({
			file: input.file,
			header: input.header,
			key,
			name,
			problems,
			role:
				role === AdminUploadFileRole.MODEL && shard && shard.count > 1
					? AdminUploadFileRole.SHARD
					: role,
			shard: shard && shard.count > 1 ? shard : null,
			size: input.file.size
		});
	});

	const groups: AdminUploadGroup[] = [];
	const sets = new Map<string, AdminUploadPickedFile[]>();

	for (const file of picked) {
		if (file.role === AdminUploadFileRole.SHARD && file.shard) {
			const prefix = parseShardName(file.name)?.prefix ?? file.name.replace(/\.gguf$/i, '');
			const setKey = `${prefix.toLowerCase()}|${file.shard.count}`;

			sets.set(setKey, [...(sets.get(setKey) ?? []), file]);
		} else if (file.role === AdminUploadFileRole.MODEL) {
			groups.push({
				files: [file],
				key: `file:${file.key}`,
				missingShards: [],
				problems: [...file.problems],
				projector: null,
				proposedName: proposeModelName(file.name)
			});
		}
	}

	for (const [setKey, files] of sets) {
		const count = files[0].shard?.count ?? files.length;
		const byNo = new Map<number, AdminUploadPickedFile>();

		for (const file of [...files].sort((a, b) => (a.shard?.no ?? 0) - (b.shard?.no ?? 0))) {
			const no = file.shard?.no ?? 0;

			if (byNo.has(no)) {
				ignored.push({ name: file.name, reason: 'duplicate_shard' });

				continue;
			}

			byNo.set(no, file);
		}

		const ordered = [...byNo.values()];
		const missing: number[] = [];

		for (let no = 1; no <= count; no++) {
			if (!byNo.has(no)) missing.push(no);
		}

		const problems = new Set<string>(ordered.flatMap((file) => file.problems));
		const arches = new Set(ordered.map((file) => file.header?.arch).filter((arch) => !!arch));

		if (missing.length > 0) problems.add('split_incomplete');

		if (arches.size > 1) problems.add('split_mismatch');

		groups.push({
			files: ordered,
			key: `set:${setKey}`,
			missingShards: missing,
			problems: [...problems],
			projector: null,
			proposedName: proposeModelName(ordered[0].name)
		});
	}

	groups.sort((a, b) => a.files[0].key - b.files[0].key);

	const projectors = picked.filter((file) => file.role === AdminUploadFileRole.PROJECTOR);
	const unpairedProjectors: AdminUploadPickedFile[] = [];

	if (groups.length === 0) {
		for (const projector of projectors) {
			ignored.push({ name: projector.name, reason: 'projector_without_model' });
		}
	} else if (groups.length === 1 && projectors.length === 1) {
		groups[0].projector = projectors[0];
	} else {
		unpairedProjectors.push(...projectors);
	}

	return { groups, ignored, unpairedProjectors };
}

/** A file problem code of the pick, as a sentence. */
export function describeUploadFileProblem(code: string): string {
	switch (code) {
		case 'header_unreadable':
			return 'The GGUF header could not be read here; the server checks the file.';
		case 'split_name_mismatch':
			return 'The shard number in the name differs from the one in the file.';
		case 'split_incomplete':
			return 'Parts of this split model are missing: pick every part.';
		case 'split_mismatch':
			return 'The parts are of different models.';
		default:
			return code;
	}
}

/** Why a picked file is not uploaded. */
export function describeIgnoredFile(reason: string): string {
	switch (reason) {
		case 'draft':
			return 'draft models (mtp-, dspark-, dflash-, eagle3-) cannot be uploaded yet';
		case 'not_gguf':
			return 'not a GGUF file';
		case 'unsupported_version':
			return 'an old or unsupported GGUF version';
		case 'adapter':
			return 'a LoRA adapter, not a model';
		case 'imatrix':
			return 'an importance matrix (imatrix), not a model';
		case 'duplicate':
			return 'picked twice';
		case 'duplicate_shard':
			return 'the same part of a split model twice';
		case 'projector_without_model':
			return 'a projector needs its model in the same upload';
		default:
			return reason;
	}
}

/** Role of a picked file for its row. */
export function describePickedRole(file: AdminUploadPickedFile): string {
	switch (file.role) {
		case AdminUploadFileRole.PROJECTOR:
			return 'projector';
		case AdminUploadFileRole.SHARD:
			return file.shard ? `part ${file.shard.no} of ${file.shard.count}` : 'part';
		default:
			return 'model';
	}
}

/** Bytes of a group (its files and its projector). */
export function groupBytes(group: AdminUploadGroup): number {
	return group.files.reduce((sum, file) => sum + file.size, 0) + (group.projector?.size ?? 0);
}

/** Files of a group in upload order: the model files, then the projector. */
export function groupFiles(group: AdminUploadGroup): AdminUploadPickedFile[] {
	return group.projector ? [...group.files, group.projector] : [...group.files];
}

/**
 * Placement of an upload as per-model settings: the GPU, the context size,
 * the parallel requests and the role. Empty fields are left out (the model
 * inherits them).
 */
export function buildUploadPlacement(input: {
	device: string;
	ctxSize: string;
	parallel: string;
	role: AdminModelRole;
}): Record<string, string> {
	const settings: Record<string, string> = {};

	if (input.device) settings[AdminSettingKey.DEVICE] = input.device;

	if (input.ctxSize.trim()) settings[AdminSettingKey.CTX_SIZE] = input.ctxSize.trim();

	if (input.parallel) settings[AdminSettingKey.PARALLEL] = input.parallel;

	if (input.role === AdminModelRole.EMBEDDINGS) settings[AdminSettingKey.EMBEDDINGS] = 'true';

	if (input.role === AdminModelRole.RERANKING) settings[AdminSettingKey.RERANKING] = 'true';

	return settings;
}

/** Why uploads are not possible on this server, and what fixes it. */
export function describeUploadUnavailable(
	reason: string | null,
	message: string | null
): { text: string; fix: string } {
	const restart = 'Then restart the server.';

	switch (reason) {
		case 'no_models_dir':
			return {
				fix: `Docker: mount a writable folder at /models-rw (the entrypoint then creates local/ and passes --models-dir). Host: add --models-dir to PROVISION_LLM_ARGS. ${restart}`,
				text: 'The server has no models directory to upload into.'
			};
		case 'models_dir_missing':
			return {
				fix: `Create the folder, or fix the --models-dir path. ${restart}`,
				text: message || 'The models directory does not exist.'
			};
		case 'not_writable':
			return {
				fix: `Give the account the server runs as write access to the models directory. ${restart}`,
				text: message || 'The server cannot write to the models directory.'
			};
		case 'staging_other_filesystem':
			return {
				fix: 'Do not mount anything over .provision-upload inside the models directory.',
				text:
					message ||
					'The upload staging folder is on another file system than the models directory.'
			};
		default:
			return {
				fix: restart,
				text: message || 'Uploads are not possible on this server right now.'
			};
	}
}

/** A rejected session or a refused file, in plain words. */
export function describeUploadRejection(code: string, message?: string | null): string {
	switch (code) {
		case 'unknown_architecture':
			return 'This server build does not support the model architecture. Update the server (image), then upload again.';
		case 'not_gguf':
			return 'This is not a GGUF model file.';
		case 'truncated':
			return 'The file is incomplete (cut short).';
		case 'adapter':
			return 'This is a LoRA adapter, not a model.';
		case 'imatrix':
			return 'This is an importance matrix (imatrix) file, not a model.';
		case 'projector_bad_metadata':
			return 'The projector (mmproj) file has damaged or wrongly typed metadata.';
		case 'projector_without_modality':
			return 'The projector supports neither images nor audio.';
		case 'malformed':
			return 'The GGUF header of the file is damaged.';
		case 'header_too_large':
			return 'The metadata of the file is larger than 64 MiB.';
		case 'split_incomplete':
			return 'Parts of the split model are missing.';
		case 'split_mismatch':
			return 'The parts of the split model do not belong together.';
		case 'two_models':
			return 'The files hold two different models: upload them one at a time.';
		case 'two_projectors':
			return 'There are two projector files: keep only one.';
		case 'projector_without_model':
			return 'A projector (mmproj) needs its model in the same upload.';
		case 'no_model':
			return 'There is no model file in the upload.';
		case 'clip_as_model':
			return 'This file is a projector (CLIP), not a model.';
		case 'load_failed':
			return `llama.cpp could not load the model${message ? `: ${message}` : '.'}`;
		case 'staging_missing':
			return 'The uploaded data disappeared from the staging folder of the server.';
		case 'content_mismatch':
			return 'This is not the same file: its bytes differ from the ones already uploaded.';
		default:
			return message || code;
	}
}

/** A message of the server as a sentence: capital first letter, a full stop at the end. */
export function asSentence(message: string): string {
	const text = message.trim();

	if (!text) return '';

	const capital = text.charAt(0).toUpperCase() + text.slice(1);

	return /[.!?]$/.test(capital) ? capital : `${capital}.`;
}

/**
 * Who holds a model name (`name_taken`: `by`, `other`, `case_variant`), as a
 * sentence without the advice that follows it.
 */
export function describeNameTaken(by: unknown, other: string, caseVariant: boolean): string {
	const what =
		by === 'upload'
			? 'another upload'
			: by === 'file'
				? 'a file in the models directory'
				: by === 'alias'
					? 'an alias'
					: by === 'preset'
						? 'a models.ini section'
						: 'a model';

	return caseVariant
		? `It differs only in case from "${other}" (${what}).`
		: `"${other}" is already used by ${what}.`;
}

/** What a ready session waits for, in plain words. */
export function describeUploadBlock(
	block: string | null,
	details: { message?: string; [key: string]: unknown } | null
): { title: string; text: string } {
	const message = typeof details?.message === 'string' ? asSentence(details.message) : '';

	switch (block) {
		case AdminUploadBlock.MANUAL:
			return {
				text: 'Every check passed. Publish it to make the model appear in the list.',
				title: 'Ready to publish'
			};
		case AdminUploadBlock.NAME_TAKEN: {
			const holder =
				typeof details?.other === 'string' && details.other
					? describeNameTaken(details.by, details.other, details.case_variant === true)
					: message || 'Another model uses this name now.';

			return {
				text: `${holder} Choose another name; nothing is uploaded again.`,
				title: 'The name was taken meanwhile'
			};
		}
		case AdminUploadBlock.PUBLISH_ERROR:
			return {
				text: message || 'The files could not be moved into the models directory.',
				title: 'Publishing failed'
			};
		case AdminUploadBlock.SHA256_MISMATCH:
			return {
				text: 'The SHA-256 you entered does not match the uploaded file. Correct it, publish anyway, or cancel.',
				title: 'SHA-256 does not match'
			};
		case AdminUploadBlock.CHECK_FAILED: {
			const limit = uploadNameLimit(details);

			if (limit !== null) {
				// Windows: llama.cpp loads a split model only from a path of at most 259 characters
				return {
					text:
						limit > 0
							? `This server loads a split model only from a short path: rename it to at most ${limit} characters, then it is checked again. Nothing is uploaded again.`
							: 'This server loads a split model only from a short path, and the path of its models directory is already too long for one: cancel the upload, or move the models directory to a shorter path.',
					title: 'The name is too long for this server'
				};
			}

			if (typeof details?.file === 'number') {
				// a file refused when the checks ran again (after a restart): the files are already
				// arranged for publishing together, so it cannot be dropped any more
				return {
					text: `${message || 'A file was refused.'} The upload cannot go on without it: Retry checks the files again, otherwise cancel the upload and upload the model again.`,
					title: 'A file was refused when the checks ran again'
				};
			}

			return {
				text: `${message || 'The check did not finish.'} The data is kept; Retry runs the checks again.`,
				title: 'The checks could not run'
			};
		}
		case AdminUploadBlock.SETTINGS_NOT_SAVED:
			return {
				text: `${message || 'The placement could not be saved.'} Publish without the placement, or fix the state directory and retry.`,
				title: 'The placement could not be saved'
			};
		case AdminUploadBlock.SETTINGS_INVALID:
			return {
				text: `${message || 'A setting is no longer valid.'} Publish without the placement and set it later.`,
				title: 'The placement is not valid any more'
			};
		default:
			return { text: message, title: 'Waiting' };
	}
}

/**
 * The longest name a ready session can be renamed to when its checks wait
 * for a shorter name (`check_failed` with code `path_too_long`), else null.
 */
export function uploadNameLimit(
	details: { code?: unknown; max_name_length?: unknown; [key: string]: unknown } | null | undefined
): number | null {
	if (details?.code !== 'path_too_long') return null;

	const limit = details.max_name_length;

	return typeof limit === 'number' && Number.isFinite(limit) && limit >= 0
		? Math.floor(limit)
		: null;
}

/** Why a queued model waits: the server has its maximum of open uploads (`too_many_uploads`). */
export function describeUploadSlotWait(open: number | null): string {
	const count = open !== null && open > 0 ? `${open} uploads` : 'its maximum of uploads';

	return `the server already has ${count} open; this one starts when one of them is published or cancelled.`;
}

/** A warning of the upload create / dry run, as a sentence. */
export function describeUploadWarning(code: string): string {
	switch (code) {
		case 'not_enough_space':
			return 'There is probably not enough free space for this upload (other uploads count too). It can be uploaded anyway; a full disk pauses it.';
		case 'larger_than_disk':
			return 'The upload is larger than the whole disk of the models directory.';
		case 'shares_disk_with_state':
			return 'The models directory is on the same disk as the admin state (settings, license counter): a full disk affects them too.';
		case 'state_not_writable':
			return 'The admin state directory is not writable: a placement cannot be saved, the publish would wait for a decision.';
		case 'device_missing':
			return 'The GPU chosen in the placement is not on this server.';
		case 'record_not_persisted':
			return 'The upload record could not be saved in the state directory: after a server restart the upload cannot be resumed with its settings.';
		case 'chown_failed':
			return 'The uploaded files could not be given to the owner of the models directory.';
		default:
			return describeModelWarning(code);
	}
}

/** Why a delete or "Remove projector" was refused (`not_deletable` reason). */
export function describeDeleteRefusal(reason: string | null | undefined): string {
	switch (reason) {
		case 'cache':
			return 'This model is in the download cache: use Delete on its row.';
		case 'preset':
			return 'The file of this model is set in models.ini or on the router command line, not by the models directory: change it there.';
		case 'not_found':
			return 'The model is no longer in the list.';
		case 'no_projector':
			return 'This model has no projector of its own (a projector set for every model, on the router command line or in [*] of models.ini, is not removed from here).';
		case 'outside_models_dir':
			return 'Its files are outside the models directory.';
		case 'symlink':
			return 'Its entry in the models directory is a symbolic link: remove it on the server.';
		case 'missing':
			return 'Its files are already gone.';
		case 'no_models_dir':
			return 'The server has no models directory.';
		case 'no_file':
			return 'The model has no file.';
		case 'other_filesystem':
			return 'Its files are on another file system (a mount inside the models directory).';
		default:
			return 'The server cannot delete this model.';
	}
}

/** Server session states that are still open (shown in the unfinished uploads banner). */
export function isUploadOpen(state: string): boolean {
	return (
		state === AdminUploadState.RECEIVING ||
		state === AdminUploadState.CHECKING ||
		state === AdminUploadState.PUBLISHING ||
		state === AdminUploadState.READY
	);
}

/** Job phase for a server session state. */
export function jobPhaseOf(state: string): AdminUploadJobPhase {
	switch (state) {
		case AdminUploadState.RECEIVING:
			return AdminUploadJobPhase.UPLOADING;
		case AdminUploadState.CHECKING:
			return AdminUploadJobPhase.CHECKING;
		case AdminUploadState.PUBLISHING:
			return AdminUploadJobPhase.PUBLISHING;
		case AdminUploadState.READY:
			return AdminUploadJobPhase.READY;
		case AdminUploadState.PUBLISHED:
			return AdminUploadJobPhase.PUBLISHED;
		case AdminUploadState.REJECTED:
			return AdminUploadJobPhase.REJECTED;
		case AdminUploadState.CANCELLED:
			return AdminUploadJobPhase.CANCELLED;
		case AdminUploadState.EXPIRED:
			return AdminUploadJobPhase.EXPIRED;
		default:
			return AdminUploadJobPhase.FAILED;
	}
}

/** The job no longer changes by itself (nothing to poll, nothing to send). */
export function isJobFinished(phase: AdminUploadJobPhase): boolean {
	return (
		phase === AdminUploadJobPhase.PUBLISHED ||
		phase === AdminUploadJobPhase.REJECTED ||
		phase === AdminUploadJobPhase.CANCELLED ||
		phase === AdminUploadJobPhase.EXPIRED ||
		phase === AdminUploadJobPhase.FAILED
	);
}

/** "3 min", "1 h 20 min", "45 s". */
export function formatDuration(seconds: number): string {
	if (!Number.isFinite(seconds) || seconds < 0) return '';

	if (seconds < 60) return `${Math.max(1, Math.round(seconds))} s`;

	const minutes = Math.round(seconds / 60);

	if (minutes < 60) return `${minutes} min`;

	const hours = Math.floor(minutes / 60);
	const rest = minutes % 60;

	return rest ? `${hours} h ${rest} min` : `${hours} h`;
}

/** "expires in 2 days" / "expires in 5 h" from an ISO time, '' when unknown. */
export function describeExpiry(expiresAt: string | null, now = Date.now()): string {
	if (!expiresAt) return '';

	const ms = Date.parse(expiresAt) - now;

	if (!Number.isFinite(ms)) return '';

	if (ms <= 0) return 'expires now';

	const hours = ms / 3_600_000;

	if (hours >= 48) return `expires in ${Math.round(hours / 24)} days`;

	if (hours >= 1) return `expires in ${Math.round(hours)} h`;

	return `expires in ${Math.max(1, Math.round(ms / 60_000))} min`;
}

/** One line for the state of a sending job. */
export function describeSendState(send: AdminUploadSendSnapshot, now = Date.now()): string {
	const retryIn = send.retryAt ? Math.max(0, Math.ceil((send.retryAt - now) / 1000)) : null;

	switch (send.state) {
		case AdminUploadSendState.UPLOADING:
			return 'Uploading';
		case AdminUploadSendState.VERIFYING:
			return 'Comparing the picked file with the uploaded part...';
		case AdminUploadSendState.RETRYING:
			return retryIn !== null ? `Connection problem, retrying in ${retryIn} s` : 'Retrying...';
		case AdminUploadSendState.SERVER_RESTARTING:
			return retryIn !== null
				? `The server is restarting, retrying in ${retryIn} s`
				: 'The server is restarting...';
		case AdminUploadSendState.UNREACHABLE:
			return 'Paused: the server cannot be reached. Retrying every minute while this page is open.';
		case AdminUploadSendState.PAUSED:
			return 'Paused';
		case AdminUploadSendState.OTHER_CLIENT:
			return 'Another browser tab or client is uploading this model right now.';
		case AdminUploadSendState.DISK_FULL:
			return `The disk is full${send.problem?.freeBytes !== undefined ? ` (${formatBytes(send.problem.freeBytes)} free)` : ''}: free space, then resume. Cancel frees the uploaded part.`;
		case AdminUploadSendState.KEY_REQUIRED:
			return 'The admin key was rejected: enter it again, the upload then continues.';
		case AdminUploadSendState.FORBIDDEN:
			return 'This address may no longer use the admin API.';
		case AdminUploadSendState.FILE_REJECTED:
			return send.problem
				? describeUploadRejection(send.problem.code, send.problem.message)
				: 'A file was refused.';
		case AdminUploadSendState.NOT_SAME_FILE:
			return 'This is not the same file: pick the file that was uploaded before.';
		case AdminUploadSendState.NEEDS_FILE:
			return 'Pick the file again to continue (the browser forgets picked files on a reload).';
		case AdminUploadSendState.DONE:
			return 'Uploaded';
		case AdminUploadSendState.FAILED:
			return send.problem?.message ?? 'The upload stopped.';
		default:
			return '';
	}
}

/** Unfinished sessions of the server list, newest first. */
export function openUploadSessions(sessions: ApiAdminUploadSummary[]): ApiAdminUploadSummary[] {
	return sessions
		.filter((session) => isUploadOpen(session.state))
		.sort((a, b) => Date.parse(b.created) - Date.parse(a.created));
}

/** The next smaller chunk size of the step-down ladder, null below the floor. */
export function nextChunkSizeDown(current: number): number | null {
	for (const size of ADMIN_UPLOAD.STEP_LADDER) {
		if (size < current) return size;
	}

	return null;
}
