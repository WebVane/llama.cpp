/**
 * LicenseService - Stateless license API layer
 *
 * Wraps the two license endpoints of provision-server: the read-only
 * status snapshot (never consumes request quota) and online activation
 * against the License Server. No reactive state; consumed by licenseStore.
 */

import { API_LICENSE } from '$lib/constants';
import type { ApiLicenseActivateResponse, ApiLicenseStatus } from '$lib/types';
import { apiFetch, apiPost } from '$lib/utils';

export class LicenseService {
	/**
	 * Online activation. The server contacts the License Server, persists the
	 * returned .lic and re-initialises; a refused activation comes back as an
	 * HTTP 502 whose message carries the reason (bad code, no free slot, ...).
	 *
	 * @param activationCode - Code issued by the License Server (XXXX-XXXX-XXXX)
	 */
	static async activate(activationCode: string): Promise<ApiLicenseActivateResponse> {
		return apiPost<ApiLicenseActivateResponse, { activationCode: string }>(API_LICENSE.ACTIVATE, {
			activationCode
		});
	}

	/**
	 * Read-only license snapshot. Safe to poll: exempt from the request
	 * counter on the server side.
	 */
	static async status(): Promise<ApiLicenseStatus> {
		return apiFetch<ApiLicenseStatus>(API_LICENSE.STATUS, { authOnly: true });
	}
}
