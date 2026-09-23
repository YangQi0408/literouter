import { describe, expect, it } from 'vitest'

import { administratorKeyAfterSave, generateApiKey, hasSecret } from '@/lib/secrets'
import { settleSavedDraft } from '@/lib/draft'
import { makeConfig } from '@/lib/config-fixture'

describe('hasSecret reads what the server actually sent', () => {
  it('counts a stored literal the server blanked but marked', () => {
    // The config endpoint never returns a literal key; it returns "" plus
    // `api_key_source: "literal"`. Treating that as "no key" would tell an
    // operator their credential had been lost.
    expect(hasSecret({ api_key: '', api_key_source: 'literal' })).toBe(true)
  })

  it('counts an environment reference, and a value being typed', () => {
    expect(hasSecret({ api_key: '${RELAY_KEY}', api_key_source: 'env' })).toBe(true)
    expect(hasSecret({ api_key: 'sk-typed' })).toBe(true)
    // Whitespace alone is not a credential.
    expect(hasSecret({ api_key: '   ' })).toBe(false)
  })

  it('lets an explicit clear win over all three', () => {
    expect(hasSecret({ api_key: '', api_key_source: 'literal', api_key_clear: true })).toBe(false)
    expect(hasSecret({ api_key: '${RELAY_KEY}', api_key_source: 'env', api_key_clear: true })).toBe(false)
    expect(hasSecret({ api_key: 'sk-typed', api_key_clear: true })).toBe(false)
  })

  it('says no key at all for an untouched empty field', () => {
    expect(hasSecret({ api_key: '' })).toBe(false)
  })
})

describe('administratorKeyAfterSave decides which key the console keeps', () => {
  const stored = { api_key: '', api_key_source: 'literal' as const }

  it('keeps what it has when the field was not really changed', () => {
    expect(administratorKeyAfterSave(stored, stored)).toBeNull()
    expect(administratorKeyAfterSave(stored, { api_key: '' })).toBeNull()
    // Unchanged reference to the literal already in localStorage.
    expect(administratorKeyAfterSave({ api_key: 'sk-same' }, { api_key: 'sk-same' })).toBeNull()
  })

  it('adopts a literal the operator just typed', () => {
    expect(administratorKeyAfterSave(stored, { api_key: 'rotated-key' })).toBe('rotated-key')
  })

  it('clears on an explicit clear', () => {
    expect(administratorKeyAfterSave(stored, { api_key: '', api_key_clear: true })).toBe('')
  })

  it('does not adopt an environment reference as a literal', () => {
    // The server resolves `${VAR}` itself. Keeping the old key means the next
    // request 401s and the console asks again, which is correct: the console
    // must never treat a placeholder as the credential.
    expect(administratorKeyAfterSave(stored, { api_key: '${ADMIN_KEY}' })).toBeNull()
    expect(administratorKeyAfterSave(stored, { api_key: '${ADMIN_KEY:-sk-fallback}' })).toBeNull()
    expect(administratorKeyAfterSave(stored, { api_key: '$ADMIN_KEY' })).toBeNull()
  })
})

describe('generateApiKey uses the platform cryptographic source', () => {
  it('produces a fresh 256-bit key per call', () => {
    const keys = new Set(Array.from({ length: 8 }, () => generateApiKey()))
    expect(keys.size).toBe(8)
    for (const key of keys) expect(key).toMatch(/^lr_[a-f0-9]{64}$/)
  })
})

describe('settleSavedDraft clears a submitted draft without dropping newer edits', () => {
  const submitted = makeConfig()
  const saved = structuredClone(submitted)

  it('takes the server copy when nothing was touched during the save', () => {
    expect(settleSavedDraft(structuredClone(submitted), submitted, saved)).toEqual(saved)
  })

  it('keeps a draft that was edited while the request was in flight', () => {
    const edited = structuredClone(submitted)
    edited.server.port = 9999
    expect(settleSavedDraft(edited, submitted, saved)).toBe(edited)
  })

  it('takes the server copy when there is no draft at all', () => {
    expect(settleSavedDraft(null, submitted, saved)).toEqual(saved)
  })
})
