import type { LogEntry } from '@/lib/api'

export const ACTIVITY_KINDS = {
  chat: 'activityKindChat',
  responses: 'activityKindResponses',
  anthropic: 'activityKindAnthropic',
  gemini: 'activityKindGemini',
  gemini_stream: 'activityKindGeminiStream',
  embeddings: 'activityKindEmbeddings',
  audio: 'kindAudio',
  images: 'kindImages',
  models: 'activityKindModels',
  admin: 'activityKindAdmin',
  system: 'activityKindSystem',
} as const

export function activityKindKey(kind: string): string {
  return ACTIVITY_KINDS[kind as keyof typeof ACTIVITY_KINDS] ?? kind
}

export function filterActivity(
  entries: LogEntry[],
  query: string,
  level: string,
  kind: string,
  outcome: string = 'all',
): LogEntry[] {
  const needle = query.trim().toLowerCase()
  return entries.filter((entry) => {
    if (level !== 'all' && entry.level !== level) return false
    if (kind !== 'all' && entry.kind !== kind) return false
    if (outcome === 'cache' && entry.cache_status !== 'hit') return false
    if (outcome === 'failover' && !entry.failover) return false
    if (outcome === 'errors' && entry.level !== 'error') return false
    return !needle || [entry.message, entry.model, entry.provider, entry.request_id,
      entry.upstream_model, entry.path, entry.client_ip, entry.user_agent]
      .some((field) => String(field ?? '').toLowerCase().includes(needle))
  }).slice().reverse()
}

export function activityTotals(entries: LogEntry[]) {
  const totals = { requests: entries.length, tokens: 0, costUsd: 0, cacheHits: 0, failed: 0 }
  for (const entry of entries) {
    totals.tokens += (entry.prompt_tokens || 0) + (entry.completion_tokens || 0)
    totals.costUsd += entry.cost_usd || 0
    if (entry.cache_status === 'hit') totals.cacheHits += 1
    if (entry.level === 'error') totals.failed += 1
  }
  return totals
}
