import type { AppConfig, ProviderConfig, RouteConfig, ServerConfig } from '@/lib/api'

/** Spread `patch` over `base`, ignoring keys the server left out or nulled. */
const merge = <T extends object>(base: T, patch: Partial<T> | null | undefined): T => {
  const out = { ...base }
  for (const [key, value] of Object.entries(patch ?? {})) {
    if (value !== undefined && value !== null) {
      ;(out as Record<string, unknown>)[key] = value
    }
  }
  return out
}

/** The defaults the contract declares, mirrored here so a field the server
 *  omitted still has a value to render. */
export const SERVER_DEFAULTS: ServerConfig = {
  host: '127.0.0.1',
  port: 8787,
  tls_cert_file: '',
  tls_key_file: '',
  api_key: '',
  pass_through_unknown: true,
  max_attempts: 0,
  routing_policy: 'priority',
  request_deadline_sec: 0,
  session_affinity_sec: 0,
  reload_on_change: false,
  circuit_failure_threshold: 3,
  circuit_cooldown_sec: 30,
  skip_open_circuits: true,
  log_capacity: 200,
  log_bodies: false,
  log_body_limit: 2048,
  persist_telemetry: true,
  web_ui: true,
  traffic_bucket_sec: 3600,
  traffic_bucket_count: 24,
  response_cache_ttl_sec: 0,
  response_cache_max_entries: 128,
  otlp_endpoint: '',
}

export const PROVIDER_DEFAULTS: ProviderConfig = {
  id: '',
  name: '',
  base_url: '',
  api_key: '',
  api_key_clear: false,
  enabled: true,
  priority: 100,
  weight: 1,
  timeout_sec: 120,
  connect_timeout_sec: 15,
  supports_stream: true,
  models: [],
  headers: {},
  chat_path: '/chat/completions',
  embeddings_path: '/embeddings',
  protocol: 'openai',
  // Written only when set, so every one of these has to be restored here — the
  // writer omitting a default is right for the file and wrong for the editors.
  api_version: '',
  region: '',
  project: '',
  credentials_file: '',
  aws_access_key: '',
  aws_secret_key: '',
  aws_session_token: '',
  max_concurrent: 0,
  requests_per_minute: 0,
  price_in_per_million: 0,
  price_out_per_million: 0,
  model_prices: {},
  note: '',
}

export const ROUTE_DEFAULTS: RouteConfig = { model: '', targets: [], enabled: true }

/** The server's writer omits a field that sits at its default, which is right for
 *  the file on disk and wrong for the console: `headers` on a relay with no
 *  custom headers, `protocol` on an OpenAI one, a price of 0, an empty `note`.
 *  Reading such a payload straight into the UI hands it `undefined` where the
 *  server means `{}`/`'openai'`/`0`, and the header editor's
 *  `Object.entries(undefined)` used to take the whole page down with it. So the
 *  defaults are put back here, once, at the boundary. */
export const normalizeProvider = (raw: Partial<ProviderConfig> | null | undefined): ProviderConfig => {
  const merged = merge(PROVIDER_DEFAULTS, raw)
  return {
    ...merged,
    // A `null` in the file arrives as `null` in the JSON; merge drops it, but a
    // wrong *type* would still reach the editors, so pin the shapes they
    // iterate over.
    models: Array.isArray(merged.models) ? merged.models : [],
    headers:
      merged.headers && typeof merged.headers === 'object' && !Array.isArray(merged.headers)
        ? merged.headers
        : {},
    model_prices:
      merged.model_prices && typeof merged.model_prices === 'object' && !Array.isArray(merged.model_prices)
        ? Object.fromEntries(
            Object.entries(merged.model_prices).map(([model, p]) => [
              model,
              {
                price_in_per_million: Number(p?.price_in_per_million) || 0,
                price_out_per_million: Number(p?.price_out_per_million) || 0,
              },
            ])
          )
        : {},
  }
}

export const normalizeRoute = (raw: Partial<RouteConfig> | null | undefined): RouteConfig => {
  const merged = merge(ROUTE_DEFAULTS, raw)
  return {
    ...merged,
    targets: Array.isArray(merged.targets)
      ? merged.targets.map((target) => ({ ...target, provider: target?.provider ?? '' }))
      : [],
  }
}

type PartialConfig = {
  schema?: number
  server?: Partial<ServerConfig> | null
  providers?: (Partial<ProviderConfig> | null)[] | null
  routes?: (Partial<RouteConfig> | null)[] | null
}

export const normalizeConfig = (raw: PartialConfig | null | undefined): AppConfig => ({
  schema: raw?.schema,
  server: merge(SERVER_DEFAULTS, raw?.server),
  providers: Array.isArray(raw?.providers) ? raw.providers.map(normalizeProvider) : [],
  routes: Array.isArray(raw?.routes) ? raw.routes.map(normalizeRoute) : [],
})
