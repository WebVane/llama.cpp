import { header, sizedFile } from './admin-upload-fixtures';
import { AdminModelRole } from '$lib/enums';
import {
	AdminUploadBlock,
	AdminUploadFileRole,
	AdminUploadSendState
} from '$lib/enums/admin.enums';
import type { AdminUploadSendSnapshot, ApiAdminUploadSummary } from '$lib/types/admin-upload';
import {
	asSentence,
	buildUploadPlacement,
	describeDeleteRefusal,
	describeExpiry,
	describeIgnoredFile,
	describeModelNameProblem,
	describeNameTaken,
	describeSendState,
	describeUploadBlock,
	describeUploadRejection,
	describeUploadSlotWait,
	describeUploadUnavailable,
	describeUploadWarning,
	formatDuration,
	groupFiles,
	groupUploadFiles,
	isUploadOpen,
	nextChunkSizeDown,
	openUploadSessions,
	parseShardName,
	proposeModelName,
	suggestProjectorGroup,
	uploadNameLimit,
	validateModelName
} from '$lib/utils/admin-upload';
import { describe, expect, it } from 'vitest';

const MIB = 1024 * 1024;

describe('parseShardName', () => {
	it('splits the loader shard names', () => {
		expect(parseShardName('qwen-7b-q4-00002-of-00004.gguf')).toEqual({
			count: 4,
			no: 2,
			prefix: 'qwen-7b-q4'
		});
		expect(parseShardName('Model-00001-OF-00002.GGUF')).toEqual({
			count: 2,
			no: 1,
			prefix: 'Model'
		});
	});

	it('refuses anything else', () => {
		expect(parseShardName('model.gguf')).toBeNull();
		expect(parseShardName('model-1-of-2.gguf')).toBeNull();
		expect(parseShardName('model-00003-of-00002.gguf')).toBeNull();
		expect(parseShardName('model-00000-of-00002.gguf')).toBeNull();
	});
});

/** The server's id rules (server-admin-upload.cpp up_name_valid), mirrored for the name field. */
describe('validateModelName', () => {
	it.each([
		['qwen2.5-7b-instruct', null],
		['A', null],
		['x'.repeat(128), null],
		['', 'empty'],
		['x'.repeat(129), 'too_long'],
		['-leading', 'charset'],
		['has space', 'charset'],
		['ümlaut', 'charset'],
		['my/model', 'charset'],
		['tiny-MMPROJ', 'mmproj'],
		['model.gguf', 'gguf'],
		['mtp-model', 'draft_prefix'],
		['DSpark-x', 'draft_prefix'],
		['eagle3-x', 'draft_prefix'],
		['model-00001-of-00002', 'shard_suffix'],
		['model.', 'trailing_dot'],
		['con', 'reserved'],
		['LPT1.model', 'reserved'],
		['console', null]
	])('%j -> %j', (name, reason) => {
		expect(validateModelName(name)).toBe(reason);
	});

	it('has a sentence for every reason', () => {
		for (const reason of [
			'empty',
			'too_long',
			'charset',
			'mmproj',
			'gguf',
			'draft_prefix',
			'shard_suffix',
			'trailing_dot',
			'reserved',
			'other'
		]) {
			expect(describeModelNameProblem(reason).length).toBeGreaterThan(5);
		}
	});
});

describe('proposeModelName', () => {
	it.each([
		['Qwen2.5-7B-Instruct-Q4_K_M.gguf', 'Qwen2.5-7B-Instruct-Q4_K_M'],
		['llama-70b-q4-00001-of-00003.gguf', 'llama-70b-q4'],
		['a.gguf.x.gguf', 'a.x'],
		['my model (final).gguf', 'my-model-final'],
		['__weird__.gguf', 'weird'],
		['mtp-qwen.gguf', 'qwen'],
		['con.gguf', 'con-model'],
		['---.gguf', 'model'],
		['model-mmproj.gguf', 'model']
	])('%j -> %j', (file, name) => {
		expect(proposeModelName(file)).toBe(name);
	});

	it('always proposes a name the server accepts', () => {
		for (const file of [
			'ÄÖÜ.gguf',
			'x'.repeat(300) + '.gguf',
			'a-00001-of-00002-00001-of-00002.gguf',
			'eagle3-mmproj.gguf',
			'.gguf',
			'aux.v2.gguf'
		]) {
			expect(validateModelName(proposeModelName(file))).toBeNull();
		}
	});
});

function input(name: string, partial: Parameters<typeof header>[0] = {}, size = 1000) {
	return { file: sizedFile(name, size), header: header(partial) };
}

describe('groupUploadFiles', () => {
	it('makes one model of a lone file', () => {
		const result = groupUploadFiles([input('tiny.gguf')]);

		expect(result.groups).toHaveLength(1);
		expect(result.groups[0].proposedName).toBe('tiny');
		expect(result.groups[0].files[0].role).toBe(AdminUploadFileRole.MODEL);
		expect(result.ignored).toEqual([]);
	});

	it('groups the shards of a split set in order and proposes the id from the first one', () => {
		const result = groupUploadFiles([
			input('big-00003-of-00003.gguf', { splitCount: 3, splitNo: 2 }),
			input('big-00001-of-00003.gguf', { splitCount: 3, splitNo: 0 }),
			input('big-00002-of-00003.gguf', { splitCount: 3, splitNo: 1 })
		]);

		expect(result.groups).toHaveLength(1);
		expect(result.groups[0].files.map((file) => file.shard?.no)).toEqual([1, 2, 3]);
		expect(result.groups[0].files.every((file) => file.role === AdminUploadFileRole.SHARD)).toBe(
			true
		);
		expect(result.groups[0].problems).toEqual([]);
		expect(result.groups[0].proposedName).toBe('big');
	});

	it('flags missing shards', () => {
		const result = groupUploadFiles([
			input('big-00001-of-00003.gguf', { splitCount: 3, splitNo: 0 }),
			input('big-00003-of-00003.gguf', { splitCount: 3, splitNo: 2 })
		]);

		expect(result.groups[0].problems).toContain('split_incomplete');
		expect(result.groups[0].missingShards).toEqual([2]);
	});

	it('flags parts of different models and a name that disagrees with the file', () => {
		const result = groupUploadFiles([
			input('big-00001-of-00002.gguf', { splitCount: 2, splitNo: 0 }),
			input('big-00002-of-00002.gguf', { arch: 'qwen2', splitCount: 2, splitNo: 0 })
		]);

		expect(result.groups[0].problems).toContain('split_mismatch');
		expect(result.groups[0].problems).toContain('split_name_mismatch');
	});

	it('keeps two sets with the same prefix but another count apart', () => {
		const result = groupUploadFiles([
			input('m-00001-of-00002.gguf', { splitCount: 2, splitNo: 0 }),
			input('m-00001-of-00003.gguf', { splitCount: 3, splitNo: 0 })
		]);

		expect(result.groups).toHaveLength(2);
	});

	it('ignores the second copy of a shard and of a file', () => {
		const result = groupUploadFiles([
			input('m-00001-of-00002.gguf', { splitCount: 2, splitNo: 0 }),
			input('m-00001-of-00002.gguf', { splitCount: 2, splitNo: 0 }),
			input('m-00002-of-00002.gguf', { splitCount: 2, splitNo: 1 })
		]);

		expect(result.groups[0].files).toHaveLength(2);
		expect(result.ignored).toEqual([{ name: 'm-00001-of-00002.gguf', reason: 'duplicate' }]);
	});

	it('pairs the one projector with the one model, never naming the model after it', () => {
		const result = groupUploadFiles([
			input('mmproj-gemma-f16.gguf', { arch: 'clip', clipVision: true }),
			input('gemma-3-4b-Q4_K_M.gguf', { arch: 'gemma3' })
		]);

		expect(result.groups).toHaveLength(1);
		expect(result.groups[0].projector?.name).toBe('mmproj-gemma-f16.gguf');
		expect(result.groups[0].proposedName).toBe('gemma-3-4b-Q4_K_M');
		expect(groupFiles(result.groups[0]).map((file) => file.name)).toEqual([
			'gemma-3-4b-Q4_K_M.gguf',
			'mmproj-gemma-f16.gguf'
		]);
	});

	it('asks which model a projector belongs to when there are several, with a suggestion', () => {
		const result = groupUploadFiles([
			input('qwen-7b.gguf'),
			input('gemma-3-4b.gguf', { arch: 'gemma3' }),
			input('mmproj-gemma-3-4b-f16.gguf', { arch: 'clip' })
		]);

		expect(result.groups).toHaveLength(2);
		expect(result.groups.every((group) => group.projector === null)).toBe(true);
		expect(result.unpairedProjectors).toHaveLength(1);
		expect(suggestProjectorGroup(result.unpairedProjectors[0], result.groups)).toBe(
			result.groups[1].key
		);
		expect(suggestProjectorGroup({ name: 'mmproj-f16.gguf' }, result.groups)).toBeNull();
	});

	it('asks when one model comes with two projectors', () => {
		const result = groupUploadFiles([
			input('gemma.gguf', { arch: 'gemma3' }),
			input('mmproj-a.gguf', { arch: 'clip' }),
			input('mmproj-b.gguf', { arch: 'clip' })
		]);

		expect(result.groups[0].projector).toBeNull();
		expect(result.unpairedProjectors).toHaveLength(2);
	});

	it('takes a file as a projector by its header, by its name only when the header is unreadable', () => {
		const byHeader = groupUploadFiles([
			input('model.gguf'),
			input('vision.gguf', { arch: 'clip' })
		]);

		expect(byHeader.groups[0].projector?.name).toBe('vision.gguf');

		const byName = groupUploadFiles([
			input('model.gguf'),
			{ file: sizedFile('mmproj-x.gguf'), header: header({ arch: null, error: 'truncated' }) }
		]);

		expect(byName.groups[0].projector?.name).toBe('mmproj-x.gguf');
	});

	it('lists what cannot be uploaded with the reason', () => {
		const result = groupUploadFiles([
			input('model.gguf'),
			input('mtp-draft.gguf'),
			input('lora.gguf', { adapterType: 'lora', type: 'adapter' }),
			input('imatrix.gguf', { type: 'imatrix' }),
			{ file: sizedFile('notes.gguf'), header: header({ error: 'not_gguf' }) },
			{ file: sizedFile('old.gguf'), header: header({ error: 'unsupported_version' }) }
		]);

		expect(result.groups).toHaveLength(1);
		expect(result.ignored.map((item) => item.reason).sort()).toEqual(
			['adapter', 'draft', 'imatrix', 'not_gguf', 'unsupported_version'].sort()
		);

		for (const item of result.ignored)
			expect(describeIgnoredFile(item.reason).length).toBeGreaterThan(5);
	});

	it('cannot upload a projector without a model', () => {
		const result = groupUploadFiles([input('mmproj-x.gguf', { arch: 'clip' })]);

		expect(result.groups).toEqual([]);
		expect(result.ignored).toEqual([{ name: 'mmproj-x.gguf', reason: 'projector_without_model' }]);
	});

	it('keeps a file with an unreadable header as a model (the server decides)', () => {
		const result = groupUploadFiles([
			{ file: sizedFile('model.gguf'), header: header({ arch: null, error: 'too_large' }) }
		]);

		expect(result.groups).toHaveLength(1);
		expect(result.groups[0].problems).toContain('header_unreadable');
	});
});

describe('buildUploadPlacement', () => {
	it('leaves empty fields out and maps the role', () => {
		expect(
			buildUploadPlacement({ ctxSize: '', device: '', parallel: '', role: AdminModelRole.CHAT })
		).toEqual({});
		expect(
			buildUploadPlacement({
				ctxSize: ' 8192 ',
				device: 'CUDA1',
				parallel: '2',
				role: AdminModelRole.EMBEDDINGS
			})
		).toEqual({ 'ctx-size': '8192', device: 'CUDA1', embeddings: 'true', parallel: '2' });
		expect(
			buildUploadPlacement({
				ctxSize: '',
				device: 'none',
				parallel: '',
				role: AdminModelRole.RERANKING
			})
		).toEqual({ device: 'none', reranking: 'true' });
	});
});

describe('texts', () => {
	it('explains every unavailable reason with a fix', () => {
		for (const reason of [
			'no_models_dir',
			'models_dir_missing',
			'not_writable',
			'staging_other_filesystem',
			'staging_unusable',
			null
		]) {
			const text = describeUploadUnavailable(reason, null);

			expect(text.text.length).toBeGreaterThan(10);
			expect(text.fix.length).toBeGreaterThan(10);
		}

		// the Docker entrypoint always passes --models-dir: without one the folder could not be created
		const noModelsDir = describeUploadUnavailable('no_models_dir', null).fix;

		expect(noModelsDir).toContain('could not create /models-rw/local');
		expect(noModelsDir).toContain('container log');
		expect(noModelsDir).toContain('--models-dir to PROVISION_LLM_ARGS');
		expect(noModelsDir).toContain('restart the server');
		expect(noModelsDir).not.toContain('mount a writable folder');
	});

	it('words every rejection code', () => {
		for (const code of [
			'unknown_architecture',
			'not_gguf',
			'truncated',
			'adapter',
			'imatrix',
			'projector_bad_metadata',
			'projector_without_modality',
			'malformed',
			'header_too_large',
			'split_incomplete',
			'split_mismatch',
			'two_models',
			'two_projectors',
			'projector_without_model',
			'no_model',
			'clip_as_model',
			'load_failed',
			'staging_missing',
			'content_mismatch'
		]) {
			expect(describeUploadRejection(code)).not.toBe(code);
		}

		expect(describeUploadRejection('unknown_architecture')).toMatch(/Update the server/);
		expect(describeUploadRejection('load_failed', 'bad tensor')).toContain('bad tensor');
		expect(describeUploadRejection('content_mismatch')).toMatch(/not the same file/);
		expect(describeUploadRejection('something_new', 'as the server says')).toBe(
			'as the server says'
		);
	});

	it('words every block of a ready upload', () => {
		for (const block of Object.values(AdminUploadBlock)) {
			const text = describeUploadBlock(block, { message: 'details' });

			expect(text.title.length).toBeGreaterThan(3);
			expect(text.text.length).toBeGreaterThan(3);
		}

		expect(describeUploadBlock(AdminUploadBlock.NAME_TAKEN, null).text).toMatch(
			/nothing is uploaded again/
		);
	});

	it('words a name taken by its holder, and server messages as sentences', () => {
		expect(
			describeUploadBlock(AdminUploadBlock.NAME_TAKEN, {
				by: 'file',
				message: "the name 'x' is already used (file)",
				other: 'x'
			}).text
		).toBe(
			'"x" is already used by a file in the models directory. Choose another name; nothing is uploaded again.'
		);
		expect(describeNameTaken('model', 'Tiny', true)).toBe(
			'It differs only in case from "Tiny" (a model).'
		);
		expect(describeNameTaken('upload', 'a', false)).toBe('"a" is already used by another upload.');
		expect(asSentence('the helper timed out')).toBe('The helper timed out.');
		expect(asSentence('Done!')).toBe('Done!');
		expect(asSentence('  ')).toBe('');
		expect(
			describeUploadBlock(AdminUploadBlock.CHECK_FAILED, { message: 'the helper timed out' }).text
		).toMatch(/^The helper timed out\. The data is kept/);
	});

	it('words a check that waits for a shorter name, or a file refused when the checks ran again', () => {
		// Windows, split model: the server names the longest name that fits
		const tooLong = { code: 'path_too_long', max_name_length: 42, message: 'rename it' };

		expect(uploadNameLimit(tooLong)).toBe(42);
		expect(uploadNameLimit({ ...tooLong, max_name_length: 0 })).toBe(0);
		expect(uploadNameLimit({ ...tooLong, max_name_length: 'x' })).toBeNull();
		expect(uploadNameLimit({ code: 'check_failed', max_name_length: 42 })).toBeNull();
		expect(uploadNameLimit(null)).toBeNull();

		const rename = describeUploadBlock(AdminUploadBlock.CHECK_FAILED, tooLong);

		expect(rename.title).toBe('The name is too long for this server');
		expect(rename.text).toMatch(/at most 42 characters/);
		expect(rename.text).toMatch(/Nothing is uploaded again/);
		expect(
			describeUploadBlock(AdminUploadBlock.CHECK_FAILED, { ...tooLong, max_name_length: 0 }).text
		).toMatch(/cancel the upload, or move the models directory/);

		// after a restart the arranged files are checked again: Retry or cancel, never "drop"
		const refused = describeUploadBlock(AdminUploadBlock.CHECK_FAILED, {
			code: 'not_gguf',
			file: 1,
			message: 'file 1 (mmproj.gguf): not a GGUF file'
		});

		expect(refused.title).toBe('A file was refused when the checks ran again');
		expect(refused.text).toMatch(/^File 1 \(mmproj\.gguf\): not a GGUF file\. /);
		expect(refused.text).toMatch(/Retry checks the files again, otherwise cancel/);
		expect(refused.text).not.toMatch(/[Dd]rop/);

		expect(describeUploadSlotWait(4)).toBe(
			'the server already has 4 uploads open; this one starts when one of them is published or cancelled.'
		);
		expect(describeUploadSlotWait(null)).toMatch(/its maximum of uploads open/);
	});

	it('words the warnings and the delete refusals', () => {
		for (const code of [
			'not_enough_space',
			'larger_than_disk',
			'shares_disk_with_state',
			'state_not_writable',
			'device_missing',
			'record_not_persisted',
			'chown_failed'
		]) {
			expect(describeUploadWarning(code)).not.toBe(code);
		}

		for (const reason of [
			'cache',
			'preset',
			'not_found',
			'no_projector',
			'outside_models_dir',
			'symlink',
			'missing',
			'no_models_dir',
			'no_file',
			'other_filesystem'
		]) {
			expect(describeDeleteRefusal(reason)).not.toBe(describeDeleteRefusal('???'));
		}
	});

	it('words every scheduler state that is shown', () => {
		const base: AdminUploadSendSnapshot = {
			chunkSize: 8 * MIB,
			confirmed: 0,
			eta: null,
			failures: 1,
			inFlight: 0,
			offsets: {},
			problem: { code: 'disk_full', freeBytes: 5 * MIB, message: 'The write failed' },
			proxyLimit: null,
			retryAt: 10_000,
			speed: null,
			state: AdminUploadSendState.UPLOADING,
			total: 100
		};

		for (const state of Object.values(AdminUploadSendState)) {
			if (
				state === AdminUploadSendState.IDLE ||
				state === AdminUploadSendState.GONE ||
				state === AdminUploadSendState.SERVER_BUSY
			)
				continue;

			expect(describeSendState({ ...base, state }, 4_000).length).toBeGreaterThan(3);
		}

		expect(describeSendState({ ...base, state: AdminUploadSendState.RETRYING }, 4_000)).toContain(
			'6 s'
		);
		expect(describeSendState({ ...base, state: AdminUploadSendState.DISK_FULL }, 0)).toContain(
			'5.00 MB'
		);
	});

	it('formats durations and expiry', () => {
		expect(formatDuration(5)).toBe('5 s');
		expect(formatDuration(150)).toBe('3 min');
		expect(formatDuration(3600 + 20 * 60)).toBe('1 h 20 min');
		expect(formatDuration(7200)).toBe('2 h');
		expect(formatDuration(-1)).toBe('');

		const now = Date.parse('2026-10-07T10:00:00Z');

		expect(describeExpiry('2026-10-10T10:00:00Z', now)).toBe('expires in 3 days');
		expect(describeExpiry('2026-10-07T15:00:00Z', now)).toBe('expires in 5 h');
		expect(describeExpiry('2026-10-07T10:20:00Z', now)).toBe('expires in 20 min');
		expect(describeExpiry('2026-10-07T09:00:00Z', now)).toBe('expires now');
		expect(describeExpiry(null, now)).toBe('');
	});
});

describe('sessions and chunk sizes', () => {
	function summary(id: string, state: string, created: string): ApiAdminUploadSummary {
		return {
			auto_publish: true,
			blocked: null,
			blocked_by: null,
			created,
			error: null,
			expires_at: null,
			id,
			last_activity: created,
			n_files: 1,
			name: id,
			received_bytes: 0,
			state,
			substate: null,
			total_bytes: 10
		};
	}

	it('lists only the open sessions, newest first (finished ones stay 24 h on the server)', () => {
		const list = openUploadSessions([
			summary('a', 'receiving', '2026-10-07T10:00:00Z'),
			summary('b', 'published', '2026-10-07T11:00:00Z'),
			summary('c', 'ready', '2026-10-07T12:00:00Z'),
			summary('d', 'cancelled', '2026-10-07T12:30:00Z'),
			summary('e', 'checking', '2026-10-07T09:00:00Z'),
			summary('f', 'rejected', '2026-10-07T13:00:00Z'),
			summary('g', 'expired', '2026-10-07T13:00:00Z')
		]);

		expect(list.map((item) => item.id)).toEqual(['c', 'a', 'e']);
		expect(isUploadOpen('publishing')).toBe(true);
	});

	it('steps down 8 MiB -> 960 KiB -> 480 KiB -> 240 KiB, then gives up', () => {
		expect(nextChunkSizeDown(16 * MIB)).toBe(8 * MIB);
		expect(nextChunkSizeDown(8 * MIB)).toBe(960 * 1024);
		expect(nextChunkSizeDown(960 * 1024)).toBe(480 * 1024);
		expect(nextChunkSizeDown(480 * 1024)).toBe(240 * 1024);
		expect(nextChunkSizeDown(240 * 1024)).toBeNull();
	});
});
