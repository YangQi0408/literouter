import { Card, CardContent, CardHeader, CardTitle } from '@/components/ui/card'
import type { TrafficBucket } from '@/lib/api'
import { humanBytes, humanCount } from '@/lib/format'
import { useI18n } from '@/lib/i18n'

/** "24h" for hour-wide buckets, "2h" for minute-wide ones: the count alone
 *  would read as hours whatever the width is. */
function trendWindow(count: number, bucketSec: number): string {
  const seconds = count * Math.max(60, bucketSec)
  if (seconds % 86400 === 0) return `${seconds / 86400}d`
  if (seconds % 3600 === 0) return `${seconds / 3600}h`
  return `${Math.round(seconds / 60)}m`
}

/** The last day of traffic, one bar per hour.
 *
 *  A single set of totals answers "how much, overall"; this answers "when", which
 *  is what tells an operator whether the relay that started failing at 03:00 is
 *  a relay problem or a schedule. Bars are scaled to the busiest hour in the
 *  window rather than to an absolute value, because the interesting shape is
 *  relative. */
/** The trend, with the bucket width the server recorded it at. `bucketSec`
 *  defaults to an hour because that is what a history written before the width
 *  was configurable was recorded at; taking it from the snapshot is what keeps a
 *  minute-resolution chart from being labelled "24h". */
export function HourlyTrend({
  buckets,
  bucketSec = 3600,
}: {
  buckets: TrafficBucket[]
  bucketSec?: number
}) {
  const { t } = useI18n()
  if (buckets.length === 0) {
    return null
  }

  const peak = buckets.reduce((most, bucket) => Math.max(most, bucket.requests), 0)
  const scale = peak > 0 ? peak : 1

  return (
    <Card>
      <CardHeader>
        <CardTitle>{t('hourlyTrend')}</CardTitle>
        <span className="ml-1 text-xs text-muted-foreground">{t('hourlyTrendNote')}</span>
        <span className="ml-auto font-mono text-xs text-muted-foreground tnum">
          {t('peak')} {humanCount(peak)}
        </span>
      </CardHeader>
      <CardContent>
        <div className="flex h-24 items-end gap-1">
          {buckets.map((bucket) => {
            const failed = bucket.failures > 0
            return (
              <div
                key={bucket.hour_unix}
                className="group relative flex-1"
                title={[
                  `${humanCount(bucket.requests)} ${t('colRequests')}`,
                  `${humanCount(bucket.successes)} ${t('ok')}`,
                  `${humanCount(bucket.failures)} ${t('failed')}`,
                  humanBytes(bucket.bytes_out),
                  bucket.cost_usd > 0 ? `$${bucket.cost_usd.toFixed(4)}` : '',
                ]
                  .filter(Boolean)
                  .join(' · ')}
              >
                <div
                  className={
                    failed
                      ? 'w-full rounded-t bg-warn/60 transition-colors group-hover:bg-warn'
                      : 'w-full rounded-t bg-primary/50 transition-colors group-hover:bg-primary'
                  }
                  style={{ height: `${Math.max(2, Math.round((bucket.requests / scale) * 96))}px` }}
                />
              </div>
            )
          })}
        </div>
        <div className="mt-2 flex justify-between text-[11px] text-muted-foreground">
          <span>{trendWindow(buckets.length, bucketSec)}</span>
          <span>{t('now')}</span>
        </div>
      </CardContent>
    </Card>
  )
}
