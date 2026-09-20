import type { AppConfig, UInt64 } from '@/lib/api'

export const MAX_CLIENT_QUOTA = BigInt(Number.MAX_SAFE_INTEGER)
export const MAX_UINT64 = 18446744073709551615n
export const QUOTA_FIELDS = new Set(['requests_per_day', 'tokens_per_day', 'token_reservation'])
const CLIENT_INTEGERS = new Set([...QUOTA_FIELDS, 'requests', 'successes', 'failures', 'tokens_prompt',
  'tokens_completion', 'active_requests', 'requests_today', 'tokens_today', 'reserved_tokens'])

export function parseUInt64(raw: string, max = MAX_UINT64): UInt64 | null {
  if (!/^\d+$/.test(raw)) return null
  const value = BigInt(raw)
  if (value > max) return null
  return value <= BigInt(Number.MAX_SAFE_INTEGER) ? Number(value) : value.toString()
}

export function uintValue(value: UInt64): bigint {
  if (typeof value === 'number' && (!Number.isSafeInteger(value) || value < 0)) {
    throw new Error('Unsafe integer: reload the configuration before saving')
  }
  const parsed = parseUInt64(String(value))
  if (parsed === null) throw new Error('Invalid unsigned 64-bit integer')
  return BigInt(parsed)
}

/** Keep client quotas/counters exact before JSON.parse can round uint64 values.
 * Other API numbers retain their existing number representation. The marker is
 * chosen outside the input, so a string from a prompt cannot imitate a token. */
export function parseApiJson(text: string): unknown {
  let marker = '__lr_integer__'
  while (text.includes(marker)) marker += '_'
  const integers: string[] = []
  const marked = text.replace(/"(?:[^"\\]|\\.)*"|-?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?/g, (token) => {
    if (!/^-?\d+$/.test(token) || Number.isSafeInteger(Number(token))) return token
    return JSON.stringify(marker + (integers.push(token) - 1))
  })
  const restore = (value: unknown, path: string[]): unknown => {
    if (typeof value === 'string' && value.startsWith(marker)) {
      const token = integers[Number(value.slice(marker.length))]!
      return path.includes('clients') && CLIENT_INTEGERS.has(path.at(-1) ?? '') ? token : Number(token)
    }
    if (Array.isArray(value)) return value.map((entry, index) => restore(entry, [...path, String(index)]))
    if (value && typeof value === 'object') {
      return Object.fromEntries(Object.entries(value).map(([key, entry]) => [key, restore(entry, [...path, key])]))
    }
    return value
  }
  return restore(JSON.parse(marked), [])
}

/** The core accepts safe JSON integers for editable quotas. Refuse larger
 * values even if decoded losslessly; counter displays may still use uint64. */
export function stringifyConfig(config: AppConfig): string {
  for (const client of config.clients ?? []) {
    for (const key of QUOTA_FIELDS) {
      const value = client[key as 'requests_per_day' | 'tokens_per_day' | 'token_reservation']
      if (typeof value !== 'number' || !Number.isSafeInteger(value) || value < 0) {
        throw new Error('Unsafe integer: client quotas must be integers from 0 to 9007199254740991')
      }
    }
  }
  return JSON.stringify(config, null, 2)
}

export function quotaPercent(used: UInt64, limit: UInt64): number {
  const cap = uintValue(limit)
  return cap === 0n ? 0 : Number((uintValue(used) * 100n / cap) > 100n ? 100n : uintValue(used) * 100n / cap)
}

/** The same meter for a limit measured in money. Separate because a daily budget
 *  is a real number, not a uint64: routing `7.5` through quotaPercent() throws
 *  ("unsafe integer"), which took the whole clients view down — the failure mode
 *  the integer helper exists to prevent, arrived at from the other side. */
export function moneyPercent(used: number, limit: number): number {
  if (!Number.isFinite(used) || !Number.isFinite(limit) || limit <= 0) return 0
  return Math.max(0, Math.min(100, Math.round((used / limit) * 100)))
}
