import { describe, expect, it } from 'vitest'

import { makeConfig } from '@/lib/config-fixture'
import { shouldReplaceDraft } from '@/lib/draft'

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
