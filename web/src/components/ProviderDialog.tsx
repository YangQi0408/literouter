import { Braces, Eye, EyeOff, Layers, Network, Plus, Settings2, ShieldCheck, Trash2 } from 'lucide-react'
import { useEffect, useId, useState } from 'react'

import { Field, HeadersField, LinesField, NumberField, SwitchField, TextField } from '@/components/fields'
import { Button } from '@/components/ui/button'
import { Input } from '@/components/ui/input'
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
import type { ProviderConfig, Protocol } from '@/lib/api'
import { PROVIDER_DEFAULTS } from '@/lib/normalize'
import { useI18n } from '@/lib/i18n'
import { cn } from '@/lib/utils'

const PROTOCOLS: Protocol[] = [
  'openai',
  'anthropic',
  'gemini',
  'openai_responses',
  'azure',
  'vertex',
  'bedrock',
  'ollama',
]

/** Which extra fields a protocol actually needs. Showing an AWS secret key on a
 *  plain OpenAI relay is how an operator comes to believe it does something. */
const needsApiVersion = (protocol: Protocol) => protocol === 'azure' || protocol === 'vertex'
const needsRegion = (protocol: Protocol) => protocol === 'bedrock' || protocol === 'vertex'
const needsVertex = (protocol: Protocol) => protocol === 'vertex'
const needsAws = (protocol: Protocol) => protocol === 'bedrock'

/** A relay as the server would default it: `PROVIDER_DEFAULTS` is the one place
 *  the defaults live, so a new relay cannot start out different from a relay the
 *  file merely omitted fields for. */
export const blankProvider = (): ProviderConfig => ({ ...PROVIDER_DEFAULTS })

const sections = [
  { id: 'connection', label: 'managementConnection', icon: Network },
  { id: 'models', label: 'managementModelsPricing', icon: Layers },
  { id: 'traffic', label: 'managementTraffic', icon: Settings2 },
  { id: 'advanced', label: 'managementAdvanced', icon: Braces },
] as const

type Section = typeof sections[number]['id']

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
  const sectionId = useId()
  const [provider, setProvider] = useState<ProviderConfig>(draft)
  const [section, setSection] = useState<Section>('connection')
  const [showKey, setShowKey] = useState(false)

  useEffect(() => {
    if (open) {
      setProvider(draft)
      setSection('connection')
      setShowKey(false)
    }
  }, [open, draft])

  const patch = (changes: Partial<ProviderConfig>) => setProvider((prev) => ({ ...prev, ...changes }))
  const keyStored = provider.api_key_source === 'literal' && !provider.api_key_clear
  const keyPlaceholder = keyStored ? t('keyStored') : 'sk-…  /  ${OPENAI_API_KEY}'
  const panelProps = (id: Section) => ({
    id: `${sectionId}-${id}-panel`,
    role: 'tabpanel',
    'aria-labelledby': `${sectionId}-${id}-tab`,
    hidden: section !== id,
    className: 'space-y-6',
  })

  return (
    <Dialog open={open} onOpenChange={(next) => (next ? undefined : onCancel())}>
      <DialogContent className="flex max-h-[calc(100dvh-2rem)] flex-col gap-0 overflow-hidden p-0 sm:max-w-3xl">
        <DialogHeader className="border-b px-5 py-5 text-left sm:px-6">
          <div className="flex items-center gap-3 pr-6">
            <div className="flex size-11 shrink-0 items-center justify-center rounded-xl bg-primary/10 text-primary"><Network className="size-5" /></div>
            <div className="min-w-0">
              <DialogTitle>{isNew ? t('newProvider') : t('editProvider')}</DialogTitle>
              <DialogDescription className="mt-1.5 text-xs leading-relaxed">{t('managementProviderDialogHint')}</DialogDescription>
            </div>
          </div>
        </DialogHeader>

        <div className="grid grid-cols-4 border-b bg-muted/30 px-3 sm:px-5" role="tablist" aria-label={t('managementProviderSections')}>
          {sections.map(({ id, label, icon: Icon }, index) => (
            <button key={id} type="button" role="tab" id={`${sectionId}-${id}-tab`} aria-controls={`${sectionId}-${id}-panel`} aria-selected={section === id} tabIndex={section === id ? 0 : -1}
              className={cn('flex min-h-12 items-center justify-center gap-2 border-b-2 px-1 py-3 text-xs font-medium transition-colors focus-visible:outline-2 focus-visible:outline-ring', section === id ? 'border-primary text-primary' : 'border-transparent text-muted-foreground hover:text-foreground')}
              onClick={() => setSection(id)}
              onKeyDown={(event) => {
                let destination = index
                if (event.key === 'ArrowRight') destination = (index + 1) % sections.length
                else if (event.key === 'ArrowLeft') destination = (index + sections.length - 1) % sections.length
                else if (event.key === 'Home') destination = 0
                else if (event.key === 'End') destination = sections.length - 1
                else return
                event.preventDefault()
                const next = sections[destination]!
                setSection(next.id)
                document.getElementById(`${sectionId}-${next.id}-tab`)?.focus()
              }}>
              <Icon className="hidden size-3.5 sm:block" />{t(label)}
            </button>
          ))}
        </div>

        <div className="max-h-[62dvh] min-h-0 overflow-y-auto overscroll-contain px-5 py-6 sm:px-6">
          <div {...panelProps('connection')}>
            <div className="grid gap-5 sm:grid-cols-2">
              <Field label={t('managementProviderId')} hint={t('hintId')}>
                <TextField value={provider.id} placeholder="my-relay" onChange={(id) => patch({ id })} />
              </Field>
              <Field label={t('managementDisplayName')}>
                <TextField value={provider.name} mono={false} onChange={(name) => patch({ name })} />
              </Field>
              <Field label={t('colBaseUrl')} hint={t('hintBaseUrl')} wide>
                <TextField value={provider.base_url} placeholder="https://api.example.com/v1" onChange={(base_url) => patch({ base_url })} />
              </Field>
              <Field label={t('colKey')} hint={t('hintApiKey')} wide>
                <div className="flex items-center gap-2">
                  <div className="min-w-0 flex-1"><TextField type={showKey ? 'text' : 'password'} value={provider.api_key} placeholder={keyPlaceholder} onChange={(api_key) => patch({ api_key, api_key_clear: false })} /></div>
                  <Button type="button" variant="outline" size="icon" aria-label={t('showKey')} title={t('showKey')} onClick={() => setShowKey((prev) => !prev)}>{showKey ? <EyeOff className="size-4" /> : <Eye className="size-4" />}</Button>
                </div>
                {keyStored ? <Button type="button" variant="ghost" size="sm" className="h-auto self-start px-0 text-xs text-muted-foreground hover:bg-transparent hover:text-destructive" onClick={() => patch({ api_key: '', api_key_clear: true })}>{t('keyClearAction')}</Button> : null}
                {provider.api_key_clear ? <p className="text-xs text-warn">{t('keyWillClear')}</p> : null}
              </Field>
              <Field label={t('managementProtocol')}>
                <Select value={provider.protocol} onValueChange={(value) => patch({ protocol: value as Protocol })}>
                  <SelectTrigger aria-label={t('managementProtocol')} className="h-10 w-full font-mono text-sm"><SelectValue /></SelectTrigger>
                  <SelectContent>{PROTOCOLS.map((protocol) => <SelectItem key={protocol} value={protocol} className="font-mono text-sm">{protocol}</SelectItem>)}</SelectContent>
                </Select>
              </Field>
              <Field label={t('managementAvailability')}>
                <SwitchField checked={provider.enabled} onChange={(enabled) => patch({ enabled })} label={t('managementProviderEnabled')} />
              </Field>
              {needsApiVersion(provider.protocol) ? <Field label={t('managementApiVersion')} hint={t('hintApiVersion')}><TextField value={provider.api_version} placeholder={provider.protocol === 'azure' ? '2024-10-21' : 'v1'} onChange={(api_version) => patch({ api_version })} /></Field> : null}
              {needsRegion(provider.protocol) ? <Field label={t('managementRegion')} hint={t('hintRegion')}><TextField value={provider.region} placeholder={provider.protocol === 'bedrock' ? 'us-east-1' : 'us-central1'} onChange={(region) => patch({ region })} /></Field> : null}
              {needsVertex(provider.protocol) ? <>
                <Field label={t('managementProject')} hint={t('hintVertexProject')}><TextField value={provider.project} onChange={(project) => patch({ project })} /></Field>
                <Field label={t('managementCredentialsFile')} hint={t('hintCredentialsFile')} wide><TextField value={provider.credentials_file} placeholder="/etc/literouter/service-account.json" onChange={(credentials_file) => patch({ credentials_file })} /></Field>
              </> : null}
              {needsAws(provider.protocol) ? <>
                <Field label={t('managementAwsAccessKey')} hint={t('hintAwsKey')}><TextField value={provider.aws_access_key} placeholder="${AWS_ACCESS_KEY_ID}" onChange={(aws_access_key) => patch({ aws_access_key })} /></Field>
                <Field label={t('managementAwsSecret')} hint={t('hintAwsSecret')}><TextField type="password" value={provider.aws_secret_key} placeholder="${AWS_SECRET_ACCESS_KEY}" onChange={(aws_secret_key) => patch({ aws_secret_key })} /></Field>
                <Field label={t('managementAwsSession')} hint={t('hintAwsSession')} wide><TextField type="password" value={provider.aws_session_token} onChange={(aws_session_token) => patch({ aws_session_token })} /></Field>
              </> : null}
            </div>
          </div>

          <div {...panelProps('models')}>
            <div className="grid gap-5 sm:grid-cols-2">
              <Field label={t('models')} hint={t('hintModels')} wide><LinesField value={provider.models ?? []} rows={6} placeholder={'gpt-4o\ntext-embedding-3-small'} onChange={(models) => patch({ models })} /></Field>
              <Field label={t('managementStreaming')} wide><SwitchField checked={provider.supports_stream} onChange={(supports_stream) => patch({ supports_stream })} label={t('managementSupportsStreaming')} /></Field>
            </div>
            <div className="border-t pt-5">
              <h3 className="text-sm font-semibold">{t('managementDefaultPricing')}</h3>
              <p className="mt-1 text-xs leading-relaxed text-muted-foreground">{t('managementDefaultPricingHint')}</p>
              <div className="mt-4 grid gap-5 sm:grid-cols-2">
                <Field label={t('managementInputPrice')}><NumberField value={provider.price_in_per_million} step={0.1} min={0} onChange={(price_in_per_million) => patch({ price_in_per_million })} /></Field>
                <Field label={t('managementOutputPrice')}><NumberField value={provider.price_out_per_million} step={0.1} min={0} onChange={(price_out_per_million) => patch({ price_out_per_million })} /></Field>
              </div>
            </div>

            <div className="border-t pt-5">
              <div className="flex flex-wrap items-center justify-between gap-2">
                <div>
                  <h3 className="text-sm font-semibold">{t('managementModelPricing')}</h3>
                  <p className="mt-1 text-xs leading-relaxed text-muted-foreground">{t('managementModelPricingHint')}</p>
                </div>
                <Button
                  type="button"
                  variant="outline"
                  size="sm"
                  onClick={() => {
                    const existing = Object.keys(provider.model_prices ?? {})
                    const candidate = (provider.models ?? []).find((m) => !existing.includes(m)) ?? ''
                    const nextKey = candidate || `model-${existing.length + 1}`
                    patch({
                      model_prices: {
                        ...(provider.model_prices ?? {}),
                        [nextKey]: { price_in_per_million: 0, price_out_per_million: 0 },
                      },
                    })
                  }}
                >
                  <Plus className="size-3.5" />
                  {t('managementAddModelPrice')}
                </Button>
              </div>

              {Object.keys(provider.model_prices ?? {}).length === 0 ? (
                <p className="mt-4 rounded-xl border border-dashed p-4 text-center text-xs text-muted-foreground">
                  {t('managementNoModelPrices')}
                </p>
              ) : (
                <div className="mt-4 space-y-3">
                  {Object.entries(provider.model_prices ?? {}).map(([modelName, pricing]) => (
                    <div key={modelName} className="flex flex-col gap-2 rounded-xl border bg-muted/20 p-3 sm:flex-row sm:items-center sm:gap-3">
                      <div className="min-w-0 flex-1">
                        <label className="mb-1 block text-[11px] font-medium text-muted-foreground">{t('managementModelName')}</label>
                        <Input
                          value={modelName}
                          placeholder="gpt-4o"
                          className="h-9 font-mono text-xs"
                          onChange={(e) => {
                            const newName = e.target.value
                            const nextPrices = { ...(provider.model_prices ?? {}) }
                            delete nextPrices[modelName]
                            nextPrices[newName] = pricing
                            patch({ model_prices: nextPrices })
                          }}
                        />
                      </div>
                      <div className="w-full sm:w-32">
                        <label className="mb-1 block text-[11px] font-medium text-muted-foreground">{t('managementInputPrice')}</label>
                        <Input
                          type="number"
                          step="0.1"
                          min="0"
                          value={pricing.price_in_per_million}
                          className="h-9 font-mono text-xs"
                          onChange={(e) => {
                            const val = Number(e.target.value) || 0
                            patch({
                              model_prices: {
                                ...(provider.model_prices ?? {}),
                                [modelName]: { ...pricing, price_in_per_million: val },
                              },
                            })
                          }}
                        />
                      </div>
                      <div className="w-full sm:w-32">
                        <label className="mb-1 block text-[11px] font-medium text-muted-foreground">{t('managementOutputPrice')}</label>
                        <Input
                          type="number"
                          step="0.1"
                          min="0"
                          value={pricing.price_out_per_million}
                          className="h-9 font-mono text-xs"
                          onChange={(e) => {
                            const val = Number(e.target.value) || 0
                            patch({
                              model_prices: {
                                ...(provider.model_prices ?? {}),
                                [modelName]: { ...pricing, price_out_per_million: val },
                              },
                            })
                          }}
                        />
                      </div>
                      <div className="flex sm:mb-0.5 sm:self-end">
                        <Button
                          type="button"
                          variant="ghost"
                          size="icon-sm"
                          className="text-muted-foreground hover:text-destructive"
                          aria-label={t('managementRemoveModelPrice')}
                          title={t('managementRemoveModelPrice')}
                          onClick={() => {
                            const nextPrices = { ...(provider.model_prices ?? {}) }
                            delete nextPrices[modelName]
                            patch({ model_prices: nextPrices })
                          }}
                        >
                          <Trash2 className="size-4" />
                        </Button>
                      </div>
                    </div>
                  ))}
                </div>
              )}
            </div>
          </div>

          <div {...panelProps('traffic')}>
            <div className="grid gap-5 sm:grid-cols-2">
              <Field label={t('managementPriority')} hint={t('providerHint')}><NumberField value={provider.priority} onChange={(priority) => patch({ priority })} /></Field>
              <Field label={t('managementWeight')}><NumberField value={provider.weight} onChange={(weight) => patch({ weight })} /></Field>
              <Field label={t('managementRequestTimeout')}><NumberField value={provider.timeout_sec} min={1} onChange={(timeout_sec) => patch({ timeout_sec })} /></Field>
              <Field label={t('managementConnectTimeout')}><NumberField value={provider.connect_timeout_sec} min={1} onChange={(connect_timeout_sec) => patch({ connect_timeout_sec })} /></Field>
              <Field label={t('managementConcurrency')} hint={t('hintRelayConcurrent')}><NumberField value={provider.max_concurrent} min={0} onChange={(max_concurrent) => patch({ max_concurrent })} /></Field>
              <Field label={t('managementRequestsMinute')} hint={t('hintRelayRpm')}><NumberField value={provider.requests_per_minute} min={0} onChange={(requests_per_minute) => patch({ requests_per_minute })} /></Field>
            </div>
          </div>

          <div {...panelProps('advanced')}>
            <div className="grid gap-5 sm:grid-cols-2">
              <Field label={t('managementHeaders')} hint={t('hintHeaders')} wide><HeadersField value={provider.headers ?? {}} onChange={(headers) => patch({ headers })} /></Field>
              <Field label={t('managementChatPath')}><TextField value={provider.chat_path} onChange={(chat_path) => patch({ chat_path })} /></Field>
              <Field label={t('managementEmbeddingsPath')}><TextField value={provider.embeddings_path} onChange={(embeddings_path) => patch({ embeddings_path })} /></Field>
              <Field label={t('managementNote')} wide><TextField value={provider.note} mono={false} onChange={(note) => patch({ note })} /></Field>
            </div>
          </div>
        </div>

        <DialogFooter className="border-t bg-muted/20 p-4 sm:items-center sm:px-6">
          <p className="mr-auto hidden items-center gap-1.5 text-xs text-muted-foreground sm:flex"><ShieldCheck className="size-3.5" />{t('managementDraftHint')}</p>
          <Button variant="outline" onClick={onCancel}>{t('cancel')}</Button>
          <Button disabled={!provider.id.trim() || !provider.base_url.trim()} onClick={() => onApply(provider)}>{t('apply')}</Button>
        </DialogFooter>
      </DialogContent>
    </Dialog>
  )
}
