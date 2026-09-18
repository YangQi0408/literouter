import { describe, expect, it } from 'vitest'
import { CLIENT_DEFAULTS, normalizeClient, normalizeConfig } from '@/lib/normalize'
import { administratorKeyAfterSave, clientProblem, generateApiKey, hasSecret } from '@/lib/clients'
import { parseApiJson, parseUInt64, quotaPercent, stringifyConfig } from '@/lib/client-integers'
import { makeConfig } from '@/lib/config-fixture'
import { countChanges } from '@/lib/diff'
import { settleSavedDraft } from '@/lib/draft'
import type { ClientConfig } from '@/lib/api'

const client = (patch: Partial<ClientConfig> = {}): ClientConfig => ({ ...structuredClone(CLIENT_DEFAULTS), id: 'team', ...patch })

describe('distribution compatibility and draft boundaries', () => {
  it('keeps personal configs usable and normalizes nested old/partial client fields', () => {
    expect(normalizeConfig({}).clients).toEqual([])
    expect(normalizeConfig({ providers: [{ id: 'p' }] }).providers[0]!.groups).toEqual([])
    expect(normalizeClient({ id: 'one' })).toEqual(client({ id: 'one' }))
    expect(normalizeClient({ keys: null, provider_groups: null, models: null } as never).keys).toEqual([])
    expect(normalizeClient({ keys: [{ id: 'k' }] } as never).keys[0]).toEqual({ id: 'k', api_key: '', enabled: true })
  })
  it('counts clients, nested key revocation, and provider groups as unsaved changes', () => {
    const saved = makeConfig({ clients: [client({ keys: [{ id: 'k', enabled: true, api_key: '' }] })] })
    const draft = structuredClone(saved)
    draft.clients[0]!.keys[0]!.enabled = false
    draft.providers[0]!.groups = ['paid']
    expect(countChanges(draft, saved)).toBe(2)
    expect(countChanges(makeConfig({ clients: [client(), client({ id: 'two' })] }), makeConfig())).toBe(2)
  })
  it('clears submitted literal keys after save without discarding concurrent editing', () => {
    const submitted = makeConfig({ clients: [client({ keys: [{ id: 'k', enabled: true, api_key: 'new-key' }] })] })
    const saved = structuredClone(submitted)
    saved.clients[0]!.keys[0] = { id: 'k', enabled: true, api_key: '', api_key_source: 'literal' }
    expect(settleSavedDraft(structuredClone(submitted), submitted, saved)).toEqual(saved)
    const edited = structuredClone(submitted); edited.clients[0]!.name = 'edited during save'
    expect(settleSavedDraft(edited, submitted, saved)).toBe(edited)
  })
})

describe('credential preservation and rotation', () => {
  it('distinguishes stored, deliberately cleared and enabled empty keys', () => {
    expect(hasSecret({ api_key: '', api_key_source: 'literal' })).toBe(true)
    expect(hasSecret({ api_key: '', api_key_source: 'env' })).toBe(true)
    expect(hasSecret({ api_key: '', api_key_source: 'literal', api_key_clear: true })).toBe(false)
    expect(clientProblem(client({ keys: [{ id: 'k', enabled: true, api_key: '', api_key_source: 'literal' }] }), [])).toBeNull()
    expect(clientProblem(client({ keys: [{ id: 'k', enabled: true, api_key: '' }] }), [])).toBe('enabledKeyRequired')
    expect(clientProblem(client({ keys: [{ id: 'k', enabled: false, api_key: '' }] }), [])).toBeNull()
    expect(clientProblem(client(), [client()])).toBe('clientIdInvalid')
    expect(clientProblem(client({ tokens_per_day: 1 }), [])).toBe('reservationInvalid')
  })
  it('uses a changed literal administrator key after a successful save; env keys need reauthentication', () => {
    const stored = { api_key: '', api_key_source: 'literal' as const }
    expect(administratorKeyAfterSave(stored, stored)).toBeNull()
    expect(administratorKeyAfterSave(stored, { api_key: 'rotated-admin' })).toBe('rotated-admin')
    expect(administratorKeyAfterSave(stored, { api_key: '${ADMIN_KEY}' })).toBeNull()
    expect(administratorKeyAfterSave(stored, { api_key: '', api_key_clear: true })).toBe('')
  })
  it('generates 256-bit keys using the platform cryptographic source', () => {
    const keys = new Set(Array.from({ length: 8 }, () => generateApiKey()))
    expect(keys.size).toBe(8)
    for (const key of keys) expect(key).toMatch(/^lr_[a-f0-9]{64}$/)
  })
})

describe('uint64 quotas survive the browser without rounding', () => {
  it('accepts exact bounds and refuses fractional, negative, overflowing or exponent input', () => {
    expect(parseUInt64('9007199254740991')).toBe(9007199254740991)
    expect(parseUInt64('9007199254740993')).toBe('9007199254740993')
    expect(parseUInt64('18446744073709551615')).toBe('18446744073709551615')
    for (const input of ['18446744073709551616', '1.5', '-1', '1e5', '', 'Infinity']) expect(parseUInt64(input)).toBeNull()
    expect(parseUInt64('2147483648', 2147483647n)).toBeNull()
  })
  it('round-trips the largest editable quota and rejects oversized server values without rounding', () => {
    const config = makeConfig({ clients: [client({ tokens_per_day: Number.MAX_SAFE_INTEGER })] })
    const encoded = stringifyConfig(config)
    expect(encoded).toContain('"tokens_per_day": 9007199254740991')
    expect(normalizeConfig(parseApiJson(encoded) as typeof config).clients[0]!.tokens_per_day).toBe(Number.MAX_SAFE_INTEGER)
    const oversized = normalizeConfig(parseApiJson('{"clients":[{"id":"team","tokens_per_day":18446744073709551615}]}') as typeof config)
    expect(() => stringifyConfig(oversized)).toThrow('Unsafe integer')
    expect(quotaPercent('18446744073709551614', '18446744073709551615')).toBe(99)
  })
  it('preserves oversized usage and quoted strings and never serializes an already-rounded quota', () => {
    const parsed = parseApiJson('{"clients":[{"tokens_today":9007199254740993}],"text":"__lr_integer__0 9007199254740993 \\"test\\""}') as { clients: { tokens_today: string }[]; text: string }
    expect(parsed.clients[0]!.tokens_today).toBe('9007199254740993')
    expect(parsed.text).toContain('9007199254740993')
    const config = makeConfig({ clients: [client({ tokens_per_day: 9007199254740992 })] })
    expect(() => stringifyConfig(config)).toThrow('Unsafe integer')
  })
})
