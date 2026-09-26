import { describe, expect, it } from 'vitest'

import { readConfigImport } from '@/lib/config-import'

describe('configuration imports', () => {
  it('restores defaults from a minimal configuration and preserves secret references', () => {
    const config = readConfigImport({ providers: [{ id: 'relay', api_key: '${RELAY_KEY}' }] })
    expect(config.providers[0]!.api_key).toBe('${RELAY_KEY}')
    expect(config.providers[0]!.headers).toEqual({})
    expect(config.server.web_ui).toBe(true)
  })

  it('rejects unrelated JSON instead of silently replacing the configuration with defaults', () => {
    for (const raw of [null, [], {}, { result: 'ok' }]) expect(() => readConfigImport(raw)).toThrow()
  })

  it('reports invalid field paths before malformed values reach form controls', () => {
    expect(() => readConfigImport({ server: { api_key: 123 } })).toThrow('server.api_key')
    expect(() => readConfigImport({ providers: [{ models: [null] }] })).toThrow('providers[0].models')
    expect(() => readConfigImport({ providers: [{ headers: { Authorization: 123 } }] })).toThrow('providers[0].headers')
    expect(() => readConfigImport({ routes: [{ targets: [null] }] })).toThrow('routes[0].targets[0]')
    expect(() => readConfigImport({ providers: {} })).toThrow('providers')
  })

  it('permits omitted defaults and leaves value ranges to server validation', () => {
    expect(readConfigImport({ server: { port: -1 }, routes: [{}] }).server.port).toBe(-1)
  })
})
