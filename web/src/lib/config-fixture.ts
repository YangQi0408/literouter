import type { AppConfig, ProviderConfig, RouteConfig, ServerConfig } from '@/lib/api'

/** Fixtures for the console's pure-function tests.
 *
 *  Every field is spelled out rather than cast into existence, so a field added
 *  to the API types is a compile error here — which is the point of having them
 *  in a typed file instead of inline in each test. The values are the ones the
 *  server's own defaults produce. */

export function makeServer(overrides: Partial<ServerConfig> = {}): ServerConfig {
  return {
    host: '127.0.0.1',
    port: 8787,
    api_key: '',
    pass_through_unknown: true,
    max_attempts: 0,
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
    language: 'auto',
    ui_scale: 1.0,
    ...overrides,
  }
}

export function makeProvider(overrides: Partial<ProviderConfig> = {}): ProviderConfig {
  return {
    id: 'relay',
    name: 'Relay',
    base_url: 'https://relay.example/v1',
    api_key: '${RELAY_KEY}',
    enabled: true,
    priority: 100,
    weight: 1,
    timeout_sec: 120,
    connect_timeout_sec: 15,
    supports_stream: true,
    models: ['gpt-4o'],
    headers: {},
    chat_path: '/chat/completions',
    embeddings_path: '/embeddings',
    protocol: 'openai',
    price_in_per_million: 0,
    price_out_per_million: 0,
    note: '',
    ...overrides,
  }
}

export function makeRoute(overrides: Partial<RouteConfig> = {}): RouteConfig {
  return {
    model: 'gpt-4o',
    targets: [{ provider: 'relay' }],
    enabled: true,
    ...overrides,
  }
}

export function makeConfig(overrides: Partial<AppConfig> = {}): AppConfig {
  return {
    schema: 1,
    server: makeServer(),
    providers: [makeProvider()],
    routes: [makeRoute()],
    ...overrides,
  }
}
