import { Check, Copy, Download, Upload } from 'lucide-react'
import { useRef, useState } from 'react'

import { SecretInput } from '@/components/SecretInput'
import { Field, NumberField, SwitchField, TextField } from '@/components/fields'
import { Issues } from '@/components/Issues'
import { Button } from '@/components/ui/button'
import { Card, CardContent, CardHeader, CardTitle } from '@/components/ui/card'
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from '@/components/ui/select'
import { useI18n } from '@/lib/i18n'
import { normalizeConfig } from '@/lib/normalize'
import { useStore } from '@/store'

export function Settings() {
  const { working, update, loaded, serverReport, saveIssues } = useStore()
  const { t } = useI18n()
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

  async function copy() {
    try {
      await navigator.clipboard.writeText(json)
      setCopied(true)
      window.setTimeout(() => setCopied(false), 2000)
    } catch {
      /* clipboard needs a secure context; the text is selectable anyway */
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
      // Normalised at the boundary for the same reason the server's reply is: a
      // hand-written or older file omits every field at its default, and the
      // editors iterate over `headers` and `models`.
      const incoming = normalizeConfig(parsed)
      update((draft) => {
        draft.schema = incoming.schema
        draft.server = incoming.server
        draft.providers = incoming.providers
        draft.routes = incoming.routes
      })
    } catch (error) {
      setImportError(`${t('importFailed')}: ${error instanceof Error ? error.message : String(error)}`)
    }
  }

  return (
    <div className="flex flex-col gap-5">
      <Card>
        <CardHeader>
          <CardTitle>{t('tabSettings')}</CardTitle>
          <span className="ml-auto font-mono text-xs text-muted-foreground">
            {loaded?.path || ''}
          </span>
        </CardHeader>
        <CardContent className="grid gap-4 sm:grid-cols-2">
          <Field label="host">
            <TextField value={server.host} onChange={(host) => patch({ host })} />
          </Field>
          <Field label="port">
            <NumberField value={server.port} min={0} max={65535} onChange={(port) => patch({ port })} />
          </Field>

          <Field label="tls_cert_file" hint={t('hintTls')} wide>
            <TextField value={server.tls_cert_file} onChange={(tls_cert_file) => patch({ tls_cert_file })} />
          </Field>
          <Field label="tls_key_file" hint={t('hintTlsKey')} wide>
            <TextField value={server.tls_key_file} onChange={(tls_key_file) => patch({ tls_key_file })} />
          </Field>

          <Field label="api_key" hint={t('adminKeyHint')} wide>
            <SecretInput label={t('administratorKey')} value={server} onChange={patch} generate clear />
          </Field>

          <Field label="web_ui" hint={t('hintWebUi')}>
            <SwitchField
              checked={server.web_ui}
              onChange={(web_ui) => patch({ web_ui })}
              label={server.web_ui ? 'on' : 'off'}
            />
          </Field>
          <Field label="pass_through_unknown">
            <SwitchField
              checked={server.pass_through_unknown}
              onChange={(pass_through_unknown) => patch({ pass_through_unknown })}
              label="pass_through_unknown"
            />
          </Field>
          <Field label="skip_open_circuits">
            <SwitchField
              checked={server.skip_open_circuits}
              onChange={(skip_open_circuits) => patch({ skip_open_circuits })}
              label="skip_open_circuits"
            />
          </Field>

          <Field label="max_attempts">
            <NumberField
              value={server.max_attempts}
              min={0}
              onChange={(max_attempts) => patch({ max_attempts })}
            />
          </Field>
          <Field label="reload_on_change" hint={t('hintReload')}>
            <SwitchField
              checked={server.reload_on_change}
              onChange={(reload_on_change) => patch({ reload_on_change })}
              label="reload_on_change"
            />
          </Field>
          <Field label="session_affinity_sec" hint={t('hintAffinity')}>
            <NumberField
              value={server.session_affinity_sec}
              min={0}
              onChange={(session_affinity_sec) => patch({ session_affinity_sec })}
            />
          </Field>
          <Field label="routing_policy" hint={t('hintPolicy')}>
            <Select
              value={server.routing_policy}
              onValueChange={(routing_policy) => patch({ routing_policy })}
            >
              <SelectTrigger className="h-9 w-full font-mono text-xs">
                <SelectValue />
              </SelectTrigger>
              <SelectContent>
                <SelectItem value="priority">priority</SelectItem>
                <SelectItem value="fastest">fastest</SelectItem>
                <SelectItem value="cheapest">cheapest</SelectItem>
              </SelectContent>
            </Select>
          </Field>
          <Field label="request_deadline_sec" hint={t('hintDeadline')}>
            <NumberField
              value={server.request_deadline_sec}
              min={0}
              onChange={(request_deadline_sec) => patch({ request_deadline_sec })}
            />
          </Field>

          <Field label="traffic_bucket_sec" hint={t('hintBucketSec')}>
            <NumberField
              value={server.traffic_bucket_sec}
              min={60}
              step={60}
              onChange={(traffic_bucket_sec) => patch({ traffic_bucket_sec })}
            />
          </Field>
          <Field label="traffic_bucket_count" hint={t('hintBucketCount')}>
            <NumberField
              value={server.traffic_bucket_count}
              min={2}
              onChange={(traffic_bucket_count) => patch({ traffic_bucket_count })}
            />
          </Field>

          <Field label="response_cache_ttl_sec" hint={t('hintCacheTtl')}>
            <NumberField
              value={server.response_cache_ttl_sec}
              min={0}
              onChange={(response_cache_ttl_sec) => patch({ response_cache_ttl_sec })}
            />
          </Field>
          <Field label="response_cache_max_entries">
            <NumberField
              value={server.response_cache_max_entries}
              min={1}
              onChange={(response_cache_max_entries) => patch({ response_cache_max_entries })}
            />
          </Field>

          <Field label="otlp_endpoint" hint={t('hintOtlp')} wide>
            <TextField
              value={server.otlp_endpoint}
              placeholder="http://127.0.0.1:4318"
              onChange={(otlp_endpoint) => patch({ otlp_endpoint })}
            />
          </Field>

          <Field label="circuit_failure_threshold">
            <NumberField
              value={server.circuit_failure_threshold}
              min={1}
              onChange={(circuit_failure_threshold) => patch({ circuit_failure_threshold })}
            />
          </Field>
          <Field label="circuit_cooldown_sec">
            <NumberField
              value={server.circuit_cooldown_sec}
              min={1}
              onChange={(circuit_cooldown_sec) => patch({ circuit_cooldown_sec })}
            />
          </Field>

          <Field label="log_capacity">
            <NumberField
              value={server.log_capacity}
              min={16}
              onChange={(log_capacity) => patch({ log_capacity })}
            />
          </Field>
          <Field label="log_bodies" hint={t('hintLogBodies')}>
            <SwitchField
              checked={server.log_bodies}
              onChange={(log_bodies) => patch({ log_bodies })}
              label="log_bodies"
            />
          </Field>

          <Field label="log_body_limit">
            <NumberField
              value={server.log_body_limit}
              min={64}
              onChange={(log_body_limit) => patch({ log_body_limit })}
            />
          </Field>
          <Field label="persist_telemetry" hint={t('hintPersistTelemetry')}>
            <SwitchField
              checked={server.persist_telemetry}
              onChange={(persist_telemetry) => patch({ persist_telemetry })}
              label="persist_telemetry"
            />
          </Field>
        </CardContent>
      </Card>


      <Card>
        <CardHeader>
          <CardTitle>{t('tabConfig')}</CardTitle>
          <span className="ml-1 text-xs text-muted-foreground">{t('configNote')}</span>
          <div className="ml-auto flex items-center gap-1">
            <Button size="sm" variant="ghost" onClick={() => void copy()}>
              {copied ? <Check className="size-3.5" /> : <Copy className="size-3.5" />}
              {copied ? t('copied') : t('copy')}
            </Button>
            <Button size="sm" variant="ghost" onClick={download}>
              <Download className="size-3.5" />
              {t('exportConfig')}
            </Button>
            <Button size="sm" variant="ghost" onClick={() => fileInput.current?.click()}>
              <Upload className="size-3.5" />
              {t('importConfig')}
            </Button>
            <input
              ref={fileInput}
              type="file"
              accept="application/json,.json"
              className="hidden"
              onChange={(event) => {
                const file = event.target.files?.[0]
                // Reset so choosing the same file twice fires again.
                event.target.value = ''
                if (file) void importFile(file)
              }}
            />
          </div>
        </CardHeader>
        {issues.length || importError ? (
          <CardContent>
            {importError ? <p className="text-xs text-danger">{importError}</p> : null}
            {issues.length ? <Issues issues={issues} /> : null}
          </CardContent>
        ) : null}
        <CardContent className="pb-0 pt-0">
          <p className="text-xs text-muted-foreground">{t('importHint')}</p>
        </CardContent>
        <pre className="max-h-[46vh] overflow-auto bg-muted/40 p-4 font-mono text-xs leading-relaxed text-muted-foreground">
          {json}
        </pre>
      </Card>
    </div>
  )
}
