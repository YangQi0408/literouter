import { Activity, ArrowUpRight, ChevronRight, Clock3, Search } from 'lucide-react'
import { useState, type ReactNode } from 'react'

import { Badge } from '@/components/ui/badge'
import { Button } from '@/components/ui/button'
import { Dialog, DialogContent, DialogDescription, DialogHeader, DialogTitle } from '@/components/ui/dialog'
import type { LogEntry } from '@/lib/api'
import { humanBytes, humanMillis } from '@/lib/format'
import { levelBadgeClass, statusBadgeClass } from '@/lib/present'
import { useI18n } from '@/lib/i18n'

function DetailField({ label, children }: { label: string; children: ReactNode }) {
  return <div className="min-w-0 space-y-1.5"><dt className="text-xs text-muted-foreground">{label}</dt><dd className="break-words font-mono text-xs leading-relaxed">{children || '—'}</dd></div>
}

export function LogTable({ entries, emptyFiltered = false }: { entries: LogEntry[]; emptyFiltered?: boolean }) {
  const { t } = useI18n()
  const [selected, setSelected] = useState<LogEntry | null>(null)
  const levelName = (level: string) => t(level === 'warn' ? 'activityLevelWarn' : level === 'error' ? 'activityLevelError' : 'activityLevelInfo')
  const kindName = (kind: string) => {
    const keys: Record<string, string> = { chat: 'activityKindChat', embeddings: 'activityKindEmbeddings', audio: 'kindAudio', images: 'kindImages', models: 'activityKindModels', admin: 'activityKindAdmin', system: 'activityKindSystem' }
    return keys[kind] ? t(keys[kind]) : kind
  }

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
        <table className="w-full min-w-[880px]">
          <thead>
            <tr>
              <th>{t('colTime')}</th>
              <th>{t('colLevel')}</th>
              <th>{t('colModel')}</th>
              <th>{t('colProvider')}</th>
              <th className="text-right">{t('colStatus')}</th>
              <th className="text-right">{t('colLatency')}</th>
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
                <td><div className="max-w-36 truncate text-xs" title={entry.provider}>{entry.provider || '—'}</div></td>
                <td className="text-right"><Badge variant="outline" className={statusBadgeClass(entry.status)}>{entry.status || '—'}</Badge></td>
                <td className="whitespace-nowrap text-right font-mono text-xs tnum" title={latencyTitle(entry)}>{entry.latency_ms ? humanMillis(entry.latency_ms) : '—'}</td>
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
            <div className="space-y-1"><p className="truncate font-mono text-sm font-medium">{entry.model || kindName(entry.kind)}</p><p className="truncate text-xs text-muted-foreground">{entry.provider || kindName(entry.kind)}{entry.stream ? ` · ${t('activityStream')}` : ''}{entry.failover ? ` · ${t('activityFailover')}` : ''}</p></div>
            {entry.message ? <p className="line-clamp-2 break-words text-xs leading-relaxed text-muted-foreground">{entry.message}</p> : null}
            {entry.latency_ms ? <span className="flex items-center gap-1.5 font-mono text-[11px] text-muted-foreground"><Clock3 className="size-3" />{humanMillis(entry.latency_ms)}</span> : null}
          </button>
        ))}
      </div>

      <Dialog open={Boolean(selected)} onOpenChange={(open) => { if (!open) setSelected(null) }}>
        <DialogContent className="gap-6 sm:max-w-2xl">
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
            </div>
            <dl className="grid grid-cols-2 gap-x-5 gap-y-5 rounded-xl border bg-muted/20 p-4 sm:p-5">
              <DetailField label={t('colTime')}>{selected.datetime || `${selected.date || ''} ${selected.time}`}</DetailField>
              <DetailField label={t('activityRequestId')}>{selected.request_id}</DetailField>
              <DetailField label={t('colModel')}>{selected.model}</DetailField>
              <DetailField label={t('activityUpstream')}>{selected.upstream_model}</DetailField>
              <DetailField label={t('colProvider')}>{selected.provider}</DetailField>
              <DetailField label={t('activityAttempt')}>{selected.attempt > 0 ? `${selected.attempt} / ${selected.attempts_total || selected.attempt}` : '—'}</DetailField>
              <DetailField label={t('activityPayload')}>{humanBytes(selected.bytes)}</DetailField>
              <DetailField label={t('colLatency')}>{selected.latency_ms ? humanMillis(selected.latency_ms) : '—'}</DetailField>
            </dl>
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
          </> : null}
        </DialogContent>
      </Dialog>
    </>
  )
}
