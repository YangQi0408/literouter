import { describe, expect, it } from 'vitest'

import { countChanges } from '@/lib/diff'
import { makeConfig, makeProvider, makeRoute, makeServer } from '@/lib/config-fixture'

describe('countChanges counts what the operator touched', () => {
  it('says nothing changed when nothing did', () => {
    const saved = makeConfig()
    // A structurally different but equal object: the comparison must be by
    // value, not by identity, or every re-read would look like an edit.
    expect(countChanges(structuredClone(saved), saved)).toBe(0)
  })

  it('counts the server block once, however many fields moved', () => {
    const saved = makeConfig()
    const edited = structuredClone(saved)
    edited.server.port = 9999
    edited.server.log_capacity = 500
    expect(countChanges(edited, saved)).toBe(1)
  })

  it('counts each changed relay once', () => {
    const saved = makeConfig({ providers: [makeProvider({ id: 'a' }), makeProvider({ id: 'b' })] })
    const edited = structuredClone(saved)
    edited.providers[0]!.priority = 1
    edited.providers[1]!.note = 'touched'
    expect(countChanges(edited, saved)).toBe(2)
  })

  it('counts two new relays while both are still nameless', () => {
    // The case the id-keyed version got wrong: two drafts are both `id: ""`
    // until they are filled in, and keying by id made the second one invisible.
    const saved = makeConfig({ providers: [] })
    const edited = makeConfig({
      providers: [makeProvider({ id: '' }), makeProvider({ id: '', base_url: '' })],
    })
    expect(countChanges(edited, saved)).toBe(2)
  })

  it('counts a reordering, which the id-keyed version also missed', () => {
    const saved = makeConfig({ providers: [makeProvider({ id: 'a' }), makeProvider({ id: 'b' })] })
    const edited = structuredClone(saved)
    edited.providers.reverse()
    expect(countChanges(edited, saved)).toBe(2)
  })

  it('counts a removal and an addition', () => {
    const saved = makeConfig({ routes: [makeRoute({ model: 'one' }), makeRoute({ model: 'two' })] })
    const edited = structuredClone(saved)
    edited.routes.splice(0, 1)
    expect(countChanges(edited, saved)).toBe(2)
  })

  it('counts a moved target inside one route', () => {
    const saved = makeConfig({
      routes: [makeRoute({ targets: [{ provider: 'a' }, { provider: 'b' }] })],
    })
    const edited = structuredClone(saved)
    edited.routes[0]!.targets.reverse()
    expect(countChanges(edited, saved)).toBe(1)
  })

  it('counts a changed server field alongside a changed relay', () => {
    const saved = makeConfig()
    const edited = structuredClone(saved)
    edited.server = makeServer({ api_key: 'sk-new' })
    edited.providers[0]!.enabled = false
    expect(countChanges(edited, saved)).toBe(2)
  })
})
