import { useEffect, useRef, useState, type ReactNode } from 'react'

import {
  Activity,
  ArrowDownUp,
  ArrowRight,
  ArrowUpRight,
  Box,
  CheckCircle2,
  Clock,
  Coins,
  Copy,
  Database,
  KeyRound,
  Link2,
  Network,
  RefreshCw,
  ShieldAlert,
  Timer,
  WifiOff,
  type LucideIcon,
} from 'lucide-react'

import { Metric } from '@/components/Metric'
import { toast } from 'sonner'
import { HourlyTrend } from '@/components/HourlyTrend'
import { RelayHealth } from '@/components/RelayHealth'
import { Badge } from '@/components/ui/badge'
import { Button } from '@/components/ui/button'
import { Card, CardContent, CardHeader, CardTitle } from '@/components/ui/card'
import { humanBytes, humanCount, humanUptime, humanMillis, pct, successRatio } from '@/lib/format'
import { useI18n } from '@/lib/i18n'
import { clientBaseUrl, totalTokens } from '@/lib/present'
import { cn } from '@/lib/utils'
import { useStore } from '@/store'

/** The one tile that has to keep moving between status polls.
 *
 *  Every other number here is an event count, so a two-second refresh is
 *  invisible; a clock is the one value a reader can watch stall. It ticks once
 *  a second, anchored to the value the last poll carried plus the time elapsed
 *  since — never to `started_unix` against the local clock, since the browser
 *  and the server are under no obligation to agree on what time it is. */
function Uptime({ uptime, running }: { uptime: number; running: boolean }) {
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

  const seconds = running ? anchor.current.uptime + (Date.now() - anchor.current.at) / 1000 : uptime
  return <span className="tnum">{humanUptime(seconds)}</span>
}

function RuntimeRow({ icon: Icon, label, value, note, danger }: {
  icon: LucideIcon
  label: string
  value: ReactNode
  note?: string
  danger?: boolean
}) {
  return (
    <div className="flex items-center gap-3 py-3">
      <Icon className="size-4 shrink-0 text-muted-foreground" aria-hidden="true" />
      <div className="min-w-0 flex-1">
        <div className="text-xs text-muted-foreground">{label}</div>
        {note ? <div className="mt-0.5 text-[10px] text-muted-foreground/75">{note}</div> : null}
      </div>
      <div className={cn('shrink-0 text-right text-sm font-medium tnum', danger && 'text-destructive')}>{value}</div>
    </div>
  )
}

export function Overview({ onNavigate, onInspectRelay }: { onNavigate?: (page: 'providers' | 'routes' | 'logs') => void; onInspectRelay: (provider: string) => void }) {
  const { snapshot, models, online, refresh, openKeyPrompt } = useStore()
  const { t } = useI18n()
  const endpoint = clientBaseUrl(snapshot?.base_url ?? '')

  async function copyEndpoint() {
    try {
      await navigator.clipboard.writeText(endpoint)
      toast.success(t('copied'))
    } catch {
      toast.error(t('clipboardFailed'))
    }
  }

  return (
    <div className="flex min-w-0 flex-col gap-6">
      <div className="flex flex-col justify-between gap-4 sm:flex-row sm:items-center">
        <div>
          <div className="mb-2 flex items-center gap-2 text-xs text-muted-foreground">
            <span className={cn('size-1.5 rounded-full', online ? 'bg-ok' : 'bg-muted-foreground')} />
            {t(online ? 'overviewLive' : 'overviewOffline')}
          </div>
          <h1 className="text-2xl font-semibold tracking-tight sm:text-[28px]">{t('overviewTitle')}</h1>
          <p className="mt-2 text-sm leading-relaxed text-muted-foreground">{t('overviewDescription')}</p>
        </div>
        {onNavigate ? (
          <Button variant="outline" className="self-start sm:self-auto" onClick={() => onNavigate('logs')}>
            {t('overviewViewLogs')}
            <ArrowUpRight className="size-4" aria-hidden="true" />
          </Button>
        ) : null}
      </div>

      {!snapshot ? (
        <Card className="items-center border-dashed px-6 py-16 text-center sm:py-24">
          <span className="flex size-14 items-center justify-center rounded-2xl bg-primary/10 text-primary">
            <WifiOff className="size-6" aria-hidden="true" />
          </span>
          <div className="max-w-md">
            <h2 className="font-semibold">{t('overviewWaiting')}</h2>
            <p className="mt-2 text-sm leading-relaxed text-muted-foreground">{t('overviewWaitingDescription')}</p>
          </div>
          <div className="flex flex-wrap justify-center gap-2">
            <Button onClick={() => void refresh()}><RefreshCw className="size-4" aria-hidden="true" />{t('retry')}</Button>
            <Button variant="outline" onClick={openKeyPrompt}><KeyRound className="size-4" aria-hidden="true" />{t('keyTitle')}</Button>
          </div>
        </Card>
      ) : (
        <>
          <div className="grid grid-cols-2 gap-3 sm:gap-4 xl:grid-cols-4">
            <Metric icon={Activity} label={t('metricRequests')} value={humanCount(snapshot.total_requests)}
              sub={`${humanCount(snapshot.active_requests)} ${t('inFlight')}`} />
            <Metric icon={CheckCircle2} tone="green" label={t('metricSuccess')}
              value={pct(snapshot.total_success, snapshot.total_requests)}
              ratio={successRatio(snapshot.total_success, snapshot.total_requests)}
              sub={`${humanCount(snapshot.total_success)} ${t('ok')} · ${humanCount(snapshot.total_failure)} ${t('failed')}`} />
            <Metric icon={ArrowDownUp} tone="blue" label={t('metricTokens')}
              value={humanCount(totalTokens(snapshot.tokens_prompt, snapshot.tokens_completion))}
              sub={`${humanCount(snapshot.tokens_prompt)} ${t('prompt')} · ${humanCount(snapshot.tokens_completion)} ${t('completion')}`} />
            <Metric icon={Coins} tone="amber" label={t('metricCost')}
              value={snapshot.cost_usd > 0 ? `$${snapshot.cost_usd.toFixed(4)}` : '—'} sub={t('costSub')} />
          </div>

          <div className="grid min-w-0 gap-5 xl:grid-cols-[minmax(0,1fr)_320px]">
            {/* The width travels with the trend, so a chart recorded at minute
                resolution is labelled in minutes rather than assumed hourly. */}
            <HourlyTrend buckets={snapshot.hourly ?? []} bucketSec={snapshot.traffic_bucket_sec ?? 3600} />
            <Card className="gap-0 overflow-hidden py-0">
              <CardHeader className="flex flex-row items-center justify-between gap-3 border-b px-5 py-5">
                <div>
                  <CardTitle className="text-sm">{t('overviewRuntime')}</CardTitle>
                  <p className="mt-1.5 text-xs text-muted-foreground">{t('overviewRuntimeDescription')}</p>
                </div>
                <Badge variant="outline" className={cn('gap-1.5', online && snapshot.running ? 'border-ok/20 bg-ok/5 text-ok' : 'text-muted-foreground')}>
                  <span className="size-1.5 rounded-full bg-current" />
                  {t(!online ? 'disconnected' : snapshot.running ? 'overviewRunning' : 'overviewStopped')}
                </Badge>
              </CardHeader>
              <CardContent className="px-5">
                <div className="divide-y divide-border/65">
                  <RuntimeRow icon={Clock} label={t('metricUptime')} note={`v${snapshot.version}`}
                    value={<Uptime uptime={snapshot.uptime_sec} running={snapshot.running && online} />} />
                  <RuntimeRow icon={Timer} label={t('metricLatency')} note={t('overviewEwma')}
                    value={snapshot.total_requests ? humanMillis(snapshot.latency_ms_avg) : '—'} />
                  <RuntimeRow icon={ArrowUpRight} label={t('metricSent')} value={humanBytes(snapshot.bytes_out)} />
                  <RuntimeRow icon={ShieldAlert} label={t('metricBreakers')} danger={snapshot.breakers_open > 0}
                    value={`${snapshot.breakers_open} / ${snapshot.providers.length}`} />
                </div>
                <div className="mb-5 mt-2 rounded-xl bg-muted/55 p-3.5">
                  <div className="flex items-center justify-between gap-2 text-xs">
                    <span className="flex items-center gap-2 font-medium"><Database className="size-3.5 text-primary" aria-hidden="true" />{t('overviewCache')}</span>
                    <span className="text-muted-foreground">{t(snapshot.cache_enabled ? 'overviewEnabled' : 'overviewDisabled')}</span>
                  </div>
                  <div className="mt-3 grid grid-cols-2 gap-3">
                    <div><div className="text-lg font-semibold tnum">{snapshot.cache_enabled ? humanCount(Number(snapshot.cache_hits ?? 0)) : '—'}</div><div className="text-[11px] text-muted-foreground">{t('cacheHits')}</div></div>
                    <div><div className="text-lg font-semibold tnum">{snapshot.cache_enabled ? humanCount(Number(snapshot.cache_entries ?? 0)) : '—'}</div><div className="text-[11px] text-muted-foreground">{t('cacheEntries')}</div></div>
                  </div>
                </div>
              </CardContent>
            </Card>
          </div>

          <Card className="gap-0 overflow-hidden py-0">
            <CardHeader className="flex flex-wrap items-center justify-between gap-3 px-5 py-5 sm:px-6">
              <div>
                <div className="flex items-center gap-2.5"><CardTitle className="text-base">{t('relayHealth')}</CardTitle><Badge variant="secondary" className="text-[10px] tnum">{snapshot.providers.length}</Badge></div>
                <p className="mt-2 text-xs text-muted-foreground">{t('overviewHealthDescription')}</p>
              </div>
              {onNavigate ? <Button size="sm" variant="ghost" onClick={() => onNavigate('providers')}>{t('overviewManageRelays')}<ArrowRight className="size-3.5" aria-hidden="true" /></Button> : null}
            </CardHeader>
            <CardContent className="p-0"><RelayHealth onInspect={onInspectRelay} /></CardContent>
          </Card>

          <div className="grid min-w-0 gap-5 xl:grid-cols-[minmax(0,1fr)_320px]">
            <Card className="gap-0 overflow-hidden py-0">
              <CardHeader className="flex flex-wrap items-center justify-between gap-3 px-5 py-5 sm:px-6">
                <div>
                  <div className="flex items-center gap-2.5"><CardTitle className="text-base">{t('models')}</CardTitle><Badge variant="secondary" className="text-[10px] tnum">{models.length}</Badge></div>
                  <p className="mt-2 text-xs text-muted-foreground">{t('overviewModelDescription')}</p>
                </div>
                {onNavigate ? <Button size="sm" variant="ghost" onClick={() => onNavigate('routes')}>{t('overviewManageRoutes')}<ArrowRight className="size-3.5" aria-hidden="true" /></Button> : null}
              </CardHeader>
              <CardContent className="px-5 pb-5 sm:px-6 sm:pb-6">
                {models.length === 0 ? (
                  <div className="flex flex-col items-center rounded-xl border border-dashed bg-muted/20 px-5 py-9 text-center">
                    <Box className="mb-3 size-6 text-muted-foreground/60" aria-hidden="true" />
                    <p className="text-sm font-medium">{t('overviewNoModels')}</p>
                    <p className="mt-1.5 max-w-sm text-xs leading-relaxed text-muted-foreground">{t('overviewNoModelsDescription')}</p>
                  </div>
                ) : (
                  <div className="grid gap-2.5 sm:grid-cols-2">
                    {models.map((model) => {
                      const targets = model.literouter?.targets ?? []
                      return (
                        <div key={model.id} className="flex min-w-0 items-start gap-3 rounded-xl border border-border/70 px-3.5 py-3.5">
                          <span className="flex size-8 shrink-0 items-center justify-center rounded-lg bg-primary/7 text-primary"><Box className="size-4" aria-hidden="true" /></span>
                          <div className="min-w-0">
                            <div className="break-all font-mono text-xs font-medium leading-5">{model.id}</div>
                            <div className="mt-1 flex items-start gap-1 text-[11px] leading-5 text-muted-foreground">
                              {targets.length ? <><ArrowRight className="mt-1 size-3 shrink-0" aria-hidden="true" /><span className="break-all">{targets.join(' · ')}</span></> : t('overviewDirectModel')}
                            </div>
                          </div>
                        </div>
                      )
                    })}
                  </div>
                )}
              </CardContent>
            </Card>
            <Card className="relative gap-0 overflow-hidden border-primary/15 bg-primary/4 py-0">
              <CardHeader className="px-5 pb-4 pt-5">
                <span className="mb-3 flex size-10 items-center justify-center rounded-xl border border-primary/10 bg-card text-primary"><Network className="size-5" aria-hidden="true" /></span>
                <CardTitle className="text-sm">{t('metricEndpoint')}</CardTitle>
                <p className="mt-1.5 text-xs leading-relaxed text-muted-foreground">{t('overviewEndpointDescription')}</p>
              </CardHeader>
              <CardContent className="px-5 pb-5">
                <div className="flex items-start gap-2 rounded-lg border border-primary/10 bg-card p-3"><Link2 className="mt-0.5 size-3.5 shrink-0 text-primary" aria-hidden="true" /><code className="min-w-0 flex-1 break-all text-xs leading-5">{endpoint || '—'}</code><Button size="icon" variant="ghost" className="size-7 shrink-0" aria-label={t('overviewCopyEndpoint')} title={t('overviewCopyEndpoint')} disabled={!endpoint} onClick={() => void copyEndpoint()}><Copy className="size-3.5" /></Button></div>
                {snapshot.config_path ? <div className="mt-5"><p className="text-[11px] font-medium text-muted-foreground">{t('overviewConfigPath')}</p><p className="mt-1.5 break-all font-mono text-[11px] leading-5 text-muted-foreground">{snapshot.config_path}</p></div> : null}
              </CardContent>
            </Card>
          </div>
        </>
      )}
    </div>
  )
}
