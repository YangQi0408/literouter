import type { LogEntry } from '@/lib/api'

const columns = [
  'seq', 'time_unix', 'time', 'date', 'datetime', 'level', 'request_id', 'kind',
  'method', 'path', 'ingress_protocol', 'client_ip', 'user_agent', 'model', 'provider',
  'upstream_model', 'upstream_protocol', 'status', 'stream', 'failover', 'attempt',
  'attempts_total', 'latency_ms', 'wait_ms', 'ttfb_ms', 'stream_ms', 'request_bytes',
  'bytes', 'prompt_tokens', 'completion_tokens', 'cost_usd', 'cache_status', 'message',
  'request_body', 'response_body',
] satisfies (keyof LogEntry)[]

export function activityAsJson(entries: readonly LogEntry[]): string {
  return JSON.stringify(entries, null, 2)
}

function csvCell(value: unknown): string {
  let text = String(value ?? '')
  if (/^[\t\r ]*[=+\-@]/.test(text)) text = `'${text}`
  return `"${text.replaceAll('"', '""')}"`
}

export function activityAsCsv(entries: readonly LogEntry[]): string {
  const header = columns.map(csvCell).join(',')
  const rows = entries.map((entry) => columns.map((key) => csvCell(entry[key])).join(','))
  return `\uFEFF${[header, ...rows].join('\r\n')}${rows.length ? '\r\n' : ''}`
}
