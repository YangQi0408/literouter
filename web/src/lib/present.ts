import type { HealthState, IssueLevel, ProviderConfig, ProviderStat } from '@/lib/api'
import { humanCount } from '@/lib/format'

/** Presentational helpers shared by the tables — kept out of the components so
 *  a badge looks the same wherever the same state is shown. */

export function stateBadgeClass(state: HealthState | string): string {
  switch (state) {
    case 'healthy':
      return 'border-ok/35 bg-ok/10 text-ok'
    case 'degraded':
      return 'border-warn/35 bg-warn/10 text-warn'
    case 'open':
      return 'border-destructive/35 bg-destructive/10 text-destructive'
    default:
      return 'border-border bg-muted/40 text-muted-foreground'
  }
}

export function levelBadgeClass(level: IssueLevel | string): string {
  switch (level) {
    case 'error':
      return 'border-destructive/35 bg-destructive/10 text-destructive'
    case 'warn':
    case 'warning':
      return 'border-warn/35 bg-warn/10 text-warn'
    default:
      return 'border-border bg-muted/40 text-muted-foreground'
  }
}

export function statusBadgeClass(status: number): string {
  if (status >= 200 && status < 300) return 'border-ok/35 bg-ok/10 text-ok'
  if (status === 0) return 'border-border bg-muted/40 text-muted-foreground'
  return 'border-destructive/35 bg-destructive/10 text-destructive'
}

export function formatTokens(stat: ProviderStat): string {
  const total = stat.tokens_prompt + stat.tokens_completion
  return total ? humanCount(total) : '—'
}

export function totalTokens(prompt: number, completion: number): number {
  return (prompt || 0) + (completion || 0)
}

/** How a provider's stored secret should read in a table or a form. */
export function keySummary(provider: ProviderConfig, t: (key: string, vars?: Record<string, string | number>) => string) {
  if (provider.api_key_source === 'env' && provider.api_key) {
    const name = provider.api_key.replace(/^\$\{?/, '').replace(/\}$/, '').split(':-')[0] ?? provider.api_key
    return { kind: 'env' as const, text: t('keyEnv', { name }) }
  }
  if (provider.api_key) return { kind: 'literal' as const, text: provider.api_key }
  if (provider.api_key_source === 'literal') return { kind: 'stored' as const, text: t('keyStored') }
  return { kind: 'none' as const, text: t('keyNone') }
}
