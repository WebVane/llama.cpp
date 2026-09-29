/**
 * licenseStore - License state of the connected provision-server
 *
 * Owns the /license/status snapshot (mode, budget, validity) and drives
 * online activation. A server without the license subsystem (older build,
 * or one compiled without it) answers 404 / mode=disabled — the store then
 * reports `isEnabled === false` and the UI shows nothing license related.
 * A transient failure (503 while the model loads, network hiccup) keeps the
 * last snapshot and is surfaced as `unavailable` instead.
 */

import { LicenseService } from '$lib/services/license.service';
import type { ApiLicenseStatus } from '$lib/types';
import { ApiError } from '$lib/utils';

const REFRESH_INTERVAL_MS = 30_000;
// the server answers 503 to everything while a model is loading — poll faster then
const UNAVAILABLE_RETRY_MS = 2_000;

class LicenseStore {
	activating = $state(false);
	/** The server has no license routes at all (404) — nothing to show. */
	disabled = $state(false);
	error = $state<string | null>(null);
	loading = $state(false);
	status = $state<ApiLicenseStatus | null>(null);
	/** Last status fetch failed for a reason other than "no subsystem". */
	unavailable = $state<string | null>(null);

	private pollers = 0;
	private refreshTimer: ReturnType<typeof setInterval> | null = null;
	private retryTimer: ReturnType<typeof setTimeout> | null = null;

	/** The build gates requests through the license subsystem. */
	get isEnabled(): boolean {
		return !!this.status && this.status.mode !== 'disabled';
	}

	/** Inference is refused until a license is activated / renewed. */
	get isLocked(): boolean {
		return this.isEnabled && !this.isValid;
	}

	get isTrial(): boolean {
		return this.status?.mode === 'trial';
	}

	get isValid(): boolean {
		return this.status?.status === 'valid';
	}

	/** Requests left in the current budget; null when unlimited, unknown or no license. */
	get remaining(): number | null {
		if (!this.status || this.status.limit <= 0) return null;

		return Math.max(0, this.status.limit - this.status.used);
	}

	/**
	 * Online activation with the code issued by the License Server.
	 * @returns true when the server accepted the code and switched to a licensed mode
	 */
	async activate(activationCode: string): Promise<boolean> {
		const code = activationCode.trim();

		if (!code || this.activating) return false;

		this.activating = true;
		this.error = null;

		try {
			const result = await LicenseService.activate(code);

			this.status = result;
			this.error = result.activated ? null : result.detail;

			return result.activated;
		} catch (error) {
			this.error = error instanceof Error ? error.message : String(error);

			return false;
		} finally {
			this.activating = false;
		}
	}

	async fetch(): Promise<void> {
		if (this.loading) return;

		this.loading = true;

		try {
			this.status = await LicenseService.status();
			this.disabled = false;
			this.unavailable = null;
		} catch (error) {
			if (error instanceof ApiError && error.status === 404) {
				// no license routes: the build has no subsystem
				this.status = null;
				this.disabled = true;
				this.unavailable = null;
			} else {
				// keep the last snapshot; retry soon while the server is still loading
				this.unavailable = error instanceof Error ? error.message : String(error);

				if (this.pollers > 0 && !this.retryTimer) {
					this.retryTimer = setTimeout(() => {
						this.retryTimer = null;
						this.fetch();
					}, UNAVAILABLE_RETRY_MS);
				}
			}
		} finally {
			this.loading = false;
		}
	}

	/**
	 * Keep the snapshot fresh while at least one component is interested
	 * (the chat banner, the settings tab). Returns the matching stop function.
	 */
	startPolling(): () => void {
		this.pollers++;

		if (this.pollers === 1) {
			this.fetch();
			this.refreshTimer = setInterval(() => this.fetch(), REFRESH_INTERVAL_MS);
		}

		return () => {
			this.pollers = Math.max(0, this.pollers - 1);

			if (this.pollers === 0) {
				if (this.refreshTimer) {
					clearInterval(this.refreshTimer);
					this.refreshTimer = null;
				}

				if (this.retryTimer) {
					clearTimeout(this.retryTimer);
					this.retryTimer = null;
				}
			}
		};
	}
}

export const licenseStore = new LicenseStore();
