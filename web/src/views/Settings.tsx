import { Activity, ArrowDownToLine, ArrowUpFromLine, Check, CheckCheck, Copy, Database, FileJson2, Info, Network, ShieldCheck, SlidersHorizontal } from 'lucide-react'
import { useRef, useState, type ReactNode } from 'react'
import { toast } from 'sonner'

import { SecretInput } from '@/components/SecretInput'
import { Field, NumberField, SwitchField, TextField } from '@/components/fields'
import { Issues } from '@/components/Issues'
import { Button } from '@/components/ui/button'
import { Card } from '@/components/ui/card'
import { Select, SelectContent, SelectItem, SelectTrigger, SelectValue } from '@/components/ui/select'
import { useI18n } from '@/lib/i18n'
import { ConfigImportError, readConfigImport } from '@/lib/config-import'
import { clientBaseUrl } from '@/lib/present'
import { cn } from '@/lib/utils'
import { useStore } from '@/store'

const sections = [
  { id: 'connection', label: 'settingsConnection', note: 'settingsConnectionNote', icon: ShieldCheck },
  { id: 'routing', label: 'settingsRouting', note: 'settingsRoutingNote', icon: Network },
  { id: 'cache', label: 'settingsCache', note: 'settingsCacheNote', icon: Database },
  { id: 'telemetry', label: 'settingsTelemetry', note: 'settingsTelemetryNote', icon: Activity },
  { id: 'config', label: 'settingsConfig', note: 'settingsConfigNote', icon: FileJson2 },
] as const

export type SettingsSection = (typeof sections)[number]['id']

function FieldGroup({ title, children }: { title?: string; children: ReactNode }) {
  return (
    <section className="space-y-5 border-b border-border/70 px-5 py-6 last:border-0 sm:px-7">
      {title ? <h3 className="text-sm font-semibold tracking-tight">{title}</h3> : null}
      <div className="grid gap-x-6 gap-y-5 sm:grid-cols-2">{children}</div>
    </section>
  )
}

export function Settings({ initialSection = 'connection' }: { initialSection?: SettingsSection }) {
  const { working, update, loaded, snapshot, serverReport, saveIssues } = useStore()
  const { t } = useI18n()
  const [active, setActive] = useState<SettingsSection>(initialSection)
  const [copied, setCopied] = useState(false)
  const [importError, setImportError] = useState('')
  const fileInput = useRef<HTMLInputElement>(null)

  if (!working) return null
  const server = working.server
  const patch = (changes: Partial<typeof server>) =>
    update((draft) => {
      draft.server = { ...draft.server, ...changes }
    })
  const issues = saveIssues.length ? saveIssues : (serverReport?.issues ?? [])
  const json = JSON.stringify(working, null, 2)
  const current = sections.find((section) => section.id === active)!
  const enabled = (value: boolean) => t(value ? 'settingOn' : 'settingOff')

  async function copy() {
    try {
      await navigator.clipboard.writeText(json)
      setCopied(true)
      window.setTimeout(() => setCopied(false), 2000)
    } catch {
      // Clipboard access needs a secure context; the preview remains selectable.
      toast.error(t('configCopyFailed'))
    }
  }

  /** The whole config as a file. Secrets are written exactly as they are held —
   *  a `${VAR}` reference stays a reference and a literal stays blanked by the
   *  server — so the export is safe to keep and to diff. */
  function download() {
    const blob = new Blob([json], { type: 'application/json' })
    const url = URL.createObjectURL(blob)
    const link = document.createElement('a')
    link.href = url
    link.download = 'literouter-config.json'
    link.click()
    URL.revokeObjectURL(url)
  }

  /** Replaces the whole draft from a file. It is a draft, not a save: the same
   *  server-side validation that guards the form guards this, and nothing is
   *  written until Save. */
  async function importFile(file: File) {
    setImportError('')
    try {
      const text = await file.text()
      const parsed: unknown = JSON.parse(text)
      if (parsed === null || typeof parsed !== 'object' || Array.isArray(parsed)) {
        setImportError(t('importNotObject'))
        return
      }
      // Validate container and scalar types before replacing a usable draft;
      // omitted defaults from hand-written and older files are still restored.
      const incoming = readConfigImport(parsed)
      update((draft) => {
        draft.schema = incoming.schema
        draft.server = incoming.server
        draft.providers = incoming.providers
        draft.routes = incoming.routes
      })
    } catch (error) {
      setImportError(error instanceof ConfigImportError
        ? t(error.path ? 'importInvalidField' : 'importNotConfig', { path: error.path })
        : `${t('importFailed')}: ${error instanceof Error ? error.message : String(error)}`)
    }
  }

  return (
    <div className="space-y-7">
      <div>
        <h1 className="text-2xl font-semibold tracking-tight sm:text-3xl">{t('tabSettings')}</h1>
        <p className="mt-2 text-sm leading-relaxed text-muted-foreground">{t('settingsIntro')}</p>
      </div>
      {issues.length ? <Issues issues={issues} /> : null}
      <div className="grid min-w-0 gap-6 lg:grid-cols-[210px_minmax(0,1fr)]">
        <aside className="min-w-0">
          <div className="lg:sticky lg:top-24">
            <nav aria-label={t('settingsNavigation')} className="flex gap-1 overflow-x-auto rounded-2xl border bg-card p-1.5 lg:flex-col lg:border-0 lg:bg-transparent lg:p-0">
              {sections.map(({ id, label, icon: Icon }) => (
                <button
                  key={id}
                  type="button"
                  aria-current={active === id ? 'page' : undefined}
                  aria-controls={active === id ? `settings-panel-${id}` : undefined}
                  onClick={() => setActive(id)}
                  className={cn('flex shrink-0 items-center gap-2.5 rounded-xl px-3.5 py-3 text-left text-sm font-medium transition-colors focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-ring', active === id ? 'bg-primary/10 text-primary' : 'text-muted-foreground hover:bg-muted hover:text-foreground')}
                >
                  <Icon className="size-4 shrink-0" />
                  {t(label)}
                </button>
              ))}
            </nav>
            <p className="mt-5 hidden items-start gap-2 px-3 text-xs leading-relaxed text-muted-foreground lg:flex">
              <CheckCheck className="mt-0.5 size-3.5 shrink-0" />
              {t('settingsDraftNote')}
            </p>
          </div>
        </aside>

        <Card id={`settings-panel-${active}`} className="min-w-0 gap-0 overflow-hidden py-0">
          <div className="border-b border-border/70 px-5 py-6 sm:px-7">
            <div className="mb-4 flex size-10 items-center justify-center rounded-xl bg-primary/8 text-primary">
              <current.icon className="size-5" />
            </div>
            <h2 className="text-lg font-semibold tracking-tight">{t(current.label)}</h2>
            <p className="mt-1.5 text-sm leading-relaxed text-muted-foreground">{t(current.note)}</p>
          </div>

          {active === 'connection' ? <>
            <FieldGroup title={t('settingsListener')}>
              {snapshot ? <div className="rounded-xl bg-muted/50 px-3.5 py-3 sm:col-span-2">
                <p className="text-xs text-muted-foreground">{t('settingsCurrentEndpoint')}</p>
                <p className="mt-1 break-all font-mono text-sm">{clientBaseUrl(snapshot.base_url)}</p>
              </div> : null}
              <Field label={t('settingHost')}>
                <TextField value={server.host} onChange={(host) => patch({ host })} />
              </Field>
              <Field label={t('settingPort')}>
                <NumberField value={server.port} min={0} max={65535} onChange={(port) => patch({ port })} />
              </Field>
              <div className="flex items-start gap-2 rounded-xl bg-muted/60 px-3.5 py-3 text-xs leading-relaxed text-muted-foreground sm:col-span-2">
                <Info className="mt-0.5 size-3.5 shrink-0" />{t('settingsRestart')}
              </div>
            </FieldGroup>
            <FieldGroup title={t('settingsAccess')}>
              <Field label={t('administratorKey')} hint={t('adminKeyHint')} wide>
                <SecretInput label={t('administratorKey')} value={server} onChange={patch} generate clear />
              </Field>
              <Field label={t('settingTlsCert')} hint={t('hintTls')} wide>
                <TextField value={server.tls_cert_file} onChange={(tls_cert_file) => patch({ tls_cert_file })} />
              </Field>
              <Field label={t('settingTlsKey')} hint={t('hintTlsKey')} wide>
                <TextField value={server.tls_key_file} onChange={(tls_key_file) => patch({ tls_key_file })} />
              </Field>
            </FieldGroup>
            <FieldGroup title={t('settingsConsole')}>
              <Field label={t('settingWebUi')} hint={t('hintWebUi')}>
                <SwitchField checked={server.web_ui} onChange={(web_ui) => patch({ web_ui })} label={enabled(server.web_ui)} ariaLabel={t('settingWebUi')} />
              </Field>
              <Field label={t('settingReload')} hint={t('hintReload')}>
                <SwitchField checked={server.reload_on_change} onChange={(reload_on_change) => patch({ reload_on_change })} label={enabled(server.reload_on_change)} ariaLabel={t('settingReload')} />
              </Field>
            </FieldGroup>
          </> : null}

          {active === 'routing' ? <>
            <FieldGroup>
              <Field label={t('settingPolicy')} hint={t('hintPolicy')}>
                <Select value={server.routing_policy} onValueChange={(routing_policy) => patch({ routing_policy })}>
                  <SelectTrigger className="w-full" aria-label={t('settingPolicy')}><SelectValue /></SelectTrigger>
                  <SelectContent>
                    <SelectItem value="priority">{t('policyPriority')}</SelectItem>
                    <SelectItem value="fastest">{t('policyFastest')}</SelectItem>
                    <SelectItem value="cheapest">{t('policyCheapest')}</SelectItem>
                    <SelectItem value="round_robin">{t('policyRoundRobin')}</SelectItem>
                  </SelectContent>
                </Select>
              </Field>
              <Field label={t('settingAttempts')}>
                <NumberField value={server.max_attempts} min={0} onChange={(max_attempts) => patch({ max_attempts })} />
              </Field>
              <Field label={t('settingDeadline')} hint={t('hintDeadline')}>
                <NumberField value={server.request_deadline_sec} min={0} onChange={(request_deadline_sec) => patch({ request_deadline_sec })} />
              </Field>
              <Field label={t('settingAffinity')} hint={t('hintAffinity')}>
                <NumberField value={server.session_affinity_sec} min={0} onChange={(session_affinity_sec) => patch({ session_affinity_sec })} />
              </Field>
              <Field label={t('settingPassthrough')}>
                <SwitchField checked={server.pass_through_unknown} onChange={(pass_through_unknown) => patch({ pass_through_unknown })} label={enabled(server.pass_through_unknown)} ariaLabel={t('settingPassthrough')} />
              </Field>
            </FieldGroup>
            <FieldGroup title={t('settingsCircuit')}>
              <Field label={t('settingFailureThreshold')} hint={t('hintFailureThreshold')}>
                <NumberField value={server.circuit_failure_threshold} min={0} onChange={(circuit_failure_threshold) => patch({ circuit_failure_threshold })} />
              </Field>
              <Field label={t('settingCooldown')}>
                <NumberField value={server.circuit_cooldown_sec} min={1} onChange={(circuit_cooldown_sec) => patch({ circuit_cooldown_sec })} />
              </Field>
              <Field label={t('settingSkipCircuits')}>
                <SwitchField checked={server.skip_open_circuits} onChange={(skip_open_circuits) => patch({ skip_open_circuits })} label={enabled(server.skip_open_circuits)} ariaLabel={t('settingSkipCircuits')} />
              </Field>
            </FieldGroup>
          </> : null}

          {active === 'cache' ? <FieldGroup>
            <Field label={t('settingCacheTtl')} hint={t('hintCacheTtl')}>
              <NumberField value={server.response_cache_ttl_sec} min={0} onChange={(response_cache_ttl_sec) => patch({ response_cache_ttl_sec })} />
            </Field>
            <Field label={t('settingCacheEntries')} hint={t('hintCacheEntries')}>
              <NumberField value={server.response_cache_max_entries} min={1} onChange={(response_cache_max_entries) => patch({ response_cache_max_entries })} />
            </Field>
          </FieldGroup> : null}

          {active === 'telemetry' ? <>
            <FieldGroup title={t('settingsLogStorage')}>
              <Field label={t('settingLogCapacity')}>
                <NumberField value={server.log_capacity} min={16} onChange={(log_capacity) => patch({ log_capacity })} />
              </Field>
              <Field label={t('settingBodyLimit')} hint={t('hintBodyLimit')}>
                <NumberField value={server.log_body_limit} min={0} onChange={(log_body_limit) => patch({ log_body_limit })} />
              </Field>
              <Field label={t('settingLogBodies')} hint={t('hintLogBodies')}>
                <SwitchField checked={server.log_bodies} onChange={(log_bodies) => patch({ log_bodies })} label={enabled(server.log_bodies)} ariaLabel={t('settingLogBodies')} />
              </Field>
              <Field label={t('settingPersist')} hint={t('hintPersistTelemetry')}>
                <SwitchField checked={server.persist_telemetry} onChange={(persist_telemetry) => patch({ persist_telemetry })} label={enabled(server.persist_telemetry)} ariaLabel={t('settingPersist')} />
              </Field>
            </FieldGroup>
            <FieldGroup title={t('settingsTraffic')}>
              <Field label={t('settingBucketSec')} hint={t('hintBucketSec')}>
                <NumberField value={server.traffic_bucket_sec} min={60} max={86400} step={60} onChange={(traffic_bucket_sec) => patch({ traffic_bucket_sec })} />
              </Field>
              <Field label={t('settingBucketCount')} hint={t('hintBucketCount')}>
                <NumberField value={server.traffic_bucket_count} min={2} max={10000} onChange={(traffic_bucket_count) => patch({ traffic_bucket_count })} />
              </Field>
            </FieldGroup>
            <FieldGroup title={t('settingsMetrics')}>
              <Field label={t('settingOtlp')} hint={t('hintOtlp')} wide>
                <TextField value={server.otlp_endpoint} placeholder="http://127.0.0.1:4318" onChange={(otlp_endpoint) => patch({ otlp_endpoint })} />
              </Field>
            </FieldGroup>
          </> : null}

          {active === 'config' ? <div className="space-y-5 p-5 sm:p-7">
            <div className="flex items-start gap-3 rounded-xl bg-muted/50 px-4 py-3">
              <FileJson2 className="mt-0.5 size-4 shrink-0 text-muted-foreground" />
              <div className="min-w-0">
                <p className="text-xs text-muted-foreground">{t('configSource')}</p>
                <p className="mt-1 break-all font-mono text-xs">{loaded?.path || t('configInMemory')}</p>
              </div>
            </div>
            <div className="flex flex-wrap items-center justify-between gap-3">
              <div className="flex items-center gap-2 text-sm font-semibold"><SlidersHorizontal className="size-4 text-muted-foreground" />{t('configDraft')}</div>
              <div className="flex flex-wrap gap-2">
                <Button size="sm" variant="outline" onClick={() => void copy()}>{copied ? <Check className="size-3.5" /> : <Copy className="size-3.5" />}{copied ? t('copied') : t('copy')}</Button>
                <Button size="sm" variant="outline" onClick={download}><ArrowDownToLine className="size-3.5" />{t('exportConfig')}</Button>
                <Button size="sm" variant="outline" onClick={() => fileInput.current?.click()}><ArrowUpFromLine className="size-3.5" />{t('importConfig')}</Button>
                <input ref={fileInput} type="file" accept="application/json,.json" className="hidden" onChange={(event) => {
                  const file = event.target.files?.[0]
                  // Reset so choosing the same file twice fires again.
                  event.target.value = ''
                  if (file) void importFile(file)
                }} />
              </div>
            </div>
            {importError ? <p role="alert" className="rounded-xl border border-destructive/20 bg-destructive/5 p-3 text-sm text-destructive">{importError}</p> : null}
            <pre tabIndex={0} aria-label={t('configDraft')} className="max-h-[52vh] overflow-auto rounded-xl border bg-muted/35 p-4 font-mono text-xs leading-6 text-muted-foreground focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-ring sm:p-5">{json}</pre>
            <p className="text-xs leading-relaxed text-muted-foreground">{t('configDraftNote')}</p>
            <p className="text-xs leading-relaxed text-muted-foreground">{t('importHint')}</p>
          </div> : null}
        </Card>
      </div>
    </div>
  )
}
