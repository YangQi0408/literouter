/** Typed client for the proxy's admin API.
 *
 *  The shapes here mirror core/src/lr_json.cpp one-for-one; when a field is
 *  added there, it shows up here as a compile error rather than as a silent
 *  `undefined` in a table cell. */
import { normalizeConfig } from '@/lib/normalize'
import { parseApiJson, stringifyConfig } from '@/lib/client-integers'


export type LogLevel = 'info' | 'warn' | 'error'
export type LogKind = 'chat' | 'embeddings' | 'models' | 'admin' | 'system'
export type HealthState = 'unknown' | 'healthy' | 'degraded' | 'open'
export type Protocol =
  | 'openai'
  | 'anthropic'
  | 'gemini'
  | 'openai_responses'
  /** Azure OpenAI: OpenAI's JSON behind a deployment path, an `api-key` header
   *  and a mandatory `api-version` query. */
  | 'azure'
  /** Vertex AI: Gemini's generateContent JSON, OAuth2 bearer, project/location path. */
  | 'vertex'
  /** AWS Bedrock's Converse API: its own body, SigV4-signed, region required. */
  | 'bedrock'
  /** Ollama's /api/chat: its own body and newline-delimited JSON stream. */
  | 'ollama'

export interface LogEntry {
  client_id: string
  client_key_id: string
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

/** One bucket of traffic, for the trend chart. */
export interface TrafficBucket {
  hour_unix: number
  /** How wide this bucket is. Carried per bucket because
   *  `server.traffic_bucket_sec` can change between runs, and a reader that
   *  assumed 3600 would mislabel a restored history. */
  bucket_sec: number
  requests: number
  successes: number
  failures: number
  bytes_out: number
  tokens_prompt: number
  tokens_completion: number
  cost_usd: number
}

export interface Snapshot {
  clients: ClientUsage[]
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
  /** The most recent buckets, oldest first; empty until there has been traffic. */
  hourly: TrafficBucket[]
  /** The bucket width the trend above was recorded at, and how many buckets the
   *  server keeps. Both travel with the snapshot so a chart cannot mislabel a
   *  minute-resolution history as hourly. */
  traffic_bucket_sec: number
  traffic_bucket_count: number
  /** Whether the local response cache is storing answers. */
  cache_enabled: boolean
  cache_hits: UInt64
  cache_misses: UInt64
  cache_entries: UInt64
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
  groups: string[]
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
  /** Azure: the api-version query value. Vertex: the API version segment. */
  api_version: string
  /** Bedrock: the AWS region. Vertex: the location. */
  region: string
  /** Vertex: the project id. */
  project: string
  /** Vertex: path to a service-account JSON key. */
  credentials_file: string
  /** Bedrock SigV4 credentials; `${VAR}` references are resolved at signing time. */
  aws_access_key: string
  aws_secret_key: string
  aws_session_token: string
  /** Relay-side in-flight limit; 0 is unlimited. Not a per-client quota. */
  max_concurrent: number
  /** Relay-side rolling-minute start limit; 0 is unlimited. */
  requests_per_minute: number
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

export interface ServerConfig extends SecretConfig {
  host: string
  port: number
  /** Both empty keep HTTP; TLS files and address changes require restart. */
  tls_cert_file: string
  tls_key_file: string
  api_key: string
  pass_through_unknown: boolean
  max_attempts: number
  /** "priority" | "fastest" | "cheapest" — how the candidate chain is ordered. */
  routing_policy: string
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
  /** Width of one traffic-trend bucket in seconds (below 60 is raised to 60). */
  traffic_bucket_sec: number
  /** How many buckets the trend keeps. */
  traffic_bucket_count: number
  /** Local response cache TTL in seconds; 0 disables it. */
  response_cache_ttl_sec: number
  /** Entries kept before the least recently used is evicted. */
  response_cache_max_entries: number
  /** OpenTelemetry OTLP/HTTP metrics endpoint; empty disables export. */
  otlp_endpoint: string
}

export type UInt64 = number | string

export interface SecretConfig {
  api_key: string
  api_key_source?: 'literal' | 'env'
  api_key_clear?: boolean
}

export interface ClientKeyConfig extends SecretConfig {
  id: string
  enabled: boolean
}

export interface ClientConfig {
  id: string
  name: string
  enabled: boolean
  keys: ClientKeyConfig[]
  models: string[]
  provider_groups: string[]
  requests_per_minute: number
  max_concurrent: number
  requests_per_day: number
  tokens_per_day: number
  /** Daily spend ceiling in US dollars; 0 is unlimited. */
  budget_usd_per_day: number
  token_reservation: number
}

export interface ClientUsage {
  client: string
  requests: UInt64
  successes: UInt64
  failures: UInt64
  tokens_prompt: UInt64
  tokens_completion: UInt64
  cost_usd: number
  active_requests: UInt64
  day_unix: number
  requests_today: UInt64
  tokens_today: UInt64
  reserved_tokens: UInt64
  /** Settled spend for the UTC day, which a daily budget is measured against. */
  cost_today: number
}

export interface AppConfig {
  clients: ClientConfig[]
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
      data = parseApiJson(text)
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
  config: async (): Promise<ConfigResponse> => {
    const body = await request<ConfigResponse>('/__literouter/config')
    return { ...body, config: normalizeConfig(body.config) }
  },
  saveConfig: (config: AppConfig) =>
    request<SaveResponse>('/__literouter/config', {
      method: 'PUT',
      body: '{"config":' + stringifyConfig(config) + '}',
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
