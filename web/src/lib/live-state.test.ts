import { describe, expect, it } from 'vitest'

import type { LogEntry, ProviderProbe } from '@/lib/api'
import { appendLogEntries, probeSucceeded } from '@/lib/live-state'

describe('live API results', () => {
  it('distinguishes HTTP rejection from a successful relay probe', () => {
    const probe: ProviderProbe = { reachable: true, status: 200, latency_ms: 1, detail: '', models: [] }
    expect(probeSucceeded(probe)).toBe(true)
    for (const status of [0, 401, 403, 429, 500]) expect(probeSucceeded({ ...probe, status })).toBe(false)
    expect(probeSucceeded({ ...probe, reachable: false })).toBe(false)
  })

  it('deduplicates overlapping pages, sorts them, and bounds the retained log', () => {
    const entry = (seq: number) => ({ seq } as LogEntry)
    expect(appendLogEntries([entry(1), entry(2)], [entry(3), entry(2)], 2).map((item) => item.seq)).toEqual([2, 3])
  })
})
