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

/** A successful save clears secrets and flags from the submitted draft, while
 * edits made during the request remain available for the next save. */
export function settleSavedDraft(working: AppConfig | null, submitted: AppConfig, saved: AppConfig): AppConfig {
  return mergeSavedValue(working ?? submitted, submitted, saved) as AppConfig
}

const isRecord = (value: unknown): value is Record<string, unknown> =>
  value !== null && typeof value === 'object' && !Array.isArray(value)
const identityOf = (value: unknown, key: string) => isRecord(value) ? value[key] : undefined

/** Rebase only edits made after submission onto the server's canonical reply.
 * Keeping the entire newer draft also kept already-saved clear flags and raw
 * secrets, making the next save silently submit them again. */
function mergeSavedValue(current: unknown, submitted: unknown, saved: unknown): unknown {
  if (JSON.stringify(current) === JSON.stringify(submitted)) return structuredClone(saved)
  if (Array.isArray(current) && Array.isArray(submitted) && Array.isArray(saved)) {
    // Provider and route lists may be reordered or shortened during a save.
    // Match their stable identities so redacted credentials follow their relay.
    const identity = ['id', 'model'].find((key) => submitted.length > 0 &&
      submitted.every((entry) => isRecord(entry) && typeof entry[key] === 'string') &&
      new Set(submitted.map((entry) => identityOf(entry, key))).size === submitted.length)
    if (identity) {
      return current.map((entry) => {
        const before = isRecord(entry) ? submitted.find((candidate) => identityOf(candidate, identity) === entry[identity]) : undefined
        const after = isRecord(entry) ? saved.find((candidate) => identityOf(candidate, identity) === entry[identity]) : undefined
        return before && after ? mergeSavedValue(entry, before, after) : structuredClone(entry)
      })
    }
    return structuredClone(current)
  }
  if (isRecord(current) && isRecord(submitted) && isRecord(saved)) {
    const result = structuredClone(saved)
    for (const key of new Set([...Object.keys(submitted), ...Object.keys(current)])) {
      if (!(key in current)) delete result[key]
      else if (!(key in submitted)) result[key] = structuredClone(current[key])
      else if (JSON.stringify(current[key]) !== JSON.stringify(submitted[key])) {
        result[key] = mergeSavedValue(current[key], submitted[key], saved[key])
      }
    }
    return result
  }
  return structuredClone(current)
}
