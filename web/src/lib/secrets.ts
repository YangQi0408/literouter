import type { SecretConfig } from '@/lib/api'

/** A fresh 256-bit key from the platform's cryptographic source. Used by the
 *  "generate" button, so an operator never invents a weak access key. */
export function generateApiKey(): string {
  const bytes = new Uint8Array(32)
  crypto.getRandomValues(bytes)
  return 'lr_' + Array.from(bytes, (byte) => byte.toString(16).padStart(2, '0')).join('')
}

/** Whether the config already holds a credential for this field: a literal the
 *  server blanked but marked, an environment reference, or a value being typed.
 *  An explicit clear wins over all three. */
export function hasSecret(secret: SecretConfig): boolean {
  return (
    !secret.api_key_clear &&
    (!!secret.api_key.trim() || secret.api_key_source === 'literal' || secret.api_key_source === 'env')
  )
}

/** The console key to use after a save. A literal the operator just typed is
 *  the new credential; a `${VAR}` reference or an unchanged field needs no
 *  change, and an explicit clear returns "". Null means "keep what you have". */
export function administratorKeyAfterSave(before: SecretConfig, after: SecretConfig): string | null {
  if (after.api_key_clear) return ''
  const value = after.api_key.trim()
  if (!value || value === before.api_key || /^\$(?:[A-Za-z_]|\{)/.test(value)) return null
  return value
}
