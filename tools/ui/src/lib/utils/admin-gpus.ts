/**
 * GPU cards and diagnostics of the admin panel (stage 2): memory bar
 * segments, the words for where a reading comes from, the "no GPU" states
 * and the plain-text diagnostics report. Works for any number and mix of
 * GPUs (CUDA, ROCm, Vulkan, SYCL...) and for none. Pure, covered by unit
 * tests.
 */

import { ADMIN_NVIDIA_DOCKER_HINT } from '$lib/constants';
import {
	AdminGpuBackendState,
	AdminGpuMapping,
	AdminGpuMemorySource,
	AdminModelMemorySource,
	AdminProbeError
} from '$lib/enums';
import type {
	AdminGpuBarSegment,
	ApiAdminDiagnosticItem,
	ApiAdminDiagnostics,
	ApiAdminGpu,
	ApiAdminGpus
} from '$lib/types/admin';
import { formatBytes } from '$lib/utils/admin';
import { formatAge } from '$lib/utils/admin-fit';
import { isRunningStatus } from '$lib/utils/admin-model-settings';

/** Words for the source of a model's memory figure. */
export function describeModelMemorySource(source: string | null | undefined): string {
	switch (source) {
		case AdminModelMemorySource.REPORTED:
			return 'reported by the model';
		case AdminModelMemorySource.MEASURED:
			return 'measured';
		case AdminModelMemorySource.ESTIMATE:
			return 'estimate';
		default:
			return source ?? '';
	}
}

/**
 * Segments of a card bar: the running models with a memory figure (largest
 * first), the rest of the used memory, then free memory. The rest is
 * "other (driver, runtime, other processes)" only when every running model
 * on the card has a figure; otherwise it is that model, "estimate pending"
 * while its file is read, or "in use, not broken down". Model figures are
 * estimates and can exceed what the card reports as used; the bar then
 * scales them down to the used part.
 */
export function gpuBarSegments(gpu: ApiAdminGpu): AdminGpuBarSegment[] {
	const total = gpu.total ?? 0;
	const used = Math.max(0, gpu.used ?? 0);
	const free = Math.max(0, gpu.free ?? Math.max(0, total - used));

	if (total <= 0) return [];

	const running = gpu.models.filter((model) => isRunningStatus(model.status));
	const withMemory = running
		.filter((model) => model.memory && model.memory.bytes > 0)
		.sort((a, b) => (b.memory?.bytes ?? 0) - (a.memory?.bytes ?? 0));
	const pending = running.filter((model) => !model.memory);
	const modelBytes = withMemory.reduce((sum, model) => sum + (model.memory?.bytes ?? 0), 0);
	const scale = modelBytes > used && modelBytes > 0 ? used / modelBytes : 1;
	const segments: AdminGpuBarSegment[] = withMemory.map((model) => ({
		bytes: model.memory?.bytes ?? 0,
		fraction: ((model.memory?.bytes ?? 0) * scale) / total,
		key: `model:${model.id}`,
		kind: 'model',
		label: model.id,
		modelId: model.id,
		source: model.memory?.source ?? null
	}));
	const rest = Math.max(0, used - modelBytes);

	if (rest > 0) {
		const waiting = pending.some((model) => model.facts_status === 'pending');

		segments.push(
			pending.length > 0
				? {
						bytes: rest,
						fraction: rest / total,
						key: 'pending',
						kind: 'pending',
						label: waiting
							? `${pending.map((model) => model.id).join(', ')} (estimate pending) and other use`
							: `in use, not broken down (${pending.map((model) => model.id).join(', ')} and other use)`,
						modelId: pending.length === 1 ? pending[0].id : null,
						source: null
					}
				: {
						bytes: rest,
						fraction: rest / total,
						key: 'other',
						kind: 'other',
						label: 'other (driver, runtime, other processes)',
						modelId: null,
						source: null
					}
		);
	}

	segments.push({
		bytes: free,
		fraction: Math.min(1, free / total),
		key: 'free',
		kind: 'free',
		label: 'free',
		modelId: null,
		source: null
	});

	return segments;
}

/** "live (nvidia-smi)", "measured 5 min ago (no live reading)", "unknown". */
export function describeGpuMemorySource(gpu: ApiAdminGpu): string {
	switch (gpu.memory_source) {
		case AdminGpuMemorySource.VENDOR_TOOL:
			return gpu.tool ? `live (${gpu.tool})` : 'live';
		case AdminGpuMemorySource.INVENTORY:
			return `measured ${formatAge(gpu.memory_age_ms)} ago, no live reading`;
		default:
			return 'memory unknown';
	}
}

/** Badge text when vendor rows were not matched by PCI bus id, null when they were. */
export function describeGpuMapping(gpu: ApiAdminGpu, vendorRowsRead = true): string | null {
	// no vendor tool ran at all (off, not found, unsupported): nothing could be matched
	if (!vendorRowsRead) return null;

	switch (gpu.mapping) {
		case AdminGpuMapping.PCI:
			return null;
		case AdminGpuMapping.SINGLE:
			return 'matched as the only GPU';
		case AdminGpuMapping.NONE:
			return null;
		default:
			return 'memory not matched to this device';
	}
}

/** "4.7 GB free of 11 GB" or null when the card has no figures. */
export function describeGpuFree(gpu: ApiAdminGpu): string | null {
	if (gpu.total === null || gpu.total <= 0 || gpu.free === null) return null;

	return `${formatBytes(gpu.free)} free of ${formatBytes(gpu.total)}`;
}

/** Banner of the GPU strip when the server has no usable GPU, null when it has. */
export interface AdminGpuState {
	level: 'info' | 'warning' | 'error';
	title: string;
	text: string;
}

export function describeGpuBackend(gpus: ApiAdminGpus): AdminGpuState | null {
	const backend = gpus.gpu_backend;

	if (backend?.state === AdminGpuBackendState.CPU_ONLY) {
		return {
			level: 'info',
			text: 'This server build has no GPU backend, so models run on the CPU. A GPU build (CUDA, ROCm, Vulkan...) is needed to use GPUs.',
			title: 'CPU only'
		};
	}

	if (backend?.state === AdminGpuBackendState.NO_DEVICES) {
		const libraries = backend.libraries?.length ? ` (${backend.libraries.join(', ')})` : '';

		return {
			level: 'warning',
			text: `A GPU backend is present${libraries}, but it found no device: check the driver, permissions such as /dev/kfd or /dev/dri, or whether the container was started without GPUs. ${backend.message ?? ''}`.trim(),
			title: 'GPU backend did not initialise'
		};
	}

	if (gpus.inventory && gpus.inventory.ran && !gpus.inventory.ok) {
		return {
			level: 'warning',
			text: `The server could not measure its GPUs (${gpus.inventory.error || 'failed'}${gpus.inventory.message ? `: ${gpus.inventory.message}` : ''}). Device names come from the backend; Re-detect GPUs tries again.`,
			title: 'GPU detection failed'
		};
	}

	return null;
}

/** Why live memory figures are missing or old, null when the vendor tool works. */
export function describeProbeState(gpus: ApiAdminGpus): string | null {
	const probe = gpus.probe;

	if (!probe || probe.ok) {
		return probe?.stale ? `Live memory reading is ${formatAge(probe.age_ms)} old.` : null;
	}

	const nvidia = gpus.gpus.some((gpu) => gpu.vendor === 'nvidia');
	const withDockerHint = (text: string) => (nvidia ? `${text} ${ADMIN_NVIDIA_DOCKER_HINT}` : text);

	switch (probe.error) {
		case AdminProbeError.DISABLED:
			return 'The live memory reading is turned off (PROVISION_ADMIN_GPU_PROBE=off): memory is shown as measured at startup or at the last re-detect.';
		case AdminProbeError.NOT_FOUND:
			return withDockerHint(
				'No GPU memory tool (nvidia-smi, amd-smi, rocm-smi) was found: memory is shown as measured at startup or at the last re-detect.'
			);
		case AdminProbeError.UNSUPPORTED:
			return 'There is no live memory tool for these GPUs: memory is shown as measured at startup or at the last re-detect.';
		case AdminProbeError.TIMEOUT:
			return `The GPU memory tool did not answer in time${probe.message ? ` (${probe.message})` : ''}.`;
		case AdminProbeError.PROBE_STUCK:
			return `The GPU memory tool is stuck${probe.message ? ` (${probe.message})` : ''}: a GPU or its driver may have failed.`;
		case AdminProbeError.PARSE_ERROR:
			return `The output of the GPU memory tool could not be read${probe.message ? `: ${probe.message}` : ''}.`;
		default:
			return `The GPU memory tool failed${probe.message ? `: ${probe.message}` : ''}.`;
	}
}

/** A diagnostic value as one line of text. */
export function formatDiagnosticValue(value: unknown): string {
	if (value === null || value === undefined) return '—';

	if (typeof value === 'boolean') return value ? 'yes' : 'no';

	if (typeof value === 'string' || typeof value === 'number') return String(value);

	if (Array.isArray(value)) return value.map((item) => formatDiagnosticValue(item)).join(', ');

	if (typeof value === 'object') {
		const record = value as Record<string, unknown>;
		const parts: string[] = [];

		for (const [key, inner] of Object.entries(record)) {
			if (inner === null || inner === undefined || inner === '') continue;

			if (Array.isArray(inner) && inner.length === 0) continue;

			if (typeof inner === 'object' && !Array.isArray(inner)) {
				parts.push(`${key}: {${formatDiagnosticValue(inner)}}`);
			} else if (Array.isArray(inner) && inner.some((item) => typeof item === 'object')) {
				parts.push(
					`${key}: [${inner.map((item) => `{${formatDiagnosticValue(item)}}`).join(', ')}]`
				);
			} else {
				parts.push(`${key}: ${formatDiagnosticValue(inner)}`);
			}
		}

		return parts.join('; ');
	}

	return String(value);
}

/** One short value for the diagnostics panel (objects summarised, details in the report). */
export function summarizeDiagnosticValue(item: ApiAdminDiagnosticItem): string {
	const value = item.value;

	if (value === null || value === undefined) return '—';

	if (typeof value !== 'object') return formatDiagnosticValue(value);

	const record = value as Record<string, unknown>;

	switch (item.id) {
		case 'binary':
			return String(record.version ?? '');
		case 'gpu_backend':
			return String(record.message ?? record.state ?? '');
		case 'inventory': {
			const devices = Array.isArray(record.devices) ? record.devices.length : 0;

			return record.ok
				? `${devices} device${devices === 1 ? '' : 's'}`
				: String(record.error || 'failed');
		}
		case 'vendor_tool': {
			const tools = Array.isArray(record.tools) ? (record.tools as Record<string, unknown>[]) : [];

			return tools.length > 0
				? tools
						.map((tool) => `${tool.tool}${tool.ok ? '' : ` (${tool.error || 'failed'})`}`)
						.join(', ')
				: String(record.error || record.mode || '—');
		}
		case 'state_dir':
			return String(record.path ?? '');
		case 'model_settings':
			return `${record.entries ?? 0} saved model${record.entries === 1 ? '' : 's'}${record.read_only ? ', read-only' : ''}`;
		case 'audit': {
			// stage 3 M3: {persisted, error, since, in_memory, lost, max_bytes, ring_max, files}
			if (record.persisted === false) {
				return `in memory only (${Number(record.in_memory) || 0} records)`;
			}

			const files = Array.isArray(record.files) ? (record.files as Record<string, unknown>[]) : [];
			const bytes = files.reduce((sum, file) => sum + (Number(file.bytes) || 0), 0);

			return `written to the state directory (${files.length} file${files.length === 1 ? '' : 's'}, ${formatBytes(bytes)})`;
		}
		default:
			return formatDiagnosticValue(value);
	}
}

/** Plain-text report for "Copy report": one block per item. */
export function formatDiagnosticsReport(
	diagnostics: ApiAdminDiagnostics,
	header: string[] = []
): string {
	const lines = [
		'Provision LLM Engine diagnostics',
		`generated: ${diagnostics.generated_at}`,
		...header,
		''
	];

	for (const item of diagnostics.items) {
		lines.push(
			`[${item.status.toUpperCase()}] ${item.label}: ${formatDiagnosticValue(item.value)}`
		);

		if (item.hint) lines.push(`    ${item.hint}`);
	}

	return lines.join('\n') + '\n';
}
