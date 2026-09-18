import type { AppConfig } from '@/lib/api'

/** How many things did the operator actually touch? Used for the save bar's
 *  "N unsaved changes", so it counts entities rather than JSON characters. */
export function countChanges(current: AppConfig, saved: AppConfig): number {
  let count = 0
  if (JSON.stringify(current.server) !== JSON.stringify(saved.server)) count += 1
  count += countList(current.providers, saved.providers)
  count += countList(current.routes, saved.routes)
  count += countList(current.clients ?? [], saved.clients ?? [])
  return count
}

/** Compares two lists position by position.
 *
 *  Keying by `id` (or by a route's `model`) looked tidier but miscounted exactly
 *  where the operator needs the number most: ids are free text being typed, so
 *  two new relays are both `""` for a while, and `find()` returns the first of
 *  them for either key — a second edit then counted as zero changes, and a
 *  reordering or a duplicate id counted as none at all. Position is what the
 *  form actually edits, so position is what gets compared. */
function countList<T>(current: T[], saved: T[]): number {
  let count = 0
  for (let index = 0; index < Math.max(current.length, saved.length); index += 1) {
    if (JSON.stringify(current[index]) !== JSON.stringify(saved[index])) count += 1
  }
  return count
}
