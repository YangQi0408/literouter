import { Pencil, Plus, Trash2 } from 'lucide-react'
import { useState } from 'react'

import { ProviderDialog, blankProvider } from '@/components/ProviderDialog'
import { Button } from '@/components/ui/button'
import { Card, CardContent, CardHeader, CardTitle } from '@/components/ui/card'
import { Switch } from '@/components/ui/switch'
import type { ProviderConfig } from '@/lib/api'
import { useI18n } from '@/lib/i18n'
import { keySummary } from '@/lib/present'
import { useStore } from '@/store'

interface Editing {
  index: number
  draft: ProviderConfig
  isNew: boolean
}

export function Providers() {
  const { working, update } = useStore()
  const { t } = useI18n()
  const [editing, setEditing] = useState<Editing | null>(null)

  if (!working) return null
  const providers = working.providers

  return (
    <Card>
      <CardHeader>
        <CardTitle>{t('tabProviders')}</CardTitle>
        <span className="ml-1 text-xs text-muted-foreground">{t('providerHint')}</span>
        <Button
          size="sm"
          className="ml-auto"
          onClick={() => setEditing({ index: providers.length, draft: blankProvider(), isNew: true })}
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
                <th>{t('colProvider')}</th>
                <th>protocol</th>
                <th>{t('colBaseUrl')}</th>
                <th>{t('providerGroups')}</th>
                <th className="text-right">priority</th>
                <th>{t('colKey')}</th>
                <th>{t('colEnabled')}</th>
                <th />
              </tr>
            </thead>
            <tbody>
              {providers.length === 0 ? (
                <tr>
                  <td colSpan={8} className="py-10 text-center text-muted-foreground">
                    {t('empty')}
                  </td>
                </tr>
              ) : (
                providers.map((provider, index) => {
                  const key = keySummary(provider, t)
                  return (
                    <tr key={`${provider.id}-${index}`} className="transition-colors hover:bg-muted/40">
                      <td>
                        <div className="font-mono text-xs">{provider.id || '—'}</div>
                        {provider.name ? (
                          <div className="text-[11px] text-muted-foreground">{provider.name}</div>
                        ) : null}
                      </td>
                      <td>
                        <span className="font-mono text-xs text-muted-foreground">
                          {provider.protocol}
                        </span>
                      </td>
                      <td className="max-w-[20rem] truncate font-mono text-xs">{provider.base_url}</td>
                      <td className="max-w-48 text-xs break-words text-muted-foreground">{provider.groups.join(', ') || '—'}</td>
                      <td className="text-right font-mono text-xs tnum">{provider.priority}</td>
                      <td className="font-mono text-xs text-muted-foreground">{key.text}</td>
                      <td>
                        <Switch
                          checked={provider.enabled}
                          onCheckedChange={(enabled) =>
                            update((draft) => {
                              const target = draft.providers[index]
                              if (target) target.enabled = enabled
                            })
                          }
                        />
                      </td>
                      <td className="text-right whitespace-nowrap">
                        <Button
                          size="icon"
                          variant="ghost"
                          title={t('edit')}
                          onClick={() => setEditing({ index, draft: provider, isNew: false })}
                        >
                          <Pencil className="size-3.5" />
                        </Button>
                        <Button
                          size="icon"
                          variant="ghost"
                          title={t('remove')}
                          onClick={() => {
                            if (!window.confirm(t('confirmDeleteProvider', { id: provider.id }))) return
                            update((draft) => {
                              draft.providers.splice(index, 1)
                            })
                          }}
                        >
                          <Trash2 className="size-3.5 text-destructive" />
                        </Button>
                      </td>
                    </tr>
                  )
                })
              )}
            </tbody>
          </table>
        </div>
      </CardContent>

      {editing ? (
        <ProviderDialog
          key={`${editing.index}-${editing.isNew}`}
          open
          draft={editing.draft}
          isNew={editing.isNew}
          onCancel={() => setEditing(null)}
          onApply={(provider) => {
            update((draft) => {
              if (editing.isNew) draft.providers.push(provider)
              else draft.providers[editing.index] = provider
            })
            setEditing(null)
          }}
        />
      ) : null}
    </Card>
  )
}
