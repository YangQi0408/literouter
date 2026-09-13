import { ArrowDown, ArrowUp, Plus, Trash2 } from 'lucide-react'
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
import { Input } from '@/components/ui/input'
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from '@/components/ui/select'
import type { RouteConfig, RouteTarget } from '@/lib/api'
import { useI18n } from '@/lib/i18n'
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

  const providers = working?.providers.map((provider) => provider.id) ?? []

  const patchTarget = (index: number, changes: Partial<RouteTarget>) =>
    setRoute((prev) => ({
      ...prev,
      targets: prev.targets.map((target, at) => (at === index ? { ...target, ...changes } : target)),
    }))

  const move = (index: number, delta: number) =>
    setRoute((prev) => {
      const next = [...prev.targets]
      const target = next[index]
      const destination = index + delta
      if (!target || destination < 0 || destination >= next.length) return prev
      next.splice(index, 1)
      next.splice(destination, 0, target)
      return { ...prev, targets: next }
    })

  return (
    <Dialog open={open} onOpenChange={(next) => (next ? undefined : onCancel())}>
      <DialogContent className="sm:max-w-2xl">
        <DialogHeader>
          <DialogTitle>{isNew ? t('newRoute') : t('editRoute')}</DialogTitle>
          <DialogDescription>{t('routeHint')}</DialogDescription>
        </DialogHeader>

        <div className="grid max-h-[62vh] gap-4 overflow-auto px-4">
          <div className="grid gap-4 sm:grid-cols-2">
            <Field label="model">
              <TextField
                value={route.model}
                onChange={(model) => setRoute((prev) => ({ ...prev, model }))}
              />
            </Field>
            <Field label="enabled">
              <SwitchField
                checked={route.enabled}
                onChange={(enabled) => setRoute((prev) => ({ ...prev, enabled }))}
                label="enabled"
              />
            </Field>
          </div>

          <Field label="targets" hint={t('routeHint')}>
            <div className="flex flex-col gap-2">
              {route.targets.map((target, index) => (
                <div key={index} className="flex items-center gap-2">
                  <span className="w-4 shrink-0 text-right font-mono text-[11px] text-muted-foreground">
                    {index + 1}
                  </span>
                  <Select
                    value={target.provider || undefined}
                    onValueChange={(provider) => patchTarget(index, { provider })}
                  >
                    <SelectTrigger className="h-9 flex-1 font-mono text-xs">
                      <SelectValue placeholder={t('targetProvider')} />
                    </SelectTrigger>
                    <SelectContent>
                      {providers.map((id) => (
                        <SelectItem key={id} value={id} className="font-mono text-xs">
                          {id}
                        </SelectItem>
                      ))}
                      {target.provider && !providers.includes(target.provider) ? (
                        <SelectItem value={target.provider} className="font-mono text-xs">
                          {target.provider} (missing)
                        </SelectItem>
                      ) : null}
                    </SelectContent>
                  </Select>
                  <Input
                    value={target.model ?? ''}
                    placeholder={t('targetModel')}
                    spellCheck={false}
                    className="h-9 flex-1 font-mono text-xs"
                    onChange={(event) => patchTarget(index, { model: event.target.value })}
                  />
                  <Button
                    type="button"
                    size="icon"
                    variant="ghost"
                    title={t('moveUp')}
                    disabled={index === 0}
                    onClick={() => move(index, -1)}
                  >
                    <ArrowUp className="size-3.5" />
                  </Button>
                  <Button
                    type="button"
                    size="icon"
                    variant="ghost"
                    title={t('moveDown')}
                    disabled={index === route.targets.length - 1}
                    onClick={() => move(index, 1)}
                  >
                    <ArrowDown className="size-3.5" />
                  </Button>
                  <Button
                    type="button"
                    size="icon"
                    variant="ghost"
                    title={t('remove')}
                    disabled={route.targets.length === 1}
                    onClick={() =>
                      setRoute((prev) => ({
                        ...prev,
                        targets: prev.targets.filter((_, at) => at !== index),
                      }))
                    }
                  >
                    <Trash2 className="size-3.5" />
                  </Button>
                </div>
              ))}
              <Button
                type="button"
                variant="ghost"
                size="sm"
                className="self-start"
                onClick={() =>
                  setRoute((prev) => ({ ...prev, targets: [...prev.targets, { provider: '', model: '' }] }))
                }
              >
                <Plus className="size-3.5" />
                {t('addTarget')}
              </Button>
            </div>
          </Field>
        </div>

        <DialogFooter>
          <Button variant="ghost" onClick={onCancel}>
            {t('cancel')}
          </Button>
          <Button
            disabled={!route.model.trim() || route.targets.some((target) => !target.provider)}
            onClick={() =>
              onApply({
                ...route,
                targets: route.targets.map((target) => ({
                  provider: target.provider,
                  ...(target.model?.trim() ? { model: target.model.trim() } : {}),
                })),
              })
            }
          >
            {t('apply')}
          </Button>
        </DialogFooter>
      </DialogContent>
    </Dialog>
  )
}
