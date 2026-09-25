import { ArrowDownWideNarrow, Pause, Play, Search, SlidersHorizontal, Trash2, X } from 'lucide-react'
import { useDeferredValue, useMemo, useState } from 'react'

import { LogTable } from '@/components/LogTable'
import { Button } from '@/components/ui/button'
import { Card } from '@/components/ui/card'
import { Input } from '@/components/ui/input'
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@/components/ui/select'
import { useI18n } from '@/lib/i18n'
import { cn } from '@/lib/utils'
import { useStore } from '@/store'

export function Logs() {
  const { logs, logsPaused, setLogsPaused, clearLogs, online } = useStore()
  const { t } = useI18n()
  const [query, setQuery] = useState('')
  const [level, setLevel] = useState('all')
  const [kind, setKind] = useState('all')
  const deferredQuery = useDeferredValue(query)
  const hasFilters = Boolean(query.trim() || level !== 'all' || kind !== 'all')

  const filtered = useMemo(() => {
    const needle = deferredQuery.trim().toLowerCase()
    return logs
      .filter((entry) => {
        if (level !== 'all' && entry.level !== level) return false
        if (kind !== 'all' && entry.kind !== kind) return false
        if (!needle) return true
        return [entry.message, entry.model, entry.provider, entry.request_id, entry.upstream_model]
          .some((field) => String(field ?? '').toLowerCase().includes(needle))
      })
      .slice(-800)
      .reverse()
  }, [logs, deferredQuery, level, kind])

  function resetFilters() {
    setQuery('')
    setLevel('all')
    setKind('all')
  }

  return (
    <div className="space-y-7">
      <div className="flex flex-col justify-between gap-4 sm:flex-row sm:items-center">
        <div>
          <h1 className="text-2xl font-semibold tracking-tight sm:text-3xl">{t('activityTitle')}</h1>
          <p className="mt-2 text-sm leading-relaxed text-muted-foreground">{t('activityIntro')}</p>
        </div>
        <div className={cn('flex w-fit shrink-0 items-center gap-2 rounded-full border px-3 py-1.5 text-xs font-medium', logsPaused ? 'border-warn/20 bg-warn/5 text-warn' : online ? 'border-ok/20 bg-ok/5 text-ok' : 'border-border bg-muted/40 text-muted-foreground')}>
          <span className={cn('size-1.5 rounded-full bg-current', online && !logsPaused && 'motion-safe:animate-pulse')} />
          {t(logsPaused ? 'activityPaused' : online ? 'activityLive' : 'activityOffline')}
        </div>
      </div>

      <Card className="gap-0 overflow-hidden py-0">
        <div className="flex flex-col gap-3 border-b border-border/70 p-4 xl:flex-row xl:items-center">
          <div className="relative min-w-0 flex-1">
            <Search className="pointer-events-none absolute left-3.5 top-1/2 size-4 -translate-y-1/2 text-muted-foreground" />
            <Input
              value={query}
              aria-label={t('activitySearch')}
              onChange={(event) => setQuery(event.target.value)}
              placeholder={t('activitySearch')}
              className="h-10 w-full border-transparent bg-muted/60 pl-10 pr-10 text-sm shadow-none focus-visible:bg-background"
            />
            {query ? <button type="button" onClick={() => setQuery('')} aria-label={t('clear')} className="absolute right-2.5 top-1/2 -translate-y-1/2 rounded p-1 text-muted-foreground hover:text-foreground focus-visible:ring-2 focus-visible:ring-ring"><X className="size-3.5" /></button> : null}
          </div>
          <div className="flex flex-wrap items-center gap-2">
            <SlidersHorizontal className="mr-1 hidden size-4 text-muted-foreground sm:block" />
            <Select value={level} onValueChange={setLevel}>
              <SelectTrigger aria-label={t('activityFilterLevel')} className="h-10 min-w-0 flex-1 text-xs sm:w-32 sm:flex-none"><SelectValue /></SelectTrigger>
              <SelectContent>
                <SelectItem value="all">{t('allLevels')}</SelectItem>
                <SelectItem value="info">{t('activityLevelInfo')}</SelectItem>
                <SelectItem value="warn">{t('activityLevelWarn')}</SelectItem>
                <SelectItem value="error">{t('activityLevelError')}</SelectItem>
              </SelectContent>
            </Select>
            <Select value={kind} onValueChange={setKind}>
              <SelectTrigger aria-label={t('activityFilterKind')} className="h-10 min-w-0 flex-1 text-xs sm:w-36 sm:flex-none"><SelectValue /></SelectTrigger>
              <SelectContent>
                <SelectItem value="all">{t('allKinds')}</SelectItem>
                <SelectItem value="chat">{t('activityKindChat')}</SelectItem>
                <SelectItem value="embeddings">{t('activityKindEmbeddings')}</SelectItem>
                <SelectItem value="audio">{t('kindAudio')}</SelectItem>
                <SelectItem value="images">{t('kindImages')}</SelectItem>
                <SelectItem value="models">{t('activityKindModels')}</SelectItem>
                <SelectItem value="admin">{t('activityKindAdmin')}</SelectItem>
                <SelectItem value="system">{t('activityKindSystem')}</SelectItem>
              </SelectContent>
            </Select>
            <span className="mx-1 hidden h-5 w-px bg-border sm:block" />
            <Button size="icon" variant={logsPaused ? 'secondary' : 'ghost'} aria-label={logsPaused ? t('resume') : t('pause')} title={logsPaused ? t('resume') : t('pause')} onClick={() => setLogsPaused(!logsPaused)}>
              {logsPaused ? <Play className="size-4" /> : <Pause className="size-4" />}
            </Button>
            <Button size="icon" variant="ghost" aria-label={t('clear')} title={t('clear')} onClick={clearLogs} disabled={!logs.length}><Trash2 className="size-4" /></Button>
          </div>
        </div>
        <div className="flex min-h-12 flex-wrap items-center justify-between gap-2 border-b border-border/70 bg-muted/15 px-5 py-3 text-xs text-muted-foreground">
          <span aria-live="polite">{t('activityCount', { shown: filtered.length, total: logs.length })}</span>
          {hasFilters ? <button type="button" className="flex items-center gap-1 text-primary hover:underline focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-ring" onClick={resetFilters}><X className="size-3" />{t('activityReset')}</button> : <span className="flex items-center gap-1.5"><ArrowDownWideNarrow className="size-3.5" />{t('activityNewest')}</span>}
        </div>
        <LogTable entries={filtered} emptyFiltered={hasFilters} />
      </Card>
    </div>
  )
}
