import { makeFacts, makeRow, SCHEMA } from '../unit/admin-fixtures';
import AdminModelSettingsFormWrapper from './components/AdminModelSettingsFormWrapper.svelte';
import { describe, expect, it } from 'vitest';
import { page, userEvent } from 'vitest/browser';
import { render } from 'vitest-browser-svelte';

/**
 * Client-side tests of the per-model settings form (admin panel, stage 2):
 * where each value comes from, locks, and that a value equal to the
 * inherited one is kept (a pin) until "back to" / "Inherit" removes it. The
 * rules themselves are covered in `tests/unit/admin-model-settings.test.ts`.
 */
describe('AdminModelSettingsForm', () => {
	function row() {
		return makeRow({
			facts: makeFacts({ n_ctx_train: 32768 }),
			id: 'qwen2.5-chat',
			inherited: { 'ctx-size': '8192', device: 'CUDA1', 'n-gpu-layers': '99' },
			locked: { device: { by: 'model_devices', value: 'CUDA1' } }
		});
	}

	async function mount() {
		return render(AdminModelSettingsFormWrapper, {
			devices: ['CUDA0', 'CUDA1'],
			row: row(),
			schema: SCHEMA
		});
	}

	it('shows a MODEL_DEVICES lock and disables the GPU field', async () => {
		const screen = await mount();
		const device = screen.container.querySelector('[data-field="ms-device"]') as HTMLElement;

		expect(device.textContent).toContain('MODEL_DEVICES');
		expect(device.querySelector('[data-testid="field-lock"]')).not.toBeNull();
		expect((screen.container.querySelector('#ms-device') as HTMLButtonElement).disabled).toBe(true);
	});

	it('shows the inherited context size with its origin', async () => {
		const screen = await mount();
		const ctx = screen.container.querySelector('[data-field="ms-ctx"]') as HTMLElement;

		expect(ctx.textContent).toContain('from models.ini');
		expect((screen.container.querySelector('#ms-ctx') as HTMLInputElement).value).toBe('8192');
		await expect.element(screen.getByTestId('draft-update')).toHaveTextContent('{}');
	});

	it('saves a typed value, keeps the inherited value typed again, and goes back to it', async () => {
		const screen = await mount();
		const input = screen.getByRole('spinbutton').first();

		await input.fill('16384');
		await expect
			.element(screen.getByTestId('draft-update'))
			.toHaveTextContent('{"ctx-size":"16384"}');

		// the inherited value typed into the field pins the model to it
		await input.fill('8192');
		await expect
			.element(screen.getByTestId('draft-update'))
			.toHaveTextContent('{"ctx-size":"8192"}');

		// "back to" removes the value: the model inherits again
		await screen.getByRole('button', { name: /inherited value for Context size/i }).click();
		await expect.element(screen.getByTestId('draft-update')).toHaveTextContent('{}');
	});

	it('keeps a GPU chosen in the list even when the model inherits it (a pin)', async () => {
		const pinRow = makeRow({
			facts: makeFacts({ n_ctx_train: 32768 }),
			inherited: { device: 'CUDA0' }
		});

		pinRow.fields.device.inherited_origin = 'default_device';

		const screen = render(AdminModelSettingsFormWrapper, {
			devices: ['CUDA0', 'CUDA1'],
			row: pinRow,
			schema: SCHEMA
		});

		await userEvent.click(screen.container.querySelector<HTMLButtonElement>('#ms-device')!);
		// the list opens in a portal, outside the test container
		await page.getByRole('option', { exact: true, name: 'CUDA0' }).click();
		await expect
			.element(screen.getByTestId('draft-update'))
			.toHaveTextContent('{"device":"CUDA0"}');

		await userEvent.click(screen.container.querySelector<HTMLButtonElement>('#ms-device')!);
		await page.getByRole('option', { name: /^Inherit: CUDA0/ }).click();
		await expect.element(screen.getByTestId('draft-update')).toHaveTextContent('{}');
	});

	it('writes both keys in "per request x slots" mode', async () => {
		const screen = await mount();

		await screen.getByRole('radio', { name: /per request/i }).click();
		await expect
			.element(screen.getByTestId('draft-update'))
			.toHaveTextContent(/"kv-unified-per-slot":"\d+"/);
		await expect
			.element(screen.getByTestId('ms-context-help'))
			.toHaveTextContent(/Every one of the 4 slots/);
	});
});
