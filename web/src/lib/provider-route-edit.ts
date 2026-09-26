import type { AppConfig, ModelPricing, ProviderConfig, RouteConfig } from '@/lib/api'

export interface PriceRow extends ModelPricing { id: number; model: string }
export interface EditorIssue { key: string; values?: Record<string, string | number> }

export const priceRowsOf = (prices: ProviderConfig['model_prices']): PriceRow[] =>
  Object.entries(prices ?? {}).map(([model, price], id) => ({ id, model, ...price }))

export const pricesOfRows = (rows: PriceRow[]): ProviderConfig['model_prices'] =>
  Object.fromEntries(rows.map(({ model, price_in_per_million, price_out_per_million }) =>
    [model.trim(), { price_in_per_million, price_out_per_million }]))

export function providerIssue(provider: ProviderConfig, otherIds: string[], rows: PriceRow[]): EditorIssue | null {
  const id = provider.id.trim()
  if (!id || !provider.base_url.trim()) return { key: 'managementRequiredProvider' }
  if (otherIds.includes(id)) return { key: 'managementDuplicateProvider', values: { id } }
  try {
    const url = new URL(provider.base_url.trim())
    if (!['https:', 'http:'].includes(url.protocol) || !url.hostname) return { key: 'managementInvalidUrl' }
  } catch { return { key: 'managementInvalidUrl' } }
  const names = new Set<string>()
  for (const row of rows) {
    const model = row.model.trim()
    if (!model) return { key: 'managementPriceModelRequired' }
    if (names.has(model)) return { key: 'managementDuplicatePrice', values: { model } }
    names.add(model)
    if (![row.price_in_per_million, row.price_out_per_million].every((price) => Number.isFinite(price) && price >= 0)) return { key: 'managementInvalidPrice' }
  }
  if (![provider.price_in_per_million, provider.price_out_per_million].every((price) => Number.isFinite(price) && price >= 0)) return { key: 'managementInvalidPrice' }
  if (![provider.priority, provider.weight, provider.timeout_sec, provider.connect_timeout_sec, provider.max_concurrent, provider.requests_per_minute].every(Number.isInteger)
    || provider.timeout_sec < 1 || provider.connect_timeout_sec < 1 || provider.max_concurrent < 0 || provider.requests_per_minute < 0) return { key: 'managementInvalidTraffic' }
  if (provider.protocol === 'vertex' && (!provider.project.trim() || !provider.credentials_file.trim())) return { key: 'managementVertexRequired' }
  if (provider.protocol === 'bedrock' && !provider.region.trim()) return { key: 'managementBedrockRegionRequired' }
  return null
}

export function routeIssue(route: RouteConfig, otherModels: string[], providerIds: string[]): EditorIssue | null {
  const model = route.model.trim()
  if (!model || !route.targets.length || route.targets.some((target) => !target.provider)) return { key: 'managementRequiredRoute' }
  if (otherModels.includes(model)) return { key: 'managementDuplicateRoute', values: { model } }
  const unknown = route.targets.find((target) => !providerIds.includes(target.provider))
  return unknown ? { key: 'managementUnknownTarget', values: { id: unknown.provider } } : null
}

/** Probe endpoints read saved provider settings. Cosmetic/model edits may be
 * pending, but a changed address, protocol or credential cannot be probed by id. */
export function providerConnectionKey(provider: ProviderConfig): string {
  return JSON.stringify([
    provider.id, provider.base_url, provider.api_key, provider.api_key_source, !!provider.api_key_clear,
    provider.protocol, provider.api_version, provider.region, provider.project, provider.credentials_file,
    provider.aws_access_key, provider.aws_secret_key, provider.aws_session_token,
    provider.timeout_sec, provider.connect_timeout_sec,
    Object.entries(provider.headers ?? {}).sort(([a], [b]) => a.localeCompare(b)),
  ])
}

export const canProbeProvider = (provider: ProviderConfig, saved?: ProviderConfig): boolean =>
  !!saved && providerConnectionKey(provider) === providerConnectionKey(saved)

export function providerRemovalImpact(config: AppConfig, id: string) {
  const routes = config.routes.filter((route) => route.targets.some((target) => target.provider === id))
  return { targets: routes.reduce((count, route) => count + route.targets.filter((target) => target.provider === id).length, 0),
    routes: routes.filter((route) => route.targets.every((target) => target.provider === id)).map((route) => route.model) }
}

export function removeProvider(config: AppConfig, index: number): void {
  const provider = config.providers[index]
  if (!provider) return
  const id = provider.id
  config.providers.splice(index, 1)
  config.routes = config.routes.flatMap((route) => {
    const targets = route.targets.filter((target) => target.provider !== id)
    return targets.length === route.targets.length ? [route] : targets.length ? [{ ...route, targets }] : []
  })
}

export function replaceProvider(config: AppConfig, index: number, provider: ProviderConfig): void {
  const previous = config.providers[index]
  if (!previous) return
  config.providers[index] = provider
  if (previous.id !== provider.id) {
    for (const route of config.routes) {
      route.targets = route.targets.map((target) => target.provider === previous.id ? { ...target, provider: provider.id } : target)
    }
  }
}

export const mergeModels = (existing: string[], discovered: string[]): string[] =>
  [...new Set([...existing, ...discovered].map((model) => model.trim()).filter(Boolean))]
