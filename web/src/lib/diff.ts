import type { AppConfig } from '@/lib/api'

/** How many things did the operator actually touch? Used for the save bar's
 *  "N unsaved changes", so it counts entities rather than JSON characters. */
export function countChanges(current: AppConfig, saved: AppConfig): number {
  let count = 0
  if (JSON.stringify(current.server) !== JSON.stringify(saved.server)) count += 1

  const providerIds = new Set([
    ...current.providers.map((provider) => provider.id),
    ...saved.providers.map((provider) => provider.id),
  ])
  for (const id of providerIds) {
    const a = current.providers.find((provider) => provider.id === id)
    const b = saved.providers.find((provider) => provider.id === id)
    if (JSON.stringify(a) !== JSON.stringify(b)) count += 1
  }

  const routeModels = new Set([
    ...current.routes.map((route) => route.model),
    ...saved.routes.map((route) => route.model),
  ])
  for (const model of routeModels) {
    const a = current.routes.find((route) => route.model === model)
    const b = saved.routes.find((route) => route.model === model)
    if (JSON.stringify(a) !== JSON.stringify(b)) count += 1
  }

  return count
}
