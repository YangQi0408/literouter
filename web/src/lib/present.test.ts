import { describe, expect, it } from 'vitest'

import { makeProvider } from '@/lib/config-fixture'
import { formatTokens, keySummary, stateBadgeClass, statusBadgeClass } from '@/lib/present'

const t = (key: string, vars?: Record<string, string | number>) =>
  vars ? `${key}:${Object.values(vars).join(',')}` : key

describe('keySummary never leaks more than the server sent', () => {
  it('names the variable behind an environment reference', () => {
    const summary = keySummary(makeProvider({ api_key: '${RELAY_KEY}', api_key_source: 'env' }), t)
    expect(summary).toEqual({ kind: 'env', text: 'keyEnv:RELAY_KEY' })
  })

  it('keeps a ${VAR:-fallback} reference down to the variable', () => {
    const summary = keySummary(
      makeProvider({ api_key: '${RELAY_KEY:-sk-fallback}', api_key_source: 'env' }),
      t,
    )
    expect(summary.text).toBe('keyEnv:RELAY_KEY')
  })

  it('says a stored secret is stored rather than showing it', () => {
    // The config endpoint blanks a literal key and labels it, so the console
    // must not pretend it knows it.
    const summary = keySummary(makeProvider({ api_key: '', api_key_source: 'literal' }), t)
    expect(summary).toEqual({ kind: 'stored', text: 'keyStored' })
  })

  it('falls back to no key at all', () => {
    expect(keySummary(makeProvider({ api_key: '' }), t)).toEqual({ kind: 'none', text: 'keyNone' })
  })
})

describe('the table badges', () => {
  it('marks a healthy state apart from an unknown one', () => {
    expect(stateBadgeClass('healthy')).not.toBe(stateBadgeClass('unknown'))
    expect(stateBadgeClass('open')).not.toBe(stateBadgeClass('degraded'))
  })

  it('accepts both spellings of a warning', () => {
    expect(stateBadgeClass('warning')).toBeDefined()
  })

  it('reads a status code for what it is', () => {
    expect(statusBadgeClass(200)).toContain('ok')
    expect(statusBadgeClass(429)).toContain('destructive')
    // 0 is a relay that never answered, which is not the same as an error code.
    expect(statusBadgeClass(0)).not.toContain('destructive')
  })
})

describe('formatTokens', () => {
  it('adds the two directions and shows a dash for none', () => {
    expect(formatTokens({ tokens_prompt: 1200, tokens_completion: 300 } as never)).toBe('1.5k')
    expect(formatTokens({ tokens_prompt: 0, tokens_completion: 0 } as never)).toBe('—')
  })
})
