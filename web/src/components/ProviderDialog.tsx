import { useEffect, useState } from 'react'

import { Field, HeadersField, LinesField, NumberField, SwitchField, TextField } from '@/components/fields'
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
import type { ProviderConfig, Protocol } from '@/lib/api'
import { useI18n } from '@/lib/i18n'

const PROTOCOLS: Protocol[] = ['openai', 'anthropic', 'gemini', 'openai_responses']

export const blankProvider = (): ProviderConfig => ({
  id: '',
  name: '',
  base_url: '',
  api_key: '',
  enabled: true,
  priority: 100,
  weight: 1,
  timeout_sec: 120,
  connect_timeout_sec: 5,
  supports_stream: true,
  models: [],
  headers: {},
  chat_path: '/chat/completions',
  embeddings_path: '/embeddings',
  protocol: 'openai',
  price_in_per_million: 0,
  price_out_per_million: 0,
  note: '',
})

export function ProviderDialog({
  open,
  draft,
  isNew,
  onCancel,
  onApply,
}: {
  open: boolean
  draft: ProviderConfig
  isNew: boolean
  onCancel: () => void
  onApply: (provider: ProviderConfig) => void
}) {
  const { t } = useI18n()
  const [provider, setProvider] = useState<ProviderConfig>(draft)

  useEffect(() => {
    if (open) setProvider(draft)
  }, [open, draft])

  const patch = (changes: Partial<ProviderConfig>) => setProvider((prev) => ({ ...prev, ...changes }))
  const keyStored = provider.api_key_source === 'literal' && !provider.api_key_clear
  const keyPlaceholder = keyStored ? t('keyStored') : 'sk-…  /  ${OPENAI_API_KEY}'

  return (
    <Dialog open={open} onOpenChange={(next) => (next ? undefined : onCancel())}>
      <DialogContent className="sm:max-w-3xl">
        <DialogHeader>
          <DialogTitle>{isNew ? t('newProvider') : t('editProvider')}</DialogTitle>
          <DialogDescription className="font-mono text-xs">{provider.id || '—'}</DialogDescription>
        </DialogHeader>

        <div className="grid max-h-[62vh] gap-4 overflow-auto px-4 sm:grid-cols-2">
          <Field label="id" hint={t('hintId')}>
            <TextField value={provider.id} onChange={(id) => patch({ id })} />
          </Field>
          <Field label="name">
            <TextField value={provider.name} mono={false} onChange={(name) => patch({ name })} />
          </Field>

          <Field label="base_url" hint={t('hintBaseUrl')} wide>
            <TextField value={provider.base_url} onChange={(base_url) => patch({ base_url })} />
          </Field>

          <Field label="api_key" hint={t('hintApiKey')} wide>
            <div className="flex items-center gap-2">
              <Input
                value={provider.api_key}
                placeholder={keyPlaceholder}
                spellCheck={false}
                className="h-9 font-mono text-xs"
                onChange={(event) =>
                  patch({ api_key: event.target.value, api_key_clear: false })
                }
              />
              {keyStored ? (
                <Button
                  type="button"
                  variant="ghost"
                  size="sm"
                  onClick={() => patch({ api_key: '', api_key_clear: true })}
                >
                  {t('keyClearAction')}
                </Button>
              ) : null}
            </div>
          </Field>

          <Field label="protocol">
            <Select
              value={provider.protocol}
              onValueChange={(value) => patch({ protocol: value as Protocol })}
            >
              <SelectTrigger className="h-9 w-full font-mono text-xs">
                <SelectValue />
              </SelectTrigger>
              <SelectContent>
                {PROTOCOLS.map((protocol) => (
                  <SelectItem key={protocol} value={protocol} className="font-mono text-xs">
                    {protocol}
                  </SelectItem>
                ))}
              </SelectContent>
            </Select>
          </Field>
          <Field label="priority">
            <NumberField value={provider.priority} onChange={(priority) => patch({ priority })} />
          </Field>

          <Field label="enabled">
            <SwitchField
              checked={provider.enabled}
              onChange={(enabled) => patch({ enabled })}
              label="enabled"
            />
          </Field>
          <Field label="supports_stream">
            <SwitchField
              checked={provider.supports_stream}
              onChange={(supports_stream) => patch({ supports_stream })}
              label="supports_stream"
            />
          </Field>

          <Field label="weight">
            <NumberField value={provider.weight} onChange={(weight) => patch({ weight })} />
          </Field>
          <Field label="timeout_sec">
            <NumberField
              value={provider.timeout_sec}
              min={1}
              onChange={(timeout_sec) => patch({ timeout_sec })}
            />
          </Field>

          <Field label="connect_timeout_sec">
            <NumberField
              value={provider.connect_timeout_sec}
              min={1}
              onChange={(connect_timeout_sec) => patch({ connect_timeout_sec })}
            />
          </Field>
          <div className="hidden sm:block" />

          <Field label="models" hint={t('hintModels')} wide>
            <LinesField
              value={provider.models}
              placeholder={'gpt-4o\ntext-embedding-3-small'}
              onChange={(models) => patch({ models })}
            />
          </Field>

          <Field label="headers" hint={t('hintHeaders')} wide>
            <HeadersField value={provider.headers} onChange={(headers) => patch({ headers })} />
          </Field>

          <Field label="chat_path">
            <TextField value={provider.chat_path} onChange={(chat_path) => patch({ chat_path })} />
          </Field>
          <Field label="embeddings_path">
            <TextField
              value={provider.embeddings_path}
              onChange={(embeddings_path) => patch({ embeddings_path })}
            />
          </Field>

          <Field label="price_in_per_million" hint={t('hintPrice')}>
            <NumberField
              value={provider.price_in_per_million}
              step={0.1}
              min={0}
              onChange={(price_in_per_million) => patch({ price_in_per_million })}
            />
          </Field>
          <Field label="price_out_per_million" hint={t('hintPrice')}>
            <NumberField
              value={provider.price_out_per_million}
              step={0.1}
              min={0}
              onChange={(price_out_per_million) => patch({ price_out_per_million })}
            />
          </Field>
          <Field label="note" wide>
            <TextField value={provider.note} mono={false} onChange={(note) => patch({ note })} />
          </Field>
        </div>

        <DialogFooter>
          <Button variant="ghost" onClick={onCancel}>
            {t('cancel')}
          </Button>
          <Button
            disabled={!provider.id.trim() || !provider.base_url.trim()}
            onClick={() => onApply(provider)}
          >
            {t('apply')}
          </Button>
        </DialogFooter>
      </DialogContent>
    </Dialog>
  )
}
