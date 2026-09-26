import { ArrowDown, ArrowRight, GitBranch, Pencil, Plus, Search, ShieldCheck, Trash2 } from 'lucide-react'
import { useState } from 'react'
import { toast } from 'sonner'

import { RouteDialog, blankRoute } from '@/components/RouteDialog'
import { Badge } from '@/components/ui/badge'
import { Button } from '@/components/ui/button'
import { Input } from '@/components/ui/input'
import { Switch } from '@/components/ui/switch'
import type { RouteConfig } from '@/lib/api'
import { useI18n } from '@/lib/i18n'
import { cn } from '@/lib/utils'
import { useStore } from '@/store'

interface Editing {
  index: number
  draft: RouteConfig
  isNew: boolean
}

export function Routes() {
  const { working, update } = useStore()
  const { t } = useI18n()
  const [editing, setEditing] = useState<Editing | null>(null)
  const [query, setQuery] = useState('')

  if (!working) return null
  const routes = working.routes ?? []
  const known = new Map((working.providers ?? []).map((provider) => [provider.id, provider]))
  const search = query.trim().toLowerCase()
  const visible = routes.map((route, index) => ({ route, index })).filter(({ route }) =>
    [route.model, ...(route.targets ?? []).flatMap((target) => [target.provider, target.model ?? ''])].some((value) => value.toLowerCase().includes(search)),
  )
  const addRoute = () => setEditing({ index: routes.length, draft: blankRoute(), isNew: true })

  return (
    <div className="space-y-6">
      <div className="flex flex-col gap-4 sm:flex-row sm:items-center sm:justify-between">
        <div>
          <div className="flex items-center gap-3"><h1 className="text-2xl font-semibold tracking-tight sm:text-3xl">{t('tabRoutes')}</h1><Badge variant="secondary">{routes.length}</Badge></div>
          <p className="mt-2 text-sm leading-relaxed text-muted-foreground">{t('managementRouteIntro')}</p>
        </div>
        <Button className="shrink-0 self-start sm:self-auto" onClick={addRoute}><Plus className="size-4" />{t('newRoute')}</Button>
      </div>

      <div className="flex items-start gap-3 rounded-2xl border border-primary/15 bg-primary/5 p-4 sm:p-5">
        <ShieldCheck aria-hidden className="mt-0.5 size-5 shrink-0 text-primary" />
        <div><p className="text-sm font-medium">{t('managementFailoverTitle')}</p><p className="mt-1 text-xs leading-relaxed text-muted-foreground">{t(working.server.routing_policy === 'fastest' ? 'managementRoutingFastestHint' : working.server.routing_policy === 'cheapest' ? 'managementRoutingCheapestHint' : 'routeHint')}</p></div>
      </div>

      {working.server.session_affinity_sec > 0 ? <p className="text-xs text-muted-foreground">{t('managementAffinityHint', { seconds: working.server.session_affinity_sec })}</p> : null}
      <div className="relative w-full sm:max-w-sm">
        <Search aria-hidden className="pointer-events-none absolute top-1/2 left-3 size-4 -translate-y-1/2 text-muted-foreground" />
        <Input value={query} onChange={(event) => setQuery(event.target.value)} placeholder={t('managementSearchRoutes')} aria-label={t('managementSearchRoutes')} className="h-10 bg-card pl-9" />
      </div>

      {visible.length ? (
        <div className="space-y-4">
          {visible.map(({ route, index }) => (
            <article key={`${route.model}-${index}`} className="overflow-hidden rounded-2xl border bg-card">
              <div className="flex flex-wrap items-center gap-3 border-b px-4 py-4 sm:px-5">
                <div className="flex size-10 shrink-0 items-center justify-center rounded-xl bg-primary/10 text-primary"><GitBranch className="size-5" /></div>
                <div className="min-w-0 flex-1"><h2 className="truncate font-mono text-sm font-semibold">{route.model || '—'}</h2><p className="mt-1 text-xs text-muted-foreground">{t('managementTargetCount', { n: (route.targets ?? []).length })}</p></div>
                <Switch aria-label={t('managementToggleRoute', { model: route.model })} checked={route.enabled} onCheckedChange={(enabled) => update((draft) => { const target = draft.routes[index]; if (target) target.enabled = enabled })} />
                <div className="flex items-center gap-1">
                  <Button size="icon" variant="ghost" title={t('edit')} aria-label={t('managementEditRoute', { model: route.model })} onClick={() => setEditing({ index, draft: route, isNew: false })}><Pencil className="size-4" /></Button>
                  <Button size="icon" variant="ghost" title={t('remove')} aria-label={t('managementRemoveRoute', { model: route.model })} className="text-muted-foreground hover:bg-destructive/10 hover:text-destructive" onClick={() => {
                    if (!window.confirm(t('confirmDeleteRoute', { model: route.model }))) return
                    update((draft) => { draft.routes.splice(index, 1) })
                  }}><Trash2 className="size-4" /></Button>
                </div>
              </div>
              <ol className="flex flex-col gap-3 p-4 sm:flex-row sm:flex-wrap sm:items-stretch sm:p-5" aria-label={t('colTargets')}>
                {(route.targets ?? []).map((target, at) => {
                  const provider = known.get(target.provider)
                  return (
                    <li key={at} className="flex min-w-0 flex-col items-stretch gap-3 sm:flex-row sm:items-center">
                      {at > 0 ? <><ArrowDown aria-hidden className="ml-4 size-4 text-muted-foreground/60 sm:hidden" /><ArrowRight aria-hidden className="hidden size-4 shrink-0 text-muted-foreground/60 sm:block" /></> : null}
                      <div className={cn('flex min-w-0 items-start gap-3 rounded-xl border p-3 sm:w-60', at === 0 ? 'border-primary/25 bg-primary/4' : 'bg-muted/30', !provider && 'border-destructive/30')}>
                        <span className={cn('flex size-7 shrink-0 items-center justify-center rounded-lg text-xs font-semibold tabular-nums', at === 0 ? 'bg-primary text-primary-foreground' : 'bg-muted text-muted-foreground')}>{at + 1}</span>
                        <div className="min-w-0 flex-1">
                          <p className="mb-1 text-[10px] font-medium tracking-wide text-muted-foreground">{t(at === 0 ? 'managementPrimary' : 'managementFallback')}</p>
                          <p title={target.provider} className={cn('truncate font-mono text-xs font-medium', !provider && 'text-destructive')}>{target.provider || '—'}</p>
                          <p title={target.model || route.model} className="mt-1.5 truncate font-mono text-[11px] text-muted-foreground">{target.model || route.model}</p>
                          {!provider ? <p className="mt-1.5 text-[11px] text-destructive">{t('managementMissingProvider')}</p> : !provider.enabled ? <p className="mt-1.5 text-[11px] text-warn">{t('managementDisabled')}</p> : null}
                        </div>
                      </div>
                    </li>
                  )
                })}
                {!(route.targets ?? []).length ? <li className="text-sm text-muted-foreground">{t('managementNoTargets')}</li> : null}
              </ol>
              {!route.enabled ? <div className="border-t px-5 py-2.5 text-xs text-muted-foreground">{t(working.server.pass_through_unknown ? 'managementDisabledRouteFallback' : 'managementRouteDisabled')}</div> : null}
            </article>
          ))}
        </div>
      ) : (
        <div className="flex flex-col items-center rounded-2xl border border-dashed bg-card px-6 py-16 text-center">
          <div className="mb-5 rounded-2xl bg-primary/8 p-4 text-primary"><GitBranch className="size-7" /></div>
          <h2 className="text-lg font-semibold">{t(routes.length ? 'managementNoMatches' : 'managementEmptyRoutes')}</h2>
          <p className="mt-2 max-w-sm text-sm leading-relaxed text-muted-foreground">{t(routes.length ? 'managementNoMatchesHint' : 'managementEmptyRoutesHint')}</p>
          <Button className="mt-6" variant={routes.length ? 'outline' : 'default'} onClick={() => { if (routes.length) setQuery(''); else addRoute() }}>{routes.length ? t('managementResetFilters') : <><Plus className="size-4" />{t('newRoute')}</>}</Button>
        </div>
      )}

      {editing ? <RouteDialog key={`${editing.index}-${editing.isNew}`} open draft={editing.draft} isNew={editing.isNew} onCancel={() => setEditing(null)} onApply={(route) => {
        if (!editing.isNew && JSON.stringify(working.routes[editing.index]) !== JSON.stringify(editing.draft)) {
          toast.error(t('managementEditorConflict'))
          return
        }
        update((draft) => { if (editing.isNew) draft.routes.push(route); else draft.routes[editing.index] = route })
        setEditing(null)
      }} /> : null}
    </div>
  )
}
