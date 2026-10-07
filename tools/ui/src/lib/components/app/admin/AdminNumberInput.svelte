<script lang="ts">
	import { Input } from '$lib/components/ui/input';
	import { untrack } from 'svelte';

	interface Props {
		id: string;
		/** The value the field stands for now (draft, else inherited / default) */
		value: string | null;
		onValueChange: (text: string) => void;
		class?: string;
		disabled?: boolean;
		invalid?: boolean;
		max?: number;
		min?: number;
		placeholder?: string;
		step?: number;
	}

	let {
		class: className = 'w-36',
		disabled = false,
		id,
		invalid = false,
		max,
		min,
		onValueChange,
		placeholder,
		step,
		value
	}: Props = $props();

	// what the user typed; while the field has the focus it is not overwritten,
	// so clearing it to type a new number does not snap back to the inherited value
	let text = $state('');
	let focused = false;

	$effect(() => {
		const next = value ?? '';

		untrack(() => {
			if (!focused) text = next;
		});
	});
</script>

<!-- not bind:value: on a number input that yields a number or null, the text is kept as typed -->
<Input
	aria-invalid={invalid ? 'true' : undefined}
	class={className}
	{disabled}
	{id}
	inputmode="numeric"
	{max}
	{min}
	onblur={() => {
		focused = false;
		text = value ?? '';
	}}
	onfocus={() => (focused = true)}
	oninput={(event) => {
		text = event.currentTarget.value;
		onValueChange(text);
	}}
	{placeholder}
	{step}
	type="number"
	value={text}
/>
