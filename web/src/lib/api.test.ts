import { describe, expect, it } from 'vitest'

import {
  PROVIDER_DEFAULTS,
  normalizeConfig,
  normalizeProvider,
  normalizeRoute,
} from '@/lib/normalize'

/** Captured from a running instance: `GET /__literouter/config` for a relay that
 *  sets only `id` and `base_url`. The writer omits every field at its default, so
 *  this is what the console actually receives — and what the edit dialog used to
 *  crash on. */
const MINIMAL_PAYLOAD = {
  server: { port: 18790 },
  providers: [
    {
      api_key: '',
      api_key_source: 'literal' as const,
      base_url: 'http://127.0.0.1:9',
      chat_path: '/chat/completions',
      connect_timeout_sec: 15,
      embeddings_path: '/embeddings',
      enabled: true,
      id: 'plain',
      models: [],
      name: 'plain',
      priority: 100,
      supports_stream: true,
      timeout_sec: 120,
      weight: 1,
    },
  ],
  routes: [{ model: 'm', targets: [{ provider: 'plain' }] }],
}

describe('normalizeConfig', () => {
  it('fills in every field the writer omitted', () => {
    const config = normalizeConfig(MINIMAL_PAYLOAD)
    const provider = config.providers[0]!

    // The five the writer drops at their default. `headers` is the one that used
    // to throw while rendering the edit dialog.
    expect(provider.headers).toEqual({})
    expect(provider.protocol).toBe('openai')
    expect(provider.price_in_per_million).toBe(0)
    expect(provider.price_out_per_million).toBe(0)
    expect(provider.note).toBe('')

    // Nothing at all may be undefined, whatever the writer chose to omit.
    for (const [key, value] of Object.entries(provider)) {
      expect(value, `provider.${key}`).not.toBeUndefined()
    }
    for (const [key, value] of Object.entries(config.server)) {
      expect(value, `server.${key}`).not.toBeUndefined()
    }
    for (const [key, value] of Object.entries(config.routes[0]!)) {
      expect(value, `route.${key}`).not.toBeUndefined()
    }
  })

  it('keeps the values the payload did carry', () => {
    const config = normalizeConfig(MINIMAL_PAYLOAD)
    expect(config.providers[0]!.id).toBe('plain')
    expect(config.providers[0]!.base_url).toBe('http://127.0.0.1:9')
    expect(config.server.port).toBe(18790)
    expect(config.routes[0]!.targets).toEqual([{ provider: 'plain' }])
  })

  it('restores the server defaults for keys the payload left out', () => {
    const config = normalizeConfig(MINIMAL_PAYLOAD)
    expect(config.server.host).toBe('127.0.0.1')
    expect(config.server.tls_cert_file).toBe('')
    expect(config.server.tls_key_file).toBe('')
    expect(config.server.web_ui).toBe(true)
    expect(config.server.log_body_limit).toBe(2048)
    expect(config.server.routing_policy).toBe('priority')
  })

  it('survives a null or a wrong type where an object or array belongs', () => {
    const provider = normalizeProvider({
      headers: null,
      models: null,
    } as never)
    expect(provider.headers).toEqual({})
    expect(provider.models).toEqual([])

    const config = normalizeConfig({ providers: null, routes: null } as never)
    expect(config.providers).toEqual([])
    expect(config.routes).toEqual([])

    expect(normalizeConfig(null).providers).toEqual([])
    expect(normalizeRoute({ targets: null } as never).targets).toEqual([])
  })

  it('does not mutate its input', () => {
    const raw = { providers: [{ id: 'a' }] }
    const before = JSON.stringify(raw)
    normalizeConfig(raw)
    expect(JSON.stringify(raw)).toBe(before)
  })
})

describe('PROVIDER_DEFAULTS', () => {
  it('matches what a relay with nothing set looks like after normalization', () => {
    expect(normalizeProvider({})).toEqual(PROVIDER_DEFAULTS)
  })
})
