/** Typed client for the proxy's admin API.
 *
 *  The shapes here mirror core/src/lr_json.cpp one-for-one; when a field is
 *  added there, it shows up here as a compile error rather than as a silent
 *  `undefined` in a table cell. */

export type LogLevel = 'info' | 'warn' | 'error'
export type LogKind = 'chat' | 'embeddings' | 'models' | 'admin' | 'system'
export type HealthState = 'unknown' | 'healthy' | 'degraded' | 'open'
export type Protocol = 'openai' | 'anthropic' | 'gemini' | 'openai_responses'

export interface LogEntry {
  seq: number
  time_unix: number
  time: string
  date: string
  datetime: string
  level: LogLevel
  request_id: string
  kind: LogKind
  model: string
  provider: string
  upstream_model: string
  status: number
  stream: boolean
  failover: boolean
  attempt: number
  attempts_total: number
  latency_ms: number
  /** Where that latency went: waiting for an earlier candidate, the relay's time
   *  to its first byte, and the streaming phase (0 when it arrived at once). */
  wait_ms: number
  ttfb_ms: number
  stream_ms: number
  bytes: number
  message: string
}

export interface ProviderStat {
  provider: string
  requests: number
  successes: number
  failures: number
  aborted: number
  retries_in: number
  bytes_out: number
  bytes_in: number
  tokens_prompt: number
  tokens_completion: number
  latency_ms_last: number
  latency_ms_avg: number
  latency_ms_p95: number
  cost_usd: number
  last_used_unix: number
}

export interface ProviderHealth {
  provider: string
  state: HealthState
  consecutive_failures: number
  total_failures: number
  last_error: string
  cooldown_remaining: number
}

export interface Snapshot {
  running: boolean
  host: string
  port: number
  base_url: string
  started_unix: number
  uptime_sec: number
  config_path: string
  version: string
  total_requests: number
  total_success: number
  total_failure: number
  active_requests: number
  bytes_out: number
  tokens_prompt: number
  tokens_completion: number
  latency_ms_avg: number
  cost_usd: number
  log_seq: number
  breakers_open: number
  providers: ProviderStat[]
  health: ProviderHealth[]
}

export interface ProviderProbe {
  reachable: boolean
  status: number
  latency_ms: number
  detail: string
  models: string[]
}

export interface ModelInfo {
  id: string
  object: string
  owned_by?: string
  literouter?: { enabled?: boolean; kind?: string; targets?: string[] }
}

/** A provider's secret as it is held in the config file: a literal, a ${VAR}
 *  reference, or empty. The console only ever sees the reference; a literal is
 *  blanked by the server and marked, and an untouched blank means "keep". */
export interface ProviderConfig {
  id: string
  name: string
  base_url: string
  api_key: string
  api_key_source?: 'literal' | 'env'
  api_key_clear?: boolean
  enabled: boolean
  priority: number
  weight: number
  timeout_sec: number
  connect_timeout_sec: number
  supports_stream: boolean
  models: string[]
  headers: Record<string, string>
  chat_path: string
  embeddings_path: string
  protocol: Protocol
  /** Dollars per million tokens. 0 means "not written down". */
  price_in_per_million: number
  price_out_per_million: number
  note: string
}

export interface RouteTarget {
  provider: string
  model?: string
}

export interface RouteConfig {
  model: string
  targets: RouteTarget[]
  enabled: boolean
}

export interface ServerConfig {
  host: string
  port: number
  api_key: string
  pass_through_unknown: boolean
  max_attempts: number
  /** Whole-request budget in seconds; 0 disables it. */
  request_deadline_sec: number
  /** Seconds a conversation stays pinned to the relay that answered it; 0 off. */
  session_affinity_sec: number
  /** Apply the config file when it changes on disk. */
  reload_on_change: boolean
  circuit_failure_threshold: number
  circuit_cooldown_sec: number
  skip_open_circuits: boolean
  log_capacity: number
  log_bodies: boolean
  log_body_limit: number
  persist_telemetry: boolean
  web_ui: boolean
  language: string
  ui_scale: number
}

export interface AppConfig {
  schema?: number
  server: ServerConfig
  providers: ProviderConfig[]
  routes: RouteConfig[]
}

export type IssueLevel = 'error' | 'warning' | 'info'

export interface ValidationIssue {
  level: IssueLevel
  path: string
  message: string
}

export interface ValidationReport {
  ok: boolean
  summary: string
  issues: ValidationIssue[]
}

export interface ConfigResponse {
  path: string
  exists: boolean
  config: AppConfig
  validation: ValidationReport
}

export interface SaveResponse extends ValidationReport {
  saved: boolean
  path: string
}

export class ApiError extends Error {
  status: number
  data: unknown
  constructor(message: string, status: number, data: unknown) {
    super(message)
    this.name = 'ApiError'
    this.status = status
    this.data = data
  }
}

const KEY_STORAGE = 'lr.key'

let apiKey = localStorage.getItem(KEY_STORAGE) ?? ''

export const getApiKey = () => apiKey

export function setApiKey(key: string) {
  apiKey = key.trim()
  if (apiKey) localStorage.setItem(KEY_STORAGE, apiKey)
  else localStorage.removeItem(KEY_STORAGE)
}

function errorMessage(body: unknown, fallback: string): string {
  if (body && typeof body === 'object') {
    const error = (body as { error?: { message?: string } }).error
    if (error?.message) return error.message
    const summary = (body as { summary?: string }).summary
    if (summary) return summary
  }
  return fallback
}

async function request<T>(path: string, init: RequestInit = {}): Promise<T> {
  const headers: Record<string, string> = { Accept: 'application/json' }
  if (apiKey) headers['x-api-key'] = apiKey
  if (init.body) headers['Content-Type'] = 'application/json'

  const response = await fetch(path, { ...init, headers })
  const text = await response.text()
  let data: unknown = null
  if (text) {
    try {
      data = JSON.parse(text)
    } catch {
      data = text
    }
  }
  if (!response.ok) {
    throw new ApiError(errorMessage(data, response.statusText), response.status, data)
  }
  return data as T
}

export const api = {
  status: () => request<Snapshot>('/__literouter/status'),
  logs: (since: number, limit = 500) =>
    request<{ entries: LogEntry[]; seq: number }>(
      `/__literouter/logs?since=${since}&limit=${limit}`,
    ),
  config: () => request<ConfigResponse>('/__literouter/config'),
  saveConfig: (config: AppConfig) =>
    request<SaveResponse>('/__literouter/config', {
      method: 'PUT',
      body: JSON.stringify({ config }),
    }),
  reload: () => request<ValidationReport>('/__literouter/reload', { method: 'POST' }),
  resetStats: () => request<{ ok: boolean }>('/__literouter/reset-stats', { method: 'POST' }),
  shutdown: () => request<{ ok: boolean }>('/__literouter/shutdown', { method: 'POST' }),
  probe: (provider: string) =>
    request<ProviderProbe>('/__literouter/probe', {
      method: 'POST',
      body: JSON.stringify({ provider }),
    }),
  models: () => request<{ data: ModelInfo[] }>('/v1/models'),
}
