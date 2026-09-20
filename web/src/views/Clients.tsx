import { Pencil, Plus, Trash2, Users } from 'lucide-react'
import { useState } from 'react'

import { ClientDialog } from '@/components/ClientDialog'
import { Badge } from '@/components/ui/badge'
import { Button } from '@/components/ui/button'
import { Card, CardContent, CardHeader, CardTitle } from '@/components/ui/card'
import { Switch } from '@/components/ui/switch'
import type { ClientConfig, UInt64 } from '@/lib/api'
import { moneyPercent, quotaPercent, uintValue } from '@/lib/client-integers'
import { hasSecret } from '@/lib/clients'
import { useI18n } from '@/lib/i18n'
import { CLIENT_DEFAULTS } from '@/lib/normalize'
import { useStore } from '@/store'

export function Clients() {
  const { working, snapshot, update } = useStore()
  const { t, lang } = useI18n()
  const [editing, setEditing] = useState<{ index: number; draft: ClientConfig; isNew: boolean } | null>(null)
  if (!working) return null
  const number = (value: UInt64 = 0) => uintValue(value).toLocaleString(lang === 'zh' ? 'zh-CN' : 'en-US')
  const groups = [...new Set(working.providers.flatMap((provider) => provider.groups))].sort()
  const models = [...new Set([...working.routes.map((route) => route.model), ...working.providers.flatMap((provider) => provider.models)])].sort()
  /** A used/limit meter. `money` switches the two numbers to dollars: the daily
   *  budget is measured in currency, so rendering it through the integer
   *  formatter would show every fractional spend as 0. */
  const quota = (label: string, used: UInt64, limit: UInt64, money = false) => {
    const shown = (value: UInt64) => (money ? `$${Number(value).toFixed(4)}` : number(value))
    const bounded = money ? Number(limit) > 0 : uintValue(limit) > 0n
    // Two percentage paths on purpose: a count is an exact uint64 and a budget is
    // a real number, and each one's helper rejects the other's input.
    const percent = money ? moneyPercent(Number(used), Number(limit)) : quotaPercent(used, limit)
    return <div className="space-y-1.5">
      <div className="flex flex-wrap justify-between gap-2 text-xs"><span className="text-muted-foreground">{label}</span>
        <span className="font-mono">{shown(used)} / {bounded ? shown(limit) : t('unlimited')}</span></div>
      {bounded ? <div className="h-1.5 overflow-hidden rounded-full bg-muted" role="meter"
        aria-label={label} aria-valuenow={percent} aria-valuemin={0} aria-valuemax={100}>
        <div className="h-full bg-primary" style={{ width: `${percent}%` }} /></div> : null}
    </div>
  }
  return <div className="flex flex-col gap-5">
    <Card><CardHeader><CardTitle>{t('tabClients')}</CardTitle>
      <Button size="sm" className="ml-auto" onClick={() => setEditing({ index: working.clients.length,
        draft: structuredClone(CLIENT_DEFAULTS), isNew: true })}><Plus className="size-3.5" />{t('addClient')}</Button>
    </CardHeader><CardContent className="space-y-2 text-xs text-muted-foreground">
      <p>{t('distributionHint')}</p><p>{t('quotaAccountingHint')}</p>
      {!hasSecret(working.server) ? <p className="text-warn">{t('adminKeyRequired')}</p> : null}
    </CardContent></Card>
    {working.clients.length === 0 ? <Card><CardContent className="flex flex-col items-center gap-3 py-14 text-center">
      <Users className="size-7 text-muted-foreground" /><p className="text-sm">{t('clientsEmpty')}</p>
      <p className="max-w-lg text-xs text-muted-foreground">{t('personalUseHint')}</p>
    </CardContent></Card> : <div className="grid gap-4 xl:grid-cols-2">
      {working.clients.map((client, index) => {
        const usage = snapshot?.clients?.find((entry) => entry.client === client.id)
        return <Card key={`${client.id}-${index}`}><CardHeader>
          <div className="min-w-0"><CardTitle className="break-words">{client.name || client.id}</CardTitle>
            <div className="mt-1 break-all font-mono text-xs text-muted-foreground">{client.id}</div></div>
          <div className="ml-auto flex items-center gap-2">
            <Switch aria-label={t('clientToggle', { id: client.id })} checked={client.enabled}
              onCheckedChange={(enabled) => update((draft) => { draft.clients[index]!.enabled = enabled })} />
            <Button size="icon" variant="ghost" title={t('editClient')} aria-label={t('editClient')}
              onClick={() => setEditing({ index, draft: client, isNew: false })}><Pencil className="size-3.5" /></Button>
            <Button size="icon" variant="ghost" title={t('remove')} aria-label={t('remove')}
              onClick={() => { if (window.confirm(t('confirmDeleteClient', { id: client.id }))) update((draft) => { draft.clients.splice(index, 1) }) }}>
              <Trash2 className="size-3.5 text-destructive" /></Button>
          </div>
        </CardHeader><CardContent className="space-y-4">
          <div className="flex flex-wrap gap-2 text-xs">
            <Badge variant="outline">{t('clientKeyCount', { n: client.keys.length, enabled: client.keys.filter((key) => key.enabled).length })}</Badge>
            <Badge variant="outline">{t('activeClientRequests', { n: number(usage?.active_requests) })}</Badge>
            {!client.enabled ? <Badge variant="outline">{t('clientDisabled')}</Badge> : null}
          </div>
          <div className="grid gap-2 break-words text-xs sm:grid-cols-2">
            <div><span className="text-muted-foreground">{t('models')}: </span>{client.models.length ? client.models.join(', ') : t('allModels')}</div>
            <div><span className="text-muted-foreground">{t('providerGroups')}: </span>{client.provider_groups.length ? client.provider_groups.join(', ') : t('allGroups')}</div>
            <div>{t('clientRpm')}: <span className="font-mono">{client.requests_per_minute || t('unlimited')}</span></div>
            <div>{t('clientConcurrency')}: <span className="font-mono">{client.max_concurrent || t('unlimited')}</span></div>
            <div>{t('dailyBudget')}: <span className="font-mono">{client.budget_usd_per_day > 0 ? `$${client.budget_usd_per_day.toFixed(2)}` : t('unlimited')}</span></div>
          </div>
          {quota(t('requestsToday'), usage?.requests_today ?? 0, client.requests_per_day)}
          {quota(t('tokensToday'), usage?.tokens_today ?? 0, client.tokens_per_day)}
          {quota(t('spendToday'), usage?.cost_today ?? 0, client.budget_usd_per_day, true)}
          <div className="flex flex-wrap justify-between gap-2 border-t pt-3 text-xs text-muted-foreground">
            <span>{t('clientTotal')}: {number(usage?.requests)} · {t('ok')} {number(usage?.successes)} · {t('failed')} {number(usage?.failures)}</span>
            <span>{t('metricCost')}: ${(usage?.cost_usd ?? 0).toFixed(6)}</span>
          </div>
          <div className="flex flex-wrap gap-x-4 gap-y-1 text-[11px] text-muted-foreground">
            <span>{t('prompt')}: {number(usage?.tokens_prompt)}</span><span>{t('completion')}: {number(usage?.tokens_completion)}</span>
            <span>{t('reservedTokens')}: {number(usage?.reserved_tokens)}</span>
            <span>{t('utcDay')}: {usage?.day_unix ? new Date(usage.day_unix * 1000).toISOString().slice(0, 10) : new Date().toISOString().slice(0, 10)}</span>
          </div>
        </CardContent></Card>
      })}
    </div>}
    {editing ? <ClientDialog key={`${editing.index}-${editing.isNew}`} draft={editing.draft}
      isNew={editing.isNew} others={working.clients.filter((_, index) => index !== editing.index)} groups={groups} models={models}
      onCancel={() => setEditing(null)} onApply={(client) => {
        update((draft) => { if (editing.isNew) draft.clients.push(client); else draft.clients[editing.index] = client })
        setEditing(null)
      }} /> : null}
  </div>
}
