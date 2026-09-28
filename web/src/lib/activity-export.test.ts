import { describe, expect, it } from 'vitest'
import type { LogEntry } from './api'
import { activityAsCsv, activityAsJson } from './activity-export'

const entry: LogEntry = {
  seq: 1, time_unix: 1, time: '12:00', date: '2026-09-28', datetime: '2026-09-28 12:00:00',
  level: 'info', request_id: 'req-1', kind: 'chat', method: 'POST', path: '/v1/chat/completions',
  ingress_protocol: 'openai', client_ip: '127.0.0.1', user_agent: 'test', model: 'model',
  provider: 'relay', upstream_model: 'actual-model', upstream_protocol: 'openai', status: 200,
  stream: false, failover: false, attempt: 1, attempts_total: 1, latency_ms: 12, wait_ms: 0,
  ttfb_ms: 12, stream_ms: 0, request_bytes: 20, bytes: 30, prompt_tokens: 2,
  completion_tokens: 3, cost_usd: 0.001, cache_status: '', message: 'ok',
}

describe('activity exports', () => {
  it('exports the complete structured log entry as JSON', () => {
    const parsed = JSON.parse(activityAsJson([{ ...entry, request_body: '{"prompt":"hello"}' }]))
    expect(parsed[0]).toMatchObject({ seq: 1, user_agent: 'test', request_body: '{"prompt":"hello"}' })
  })

  it('quotes commas, quotes and line breaks and prevents spreadsheet formulas', () => {
    const csv = activityAsCsv([{
      ...entry,
      user_agent: 'client, "desktop"',
      message: 'first line\nsecond line',
      model: '=HYPERLINK("https://example.invalid")',
    }])
    expect(csv).toContain('"client, ""desktop"""')
    expect(csv).toContain('"first line\nsecond line"')
    expect(csv).toContain(`"'=HYPERLINK(""https://example.invalid"")"`)
    expect(csv.startsWith('\uFEFF"seq","time_unix"')).toBe(true)
    expect(csv.endsWith('\r\n')).toBe(true)
  })

  it('uses an empty cell when body logging was disabled', () => {
    const csv = activityAsCsv([entry])
    expect(csv.split('\r\n')[0]).toContain('"request_body","response_body"')
    expect(csv.split('\r\n')[1]).toMatch(/,"",""$/)
  })
})
