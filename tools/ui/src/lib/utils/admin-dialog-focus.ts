/**
 * Focus handling shared by the admin dialogs.
 *
 * When the focused element of a dialog disappears (a step of the dialog
 * changes: Upload, Upload more models, a mode switch), the focus trap of the
 * dialog moves the focus to its first focusable element: the close button of
 * the header, which the dialog components style with a ring on any focus. A
 * pointer user would see that ring after every step, so the dialog itself
 * (tabindex -1, as on open) takes the focus back instead. Focus that the
 * keyboard put on the close button (:focus-visible), or that came from a
 * click or Tab (it has a previous element), stays where it is.
 */

/** The close button of a bits-ui dialog */
const DIALOG_CLOSE_SELECTOR = '[data-dialog-close]';

/** `focusin` handler of an admin dialog's content element. */
export function keepFocusOffDialogClose(event: FocusEvent, content: HTMLElement | null): void {
	const target = event.target;

	if (!content || !(target instanceof HTMLElement) || target === content) return;

	// moved from another element (a click, Tab): not the focus trap after a removed element
	if (event.relatedTarget !== null) return;

	try {
		if (!target.matches(DIALOG_CLOSE_SELECTOR) || target.matches(':focus-visible')) return;
	} catch {
		// a browser without :focus-visible: leave the focus alone
		return;
	}

	content.focus({ preventScroll: true });
}
