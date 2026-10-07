/**
 *
 * ADMIN
 *
 * Admin panel of provision-server (router mode with PROVISION_ADMIN=1),
 * shown on the `#/admin` route. State lives in adminStore; the model table
 * is fed by modelsStore and the public model routes, only the settings need
 * the admin key.
 *
 */

/**
 * **AdminScreen** - The admin page
 *
 * Renders one of four states driven by the GET /admin/status probe:
 * checking, "Admin API is not available from this address" (with Retry),
 * the admin key form, or the panel itself with its Models and Settings tabs.
 */
export { default as AdminScreen } from './AdminScreen.svelte';

/**
 * **AdminKeyForm** - Admin key input
 *
 * Verifies the key against the server and keeps it for the browser tab
 * (sessionStorage), or on the device when "Remember on this device" is ticked.
 */
export { default as AdminKeyForm } from './AdminKeyForm.svelte';

/**
 * **AdminUnavailable** - "Admin API is not available from this address"
 *
 * Shown when the probe failed (404 / 403 / network). Offers a manual Retry;
 * nothing is retried automatically.
 */
export { default as AdminUnavailable } from './AdminUnavailable.svelte';

/**
 * **AdminModelsTab** - Model management
 *
 * Model table with Load / Unload / Delete / Cancel, Rescan, Add model, the
 * free space of the model cache and the results of finished downloads.
 * Uses the existing public model routes only.
 */
export { default as AdminModelsTab } from './AdminModelsTab.svelte';

/**
 * **AdminModelsTable** - Rows of the model table
 *
 * Name, tags, source, status with load / download progress, pinned badge
 * and the row actions.
 */
export { default as AdminModelsTable } from './AdminModelsTable.svelte';

/**
 * **AdminSettingsTab** - Router settings form
 *
 * Max loaded models, GPU for new models and pinned models, validated in the
 * form and by the server (POST /admin/settings).
 */
export { default as AdminSettingsTab } from './AdminSettingsTab.svelte';

/**
 * **AdminServerInfo** - Read-only facts from GET /admin/status
 *
 * Where the admin key comes from, the state / cache / models directories
 * and whether the server can download.
 */
export { default as AdminServerInfo } from './AdminServerInfo.svelte';

/**
 * **AdminGpuPanel** - GPU strip of the Models tab (stage 2)
 *
 * One card per GPU device of the server, whatever the backend (CUDA, ROCm,
 * Vulkan, SYCL...) and count, the CPU-only and "backend found no device"
 * states, why live memory is missing, unmatched vendor rows, Refresh and
 * Re-detect GPUs.
 */
export { default as AdminGpuPanel } from './AdminGpuPanel.svelte';

/**
 * **AdminGpuCard** - One GPU: memory bar, where the figures come from and
 * their age, settling and mapping state, and the models placed on it.
 */
export { default as AdminGpuCard } from './AdminGpuCard.svelte';

/**
 * **AdminFitIndicator** - Will the model fit (warns, never blocks)
 *
 * Verdict, per-device need against the free memory, and whether it was
 * checked by llama.cpp or is a rough estimate.
 */
export { default as AdminFitIndicator } from './AdminFitIndicator.svelte';

/**
 * **AdminModelFailure** - Why a model did not start: reason, hint and the
 * output tail with Copy.
 */
export { default as AdminModelFailure } from './AdminModelFailure.svelte';

/**
 * **AdminModelSettingsForm** - The fields of the per-model settings dialog
 */
export { default as AdminModelSettingsForm } from './AdminModelSettingsForm.svelte';

/**
 * **AdminSettingField** - One setting: label, origin, clear button, lock
 * and messages around its control.
 */
export { default as AdminSettingField } from './AdminSettingField.svelte';

/**
 * **AdminNumberInput** - Number field that keeps the typed text while it
 * has the focus.
 */
export { default as AdminNumberInput } from './AdminNumberInput.svelte';

/**
 * **AdminSettingsImportPreview** - What an imported settings file would
 * change: current value, value from the file, a checkbox each.
 */
export { default as AdminSettingsImportPreview } from './AdminSettingsImportPreview.svelte';

/**
 * **AdminDiagnostics** - What the server detected about itself (stage 2)
 *
 * Deployment, binary, GPU backend, inventory, vendor tool, memory mapping,
 * state directory, admin key, proxy, MODEL_DEVICES and the settings file,
 * each ok / warning / error with a hint, plus Copy report.
 */
export { default as AdminDiagnostics } from './AdminDiagnostics.svelte';
