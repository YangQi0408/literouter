import { Loader2, Radar } from 'lucide-react'
import { useState } from 'react'

import { Badge } from '@/components/ui/badge'
import { Button } from '@/components/ui/button'
import { useI18n } from '@/lib/i18n'
import { formatTokens, stateBadgeClass } from '@/lib/present'
import { humanCount, humanDuration, humanMillis, pct, successRatio } from '@/lib/format'
import { useStore } from '@/store'

export function RelayHealth() {
  const { snapshot, probe } = useStore()
  const { t } = useI18n()
  const [busy, setBusy] = useState('')

  if (!snapshot) return null
  const health = new Map(snapshot.health.map((h) => [h.provider, h]))

  async function run(id: string) {
    setBusy(id)
    try {
      await probe(id)
    } catch {
      /* the store already reported it */
    } finally {
      setBusy('')
    }
  }

  return (
    <div className="overflow-auto">
      <table className="w-full text-sm">
        <thead>
          <tr>
            <th>{t('colProvider')}</th>
            <th>{t('colState')}</th>
            <th className="text-right">{t('colRequests')}</th>
            <th>{t('colSuccess')}</th>
            <th className="text-right">{t('colLatency')}</th>
            <th className="text-right">p95</th>
            <th className="text-right">{t('colTokens')}</th>
            <th>{t('lastError')}</th>
            <th />
          </tr>
        </thead>
        <tbody>
          {snapshot.providers.length === 0 ? (
            <tr>
              <td colSpan={9} className="py-8 text-center text-muted-foreground">
                {t('empty')}
              </td>
            </tr>
          ) : (
            snapshot.providers.map((stat) => {
              const state = health.get(stat.provider)
              const ratio = successRatio(stat.successes, stat.requests)
              return (
                <tr key={stat.provider} className="transition-colors hover:bg-muted/40">
                  <td className="font-mono text-xs">{stat.provider}</td>
                  <td>
                    <Badge variant="outline" className={stateBadgeClass(state?.state ?? 'unknown')}>
                      {state?.state ?? 'unknown'}
                    </Badge>
                    {state && state.cooldown_remaining > 0 ? (
                      <span className="ml-2 font-mono text-[11px] text-muted-foreground">
                        {humanDuration(state.cooldown_remaining)}
                      </span>
                    ) : null}
                  </td>
                  <td className="text-right font-mono tnum">{humanCount(stat.requests)}</td>
                  <td>
                    <div className="flex items-center gap-2">
                      <span className="h-1 w-16 overflow-hidden rounded-full bg-muted">
                        <span
                          className="block h-full rounded-full bg-gradient-to-r from-ok to-primary"
                          style={{ width: `${Math.round(ratio * 100)}%` }}
                        />
                      </span>
                      <span className="font-mono text-xs text-muted-foreground tnum">
                        {pct(stat.successes, stat.requests)}
                      </span>
                    </div>
                  </td>
                  <td className="text-right font-mono text-xs tnum">
                    {stat.latency_ms_avg ? humanMillis(stat.latency_ms_avg) : '—'}
                  </td>
                  <td className="text-right font-mono text-xs tnum">
                    {stat.latency_ms_p95 ? humanMillis(stat.latency_ms_p95) : '—'}
                  </td>
                  <td className="text-right font-mono text-xs tnum">{formatTokens(stat)}</td>
                  <td className="max-w-[22rem] truncate text-xs text-muted-foreground">
                    {state?.last_error ?? ''}
                  </td>
                  <td className="text-right">
                    <Button
                      size="sm"
                      variant="ghost"
                      disabled={busy === stat.provider}
                      onClick={() => void run(stat.provider)}
                    >
                      {busy === stat.provider ? (
                        <Loader2 className="size-3.5 animate-spin" />
                      ) : (
                        <Radar className="size-3.5" />
                      )}
                      {busy === stat.provider ? t('testing') : t('test')}
                    </Button>
                  </td>
                </tr>
              )
            })
          )}
        </tbody>
      </table>
    </div>
  )
}
