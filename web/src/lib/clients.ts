import type { ClientConfig, SecretConfig } from '@/lib/api'
import { uintValue } from '@/lib/client-integers'

export function generateApiKey(): string {
  const bytes = new Uint8Array(32)
  crypto.getRandomValues(bytes)
  return 'lr_' + Array.from(bytes, (byte) => byte.toString(16).padStart(2, '0')).join('')
}

export function hasSecret(secret: SecretConfig): boolean {
  return !secret.api_key_clear && (!!secret.api_key.trim() || secret.api_key_source === 'literal' || secret.api_key_source === 'env')
}

export function administratorKeyAfterSave(before: SecretConfig, after: SecretConfig): string | null {
  if (after.api_key_clear) return ''
  const value = after.api_key.trim()
  if (!value || value === before.api_key || /^\$(?:[A-Za-z_]|\{)/.test(value)) return null
  return value
}

export function clientProblem(client: ClientConfig, others: ClientConfig[]): string | null {
  if (!client.id.trim() || others.some((item) => item.id === client.id.trim())) return 'clientIdInvalid'
  const ids = new Set<string>()
  for (const key of client.keys) {
    if (!key.id.trim() || ids.has(key.id.trim())) return 'keyIdInvalid'
    ids.add(key.id.trim())
    if (key.enabled && !hasSecret(key)) return 'enabledKeyRequired'
  }
  if ([client.requests_per_day, client.tokens_per_day, client.token_reservation].some((value) => !Number.isSafeInteger(value) || value < 0)) return 'invalidClientQuota'
  // The budget is a real number rather than an integer, so it is checked
  // separately: NaN and Infinity would both be written to the file as `null` by
  // JSON.stringify and then read back as 0, silently removing the ceiling.
  if (!Number.isFinite(client.budget_usd_per_day) || client.budget_usd_per_day < 0) return 'invalidClientBudget'
  if (uintValue(client.tokens_per_day) > 0n && (uintValue(client.token_reservation) === 0n ||
      uintValue(client.token_reservation) > uintValue(client.tokens_per_day))) return 'reservationInvalid'
  return null
}
