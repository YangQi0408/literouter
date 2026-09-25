import { ChevronDown, Loader2, Network, Radar, Server } from 'lucide-react'
import { useState } from 'react'

import { Badge } from '@/components/ui/badge'
import { Button } from '@/components/ui/button'
import type { HealthState } from '@/lib/api'
import { useI18n } from '@/lib/i18n'
import { formatTokens, stateBadgeClass } from '@/lib/present'
import { humanBytes, humanCount, humanDuration, humanMillis, pct, successRatio } from '@/lib/format'
import { useStore } from '@/store'

const healthLabel: Record<HealthState, string> = {
  healthy: 'overviewHealthy',
  degraded: 'overviewDegraded',
  open: 'overviewOpen',
  unknown: 'overviewUnknown',
}

export function RelayHealth() {
  const { snapshot, probe } = useStore()
  const { t } = useI18n()
  const [busy, setBusy] = useState<Set<string>>(new Set())

  if (!snapshot) return null
  const health = new Map(snapshot.health.map((h) => [h.provider, h]))

  async function run(id: string) {
    setBusy((current) => new Set(current).add(id))
    try {
      await probe(id)
    } catch {
      /* the store already reported it */
    } finally {
      setBusy((current) => {
        const next = new Set(current)
        next.delete(id)
        return next
      })
    }
  }

  if (snapshot.providers.length === 0) {
    return (
      <div className="mx-5 mb-5 flex flex-col items-center rounded-xl border border-dashed bg-muted/20 px-5 py-10 text-center sm:mx-6 sm:mb-6">
        <span className="mb-3 flex size-11 items-center justify-center rounded-xl bg-muted text-muted-foreground"><Network className="size-5" aria-hidden="true" /></span>
        <p className="text-sm font-medium">{t('overviewNoRelays')}</p>
        <p className="mt-1.5 max-w-sm text-xs leading-relaxed text-muted-foreground">{t('overviewNoRelaysDescription')}</p>
      </div>
    )
  }

  return (
    <div className="divide-y divide-border/70 border-t border-border/70">
      {snapshot.providers.map((stat) => {
        const state = health.get(stat.provider)
        const ratio = successRatio(stat.successes, stat.requests)
        const status = state?.state ?? 'unknown'
        const probing = busy.has(stat.provider)
        return (
          <div key={stat.provider} className="px-5 py-4 transition-colors hover:bg-muted/15 sm:px-6">
            <div className="grid grid-cols-[minmax(0,1fr)_auto] items-center gap-x-3 gap-y-4 lg:grid-cols-[minmax(0,1fr)_minmax(0,1.2fr)_auto] lg:gap-x-6">
              <div className="flex min-w-0 items-center gap-3">
                <span className="flex size-10 shrink-0 items-center justify-center rounded-xl border border-border/60 bg-muted/40 text-muted-foreground"><Server className="size-4.5" aria-hidden="true" /></span>
                <div className="min-w-0">
                  <div className="break-all text-sm font-semibold">{stat.provider}</div>
                  <div className="mt-1.5 flex flex-wrap items-center gap-2">
                    <Badge variant="outline" className={`${stateBadgeClass(status)} gap-1.5 px-1.5 py-0 text-[10px] leading-4`}>
                      <span className="size-1 rounded-full bg-current" />
                      {t(healthLabel[status])}
                    </Badge>
                    {state && state.cooldown_remaining > 0 ? <span className="text-[11px] text-muted-foreground tnum">{humanDuration(state.cooldown_remaining)}</span> : null}
                  </div>
                </div>
              </div>
              <div className="col-span-2 row-start-2 grid min-w-0 grid-cols-3 gap-3 rounded-xl bg-muted/30 px-3 py-3 lg:col-span-1 lg:col-start-2 lg:row-start-1 lg:bg-transparent lg:p-0">
                <div>
                  <div className="text-[11px] text-muted-foreground">{t('colRequests')}</div>
                  <div className="mt-1.5 text-sm font-medium tnum">{humanCount(stat.requests)}</div>
                </div>
                <div>
                  <div className="text-[11px] text-muted-foreground">{t('colSuccess')}</div>
                  <div className="mt-1.5 flex flex-wrap items-center gap-2">
                    <span className="text-sm font-medium tnum">{pct(stat.successes, stat.requests)}</span>
                    <span className="hidden h-1 w-10 overflow-hidden rounded-full bg-border/65 sm:block" aria-hidden="true"><span className="block h-full rounded-full bg-ok" style={{ width: `${Math.round(ratio * 100)}%` }} /></span>
                  </div>
                </div>
                <div>
                  <div className="text-[11px] text-muted-foreground">{t('colLatency')}</div>
                  <div className="mt-1.5 text-sm font-medium tnum">{stat.latency_ms_avg ? humanMillis(stat.latency_ms_avg) : '—'}</div>
                </div>
              </div>
              <Button size="sm" variant="outline" className="col-start-2 row-start-1 lg:col-start-3" disabled={probing}
                aria-label={t('overviewProbeRelay', { provider: stat.provider })} onClick={() => void run(stat.provider)}>
                {probing ? <Loader2 className="size-3.5 animate-spin" aria-hidden="true" /> : <Radar className="size-3.5" aria-hidden="true" />}
                {probing ? t('testing') : t('test')}
              </Button>
            </div>
            <details className="group mt-3">
              <summary className="flex w-fit cursor-pointer list-none items-center gap-1.5 rounded text-[11px] text-muted-foreground transition-colors hover:text-foreground focus-visible:outline-2 focus-visible:outline-offset-4 focus-visible:outline-ring [&::-webkit-details-marker]:hidden">
                <ChevronDown className="size-3 transition-transform group-open:rotate-180" aria-hidden="true" />
                {t('overviewMoreMetrics')}
                {state?.last_error ? <span className="ml-2 size-1.5 rounded-full bg-warn" aria-label={t('lastError')} /> : null}
              </summary>
              <div className="mt-3 grid grid-cols-2 gap-4 rounded-xl border border-border/60 bg-muted/20 p-4 lg:grid-cols-4">
                <div><div className="text-[11px] text-muted-foreground">{t('overviewP95')}</div><div className="mt-1.5 text-xs font-medium tnum">{stat.latency_ms_p95 ? humanMillis(stat.latency_ms_p95) : '—'}</div></div>
                <div><div className="text-[11px] text-muted-foreground">{t('colTokens')}</div><div className="mt-1.5 text-xs font-medium tnum">{formatTokens(stat)}</div></div>
                <div><div className="text-[11px] text-muted-foreground">{t('colBytes')}</div><div className="mt-1.5 break-words text-xs font-medium tnum">{humanBytes(stat.bytes_in)} / {humanBytes(stat.bytes_out)}</div></div>
                <div><div className="text-[11px] text-muted-foreground">{t('colCost')}</div><div className="mt-1.5 text-xs font-medium tnum">{stat.cost_usd > 0 ? `$${stat.cost_usd.toFixed(4)}` : '—'}</div></div>
                <div className="col-span-2 border-t border-border/65 pt-3 lg:col-span-4"><div className="text-[11px] text-muted-foreground">{t('lastError')}</div><p className="mt-1.5 break-words text-xs leading-relaxed text-muted-foreground">{state?.last_error || t('overviewNoError')}</p></div>
              </div>
            </details>
          </div>
        )
      })}
    </div>
  )
}
