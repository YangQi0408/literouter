import { Pause, Play, Search, Trash2 } from 'lucide-react'
import { useDeferredValue, useMemo, useState } from 'react'

import { LogTable } from '@/components/LogTable'
import { Button } from '@/components/ui/button'
import { Card, CardHeader, CardTitle } from '@/components/ui/card'
import { Input } from '@/components/ui/input'
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from '@/components/ui/select'
import { useI18n } from '@/lib/i18n'
import { useStore } from '@/store'

export function Logs() {
  const { logs, logsPaused, setLogsPaused, clearLogs } = useStore()
  const { t } = useI18n()
  const [query, setQuery] = useState('')
  const [level, setLevel] = useState('all')
  const [kind, setKind] = useState('all')
  const deferredQuery = useDeferredValue(query)

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

  return (
    <Card>
      <CardHeader>
        <CardTitle>{t('tabLogs')}</CardTitle>
        <span className="ml-1 font-mono text-xs text-muted-foreground">
          {filtered.length}/{logs.length}
        </span>
        <div className="ml-auto flex flex-wrap items-center gap-2">
          <div className="relative">
            <Search className="absolute top-1/2 left-2.5 size-3.5 -translate-y-1/2 text-muted-foreground" />
            <Input
              value={query}
              onChange={(event) => setQuery(event.target.value)}
              placeholder={t('search')}
              className="h-8 w-48 pl-8 text-xs"
            />
          </div>
          <Select value={level} onValueChange={setLevel}>
            <SelectTrigger size="sm" className="h-8 w-32 text-xs">
              <SelectValue />
            </SelectTrigger>
            <SelectContent>
              <SelectItem value="all">{t('allLevels')}</SelectItem>
              <SelectItem value="info">info</SelectItem>
              <SelectItem value="warn">warn</SelectItem>
              <SelectItem value="error">error</SelectItem>
            </SelectContent>
          </Select>
          <Select value={kind} onValueChange={setKind}>
            <SelectTrigger size="sm" className="h-8 w-32 text-xs">
              <SelectValue />
            </SelectTrigger>
            <SelectContent>
              <SelectItem value="all">{t('allKinds')}</SelectItem>
              <SelectItem value="chat">chat</SelectItem>
              <SelectItem value="embeddings">embeddings</SelectItem>
              <SelectItem value="audio">{t('kindAudio')}</SelectItem>
              <SelectItem value="images">{t('kindImages')}</SelectItem>
              <SelectItem value="models">models</SelectItem>
              <SelectItem value="admin">admin</SelectItem>
              <SelectItem value="system">system</SelectItem>
            </SelectContent>
          </Select>
          <Button
            size="icon"
            variant="ghost"
            title={logsPaused ? t('resume') : t('pause')}
            onClick={() => setLogsPaused(!logsPaused)}
          >
            {logsPaused ? <Play className="size-4" /> : <Pause className="size-4" />}
          </Button>
          <Button size="icon" variant="ghost" title={t('clear')} onClick={clearLogs}>
            <Trash2 className="size-4" />
          </Button>
        </div>
      </CardHeader>
      <LogTable entries={filtered} />
    </Card>
  )
}
