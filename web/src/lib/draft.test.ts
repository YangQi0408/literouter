import { describe, expect, it } from 'vitest'

import { makeConfig } from '@/lib/config-fixture'
import { shouldReplaceDraft, settleSavedDraft } from '@/lib/draft'

describe('a re-read keeps an unsaved draft', () => {
  const loaded = makeConfig()
  const edited = structuredClone(loaded)
  edited.server.port = 9000

  it('takes the server copy on first paint', () => {
    expect(shouldReplaceDraft(null, null, false)).toBe(true)
    expect(shouldReplaceDraft(null, loaded, false)).toBe(true)
  })

  it('takes the server copy when the draft was not touched', () => {
    // Nothing is lost, and the form keeps up with a config edited outside the
    // console (a text editor, another tab).
    expect(shouldReplaceDraft(structuredClone(loaded), loaded, false)).toBe(true)
  })

  it('keeps an edited draft, which is the whole point', () => {
    expect(shouldReplaceDraft(edited, loaded, false)).toBe(false)
  })

  it('replaces it when the caller says the draft is settled', () => {
    // Save, reload, entering a key: then the server's copy wins.
    expect(shouldReplaceDraft(edited, loaded, true)).toBe(true)
    expect(shouldReplaceDraft(structuredClone(loaded), loaded, true)).toBe(true)
  })

  it('keeps a draft that has never been saved, whatever the server holds', () => {
    // `loaded` is null before the first successful read: an edit made in that
    // window is still the operator's, not the server's.
    expect(shouldReplaceDraft(edited, null, false)).toBe(false)
  })

  it('notices a change anywhere in the document, not just the server block', () => {
    const touchedRoute = structuredClone(loaded)
    touchedRoute.routes[0]!.enabled = false
    expect(shouldReplaceDraft(touchedRoute, loaded, false)).toBe(false)
  })
})

describe('edits made during a save', () => {
  it('keeps only newer edits and removes credentials already accepted by the server', () => {
    const submitted = makeConfig()
    submitted.server.api_key = 'new-secret'
    submitted.providers[0]!.api_key_clear = true
    const saved = structuredClone(submitted)
    saved.server.api_key = ''
    saved.server.api_key_source = 'literal'
    saved.providers[0]!.api_key_clear = false
    saved.providers[0]!.api_key = ''
    const working = structuredClone(submitted)
    working.server.port = 9000
    working.providers[0]!.note = 'typed while saving'

    const settled = settleSavedDraft(working, submitted, saved)
    expect(settled.server).toEqual({ ...saved.server, port: 9000 })
    expect(settled.providers[0]).toEqual({ ...saved.providers[0], note: 'typed while saving' })
    expect(working.server.api_key).toBe('new-secret')
  })

  it('preserves a newer key and clear action rather than undoing them', () => {
    const submitted = makeConfig()
    const working = structuredClone(submitted)
    working.server.api_key = 'next-secret'
    working.providers[0]!.api_key_clear = true
    expect(settleSavedDraft(working, submitted, submitted)).toEqual(working)
  })

  it('keeps reordering and deletions while taking redacted keys from the matching relay', () => {
    const submitted = makeConfig()
    submitted.providers.push({ ...submitted.providers[0]!, id: 'second', api_key: 'second-secret' })
    const saved = structuredClone(submitted)
    saved.providers.forEach((provider) => { provider.api_key = ''; provider.api_key_source = 'literal' })
    const working = structuredClone(submitted)
    working.providers.reverse()
    expect(settleSavedDraft(working, submitted, saved).providers.map((provider) => provider.api_key)).toEqual(['', ''])
    working.providers.pop()
    expect(settleSavedDraft(working, submitted, saved).providers).toEqual([saved.providers[1]])
  })

  it('settles an unchanged draft without retaining shared references', () => {
    const submitted = makeConfig()
    const saved = makeConfig()
    saved.server.api_key_source = 'literal'
    const settled = settleSavedDraft(submitted, submitted, saved)
    expect(settled).toEqual(saved)
    expect(settled).not.toBe(saved)
  })
})
