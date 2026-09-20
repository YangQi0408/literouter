import { useEffect, useRef, useState } from 'react'

import {
  Activity,
  ArrowUpRight,
  CheckCircle2,
  Clock,
  Coins,
  Database,
  Link2,
  ShieldAlert,
  Timer,
} from 'lucide-react'

import { Metric } from '@/components/Metric'
import { HourlyTrend } from '@/components/HourlyTrend'
import { RelayHealth } from '@/components/RelayHealth'
import { Badge } from '@/components/ui/badge'
import { Card, CardContent, CardHeader, CardTitle } from '@/components/ui/card'
import { humanBytes, humanCount, humanUptime, humanMillis, pct, successRatio } from '@/lib/format'
import { useI18n } from '@/lib/i18n'
import { totalTokens } from '@/lib/present'
import { useStore } from '@/store'

/** The one tile that has to keep moving between status polls.
 *
 *  Every other number here is an event count, so a two-second refresh is
 *  invisible; a clock is the one value a reader can watch stall. It ticks once
 *  a second, anchored to the value the last poll carried plus the time elapsed
 *  since — never to `started_unix` against the local clock, since the browser
 *  and the server are under no obligation to agree on what time it is. */
function UptimeMetric({
  uptime,
  running,
  version,
  label,
}: {
  uptime: number
  running: boolean
  version: string
  label: string
}) {
  const [, tick] = useState(0)
  const anchor = useRef({ at: Date.now(), uptime })
  if (anchor.current.uptime !== uptime) {
    anchor.current = { at: Date.now(), uptime }
  }

  useEffect(() => {
    if (!running) return
    const timer = window.setInterval(() => tick((n) => n + 1), 1000)
    return () => window.clearInterval(timer)
  }, [running])

  const seconds = running ? anchor.current.uptime + (Date.now() - anchor.current.at) / 1000 : 0
  return <Metric icon={Clock} label={label} value={humanUptime(seconds)} sub={`v${version}`} />
}

export function Overview() {
  const { snapshot, models } = useStore()
  const { t } = useI18n()

  if (!snapshot) {
    return <p className="p-6 text-sm text-muted-foreground">{t('empty')}</p>
  }

  const tokens = totalTokens(snapshot.tokens_prompt, snapshot.tokens_completion)

  return (
    <div className="flex flex-col gap-5">
      <div className="grid grid-cols-2 gap-3.5 lg:grid-cols-4">
        <Metric
          icon={Activity}
          label={t('metricRequests')}
          value={humanCount(snapshot.total_requests)}
          sub={`${snapshot.active_requests} ${t('inFlight')}`}
        />
        <Metric
          icon={CheckCircle2}
          label={t('metricSuccess')}
          value={pct(snapshot.total_success, snapshot.total_requests)}
          ratio={successRatio(snapshot.total_success, snapshot.total_requests)}
          sub={`${humanCount(snapshot.total_success)} ${t('ok')} · ${humanCount(snapshot.total_failure)} ${t('failed')}`}
        />
        <Metric
          icon={Coins}
          label={t('metricTokens')}
          value={humanCount(tokens)}
          sub={`${humanCount(snapshot.tokens_prompt)} ${t('prompt')} · ${humanCount(snapshot.tokens_completion)} ${t('completion')}`}
        />
        <Metric
          icon={ArrowUpRight}
          label={t('metricSent')}
          value={humanBytes(snapshot.bytes_out)}
          sub={`${humanCount(snapshot.total_requests)} req`}
        />
        <Metric
          icon={Timer}
          label={t('metricLatency')}
          value={humanMillis(snapshot.latency_ms_avg)}
          sub="ewma"
        />
        <Metric
          icon={Coins}
          label={t('metricCost')}
          value={snapshot.cost_usd > 0 ? `$${snapshot.cost_usd.toFixed(4)}` : '—'}
          sub={t('costSub')}
        />
        <UptimeMetric
          uptime={snapshot.uptime_sec}
          running={snapshot.running}
          version={snapshot.version}
          label={t('metricUptime')}
        />
        <Metric
          icon={ShieldAlert}
          label={t('metricBreakers')}
          value={String(snapshot.breakers_open)}
          danger={snapshot.breakers_open > 0}
          sub={`${snapshot.providers.length} ${t('relays')}`}
        />
        <Metric
          icon={Link2}
          label={t('metricEndpoint')}
          value={snapshot.base_url || '—'}
          long
          sub={snapshot.config_path}
        />
        <Metric
          icon={Database}
          label={t('cacheHits')}
          value={snapshot.cache_enabled ? humanCount(Number(snapshot.cache_hits ?? 0)) : '—'}
          sub={
            snapshot.cache_enabled
              ? `${humanCount(Number(snapshot.cache_entries ?? 0))} ${t('cacheEntries')}`
              : t('cacheOff')
          }
        />
      </div>

      {/* The width travels with the trend, so a chart recorded at minute
          resolution is labelled in minutes rather than assumed hourly. */}
      <HourlyTrend buckets={snapshot.hourly ?? []} bucketSec={snapshot.traffic_bucket_sec ?? 3600} />

      <Card>
        <CardHeader>
          <CardTitle>{t('relayHealth')}</CardTitle>
        </CardHeader>
        <CardContent className="p-0">
          <RelayHealth />
        </CardContent>
      </Card>

      <Card>
        <CardHeader>
          <CardTitle>{t('models')}</CardTitle>
          <span className="ml-auto font-mono text-xs text-muted-foreground">
            {t('routed', { n: models.length })}
          </span>
        </CardHeader>
        <CardContent className="flex flex-wrap gap-2">
          {models.length === 0 ? (
            <span className="text-xs text-muted-foreground">{t('empty')}</span>
          ) : (
            models.map((model) => {
              const targets = model.literouter?.targets ?? []
              return (
                <Badge key={model.id} variant="outline" className="gap-2 py-1 font-mono text-xs">
                  {model.id}
                  {targets.length ? (
                    <span className="text-muted-foreground">→ {targets.join(' , ')}</span>
                  ) : null}
                </Badge>
              )
            })
          )}
        </CardContent>
      </Card>
    </div>
  )
}
