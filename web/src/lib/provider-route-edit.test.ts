import { describe, expect, it } from 'vitest'
import { makeConfig, makeProvider, makeRoute } from '@/lib/config-fixture'
import { canProbeProvider, mergeModels, priceRowsOf, pricesOfRows, providerIssue, providerRemovalImpact, removeProvider, replaceProvider, routeIssue } from '@/lib/provider-route-edit'

describe('provider and route editing stays valid as a whole', () => {
  it('removes references and only removes routes left with no target', () => {
    const config = makeConfig({ providers: [makeProvider({ id: 'a' }), makeProvider({ id: 'b' })], routes: [
      makeRoute({ model: 'one', targets: [{ provider: 'a' }] }),
      makeRoute({ model: 'two', targets: [{ provider: 'a' }, { provider: 'b', model: 'upstream' }] }),
    ] })
    expect(providerRemovalImpact(config, 'a')).toEqual({ targets: 2, routes: ['one'] })
    removeProvider(config, 0)
    expect(config.providers.map((provider) => provider.id)).toEqual(['b'])
    expect(config.routes).toEqual([makeRoute({ model: 'two', targets: [{ provider: 'b', model: 'upstream' }] })])
  })
  it('renames an unsaved provider and its references together', () => {
    const config = makeConfig()
    replaceProvider(config, 0, makeProvider({ id: 'renamed' }))
    expect(config.routes[0]?.targets[0]?.provider).toBe('renamed')
  })
  it('rejects duplicate names after whitespace cleanup and missing targets', () => {
    expect(providerIssue(makeProvider({ id: ' other ' }), ['other'], [])?.key).toBe('managementDuplicateProvider')
    expect(routeIssue(makeRoute({ model: ' named ' }), ['named'], ['relay'])?.key).toBe('managementDuplicateRoute')
    expect(routeIssue(makeRoute(), [], [])?.key).toBe('managementUnknownTarget')
    expect(routeIssue(makeRoute({ targets: [] }), [], ['relay'])?.key).toBe('managementRequiredRoute')
  })
  it('keeps editable price row identity while rejecting collisions instead of overwriting', () => {
    const rows = priceRowsOf({ alpha: { price_in_per_million: 1, price_out_per_million: 2 }, beta: { price_in_per_million: 3, price_out_per_million: 4 } })
    rows[0]!.model = 'beta'
    expect(rows[0]?.id).toBe(0)
    expect(rows).toHaveLength(2)
    expect(providerIssue(makeProvider(), [], rows)?.key).toBe('managementDuplicatePrice')
    rows[0]!.model = ' gamma '
    expect(pricesOfRows(rows).gamma?.price_in_per_million).toBe(1)
    expect(pricesOfRows(rows).beta?.price_in_per_million).toBe(3)
  })
  it('probes the saved connection without blocking model or pricing edits', () => {
    const saved = makeProvider()
    expect(canProbeProvider(makeProvider({ models: [], note: 'draft' }), saved)).toBe(true)
    for (const change of [{ base_url: 'https://other.example' }, { api_key: 'replacement' }, { api_key_clear: true }, { headers: { Authorization: 'new' } }, { protocol: 'anthropic' as const }]) {
      expect(canProbeProvider(makeProvider(change), saved)).toBe(false)
    }
    expect(canProbeProvider(saved)).toBe(false)
  })
  it('only adds explicitly selected models and preserves existing order', () => {
    expect(mergeModels(['old', 'shared'], [' new ', 'shared', 'new'])).toEqual(['old', 'shared', 'new'])
  })
})
