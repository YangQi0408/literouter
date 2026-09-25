import { Activity } from 'lucide-react'
import { useId, useRef, useState } from 'react'

import { Badge } from '@/components/ui/badge'
import { Card, CardContent, CardHeader, CardTitle } from '@/components/ui/card'
import type { TrafficBucket } from '@/lib/api'
import { humanBytes, humanCount, humanDuration } from '@/lib/format'
import { useI18n } from '@/lib/i18n'

/** "24h" for hour-wide buckets, "2h" for minute-wide ones: the count alone
 *  would read as hours whatever the width is. */
function trendWindow(seconds: number): string {
  if (seconds % 86400 === 0) return `${seconds / 86400}d`
  if (seconds % 3600 === 0) return `${seconds / 3600}h`
  return `${Math.round(seconds / 60)}m`
}

/** Traffic across the recorded window, one point per interval.
 *
 *  A single set of totals answers "how much, overall"; this answers "when", which
 *  is what tells an operator whether the relay that started failing at 03:00 is
 *  a relay problem or a schedule. Points are scaled to the busiest interval in the
 *  window rather than to an absolute value, because the interesting shape is
 *  relative. */
/** The trend, with the bucket width the server recorded it at. `bucketSec`
 *  defaults to an hour because that is what a history written before the width
 *  was configurable was recorded at; taking it from the snapshot is what keeps a
 *  minute-resolution chart from being labelled "24h". */
export function HourlyTrend({ buckets, bucketSec = 3600 }: { buckets: TrafficBucket[]; bucketSec?: number }) {
  const { t, lang } = useI18n()
  const gradientId = useId()
  const [selectedHour, setSelectedHour] = useState<number | null>(null)
  const buttonRefs = useRef<Array<HTMLButtonElement | null>>([])
  const selectedIndex = buckets.findIndex((bucket) => bucket.hour_unix === selectedHour)
  const activeIndex = selectedIndex >= 0 ? selectedIndex : buckets.length - 1
  const active = buckets[activeIndex]
  const peak = buckets.reduce((most, bucket) => Math.max(most, bucket.requests), 0)
  const requests = buckets.reduce((total, bucket) => total + bucket.requests, 0)
  const failures = buckets.reduce((total, bucket) => total + bucket.failures, 0)
  const scale = Math.max(4, Math.ceil(peak / 4) * 4)
  const first = buckets[0]
  const last = buckets[buckets.length - 1]
  const start = first?.hour_unix ?? 0
  const end = last ? last.hour_unix + (last.bucket_sec || bucketSec) : 0
  const duration = Math.max(60, end - start)
  const plotWidth = 720
  const plotTop = 12
  const plotBottom = 200
  const x = (bucket: TrafficBucket) => ((bucket.hour_unix - start + (bucket.bucket_sec || bucketSec) / 2) / duration) * plotWidth
  const y = (value: number) => plotBottom - (value / scale) * (plotBottom - plotTop)
  const points = buckets.map((bucket) => `${x(bucket)},${y(bucket.requests)}`)
  const requestLine = points.length ? `M${points.join(' L')}` : ''
  const failureLine = buckets.length ? `M${buckets.map((bucket) => `${x(bucket)},${y(bucket.failures)}`).join(' L')}` : ''
  const area = first && last ? `M${x(first)},${plotBottom} L${points.join(' L')} L${x(last)},${plotBottom} Z` : ''
  const formatter = new Intl.DateTimeFormat(lang === 'zh' ? 'zh-CN' : 'en-US', {
    hour: '2-digit', minute: '2-digit', hour12: false,
    ...(duration > 86400 ? { month: 'short', day: 'numeric' } as const : {}),
  })
  const time = (unix: number) => formatter.format(new Date(unix * 1000))
  const bucketLabel = (bucket: TrafficBucket) => [
    time(bucket.hour_unix),
    `${humanCount(bucket.requests)} ${t('colRequests')}`,
    `${humanCount(bucket.successes)} ${t('ok')}`,
    `${humanCount(bucket.failures)} ${t('failed')}`,
    humanBytes(bucket.bytes_out),
    bucket.cost_usd > 0 ? `$${bucket.cost_usd.toFixed(4)}` : '',
  ].filter(Boolean).join(' · ')

  return (
    <Card className="min-w-0 gap-0 overflow-hidden py-0">
      <CardHeader className="flex flex-wrap items-start justify-between gap-3 px-5 py-5 sm:px-6">
        <div>
          <CardTitle className="text-base">{t('overviewTraffic')}</CardTitle>
          <p className="mt-2 text-xs text-muted-foreground">{t('overviewTrafficDescription')}</p>
        </div>
        {buckets.length ? <Badge variant="outline" className="bg-muted/35 px-2.5 py-1 font-normal text-muted-foreground">{t('overviewWindow', { window: trendWindow(duration) })}</Badge> : null}
      </CardHeader>
      <CardContent className="flex flex-1 flex-col px-5 pb-5 sm:px-6">
        <div className="mb-5 flex flex-wrap items-end justify-between gap-3">
          <div className="flex flex-wrap items-baseline gap-2"><span className="text-3xl font-semibold tracking-tight tnum">{humanCount(requests)}</span><span className="text-xs text-muted-foreground">{t('colRequests')}</span><span className="ml-1 text-[11px] text-muted-foreground">{t('peak')} <span className="tnum">{humanCount(peak)}</span></span></div>
          <div className="flex items-center gap-4 text-[11px] text-muted-foreground">
            <span className="flex items-center gap-1.5"><span className="h-0.5 w-3 rounded-full bg-primary" />{t('colRequests')}</span>
            <span className="flex items-center gap-1.5"><span className="h-0.5 w-3 rounded-full bg-warn" />{t('failed')}</span>
          </div>
        </div>
        <div className="relative flex min-h-[220px] flex-1 items-stretch gap-2">
          <div className="flex w-7 shrink-0 flex-col justify-between pb-4 pt-2 text-right text-[10px] text-muted-foreground tnum" aria-hidden="true">
            {[scale, scale * 0.75, scale * 0.5, scale * 0.25, 0].map((value) => <span key={value}>{humanCount(value)}</span>)}
          </div>
          <div className="relative min-w-0 flex-1">
            <svg viewBox={`0 0 ${plotWidth} 216`} preserveAspectRatio="none" className="absolute inset-0 size-full overflow-visible" role="img"
              aria-label={t('overviewTrafficChart', { requests: humanCount(requests), failures: humanCount(failures), buckets: buckets.length })}>
              <defs><linearGradient id={gradientId} x1="0" y1="0" x2="0" y2="1"><stop offset="0%" stopColor="var(--primary)" stopOpacity="0.20" /><stop offset="100%" stopColor="var(--primary)" stopOpacity="0.01" /></linearGradient></defs>
              {[0, 0.25, 0.5, 0.75, 1].map((fraction) => <line key={fraction} x1="0" x2={plotWidth} y1={y(scale * fraction)} y2={y(scale * fraction)} stroke="var(--border)" strokeWidth="1" strokeDasharray={fraction === 0 ? undefined : '3 5'} vectorEffect="non-scaling-stroke" />)}
              {requests > 0 ? <>
                <path d={area} fill={`url(#${gradientId})`} />
                <path d={requestLine} fill="none" stroke="var(--primary)" strokeWidth="2.5" strokeLinecap="round" strokeLinejoin="round" vectorEffect="non-scaling-stroke" />
                {failures > 0 ? <path d={failureLine} fill="none" stroke="var(--warn)" strokeWidth="1.5" strokeDasharray="4 4" strokeLinecap="round" strokeLinejoin="round" vectorEffect="non-scaling-stroke" /> : null}
                {active ? <>
                  <line x1={x(active)} x2={x(active)} y1={plotTop} y2={plotBottom} stroke="var(--primary)" strokeOpacity="0.25" strokeDasharray="3 4" vectorEffect="non-scaling-stroke" />
                  <circle cx={x(active)} cy={y(active.requests)} r="4" fill="var(--primary)" stroke="var(--card)" strokeWidth="2" vectorEffect="non-scaling-stroke" />
                </> : null}
              </> : null}
            </svg>
            {requests > 0 ? (
              <div className="absolute inset-x-0 bottom-4 top-0" role="group" aria-label={t('overviewInterval')}>
                {buckets.map((bucket, index) => (
                  <button key={bucket.hour_unix} type="button" ref={(node) => { buttonRefs.current[index] = node }}
                    className="absolute inset-y-0 cursor-crosshair rounded-md transition-colors hover:bg-primary/4 focus-visible:bg-primary/5 focus-visible:outline-2 focus-visible:outline-ring"
                    style={{ left: `${((bucket.hour_unix - start) / duration) * 100}%`, width: `${((bucket.bucket_sec || bucketSec) / duration) * 100}%` }}
                    aria-label={bucketLabel(bucket)} aria-pressed={index === activeIndex} tabIndex={index === activeIndex ? 0 : -1}
                    onPointerEnter={() => setSelectedHour(bucket.hour_unix)} onFocus={() => setSelectedHour(bucket.hour_unix)} onClick={() => setSelectedHour(bucket.hour_unix)}
                    onKeyDown={(event) => {
                      const next = event.key === 'ArrowRight' ? Math.min(buckets.length - 1, index + 1)
                        : event.key === 'ArrowLeft' ? Math.max(0, index - 1)
                          : event.key === 'Home' ? 0 : event.key === 'End' ? buckets.length - 1 : null
                      if (next !== null) { event.preventDefault(); buttonRefs.current[next]?.focus() }
                    }} />
                ))}
              </div>
            ) : (
              <div className="absolute inset-0 flex flex-col items-center justify-center px-3 text-center">
                <span className="mb-3 flex size-11 items-center justify-center rounded-2xl border border-border/80 bg-card text-primary shadow-xs"><Activity className="size-5" aria-hidden="true" /></span>
                <p className="bg-card/85 px-2 text-sm font-medium">{t('overviewNoTraffic')}</p>
                <p className="mt-1.5 max-w-xs bg-card/85 px-2 text-xs leading-relaxed text-muted-foreground">{t('overviewNoTrafficDescription')}</p>
              </div>
            )}
          </div>
        </div>
        <div className="ml-9 flex items-center justify-between gap-2 text-[10px] text-muted-foreground tnum">
          <span>{first ? time(start) : '—'}</span>
          <span>{t('overviewBucket', { interval: humanDuration(active?.bucket_sec || bucketSec) })}</span>
          <span>{last ? time(end) : '—'}</span>
        </div>
        <div className="mt-5 flex min-h-11 flex-wrap items-center justify-between gap-x-4 gap-y-2 border-t border-border/70 pt-4 text-[11px]">
          {active ? <>
            <span className="text-muted-foreground tnum">{time(active.hour_unix)}</span>
            <span className="text-muted-foreground"><span className="font-medium text-foreground tnum">{humanCount(active.requests)}</span> {t('colRequests')} · <span className="font-medium text-foreground tnum">{humanCount(active.successes)}</span> {t('ok')} · <span className="font-medium text-foreground tnum">{humanCount(active.failures)}</span> {t('failed')}</span>
            <span className="text-muted-foreground tnum">{humanBytes(active.bytes_out)}{active.cost_usd > 0 ? ` · $${active.cost_usd.toFixed(4)}` : ''}</span>
          </> : <span className="text-muted-foreground">{t('peak')} <span className="ml-1 font-medium text-foreground tnum">0</span></span>}
        </div>
      </CardContent>
    </Card>
  )
}
