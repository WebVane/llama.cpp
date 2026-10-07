import AdminScreen from '$lib/components/app/admin/AdminScreen.svelte';
import { ADMIN_STORAGE_EPHEMERAL_WARNING } from '$lib/constants';
import { AdminProbeState, AdminStorageDirKind } from '$lib/enums';
import { adminStore } from '$lib/stores/admin.svelte';
import { adminUploadStore } from '$lib/stores/admin-upload.svelte';
import type { ApiAdminStatus } from '$lib/types/admin';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { page } from 'vitest/browser';
import { render } from 'vitest-browser-svelte';

/**
 * Client-side tests of the admin page frame (AdminScreen): the status
 * warnings of GET /admin/status are alerts above the tabs, and the
 * models_storage_ephemeral one is worded for a container or a host by the
 * `storage` object of the status. The probe and the page loaders of the
 * stores are stubbed; the status is set on the store directly.
 */

function status(partial: Partial<ApiAdminStatus> = {}): ApiAdminStatus {
	return {
		admin: true,
		authorized: true,
		dirs: { cache: '/models-rw/hf', models_dir: '/models-rw/local' },
		download: { available: true, offline: false },
		features: [],
		key_configured: true,
		key_source: 'generated',
		models: { max: 2, running: 0, total: 0 },
		state: { dir: '/var/lib/provision-admin', error: '', writable: true },
		storage: {
			container: true,
			ephemeral: [AdminStorageDirKind.CACHE, AdminStorageDirKind.MODELS_DIR],
			persistent: false
		},
		version: 1,
		warnings: [ADMIN_STORAGE_EPHEMERAL_WARNING],
		...partial
	};
}

describe('AdminScreen status warnings', () => {
	beforeEach(() => {
		// the page frame alone: no probe, no feed, no loaders
		vi.spyOn(adminStore, 'enter').mockResolvedValue(undefined);
		vi.spyOn(adminStore, 'leave').mockImplementation(() => {});
		vi.spyOn(adminStore, 'setModelsTabVisible').mockImplementation(() => {});
		vi.spyOn(adminStore, 'loadSettings').mockResolvedValue(undefined);
		vi.spyOn(adminStore, 'loadDiagnostics').mockResolvedValue(undefined);
		vi.spyOn(adminUploadStore, 'enter').mockImplementation(() => {});
		adminStore.probeState = AdminProbeState.AVAILABLE;
	});

	afterEach(() => {
		adminStore.status = null;
		adminStore.probeState = AdminProbeState.IDLE;
		vi.restoreAllMocks();
		document.body.innerHTML = '';
	});

	/** The alert of a status warning code, null while none is shown. */
	function warningAlert(code: string): HTMLElement | null {
		return document.querySelector<HTMLElement>(`[data-warning="${code}"]`);
	}

	it('shows the ephemeral storage warning for a container', async () => {
		adminStore.status = status();
		render(AdminScreen);

		await expect.element(page.getByRole('tablist', { name: 'Admin sections' })).toBeVisible();
		await vi.waitFor(() => expect(warningAlert(ADMIN_STORAGE_EPHEMERAL_WARNING)).not.toBeNull());

		const text = warningAlert(ADMIN_STORAGE_EPHEMERAL_WARNING)?.textContent ?? '';

		expect(text).toContain('Model storage is not persistent');
		expect(text).toContain('inside the container');
		expect(text).toContain('/models-rw');
		expect(text).toContain('MODELS_RW_DIR');
	});

	it('words it for a host and follows a status change', async () => {
		adminStore.status = status({
			storage: { container: false, ephemeral: [AdminStorageDirKind.CACHE], persistent: false }
		});
		render(AdminScreen);

		await vi.waitFor(() => expect(warningAlert(ADMIN_STORAGE_EPHEMERAL_WARNING)).not.toBeNull());

		const text = warningAlert(ADMIN_STORAGE_EPHEMERAL_WARNING)?.textContent ?? '';

		expect(text).toContain('temporary directory');
		expect(text).toContain('LLAMA_CACHE');
		expect(text).not.toContain('container');

		// the next probe says the storage is persistent: the alert goes
		adminStore.status = status({
			storage: { container: false, ephemeral: [], persistent: true },
			warnings: []
		});

		await vi.waitFor(() => expect(warningAlert(ADMIN_STORAGE_EPHEMERAL_WARNING)).toBeNull());
	});

	it('shows no storage alert while the status has no such warning', async () => {
		adminStore.status = status({
			storage: { container: true, ephemeral: [], persistent: true },
			warnings: ['default_device_not_present']
		});
		render(AdminScreen);

		await vi.waitFor(() => expect(warningAlert('default_device_not_present')).not.toBeNull());
		expect(warningAlert(ADMIN_STORAGE_EPHEMERAL_WARNING)).toBeNull();
		expect(warningAlert('default_device_not_present')?.textContent).toContain('GPU for new models');
	});
});
