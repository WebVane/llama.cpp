import type { PageLoad } from './$types';
import { initStores } from '$lib/stores/init';
import { validateApiKey } from '$lib/utils';

export const load: PageLoad = async ({ fetch }) => {
	// same gating as the chat pages: the settings store must have read
	// localStorage before the API key probe, and a server started with
	// --api-key shows the API key screen first
	await initStores();
	await validateApiKey(fetch);
};
