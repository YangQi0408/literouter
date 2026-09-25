import { ArrowDown, ArrowUp, GitBranch, Plus, ShieldCheck, Trash2 } from 'lucide-react'
import { useEffect, useState } from 'react'

import { Field, SwitchField, TextField } from '@/components/fields'
import { Button } from '@/components/ui/button'
import {
  Dialog,
  DialogContent,
  DialogDescription,
  DialogFooter,
  DialogHeader,
  DialogTitle,
} from '@/components/ui/dialog'
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from '@/components/ui/select'
import type { RouteConfig, RouteTarget } from '@/lib/api'
import { useI18n } from '@/lib/i18n'
import { cn } from '@/lib/utils'
import { useStore } from '@/store'

export const blankRoute = (): RouteConfig => ({
  model: '',
  targets: [{ provider: '', model: '' }],
  enabled: true,
})

export function RouteDialog({
  open,
  draft,
  isNew,
  onCancel,
  onApply,
}: {
  open: boolean
  draft: RouteConfig
  isNew: boolean
  onCancel: () => void
  onApply: (route: RouteConfig) => void
}) {
  const { t } = useI18n()
  const { working } = useStore()
  const [route, setRoute] = useState<RouteConfig>(draft)

  useEffect(() => {
    if (open) setRoute(draft)
  }, [open, draft])

  const providers = (working?.providers ?? []).map((provider) => provider.id).filter(Boolean)
  const targets = route.targets ?? []

  const patchTarget = (index: number, changes: Partial<RouteTarget>) =>
    setRoute((prev) => ({
      ...prev,
      targets: (prev.targets ?? []).map((target, at) => (at === index ? { ...target, ...changes } : target)),
    }))

  const move = (index: number, delta: number) =>
    setRoute((prev) => {
      const next = [...(prev.targets ?? [])]
      const target = next[index]
      const destination = index + delta
      if (!target || destination < 0 || destination >= next.length) return prev
      next.splice(index, 1)
      next.splice(destination, 0, target)
      return { ...prev, targets: next }
    })

  return (
    <Dialog open={open} onOpenChange={(next) => (next ? undefined : onCancel())}>
      <DialogContent className="flex max-h-[calc(100dvh-2rem)] flex-col gap-0 overflow-hidden p-0 sm:max-w-2xl">
        <DialogHeader className="border-b px-5 py-5 text-left sm:px-6">
          <div className="flex items-center gap-3 pr-6">
            <div className="flex size-11 shrink-0 items-center justify-center rounded-xl bg-primary/10 text-primary"><GitBranch className="size-5" /></div>
            <div className="min-w-0"><DialogTitle>{isNew ? t('newRoute') : t('editRoute')}</DialogTitle><DialogDescription className="mt-1.5 text-xs leading-relaxed">{t('managementRouteDialogHint')}</DialogDescription></div>
          </div>
        </DialogHeader>

        <div className="max-h-[65dvh] min-h-0 space-y-6 overflow-y-auto overscroll-contain px-5 py-6 sm:px-6">
          <div className="grid gap-5 sm:grid-cols-[1fr_auto]">
            <Field label={t('managementPublicModel')} hint={t('managementPublicModelHint')}>
              <TextField value={route.model} placeholder="gpt-4o" onChange={(model) => setRoute((prev) => ({ ...prev, model }))} />
            </Field>
            <Field label={t('managementAvailability')}>
              <SwitchField checked={route.enabled} onChange={(enabled) => setRoute((prev) => ({ ...prev, enabled }))} label={t('managementEnabled')} />
            </Field>
          </div>

          <section className="border-t pt-5">
            <div className="mb-4">
              <h3 className="text-sm font-semibold">{t('managementFailoverTitle')}</h3>
              <p className="mt-1 text-xs leading-relaxed text-muted-foreground">{t('routeHint')}</p>
            </div>
            {!providers.length ? <p className="mb-4 rounded-xl border border-warn/20 bg-warn/5 px-3 py-2.5 text-xs leading-relaxed text-warn">{t('managementAddProviderFirst')}</p> : null}
            <div className="space-y-3">
              {targets.map((target, index) => (
                <div key={index} className={cn('overflow-hidden rounded-xl border', index === 0 && 'border-primary/25')}>
                  <div className={cn('flex items-center gap-2 border-b px-3 py-2', index === 0 ? 'bg-primary/5' : 'bg-muted/40')}>
                    <span className={cn('flex size-6 items-center justify-center rounded-md text-[11px] font-semibold tabular-nums', index === 0 ? 'bg-primary text-primary-foreground' : 'bg-muted text-muted-foreground')}>{index + 1}</span>
                    <span className="text-xs font-medium">{t(index === 0 ? 'managementPrimary' : 'managementFallback')}</span>
                    <div className="ml-auto flex items-center gap-0.5">
                      <Button type="button" size="icon" variant="ghost" className="size-8" title={t('moveUp')} aria-label={t('managementMoveTargetUp', { n: index + 1 })} disabled={index === 0} onClick={() => move(index, -1)}><ArrowUp className="size-3.5" /></Button>
                      <Button type="button" size="icon" variant="ghost" className="size-8" title={t('moveDown')} aria-label={t('managementMoveTargetDown', { n: index + 1 })} disabled={index === targets.length - 1} onClick={() => move(index, 1)}><ArrowDown className="size-3.5" /></Button>
                      <Button type="button" size="icon" variant="ghost" className="size-8 text-muted-foreground hover:text-destructive" title={t('remove')} aria-label={t('managementRemoveTarget', { n: index + 1 })} disabled={targets.length === 1} onClick={() => setRoute((prev) => ({ ...prev, targets: (prev.targets ?? []).filter((_, at) => at !== index) }))}><Trash2 className="size-3.5" /></Button>
                    </div>
                  </div>
                  <div className="grid gap-4 p-4 sm:grid-cols-2">
                    <Field label={t('colProvider')}>
                      <Select value={target.provider || undefined} onValueChange={(provider) => patchTarget(index, { provider })}>
                        <SelectTrigger aria-label={t('managementTargetProviderLabel', { n: index + 1 })} className="h-10 w-full min-w-0 font-mono text-sm"><SelectValue placeholder={t('managementSelectProvider')} /></SelectTrigger>
                        <SelectContent>
                          {providers.map((id) => <SelectItem key={id} value={id} className="font-mono text-sm">{id}</SelectItem>)}
                          {target.provider && !providers.includes(target.provider) ? <SelectItem value={target.provider} className="font-mono text-sm">{target.provider} · {t('managementMissingProvider')}</SelectItem> : null}
                        </SelectContent>
                      </Select>
                    </Field>
                    <Field label={t('managementUpstreamModel')}>
                      <TextField value={target.model ?? ''} placeholder={route.model || t('managementKeepModel')} ariaLabel={t('managementTargetModelLabel', { n: index + 1 })} onChange={(model) => patchTarget(index, { model })} />
                    </Field>
                  </div>
                </div>
              ))}
              <Button type="button" variant="outline" className="h-10 w-full border-dashed bg-muted/10" onClick={() => setRoute((prev) => ({ ...prev, targets: [...(prev.targets ?? []), { provider: '', model: '' }] }))}><Plus className="size-4" />{t('addTarget')}</Button>
            </div>
          </section>
        </div>

        <DialogFooter className="border-t bg-muted/20 p-4 sm:items-center sm:px-6">
          <p className="mr-auto hidden items-center gap-1.5 text-xs text-muted-foreground sm:flex"><ShieldCheck className="size-3.5" />{t('managementDraftHint')}</p>
          <Button variant="outline" onClick={onCancel}>{t('cancel')}</Button>
          <Button disabled={!route.model.trim() || targets.length === 0 || targets.some((target) => !target.provider)} onClick={() => onApply({
            ...route,
            targets: targets.map((target) => ({ provider: target.provider, ...(target.model?.trim() ? { model: target.model.trim() } : {}) })),
          })}>{t('apply')}</Button>
        </DialogFooter>
      </DialogContent>
    </Dialog>
  )
}
