import { describe, expect, it } from 'vitest'
import type { LogEntry } from './api'
import { activityKindKey, activityTotals, filterActivity } from './activity'

const entry = (seq: number, kind: LogEntry['kind'], model = 'alias'): LogEntry => ({
  seq, kind, model, provider: 'relay', request_id: `request-${seq}`, upstream_model: 'actual-model',
  level: 'info', message: 'complete', time_unix: seq, time: '', date: '', datetime: '',
  status: 200, stream: true, failover: false, attempt: 1, attempts_total: 1,
  latency_ms: 1, wait_ms: 0, ttfb_ms: 1, stream_ms: 0, request_bytes: 1, bytes: 1,
  prompt_tokens: 0, completion_tokens: 0, cost_usd: 0, cache_status: '',
  method: 'POST', path: '/v1/chat/completions', ingress_protocol: 'openai',
  client_ip: '127.0.0.1', user_agent: 'vitest', upstream_protocol: 'openai',
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

  it('filters operational outcomes and totals the rows on screen', () => {
    const entries = [
      entry(1, 'chat'),
      { ...entry(2, 'chat'), cache_status: 'hit', cost_usd: 0.0012, prompt_tokens: 10, completion_tokens: 5 },
      { ...entry(3, 'chat'), level: 'error' as const, failover: true },
    ]
    expect(filterActivity(entries, '', 'all', 'all', 'cache').map((e) => e.seq)).toEqual([2])
    expect(filterActivity(entries, '', 'all', 'all', 'failover').map((e) => e.seq)).toEqual([3])
    expect(filterActivity(entries, '127.0.0.1', 'all', 'all').map((e) => e.seq)).toEqual([3, 2, 1])
    expect(activityTotals(entries)).toEqual({ requests: 3, tokens: 15, costUsd: 0.0012, cacheHits: 1, failed: 1 })
  })
})
