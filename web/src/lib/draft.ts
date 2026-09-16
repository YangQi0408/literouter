import type { AppConfig } from '@/lib/api'

/** Whether a config the server just handed us should replace what is in the form.
 *
 *  The console re-reads the config on mount, on a tab switch and after a save,
 *  and a re-read that overwrote an edited draft would discard typing with no
 *  undo and no warning — which it did, until this decision was given a name.
 *  Only the actions that mean "the draft is settled" (save, reload, entering a
 *  key) pass `force`; everything else keeps what the operator has.
 *
 *  An *untouched* draft is the other case: taking the server's copy then keeps
 *  the form in step with a config edited outside the console, and costs nothing
 *  because there is nothing to lose. */
export function shouldReplaceDraft(
  working: AppConfig | null,
  loaded: AppConfig | null,
  force: boolean,
): boolean {
  if (!working) return true
  if (force) return true
  return JSON.stringify(working) === JSON.stringify(loaded)
}
