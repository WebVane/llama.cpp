/**
 * License subsystem (provision-server) — wire shapes of GET /license/status
 * and POST /license/activate. The snapshot is deliberately sanitised by the
 * server: no fingerprint, no signing key, nothing machine-identifying.
 */

/** `disabled` = the build has no license subsystem (nothing is gated). */
export type ApiLicenseMode = 'trial' | 'online' | 'offline' | 'none' | 'disabled';

export type ApiLicenseStatusValue = 'valid' | 'expired' | 'none' | 'disabled';

export interface ApiLicenseStatus {
	mode: ApiLicenseMode;
	status: ApiLicenseStatusValue;
	/** Inference requests consumed in the current budget (trial: lifetime). */
	used: number;
	/** Request budget; -1 = unlimited, 0 = no license loaded. */
	limit: number;
	product?: string;
	tariff?: string;
	/** ISO-8601, only for a loaded .lic */
	expiresAt?: string;
	/** ISO-8601 start of the next 30-day budget window (licensed modes only) */
	windowResetsAt?: string;
	/** Why requests are currently refused (deny reason label), empty when valid */
	reason?: string;
	message?: string;
}

export interface ApiLicenseActivateResponse extends ApiLicenseStatus {
	activated: boolean;
	detail: string;
}
