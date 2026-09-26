import { describe, expect, it } from 'vitest'
import type { LogEntry } from './api'
import { activityKindKey, filterActivity } from './activity'

const entry = (seq: number, kind: LogEntry['kind'], model = 'alias'): LogEntry => ({
  seq, kind, model, provider: 'relay', request_id: `request-${seq}`, upstream_model: 'actual-model',
  level: 'info', message: 'complete', time_unix: seq, time: '', date: '', datetime: '',
  status: 200, stream: true, failover: false, attempt: 1, attempts_total: 1,
  latency_ms: 1, wait_ms: 0, ttfb_ms: 1, stream_ms: 0, bytes: 1,
})

describe('request activity', () => {
  it('filters native protocols and searches upstream model and request IDs', () => {
    const entries = [entry(1, 'responses'), entry(2, 'anthropic'), entry(3, 'gemini_stream')]
    expect(filterActivity(entries, ' ACTUAL-model ', 'all', 'responses').map((e) => e.seq)).toEqual([1])
    expect(filterActivity(entries, 'request-2', 'info', 'all').map((e) => e.seq)).toEqual([2])
    expect(filterActivity(entries, '', 'error', 'all')).toEqual([])
    expect(activityKindKey('responses')).toBe('activityKindResponses')
    expect(activityKindKey('future-kind')).toBe('future-kind')
  })

  it('keeps every locally retained entry reachable in newest-first order', () => {
    const entries = Array.from({ length: 1500 }, (_, i) => entry(i, 'chat'))
    const filtered = filterActivity(entries, '', 'all', 'all')
    expect(filtered).toHaveLength(1500)
    expect(filtered[0]?.seq).toBe(1499)
    expect(filtered.at(-1)?.seq).toBe(0)
    expect(entries[0]?.seq).toBe(0)
  })
})
