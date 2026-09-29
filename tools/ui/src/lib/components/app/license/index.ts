/**
 * **LicenseActivationForm** - License status card + activation code input
 *
 * Shows the /license/status snapshot (mode, budget, expiry) and submits an
 * activation code to POST /license/activate. Used by the settings tab and
 * the activation dialog.
 */
export { default as LicenseActivationForm } from './LicenseActivationForm.svelte';

/**
 * **LicenseBanner** - Chat screen notice for trial / locked servers
 *
 * Visible while the request budget is limited (trial) or inference is
 * refused (no / expired license); offers the activation dialog.
 */
export { default as LicenseBanner } from './LicenseBanner.svelte';
