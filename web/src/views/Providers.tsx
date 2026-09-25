import { ArrowUpRight, KeyRound, LoaderCircle, Network, Pencil, Plus, Radio, Search, Server, Trash2 } from 'lucide-react'
import { useState } from 'react'
import { toast } from 'sonner'

import { ProviderDialog, blankProvider } from '@/components/ProviderDialog'
import { Badge } from '@/components/ui/badge'
import { Button } from '@/components/ui/button'
import { Input } from '@/components/ui/input'
import { Switch } from '@/components/ui/switch'
import type { ProviderConfig } from '@/lib/api'
import { useI18n } from '@/lib/i18n'
import { cn } from '@/lib/utils'
import { useStore } from '@/store'

interface Editing {
  index: number
  draft: ProviderConfig
  isNew: boolean
}

export function Providers() {
  const { working, loaded, update, probe } = useStore()
  const { t } = useI18n()
  const [editing, setEditing] = useState<Editing | null>(null)
  const [query, setQuery] = useState('')
  const [filter, setFilter] = useState<'all' | 'enabled' | 'disabled'>('all')
  const [probing, setProbing] = useState<Set<string>>(new Set())

  if (!working) return null
  const providers = working.providers ?? []
  const enabledCount = providers.filter((provider) => provider.enabled).length
  const search = query.trim().toLowerCase()
  const visible = providers.map((provider, index) => ({ provider, index })).filter(({ provider }) => {
    const matchesFilter = filter === 'all' || provider.enabled === (filter === 'enabled')
    const searchable = [provider.id, provider.name, provider.base_url, provider.protocol, ...(provider.models ?? [])]
    return matchesFilter && searchable.some((value) => value?.toLowerCase().includes(search))
  })
  const addProvider = () => setEditing({ index: providers.length, draft: blankProvider(), isNew: true })

  const runProbe = async (id: string) => {
    setProbing((prev) => new Set(prev).add(id))
    try {
      await probe(id)
    } catch (error) {
      // The store handles connection and authentication failures.
      toast.error(t('probeFail', { detail: error instanceof Error ? error.message : String(error) }))
    } finally {
      setProbing((prev) => {
        const next = new Set(prev)
        next.delete(id)
        return next
      })
    }
  }

  return (
    <div className="space-y-6">
      <div className="flex flex-col gap-4 sm:flex-row sm:items-center sm:justify-between">
        <div>
          <div className="flex items-center gap-3">
            <h1 className="text-2xl font-semibold tracking-tight sm:text-3xl">{t('tabProviders')}</h1>
            <Badge variant="secondary" className="tabular-nums">{providers.length}</Badge>
          </div>
          <p className="mt-2 text-sm leading-relaxed text-muted-foreground">{t('managementProviderIntro')}</p>
        </div>
        <Button className="shrink-0 self-start sm:self-auto" onClick={addProvider}>
          <Plus className="size-4" />{t('newProvider')}
        </Button>
      </div>

      <div className="grid grid-cols-3 gap-2 sm:gap-4">
        {[
          { icon: Server, label: 'managementTotalProviders', value: providers.length },
          { icon: Radio, label: 'managementEnabledProviders', value: enabledCount },
          { icon: Network, label: 'managementProtocols', value: new Set(providers.map((provider) => provider.protocol)).size },
        ].map(({ icon: Icon, label, value }) => (
          <div key={label} className="flex min-w-0 items-center gap-3 rounded-2xl border bg-card p-3 sm:p-5">
            <div className="hidden size-10 shrink-0 items-center justify-center rounded-xl bg-primary/8 text-primary sm:flex"><Icon className="size-5" /></div>
            <div className="min-w-0">
              <p className="text-xs leading-5 text-muted-foreground">{t(label)}</p>
              <p className="mt-1 text-xl font-semibold tracking-tight tabular-nums sm:text-2xl">{value}</p>
            </div>
          </div>
        ))}
      </div>

      <div className="flex flex-col gap-3 sm:flex-row sm:items-center sm:justify-between">
        <div className="relative w-full sm:max-w-sm">
          <Search aria-hidden className="pointer-events-none absolute top-1/2 left-3 size-4 -translate-y-1/2 text-muted-foreground" />
          <Input value={query} onChange={(event) => setQuery(event.target.value)} placeholder={t('managementSearchProviders')} aria-label={t('managementSearchProviders')} className="h-10 bg-card pl-9" />
        </div>
        <div className="flex w-fit items-center gap-1 rounded-xl border bg-muted/40 p-1" role="group" aria-label={t('managementFilterStatus')}>
          {(['all', 'enabled', 'disabled'] as const).map((value) => (
            <button key={value} type="button" aria-pressed={filter === value} onClick={() => setFilter(value)} className={cn('rounded-lg px-3 py-1.5 text-xs font-medium transition-colors focus-visible:outline-2 focus-visible:outline-ring', filter === value ? 'bg-card text-foreground shadow-sm' : 'text-muted-foreground hover:text-foreground')}>
              {t(`managementFilter${value}`)}
            </button>
          ))}
        </div>
      </div>

      {visible.length ? (
        <div className="grid gap-4 xl:grid-cols-2 2xl:grid-cols-3">
          {visible.map(({ provider, index }) => {
            // A draft may replace an environment reference with a literal while
            // retaining its old source marker. Never render that literal here.
            const reference = /^\$\{([A-Za-z_][A-Za-z0-9_]*)(?::-[\s\S]*)?\}$/.exec(provider.api_key)?.[1]
              ?? /^\$([A-Za-z_][A-Za-z0-9_]*)$/.exec(provider.api_key)?.[1]
            const keyText = provider.api_key_clear ? t('keyWillClear')
              : reference ? t('keyEnv', { name: reference })
              : provider.api_key ? t('managementKeyConfigured')
              : provider.api_key_source === 'literal' ? t('keyStored') : t('keyNone')
            const models = provider.models ?? []
            const saved = loaded?.config.providers.find((item) => item.id === provider.id)
            const canProbe = !!saved && JSON.stringify(saved) === JSON.stringify(provider)
            return (
              <article key={`${provider.id}-${index}`} className="group flex min-w-0 flex-col overflow-hidden rounded-2xl border bg-card transition-[border-color,box-shadow] hover:border-primary/25 hover:shadow-md hover:shadow-primary/3">
                <div className="flex items-start gap-3 p-5 pb-4">
                  <div className={cn('flex size-11 shrink-0 items-center justify-center rounded-xl text-sm font-bold uppercase', provider.enabled ? 'bg-primary/10 text-primary' : 'bg-muted text-muted-foreground')} aria-hidden>
                    {(provider.name || provider.id || 'LR').slice(0, 2)}
                  </div>
                  <div className="min-w-0 flex-1">
                    <h2 className="truncate text-base font-semibold">{provider.name || provider.id || '—'}</h2>
                    <p className="mt-1 truncate font-mono text-xs text-muted-foreground">{provider.id || '—'}</p>
                  </div>
                  <Switch aria-label={t('managementToggleProvider', { id: provider.id })} checked={provider.enabled} onCheckedChange={(enabled) => update((draft) => { const target = draft.providers[index]; if (target) target.enabled = enabled })} />
                </div>

                <div className="flex flex-1 flex-col gap-4 px-5 pb-5">
                  <div className="flex flex-wrap items-center gap-2">
                    <Badge variant="outline" className="rounded-md font-mono text-[11px]">{provider.protocol}</Badge>
                    <span className={cn('inline-flex items-center gap-1.5 text-xs', provider.enabled ? 'text-ok' : 'text-muted-foreground')}><span className="size-1.5 rounded-full bg-current" />{t(provider.enabled ? 'managementEnabled' : 'managementDisabled')}</span>
                    <span className="ml-auto text-xs text-muted-foreground">{t('managementPriority')} <span className="font-medium text-foreground tabular-nums">{provider.priority}</span></span>
                  </div>
                  <div className="flex min-w-0 items-center gap-2 rounded-xl bg-muted/50 px-3 py-2.5">
                    <ArrowUpRight aria-hidden className="size-3.5 shrink-0 text-muted-foreground" />
                    <span title={provider.base_url} className="truncate font-mono text-xs text-muted-foreground">{provider.base_url || '—'}</span>
                  </div>
                  <div className="flex items-center gap-2 text-xs text-muted-foreground"><KeyRound aria-hidden className="size-3.5 shrink-0" /><span className="truncate">{keyText}</span></div>
                  <div className="min-h-12">
                    <p className="mb-2 text-[11px] font-medium tracking-wide text-muted-foreground">{t('models')}</p>
                    <div className="flex flex-wrap gap-1.5">
                      {models.slice(0, 3).map((model, at) => <span key={`${model}-${at}`} className="max-w-full truncate rounded-md bg-muted/70 px-2 py-1 font-mono text-[11px]">{model}</span>)}
                      {models.length > 3 ? <span className="rounded-md px-1 py-1 text-[11px] text-muted-foreground">+{models.length - 3}</span> : null}
                      {!models.length ? <span className="text-xs text-muted-foreground">{t('managementNoModels')}</span> : null}
                    </div>
                  </div>
                  {provider.note ? <p className="line-clamp-2 text-xs leading-relaxed text-muted-foreground">{provider.note}</p> : null}
                </div>

                <div className="flex items-center gap-1 border-t bg-muted/20 px-3 py-2">
                  <span title={!canProbe ? t('managementSaveToProbe') : undefined}>
                    <Button size="sm" variant="ghost" disabled={!canProbe || probing.has(provider.id)} onClick={() => void runProbe(provider.id)}>
                      {probing.has(provider.id) ? <LoaderCircle className="size-3.5 animate-spin" /> : <Radio className="size-3.5" />}{t(probing.has(provider.id) ? 'testing' : 'test')}
                    </Button>
                  </span>
                  <Button size="sm" variant="ghost" className="ml-auto" onClick={() => setEditing({ index, draft: provider, isNew: false })}><Pencil className="size-3.5" />{t('edit')}</Button>
                  <Button size="icon" variant="ghost" aria-label={t('managementRemoveProvider', { id: provider.id })} title={t('remove')} className="text-muted-foreground hover:bg-destructive/10 hover:text-destructive" onClick={() => {
                    if (!window.confirm(t('confirmDeleteProvider', { id: provider.id }))) return
                    update((draft) => { draft.providers.splice(index, 1) })
                  }}><Trash2 className="size-3.5" /></Button>
                </div>
              </article>
            )
          })}
        </div>
      ) : (
        <div className="flex flex-col items-center rounded-2xl border border-dashed bg-card px-6 py-16 text-center">
          <div className="mb-5 rounded-2xl bg-primary/8 p-4 text-primary"><Server className="size-7" /></div>
          <h2 className="text-lg font-semibold">{t(providers.length ? 'managementNoMatches' : 'managementEmptyProviders')}</h2>
          <p className="mt-2 max-w-sm text-sm leading-relaxed text-muted-foreground">{t(providers.length ? 'managementNoMatchesHint' : 'managementEmptyProvidersHint')}</p>
          <Button className="mt-6" variant={providers.length ? 'outline' : 'default'} onClick={() => { if (providers.length) { setQuery(''); setFilter('all') } else addProvider() }}>
            {providers.length ? t('managementResetFilters') : <><Plus className="size-4" />{t('newProvider')}</>}
          </Button>
        </div>
      )}

      {editing ? (
        <ProviderDialog key={`${editing.index}-${editing.isNew}`} open draft={editing.draft} isNew={editing.isNew} onCancel={() => setEditing(null)} onApply={(provider) => {
          update((draft) => { if (editing.isNew) draft.providers.push(provider); else draft.providers[editing.index] = provider })
          setEditing(null)
        }} />
      ) : null}
    </div>
  )
}
