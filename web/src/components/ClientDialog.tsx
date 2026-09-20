import { Plus, Trash2 } from 'lucide-react'
import { useId, useState } from 'react'

import { Field, LinesField, NumberField, SwitchField, TextField } from '@/components/fields'
import { SecretInput } from '@/components/SecretInput'
import { Button } from '@/components/ui/button'
import { Dialog, DialogContent, DialogDescription, DialogFooter, DialogHeader, DialogTitle } from '@/components/ui/dialog'
import { Input } from '@/components/ui/input'
import type { ClientConfig, UInt64 } from '@/lib/api'
import { MAX_CLIENT_QUOTA, parseUInt64 } from '@/lib/client-integers'
import { clientProblem } from '@/lib/clients'
import { useI18n } from '@/lib/i18n'

function IntegerField({ label, value, onChange, max = MAX_CLIENT_QUOTA }: {
  label: string; value: UInt64; onChange: (value: UInt64) => void; max?: bigint
}) {
  const { t } = useI18n()
  const [raw, setRaw] = useState(String(value))
  return <Input aria-label={label} inputMode="numeric" required pattern="[0-9]+" value={raw}
    className="h-9 font-mono text-xs" onChange={(event) => {
      const text = event.target.value
      setRaw(text)
      const number = parseUInt64(text, max)
      event.target.setCustomValidity(number === null ? t('invalidInteger', { max: max.toString() }) : '')
      if (number !== null) onChange(number)
    }} />
}

export function ClientDialog({ draft, others, isNew, groups, models, onCancel, onApply }: {
  draft: ClientConfig; others: ClientConfig[]; isNew: boolean; groups: string[]; models: string[]
  onCancel: () => void; onApply: (client: ClientConfig) => void
}) {
  const { t } = useI18n()
  const [client, setClient] = useState(() => structuredClone(draft))
  const formId = useId()
  const patch = (changes: Partial<ClientConfig>) => setClient((prev) => ({ ...prev, ...changes }))
  const problem = clientProblem(client, others)
  const choice = (field: 'models' | 'provider_groups', values: string[]) => (
    <div className="flex flex-wrap gap-1">
      {values.slice(0, 24).map((value) => <Button key={value} type="button" size="sm"
        className="max-w-full truncate font-mono text-[11px]" variant={client[field].includes(value) ? 'secondary' : 'ghost'}
        aria-pressed={client[field].includes(value)} onClick={() => patch({ [field]: client[field].includes(value)
          ? client[field].filter((item) => item !== value) : [...client[field], value] })}>{value}</Button>)}
    </div>
  )
  return <Dialog open onOpenChange={(open) => { if (!open) onCancel() }}>
    <DialogContent className="sm:max-w-3xl">
      <DialogHeader>
        <DialogTitle>{isNew ? t('newClient') : t('editClient')}</DialogTitle>
        <DialogDescription>{t('clientDialogHint')}</DialogDescription>
      </DialogHeader>
      <form id={formId} className="grid max-h-[65vh] gap-4 overflow-auto px-4 sm:grid-cols-2"
        onSubmit={(event) => { event.preventDefault(); if (!problem) onApply({ ...client, id: client.id.trim(),
          keys: client.keys.map((key) => ({ ...key, id: key.id.trim() })) }) }}>
        <Field label="id" hint={t('clientIdHint')}>
          <Input aria-label="client id" value={client.id} readOnly={!isNew}
            onChange={(event) => patch({ id: event.target.value })} className="h-9 font-mono text-xs" />
        </Field>
        <Field label="name"><TextField ariaLabel="client name" value={client.name} mono={false} onChange={(name) => patch({ name })} /></Field>
        <Field label="enabled"><SwitchField checked={client.enabled} onChange={(enabled) => patch({ enabled })}
          label={t('clientEnabled')} /></Field>
        <div className="space-y-3 rounded-lg border p-3 sm:col-span-2">
          <div className="flex items-center justify-between"><h3 className="text-sm font-medium">{t('clientKeys')}</h3>
            <Button type="button" variant="outline" size="sm" onClick={() => patch({ keys: [...client.keys,
              { id: '', api_key: '', enabled: true }] })}><Plus className="size-3.5" />{t('addKey')}</Button></div>
          <p className="text-xs text-muted-foreground">{t('keyOnceHint')}</p>
          {client.keys.map((key, index) => <div key={index} className="space-y-2 rounded-md bg-muted/30 p-3">
            <div className="flex flex-wrap items-center gap-2">
              <Input aria-label={`key id ${index + 1}`} value={key.id} placeholder={t('keyId')}
                readOnly={!!key.api_key_source}
                className="h-8 min-w-24 flex-1 font-mono text-xs" onChange={(event) => patch({ keys: client.keys.map((item, i) =>
                  i === index ? { ...item, id: event.target.value } : item) })} />
              <SwitchField ariaLabel={t('keyToggle', { id: key.id || t('keyId') })} checked={key.enabled} label={t('colEnabled')} onChange={(enabled) => patch({ keys: client.keys.map((item, i) =>
                i === index ? { ...item, enabled } : item) })} />
              <Button type="button" variant="ghost" size="icon" title={t('removeKey')}
                aria-label={t('removeKey')} onClick={() => patch({ keys: client.keys.filter((_, i) => i !== index) })}>
                <Trash2 className="size-3.5 text-destructive" /></Button>
            </div>
            <SecretInput label={`client key ${index + 1}`} value={key} generate clear={!key.enabled}
              onChange={(changes) => patch({ keys: client.keys.map((item, i) => i === index ? { ...item, ...changes } : item) })} />
          </div>)}
          {client.keys.length === 0 ? <p className="text-xs text-muted-foreground">{t('noClientKeys')}</p> : null}
        </div>
        <Field label="models" hint={t('clientModelsHint')}>
          <LinesField ariaLabel="models" value={client.models} rows={3} onChange={(models) => patch({ models })} />
          {choice('models', models)}
        </Field>
        <Field label="provider_groups" hint={t('clientGroupsHint')}>
          <LinesField ariaLabel="provider_groups" value={client.provider_groups} rows={3} onChange={(provider_groups) => patch({ provider_groups })} />
          {choice('provider_groups', groups)}
        </Field>
        <Field label="requests_per_minute" hint={t('zeroUnlimited')}>
          <IntegerField label="requests_per_minute" value={client.requests_per_minute} max={2147483647n}
            onChange={(value) => patch({ requests_per_minute: Number(value) })} />
        </Field>
        <Field label="max_concurrent" hint={t('zeroUnlimited')}>
          <IntegerField label="max_concurrent" value={client.max_concurrent} max={2147483647n}
            onChange={(value) => patch({ max_concurrent: Number(value) })} />
        </Field>
        <Field label="requests_per_day" hint={t('dailyQuotaHint')}>
          <IntegerField label="requests_per_day" value={client.requests_per_day} onChange={(requests_per_day) => patch({ requests_per_day: Number(requests_per_day) })} />
        </Field>
        <Field label="tokens_per_day" hint={t('dailyQuotaHint')}>
          <IntegerField label="tokens_per_day" value={client.tokens_per_day} onChange={(tokens_per_day) => patch({ tokens_per_day: Number(tokens_per_day) })} />
        </Field>
        <Field label="budget_usd_per_day" hint={t('budgetHint')}>
          <NumberField
            value={client.budget_usd_per_day}
            step={0.5}
            min={0}
            onChange={(budget_usd_per_day) => patch({ budget_usd_per_day })}
          />
        </Field>
        <Field label="token_reservation" hint={t('reservationHint')}>
          <IntegerField label="token_reservation" value={client.token_reservation} onChange={(token_reservation) => patch({ token_reservation: Number(token_reservation) })} />
        </Field>
      </form>
      <DialogFooter className="flex-wrap">
        {problem ? <p className="mr-auto text-xs text-warn">{t(problem)}</p> : null}
        <Button type="button" variant="ghost" onClick={onCancel}>{t('cancel')}</Button>
        <Button form={formId} type="submit" disabled={!!problem}>{t('apply')}</Button>
      </DialogFooter>
    </DialogContent>
  </Dialog>
}
