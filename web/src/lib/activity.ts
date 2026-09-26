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

export function filterActivity(entries: LogEntry[], query: string, level: string, kind: string): LogEntry[] {
  const needle = query.trim().toLowerCase()
  return entries.filter((entry) => {
    if (level !== 'all' && entry.level !== level) return false
    if (kind !== 'all' && entry.kind !== kind) return false
    return !needle || [entry.message, entry.model, entry.provider, entry.request_id, entry.upstream_model]
      .some((field) => String(field ?? '').toLowerCase().includes(needle))
  }).slice().reverse()
}
