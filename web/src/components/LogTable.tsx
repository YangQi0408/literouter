import { Activity, ArrowUpRight, ChevronRight, Clock3, Database, Search } from 'lucide-react'
import { useState, type ReactNode } from 'react'

import { Badge } from '@/components/ui/badge'
import { Button } from '@/components/ui/button'
import { Dialog, DialogContent, DialogDescription, DialogHeader, DialogTitle } from '@/components/ui/dialog'
import type { LogEntry } from '@/lib/api'
import { humanBytes, humanCount, humanMillis } from '@/lib/format'
import { levelBadgeClass, statusBadgeClass } from '@/lib/present'
import { useI18n } from '@/lib/i18n'
import { activityKindKey } from '@/lib/activity'

function DetailField({ label, children }: { label: string; children: ReactNode }) {
  return <div className="min-w-0 space-y-1.5"><dt className="text-xs text-muted-foreground">{label}</dt><dd className="break-words font-mono text-xs leading-relaxed">{children || '—'}</dd></div>
}

function tokenTotal(entry: LogEntry) {
  return (entry.prompt_tokens || 0) + (entry.completion_tokens || 0)
}

function cacheLabel(t: (key: string) => string, status: string) {
  if (status === 'hit') return t('activityCacheHit')
  if (status === 'miss') return t('activityCacheMiss')
  return t('activityCacheSkipped')
}

/** The request line and the wire protocols, kept compact enough for a table
 *  cell: the details dialog carries the full untruncated values. */
function requestLine(entry: LogEntry) {
  if (entry.method && entry.path) return `${entry.method} ${entry.path}`
  return entry.path || ''
}

function protocolLine(entry: LogEntry) {
  const ingress = entry.ingress_protocol || ''
  const upstream = entry.upstream_protocol || ''
  if (ingress && upstream && ingress !== upstream) return `${ingress} → ${upstream}`
  return ingress || upstream
}

export function LogTable({ entries, emptyFiltered = false, onConfigureLogging }: { entries: LogEntry[]; emptyFiltered?: boolean; onConfigureLogging: () => void }) {
  const { t } = useI18n()
  const [selected, setSelected] = useState<LogEntry | null>(null)
  const levelName = (level: string) => t(level === 'warn' ? 'activityLevelWarn' : level === 'error' ? 'activityLevelError' : 'activityLevelInfo')
  const kindName = (kind: string) => t(activityKindKey(kind))

  // Where the time went: the phases only mean anything together, and a column
  // each would not fit the table. Keep them in the hover hint and detail view.
  const latencyTitle = (entry: LogEntry) => entry.latency_ms ? [
    `${t('wait')} ${humanMillis(entry.wait_ms)}`,
    `${t('first byte')} ${humanMillis(entry.ttfb_ms)}`,
    entry.stream_ms > 0 ? `${t('stream')} ${humanMillis(entry.stream_ms)}` : '',
  ].filter(Boolean).join(' · ') : undefined

  if (!entries.length) return (
    <div className="flex flex-col items-center px-6 py-20 text-center">
      <div className="mb-5 flex size-14 items-center justify-center rounded-2xl border bg-muted/40 text-muted-foreground">
        {emptyFiltered ? <Search className="size-6" /> : <Activity className="size-6" />}
      </div>
      <h3 className="text-sm font-semibold">{t(emptyFiltered ? 'activityNoResults' : 'activityEmpty')}</h3>
      <p className="mt-2 max-w-sm text-sm leading-relaxed text-muted-foreground">{t(emptyFiltered ? 'activityNoResultsNote' : 'activityEmptyNote')}</p>
    </div>
  )

  return (
    <>
      <div className="hidden max-h-[calc(100dvh-20rem)] min-h-40 overflow-auto md:block">
        <table className="w-full min-w-[1120px]">
          <thead>
            <tr>
              <th>{t('colTime')}</th>
              <th>{t('colLevel')}</th>
              <th>{t('colModel')}</th>
              <th>{t('colRequestClient')}</th>
              <th>{t('colProvider')}</th>
              <th className="text-right">{t('colStatus')}</th>
              <th className="text-right">{t('colLatency')}</th>
              <th className="text-right">{t('colTokens')}</th>
              <th className="text-right">{t('colCost')}</th>
              <th>{t('colMessage')}</th>
              <th><span className="sr-only">{t('activityInspect')}</span></th>
            </tr>
          </thead>
          <tbody>
            {entries.map((entry) => (
              <tr key={entry.seq} className="group transition-colors hover:bg-muted/35">
                <td className="whitespace-nowrap font-mono text-[11px] text-muted-foreground" title={entry.datetime || entry.time}>{entry.time}</td>
                <td><Badge variant="outline" className={levelBadgeClass(entry.level)}>{levelName(entry.level)}</Badge></td>
                <td>
                  <div className="max-w-52 truncate font-mono text-xs font-medium" title={entry.model}>{entry.model || '—'}</div>
                  <div className="mt-1 flex items-center gap-1.5 text-[10px] text-muted-foreground">
                    {kindName(entry.kind)}
                    {entry.stream ? <span className="rounded bg-primary/8 px-1.5 py-0.5 text-primary">SSE</span> : null}
                    {entry.failover ? <span className="rounded bg-warn/10 px-1.5 py-0.5 text-warn">{t('activityFailover')}</span> : null}
                  </div>
                </td>
                <td className="align-top">
                  <div className="max-w-64 truncate font-mono text-[11px]" title={requestLine(entry)}>{requestLine(entry) || '—'}</div>
                  {protocolLine(entry) ? <div className="mt-1 max-w-64 truncate font-mono text-[10px] text-muted-foreground" title={protocolLine(entry)}>{protocolLine(entry)}</div> : null}
                  {entry.client_ip || entry.user_agent ? <div className="mt-1 max-w-64 truncate text-[10px] text-muted-foreground" title={[entry.client_ip, entry.user_agent].filter(Boolean).join(' · ')}>{[entry.client_ip, entry.user_agent].filter(Boolean).join(' · ')}</div> : null}
                </td>
                <td><div className="max-w-36 truncate text-xs" title={entry.provider}>{entry.provider || '—'}</div>{entry.cache_status ? <div className="mt-1 flex items-center gap-1 text-[10px] text-muted-foreground"><Database className="size-3" />{cacheLabel(t, entry.cache_status)}</div> : null}</td>
                <td className="text-right"><Badge variant="outline" className={statusBadgeClass(entry.status)}>{entry.status || '—'}</Badge></td>
                <td className="whitespace-nowrap text-right font-mono text-xs tnum" title={latencyTitle(entry)}>{entry.latency_ms ? humanMillis(entry.latency_ms) : '—'}</td>
                <td className="whitespace-nowrap text-right font-mono text-xs tnum">{tokenTotal(entry) > 0 ? humanCount(tokenTotal(entry)) : '—'}</td>
                <td className="whitespace-nowrap text-right font-mono text-xs tnum">{entry.cost_usd > 0 ? `$${entry.cost_usd.toFixed(4)}` : '—'}</td>
                <td className="max-w-72 text-xs leading-relaxed text-muted-foreground"><p className="line-clamp-2 break-words">{entry.message || '—'}</p></td>
                <td className="w-10"><Button size="icon" variant="ghost" className="size-8 text-muted-foreground group-hover:text-primary" aria-label={`${t('activityInspect')} ${entry.request_id || entry.seq}`} title={t('activityInspect')} onClick={() => setSelected(entry)}><ArrowUpRight className="size-4" /></Button></td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>

      <div className="max-h-[65dvh] divide-y overflow-auto md:hidden">
        {entries.map((entry) => (
          <button key={entry.seq} type="button" onClick={() => setSelected(entry)} className="block w-full space-y-3 px-4 py-4 text-left transition-colors hover:bg-muted/40 focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-inset focus-visible:ring-ring" aria-label={`${t('activityInspect')} ${entry.request_id || entry.seq}`}>
            <div className="flex items-center justify-between gap-2">
              <div className="flex items-center gap-2"><Badge variant="outline" className={levelBadgeClass(entry.level)}>{levelName(entry.level)}</Badge><span className="font-mono text-[11px] text-muted-foreground">{entry.time}</span></div>
              <div className="flex items-center gap-1.5"><Badge variant="outline" className={statusBadgeClass(entry.status)}>{entry.status || '—'}</Badge><ChevronRight className="size-3.5 text-muted-foreground" /></div>
            </div>
            <div className="space-y-1"><p className="truncate font-mono text-sm font-medium">{entry.model || kindName(entry.kind)}</p><p className="truncate text-xs text-muted-foreground">{entry.provider || kindName(entry.kind)}{entry.stream ? ` · ${t('activityStream')}` : ''}{entry.failover ? ` · ${t('activityFailover')}` : ''}{entry.cache_status ? ` · ${cacheLabel(t, entry.cache_status)}` : ''}</p></div>
            {requestLine(entry) ? <div className="space-y-1 font-mono text-[11px] text-muted-foreground"><p className="truncate" title={requestLine(entry)}>{requestLine(entry)}</p>{protocolLine(entry) ? <p className="truncate" title={protocolLine(entry)}>{protocolLine(entry)}</p> : null}{entry.client_ip || entry.user_agent ? <p className="truncate" title={[entry.client_ip, entry.user_agent].filter(Boolean).join(' · ')}>{[entry.client_ip, entry.user_agent].filter(Boolean).join(' · ')}</p> : null}</div> : null}
            {entry.message ? <p className="line-clamp-2 break-words text-xs leading-relaxed text-muted-foreground">{entry.message}</p> : null}
            <div className="flex flex-wrap items-center gap-x-3 gap-y-1 font-mono text-[11px] text-muted-foreground">
              {entry.latency_ms ? <span className="flex items-center gap-1.5"><Clock3 className="size-3" />{humanMillis(entry.latency_ms)}</span> : null}
              {entry.request_bytes > 0 ? <span>{humanBytes(entry.request_bytes)} ↑</span> : null}
              {entry.bytes > 0 ? <span>{humanBytes(entry.bytes)} ↓</span> : null}
              {tokenTotal(entry) > 0 ? <span>{t('activityTokenCount', { count: humanCount(tokenTotal(entry)) })}</span> : null}
              {entry.cost_usd > 0 ? <span>${entry.cost_usd.toFixed(4)}</span> : null}
            </div>
          </button>
        ))}
      </div>

      <Dialog open={Boolean(selected)} onOpenChange={(open) => { if (!open) setSelected(null) }}>
        <DialogContent className="gap-4 p-4 sm:max-w-2xl sm:gap-6 sm:p-6">
          <DialogHeader>
            <div className="mb-2 flex size-10 items-center justify-center rounded-xl bg-primary/10 text-primary"><Activity className="size-5" /></div>
            <DialogTitle>{t('activityDetails')}</DialogTitle>
            <DialogDescription>{t('activityDetailsNote')}</DialogDescription>
          </DialogHeader>
          {selected ? <>
            <div className="flex flex-wrap items-center gap-2">
              <Badge variant="outline" className={levelBadgeClass(selected.level)}>{levelName(selected.level)}</Badge>
              <Badge variant="outline" className={statusBadgeClass(selected.status)}>{selected.status || '—'}</Badge>
              <Badge variant="secondary">{kindName(selected.kind)}</Badge>
              {selected.stream ? <Badge variant="outline" className="border-primary/20 text-primary">{t('activityStream')}</Badge> : null}
              {selected.failover ? <Badge variant="outline" className="border-warn/20 text-warn">{t('activityFailover')}</Badge> : null}
              {selected.cache_status ? <Badge variant="outline" className="border-sky-500/25 text-sky-600 dark:text-sky-400"><Database className="mr-1 size-3" />{cacheLabel(t, selected.cache_status)}</Badge> : null}
            </div>
            <section className="space-y-4">
              <h3 className="text-sm font-medium">{t('activityRequest')}</h3>
              <dl className="grid grid-cols-2 gap-x-5 gap-y-4 rounded-xl border bg-muted/20 p-4 sm:gap-y-5 sm:p-5">
                <DetailField label={t('colTime')}>{selected.datetime || `${selected.date || ''} ${selected.time}`}</DetailField>
                <DetailField label={t('activityRequestId')}>{selected.request_id}</DetailField>
                <DetailField label={t('activityProtocol')}>{selected.ingress_protocol}</DetailField>
                <DetailField label={t('activityClient')}>{selected.client_ip}</DetailField>
                <DetailField label={t('activityUserAgent')}>{selected.user_agent}</DetailField>
                <DetailField label={t('activityRequest')}>{selected.method && selected.path ? `${selected.method} ${selected.path}` : ''}</DetailField>
                <DetailField label={t('activityRequestSize')}>{humanBytes(selected.request_bytes)}</DetailField>
                <DetailField label={t('activityPayload')}>{humanBytes(selected.bytes)}</DetailField>
              </dl>
            </section>
            <section className="space-y-4">
              <h3 className="text-sm font-medium">{t('activityRouting')}</h3>
              <dl className="grid grid-cols-2 gap-x-5 gap-y-4 rounded-xl border bg-muted/20 p-4 sm:gap-y-5 sm:p-5">
                <DetailField label={t('colModel')}>{selected.model}</DetailField>
                <DetailField label={t('activityUpstream')}>{selected.upstream_model}</DetailField>
                <DetailField label={t('colProvider')}>{selected.provider}</DetailField>
                <DetailField label={t('activityUpstreamProtocol')}>{selected.upstream_protocol}</DetailField>
                <DetailField label={t('activityAttempt')}>{selected.attempt > 0 ? `${selected.attempt} / ${selected.attempts_total || selected.attempt}` : '—'}</DetailField>
                <DetailField label={t('colLatency')}>{selected.latency_ms ? humanMillis(selected.latency_ms) : '—'}</DetailField>
              </dl>
            </section>
            {tokenTotal(selected) > 0 || selected.cost_usd > 0 ? <section className="space-y-4">
              <h3 className="text-sm font-medium">{t('activityUsage')}</h3>
              <dl className="grid grid-cols-2 gap-x-5 gap-y-4 rounded-xl border bg-muted/20 p-4 sm:gap-y-5 sm:p-5">
                <DetailField label={t('activityInputTokens')}>{humanCount(selected.prompt_tokens)}</DetailField>
                <DetailField label={t('activityOutputTokens')}>{humanCount(selected.completion_tokens)}</DetailField>
                <DetailField label={t('activityTotalTokens')}>{humanCount(tokenTotal(selected))}</DetailField>
                <DetailField label={t('activityCost')}>{selected.cost_usd > 0 ? `$${selected.cost_usd.toFixed(6)}` : '—'}</DetailField>
              </dl>
            </section> : null}
            {selected.latency_ms > 0 ? <section>
              <h3 className="mb-3 text-sm font-medium">{t('activityTiming')}</h3>
              <div className="grid grid-cols-3 gap-2">
                {[
                  { label: t('wait'), value: selected.wait_ms },
                  { label: t('first byte'), value: selected.ttfb_ms },
                  { label: t('stream'), value: selected.stream_ms },
                ].map((phase) => <div key={phase.label} className="rounded-xl bg-muted/50 p-3"><p className="text-[11px] text-muted-foreground">{phase.label}</p><p className="mt-1.5 font-mono text-sm font-medium">{humanMillis(phase.value)}</p></div>)}
              </div>
            </section> : null}
            {selected.message ? <section><h3 className="mb-3 text-sm font-medium">{t('activityMessage')}</h3><pre className="max-h-52 overflow-auto whitespace-pre-wrap break-words rounded-xl border bg-muted/30 p-4 font-mono text-xs leading-relaxed">{selected.message}</pre></section> : null}
            {selected.request_body || selected.response_body ? <section className="space-y-4">
              <p className="text-xs leading-relaxed text-muted-foreground">{t('activityBodiesNote')}</p>
              {([
                ['activityRequestBody', selected.request_body],
                ['activityResponseBody', selected.response_body],
              ] as const).map(([label, body]) => body ? <div key={label}>
                <h3 className="mb-3 text-sm font-medium">{t(label)}</h3>
                <pre className="max-h-64 overflow-auto whitespace-pre-wrap break-words rounded-xl border bg-muted/30 p-4 font-mono text-xs leading-relaxed">{body}</pre>
              </div> : null)}
            </section> : selected.request_id ? <section className="rounded-xl border border-dashed p-4">
              <p className="text-xs leading-relaxed text-muted-foreground">{t('activityBodiesAbsent')}</p>
              <Button size="sm" variant="outline" className="mt-3" onClick={onConfigureLogging}>{t('activityConfigureBodies')}</Button>
            </section> : null}
          </> : null}
        </DialogContent>
      </Dialog>
    </>
  )
}
