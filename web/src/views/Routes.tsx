import { Pencil, Plus, Trash2 } from 'lucide-react'
import { useState } from 'react'

import { RouteDialog, blankRoute } from '@/components/RouteDialog'
import { Button } from '@/components/ui/button'
import { Card, CardContent, CardHeader, CardTitle } from '@/components/ui/card'
import { Switch } from '@/components/ui/switch'
import type { RouteConfig } from '@/lib/api'
import { useI18n } from '@/lib/i18n'
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

  if (!working) return null
  const routes = working.routes
  const known = new Set(working.providers.map((provider) => provider.id))

  return (
    <Card>
      <CardHeader>
        <CardTitle>{t('tabRoutes')}</CardTitle>
        <span className="ml-1 text-xs text-muted-foreground">{t('routeHint')}</span>
        <Button
          size="sm"
          className="ml-auto"
          onClick={() => setEditing({ index: routes.length, draft: blankRoute(), isNew: true })}
        >
          <Plus className="size-3.5" />
          {t('add')}
        </Button>
      </CardHeader>
      <CardContent className="p-0">
        <div className="overflow-auto">
          <table>
            <thead>
              <tr>
                <th>model</th>
                <th>{t('colEnabled')}</th>
                <th>{t('colTargets')}</th>
                <th />
              </tr>
            </thead>
            <tbody>
              {routes.length === 0 ? (
                <tr>
                  <td colSpan={4} className="py-10 text-center text-muted-foreground">
                    {t('empty')}
                  </td>
                </tr>
              ) : (
                routes.map((route, index) => (
                  <tr key={`${route.model}-${index}`} className="transition-colors hover:bg-muted/40">
                    <td className="font-mono text-xs">{route.model || '—'}</td>
                    <td>
                      <Switch
                        checked={route.enabled}
                        onCheckedChange={(enabled) =>
                          update((draft) => {
                            const target = draft.routes[index]
                            if (target) target.enabled = enabled
                          })
                        }
                      />
                    </td>
                    <td>
                      <div className="flex flex-wrap items-center gap-1.5">
                        {route.targets.map((target, at) => (
                          <span key={at} className="flex items-center gap-1.5">
                            {at > 0 ? <span className="text-muted-foreground">→</span> : null}
                            <span
                              className={
                                known.has(target.provider)
                                  ? 'font-mono text-xs'
                                  : 'font-mono text-xs text-destructive'
                              }
                              title={target.model ? `${target.provider} · ${target.model}` : target.provider}
                            >
                              {target.provider || '—'}
                            </span>
                            {target.model ? (
                              <span className="font-mono text-[11px] text-muted-foreground">
                                ({target.model})
                              </span>
                            ) : null}
                          </span>
                        ))}
                      </div>
                    </td>
                    <td className="text-right whitespace-nowrap">
                      <Button
                        size="icon"
                        variant="ghost"
                        title={t('edit')}
                        onClick={() => setEditing({ index, draft: route, isNew: false })}
                      >
                        <Pencil className="size-3.5" />
                      </Button>
                      <Button
                        size="icon"
                        variant="ghost"
                        title={t('remove')}
                        onClick={() => {
                          if (!window.confirm(t('confirmDeleteRoute', { model: route.model }))) return
                          update((draft) => {
                            draft.routes.splice(index, 1)
                          })
                        }}
                      >
                        <Trash2 className="size-3.5 text-destructive" />
                      </Button>
                    </td>
                  </tr>
                ))
              )}
            </tbody>
          </table>
        </div>
      </CardContent>

      {editing ? (
        <RouteDialog
          key={`${editing.index}-${editing.isNew}`}
          open
          draft={editing.draft}
          isNew={editing.isNew}
          onCancel={() => setEditing(null)}
          onApply={(route) => {
            update((draft) => {
              if (editing.isNew) draft.routes.push(route)
              else draft.routes[editing.index] = route
            })
            setEditing(null)
          }}
        />
      ) : null}
    </Card>
  )
}
