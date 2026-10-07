import DialogAdminDeleteModel from '$lib/components/app/dialogs/DialogAdminDeleteModel.svelte';
import { AdminApiError, AdminService } from '$lib/services/admin.service';
import { adminStore } from '$lib/stores/admin.svelte';
import type {
	ApiAdminDeletePreview,
	ApiAdminDeleteRequest,
	ApiAdminDeleteResult
} from '$lib/types/admin-upload';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { page } from 'vitest/browser';
import { render } from 'vitest-browser-svelte';

/**
 * Client-side tests of the "Delete files" dialog (admin panel, stage 3): the
 * dry run (files, the entries that are kept, a running model), the typed id
 * that confirms the delete, "Remove only the projector" and the refusals.
 * The server is replaced by a stub of AdminService.deleteModelFiles.
 */

function preview(partial: Partial<ApiAdminDeletePreview> = {}): ApiAdminDeletePreview {
	return {
		dry_run: true,
		files: [
			{ bytes: 4_000_000, path: 'qwen/qwen.gguf', role: 'model' },
			{ bytes: 900_000, path: 'qwen/mmproj-qwen.gguf', role: 'projector' }
		],
		folder: 'qwen',
		folder_removed: false,
		has_settings: true,
		kept: [{ bytes: 23, kind: 'file', path: 'qwen/README.md' }],
		kept_bytes: 23,
		model: 'qwen',
		n_files: 2,
		n_kept: 1,
		pinned: false,
		projector_only: false,
		running: false,
		source: 'models_dir',
		status: 'unloaded',
		total_bytes: 4_900_000,
		warnings: [],
		...partial
	};
}

function result(partial: Partial<ApiAdminDeleteResult> = {}): ApiAdminDeleteResult {
	return {
		files: [],
		folder_removed: false,
		freed_bytes: 4_900_000,
		kept: [],
		model: 'qwen',
		pin_removed: false,
		reload_error: null,
		settings_removed: true,
		trash_left: false,
		...partial
	};
}

describe('DialogAdminDeleteModel', () => {
	let calls: ApiAdminDeleteRequest[];
	let answer: (body: ApiAdminDeleteRequest) => ApiAdminDeletePreview | ApiAdminDeleteResult;

	beforeEach(() => {
		calls = [];
		answer = (body) =>
			body.dry_run
				? preview(
						body.projector_only
							? { files: [preview().files[1]], kept: [], n_files: 1, n_kept: 0 }
							: {}
					)
				: result();
		vi.spyOn(adminStore, 'runWithKey').mockImplementation((run) => run('test-key'));
		vi.spyOn(adminStore, 'notice').mockImplementation(() => {});
		vi.spyOn(adminStore, 'refreshModels').mockResolvedValue(true);
		vi.spyOn(adminStore, 'loadModelSettings').mockResolvedValue(undefined);
		vi.spyOn(adminStore, 'loadStorage').mockResolvedValue(undefined);
		vi.spyOn(AdminService, 'deleteModelFiles').mockImplementation(async (_key, body) => {
			calls.push(body);

			return answer(body);
		});
	});

	afterEach(() => {
		vi.restoreAllMocks();
		document.body.innerHTML = '';
	});

	async function open(props: { projectorOnly?: boolean } = {}) {
		const screen = render(DialogAdminDeleteModel, { modelId: 'qwen', open: true, ...props });

		await expect.element(page.getByTestId('delete-files')).toBeVisible();

		return screen;
	}

	it('lists the files of the model and the entries that are kept', async () => {
		await open();

		await expect.element(page.getByTestId('delete-files')).toHaveTextContent('qwen/qwen.gguf');
		await expect
			.element(page.getByTestId('delete-kept'))
			.toHaveTextContent('not part of this model');
		await expect.element(page.getByTestId('delete-kept')).toHaveTextContent('qwen/README.md');
		await expect.element(page.getByTestId('admin-delete-model')).toHaveTextContent('stays because');
		expect(calls[0]).toMatchObject({ dry_run: true, model: 'qwen', projector_only: false });
	});

	it('heads the preview with what will happen, not with what happened', async () => {
		await open();

		await expect
			.element(page.getByTestId('delete-files-heading'))
			.toHaveTextContent(/^Will be deleted \(2 files, /);

		await page.getByTestId('delete-projector-only').click();
		await expect
			.element(page.getByTestId('delete-files-heading'))
			.toHaveTextContent(/^Will be removed \(1 file, /);
	});

	it('lists a projector or draft every model uses as kept, not as a reason the folder stays', async () => {
		answer = (body) =>
			body.dry_run
				? preview({
						files: [{ bytes: 4_000_000, path: 'qwen/qwen.gguf', role: 'model' }],
						folder_removed: true,
						kept: [{ bytes: 900_000, kind: 'file', path: 'mmproj-shared.gguf', shared: true }],
						kept_bytes: 900_000,
						n_files: 1,
						n_kept: 1,
						total_bytes: 4_000_000
					})
				: result();
		await open();

		const kept = page.getByTestId('delete-kept');

		await expect.element(kept).toHaveTextContent('Kept: used by every model');
		await expect.element(kept).toHaveTextContent('mmproj-shared.gguf');
		await expect.element(kept.getByText('used by every model', { exact: true })).toBeVisible();
		await expect
			.element(page.getByTestId('admin-delete-model'))
			.toHaveTextContent('is removed too');
		await expect
			.element(page.getByTestId('admin-delete-model'))
			.not.toHaveTextContent('stays because');
	});

	it('counts every kept entry when the server lists only the first 100', async () => {
		const kept = Array.from({ length: 100 }, (_, i) => ({
			bytes: 10,
			kind: 'file' as const,
			path: `qwen/notes-${String(i).padStart(3, '0')}.txt`
		}));

		answer = (body) =>
			body.dry_run
				? preview({ kept, kept_bytes: 2500, kept_truncated: true, n_kept: 250 })
				: result();
		await open();

		const box = page.getByTestId('delete-kept');

		await expect.element(box).toHaveTextContent('(250 entries');
		await expect.element(box).toHaveTextContent('... and 150 more.');
		expect(document.querySelectorAll('[data-testid="delete-kept"] li')).toHaveLength(100);
	});

	it('deletes only after the id is typed exactly', async () => {
		await open();

		const go = page.getByTestId('delete-go');

		await expect.element(go).toBeDisabled();
		await page.getByTestId('delete-confirm-input').fill('qwe');
		await expect.element(go).toBeDisabled();
		await page.getByTestId('delete-confirm-input').fill('qwen');
		await expect.element(go).toBeEnabled();
		await go.click();

		await vi.waitFor(() => expect(calls).toHaveLength(2));
		expect(calls[1]).toEqual({
			model: 'qwen',
			projector_only: false,
			remove_settings: true,
			stop: false
		});
	});

	it('keeps the saved settings when the box is cleared', async () => {
		await open();

		await page.getByTestId('delete-settings').getByRole('checkbox').click();
		await page.getByTestId('delete-confirm-input').fill('qwen');
		await page.getByTestId('delete-go').click();

		await vi.waitFor(() => expect(calls).toHaveLength(2));
		expect(calls[1].remove_settings).toBe(false);
	});

	it('says that a running model is stopped first and sends stop', async () => {
		answer = (body) => (body.dry_run ? preview({ running: true, status: 'loaded' }) : result());
		await open();

		await expect.element(page.getByTestId('delete-running')).toBeVisible();
		await expect.element(page.getByTestId('delete-go')).toHaveTextContent('Stop and delete');
		await page.getByTestId('delete-confirm-input').fill('qwen');
		await page.getByTestId('delete-go').click();

		await vi.waitFor(() => expect(calls).toHaveLength(2));
		expect(calls[1].stop).toBe(true);
	});

	it('switches to "remove only the projector" without a typed id', async () => {
		await open();

		await page.getByTestId('delete-projector-only').click();
		await expect.element(page.getByTestId('delete-go')).toHaveTextContent('Remove the projector');
		await expect.element(page.getByTestId('delete-go')).toBeEnabled();
		expect(calls.at(-1)).toMatchObject({ dry_run: true, projector_only: true });

		await page.getByTestId('delete-go').click();
		await vi.waitFor(() => expect(calls.at(-1)?.dry_run).toBeUndefined());
		expect(calls.at(-1)).toMatchObject({ model: 'qwen', projector_only: true });
		expect(calls.at(-1)?.remove_settings).toBeUndefined();
	});

	it('explains a refusal of the dry run', async () => {
		answer = () => {
			const error = new AdminApiError('not deletable', 409, 'not_deletable');

			error.details = { reason: 'preset' };

			throw error;
		};
		render(DialogAdminDeleteModel, { modelId: 'qwen', open: true });

		await expect.element(page.getByTestId('delete-refused')).toHaveTextContent('models.ini');
		await expect.element(page.getByTestId('delete-go')).toBeDisabled();
	});

	it('reports a stop timeout and deletes nothing', async () => {
		answer = (body) => {
			if (body.dry_run) return preview({ running: true, status: 'loaded' });

			throw new AdminApiError('stop timeout', 504, 'stop_timeout');
		};
		await open();

		await page.getByTestId('delete-confirm-input').fill('qwen');
		await page.getByTestId('delete-go').click();

		await expect
			.element(page.getByTestId('delete-error'))
			.toHaveTextContent('did not stop in time');
	});
});
